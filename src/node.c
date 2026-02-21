#include "node.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <uuid/uuid.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int node_init(node_t *node,
              int port, int fanout,
              int ttl, int peer_limit, int ping_interval, int peer_timeout, unsigned int seed) {



    srand(seed);
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, node->node_id);

    snprintf(node->self_addr, ADDR_STR_LEN, "127.0.0.1:%d", port);
    node->port = port;
    node->fanout = fanout;
    node->ttl = ttl;
    node->ping_interval = ping_interval;
    node->peer_timeout = peer_timeout;
    node->seed = seed;
    char log_name[64];
    snprintf(log_name, sizeof(log_name), "node_%d.log", port);

    node->log_file = fopen(log_name, "w");
    if (!node->log_file) {
        perror("log file");
        return -1;
    }

    node->sent_messages = 0;

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




void node_run(node_t *node) {
    pthread_create(&node->listener_thread, NULL, listener_thread_func, node);
    pthread_create(&node->ping_thread, NULL, ping_thread_func, node);

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
    node->sent_messages++;
    log_event(node, "SEND", hello.msg_type, hello.msg_id);
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
           node->sent_messages++;
log_event(node, "SEND", get.msg_type, get.msg_id);

    
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
        node->sent_messages++;
    log_event(node, "SEND", relay_copy.msg_type, relay_copy.msg_id);
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
        else if (strcmp(msg.msg_type, "PING") == 0) {
            handle_ping(node, &msg, &sender);
        }
        else if (strcmp(msg.msg_type, "PONG") == 0) {
            handle_pong(node, &msg, &sender);
        }

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
           node->sent_messages++;
log_event(node, "SEND", reply.msg_type, reply.msg_id);

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
        log_event(node, "RECEIVE", msg->msg_type, msg->msg_id);

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
    pthread_join(node->ping_thread, NULL);
    pthread_mutex_destroy(&node->lock);
    fclose(node->log_file);
}


void* ping_thread_func(void* arg) {

    node_t *node = (node_t*)arg;

    while (node->running) {

        sleep(node->ping_interval);

        struct sockaddr_in targets[MAX_PEERS];
        int count = membership_get_random(&node->membership,
                                          targets,
                                          node->fanout,
                                          NULL);

        for (int i = 0; i < count; i++) {

            gossip_msg_t ping;

            ping.version = 1;

            snprintf(ping.msg_id, ID_LEN, "PING_%llu",
                     (unsigned long long)current_time_ms());

            strcpy(ping.msg_type, "PING");
            strcpy(ping.sender_id, node->node_id);
            strcpy(ping.sender_addr, node->self_addr);

            ping.timestamp_ms = current_time_ms();
            ping.ttl = 1;

            snprintf(ping.payload, MSG_BUF_SIZE,
                     "{ \"ping_id\": \"%s\" }",
                     ping.msg_id);

            char buffer[MAX_SERIALIZED_LEN];
            serialize_message(&ping, buffer, MAX_SERIALIZED_LEN);

            sendto(node->sockfd,
                   buffer,
                   strlen(buffer),
                   0,
                   (struct sockaddr*)&targets[i],
                   sizeof(struct sockaddr_in));
                   
node->sent_messages++;
log_event(node, "SEND", ping.msg_type, ping.msg_id);
        }

        membership_remove_expired(node);
    }

    return NULL;
}

void membership_remove_expired(node_t *node) {

    pthread_mutex_lock(&node->membership.lock);

    uint64_t now = current_time_ms();

    for (int i = 0; i < node->membership.count; ) {

        uint64_t last = node->membership.list[i].last_seen;

        if ((now - last) > (uint64_t)(node->peer_timeout * 1000)) {

            printf("[Peer Removed] Timeout\n");

            node->membership.list[i] =
                node->membership.list[node->membership.count - 1];

            node->membership.count--;
        }
        else {
            i++;
        }
    }

    pthread_mutex_unlock(&node->membership.lock);
}

void handle_ping(node_t *node,
                 gossip_msg_t *msg,
                 struct sockaddr_in *sender) {

    membership_add(&node->membership, *sender);

    gossip_msg_t pong;

    pong.version = 1;

    snprintf(pong.msg_id, ID_LEN, "PONG_%llu",
             (unsigned long long)current_time_ms());

    strcpy(pong.msg_type, "PONG");
    strcpy(pong.sender_id, node->node_id);
    strcpy(pong.sender_addr, node->self_addr);

    pong.timestamp_ms = current_time_ms();
    pong.ttl = 1;

    snprintf(pong.payload, MSG_BUF_SIZE,
             "{ \"reply_to\": \"%s\" }",
             msg->msg_id);

    char buffer[MAX_SERIALIZED_LEN];
    serialize_message(&pong, buffer, MAX_SERIALIZED_LEN);

    sendto(node->sockfd,
           buffer,
           strlen(buffer),
           0,
           (struct sockaddr*)sender,
           sizeof(struct sockaddr_in));
           node->sent_messages++;
log_event(node, "SEND", pong.msg_type, pong.msg_id);

}


void handle_pong(node_t *node,
                 gossip_msg_t *msg,
                 struct sockaddr_in *sender) {

    membership_add(&node->membership, *sender);
}


void log_event(node_t *node,
               const char *event,
               const char *msg_type,
               const char *msg_id) {

    uint64_t now = current_time_ms();

    fprintf(node->log_file,
            "%llu,%s,%s,%s\n",
            (unsigned long long)now,
            event,
            msg_type,
            msg_id);

    fflush(node->log_file);
}
