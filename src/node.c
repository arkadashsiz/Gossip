#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <uuid/uuid.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int node_init(node_t *node, int port, int fanout, int ttl, int peer_limit) {

    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, node->node_id);

    snprintf(node->self_addr, ADDR_STR_LEN, "127.0.0.1:%d", port);
    node->port = port;
    node->fanout = fanout;
    node->ttl = ttl;
    node->running = 1;
    node->seen_count = 0;
    pthread_mutex_init(&node->lock, NULL);

    membership_init(&node->membership, peer_limit);

    node->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (node->sockfd < 0) return -1;

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(port);

    if (bind(node->sockfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) return -1;

    return 0;
}

uint64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}




void node_run(node_t *node) {
    pthread_create(&node->listener_thread, NULL, listener_thread_func, node);
}

void node_bootstrap(node_t *node, const char *boot_ip, int boot_port) {

    struct sockaddr_in boot_addr;
    memset(&boot_addr, 0, sizeof(boot_addr));
    boot_addr.sin_family = AF_INET;
    boot_addr.sin_port = htons(boot_port);
    inet_pton(AF_INET, boot_ip, &boot_addr.sin_addr);

    membership_add(&node->membership, boot_addr);

    gossip_msg_t hello;

    hello.version = 1;

    snprintf(hello.msg_id, ID_LEN, "HELLO_%s", node->node_id);
    strcpy(hello.msg_type, "HELLO");

    strcpy(hello.sender_id, node->node_id);
    strcpy(hello.sender_addr, node->self_addr);

    hello.timestamp_ms = current_time_ms();
    hello.ttl = 1;

    snprintf(hello.payload, MSG_BUF_SIZE,
             "{ \"capabilities\": [\"udp\", \"json\"] }");

    char buffer[MAX_SERIALIZED_LEN];
    serialize_message(&hello, buffer, MAX_SERIALIZED_LEN);

    sendto(node->sockfd,
           buffer,
           strlen(buffer),
           0,
           (struct sockaddr*)&boot_addr,
           sizeof(boot_addr));
    gossip_msg_t get;

    get.version = 1;
    
    snprintf(get.msg_id, ID_LEN, "GET_%llu",
             (unsigned long long)current_time_ms());
    
    strcpy(get.msg_type, "GET_PEERS");
    
    strcpy(get.sender_id, node->node_id);
    strcpy(get.sender_addr, node->self_addr);
    
    get.timestamp_ms = current_time_ms();
    get.ttl = 1;
    
    snprintf(get.payload, MSG_BUF_SIZE,
             "{ \"max_peers\": 20 }");
    
    serialize_message(&get, buffer, MAX_SERIALIZED_LEN);
    
    sendto(node->sockfd,
           buffer,
           strlen(buffer),
           0,
           (struct sockaddr*)&boot_addr,
           sizeof(boot_addr));
    
}

void relay_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *exclude) {
    if (msg->ttl <= 0) return;
    
    // Create a copy to modify TTL without affecting original seen-set logic
    gossip_msg_t relay_copy = *msg;
    relay_copy.ttl--;

    char wire_buffer[MAX_SERIALIZED_LEN];
    serialize_message(&relay_copy, wire_buffer, MAX_SERIALIZED_LEN);

    struct sockaddr_in targets[MAX_PEERS];
    int count = membership_get_random(&node->membership, targets, node->fanout, exclude);

    for (int i = 0; i < count; i++) {
        sendto(node->sockfd, wire_buffer, strlen(wire_buffer), 0, 
               (struct sockaddr*)&targets[i], sizeof(struct sockaddr_in));
    }
}

void* listener_thread_func(void* arg) {

    node_t *node = (node_t*)arg;

    struct sockaddr_in sender;
    socklen_t len = sizeof(sender);

    char recv_buf[MAX_SERIALIZED_LEN];

    while (node->running) {

        ssize_t rec = recvfrom(node->sockfd,
                               recv_buf,
                               MAX_SERIALIZED_LEN - 1,
                               0,
                               (struct sockaddr*)&sender,
                               &len);

        if (rec <= 0)
            continue;

        recv_buf[rec] = '\0';

        gossip_msg_t msg;

        if (deserialize_message(recv_buf, &msg) != 0)
            continue;   // drop malformed JSON

        // Dispatch by message type

        if (strcmp(msg.msg_type, "HELLO") == 0) {

            handle_hello(node, &msg, &sender);

        }
        else if (strcmp(msg.msg_type, "GET_PEERS") == 0) {

            handle_get_peers(node, &msg, &sender);

        }
        else if (strcmp(msg.msg_type, "PEERS_LIST") == 0) {

            handle_peers_list(node, &msg);

        }
        else if (strcmp(msg.msg_type, "GOSSIP") == 0) {

            handle_gossip(node, &msg, &sender);

        }

        //
        // else if PING
        // else if PONG
        // else if IHAVE
        // else if IWANT
    }

    return NULL;
}


void handle_hello(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {

    membership_add(&node->membership, *sender);

    // Send PEERS_LIST back
    handle_get_peers(node, msg, sender);
}


void handle_get_peers(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {

    gossip_msg_t reply;

    reply.version = 1;

    snprintf(reply.msg_id, ID_LEN, "PEERS_%llu",
             (unsigned long long)current_time_ms());

    strcpy(reply.msg_type, "PEERS_LIST");

    strcpy(reply.sender_id, node->node_id);
    strcpy(reply.sender_addr, node->self_addr);

    reply.timestamp_ms = current_time_ms();
    reply.ttl = 1;

    // Build JSON array of peers
    char peers_json[MSG_BUF_SIZE] = "[";
    pthread_mutex_lock(&node->membership.lock);

    for (int i = 0; i < node->membership.count; i++) {
        char entry[128];
        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET,
                  &node->membership.list[i].addr.sin_addr,
                  ip,
                  INET_ADDRSTRLEN);

        int port = ntohs(node->membership.list[i].addr.sin_port);

        snprintf(entry, sizeof(entry),
                 "{\"addr\":\"%s:%d\"}%s",
                 ip, port,
                 (i < node->membership.count - 1) ? "," : "");

        strcat(peers_json, entry);
    }

    pthread_mutex_unlock(&node->membership.lock);

    strcat(peers_json, "]");

    snprintf(reply.payload, MSG_BUF_SIZE,
             "{ \"peers\": %s }",
             peers_json);

    char buffer[MAX_SERIALIZED_LEN];
    serialize_message(&reply, buffer, MAX_SERIALIZED_LEN);

    sendto(node->sockfd,
           buffer,
           strlen(buffer),
           0,
           (struct sockaddr*)sender,
           sizeof(struct sockaddr_in));
}


void handle_peers_list(node_t *node, gossip_msg_t *msg) {

    // VERY simple parsing (we assume format correct)
    char *ptr = strstr(msg->payload, "addr\":\"");
    while (ptr) {
        ptr += 7;

        char ip[64];
        int port;

        sscanf(ptr, "%[^:]:%d", ip, &port);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);

        membership_add(&node->membership, addr);

        ptr = strstr(ptr, "addr\":\"");
    }
}


void handle_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {

    pthread_mutex_lock(&node->lock);

    int already_seen = 0;
    int limit = (node->seen_count > MAX_SEEN_MSGS)
                ? MAX_SEEN_MSGS
                : node->seen_count;

    for (int i = 0; i < limit; i++) {
        if (strcmp(node->seen_ids[i % MAX_SEEN_MSGS], msg->msg_id) == 0) {
            already_seen = 1;
            break;
        }
    }

    if (!already_seen) {

        printf("\n[Received GOSSIP] %s from %s\n> ",
               msg->payload,
               msg->sender_addr);

        strcpy(node->seen_ids[node->seen_count % MAX_SEEN_MSGS],
               msg->msg_id);

        node->seen_count++;

        pthread_mutex_unlock(&node->lock);

        relay_gossip(node, msg, sender);
    }
    else {
        pthread_mutex_unlock(&node->lock);
    }
}



void node_cleanup(node_t *node) {
    node->running = 0;
    close(node->sockfd);
    pthread_join(node->listener_thread, NULL);
    pthread_mutex_destroy(&node->lock);
}