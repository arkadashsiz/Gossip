#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int node_init(node_t *node, int port, int fanout, int ttl, int peer_limit) {
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

    // Initial message to let the bootstrap node know we exist
    gossip_msg_t join_msg;
    snprintf(join_msg.msg_id, ID_LEN, "JOIN_%d", node->port);
    join_msg.ttl = 1;
    strcpy(join_msg.payload, "HELLO");
    sendto(node->sockfd, &join_msg, sizeof(gossip_msg_t), 0, (struct sockaddr*)&boot_addr, sizeof(boot_addr));
}

void relay_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *exclude) {
    if (msg->ttl <= 0) return;
    msg->ttl--;

    struct sockaddr_in targets[MAX_PEERS];
    int count = membership_get_random(&node->membership, targets, node->fanout, exclude);

    for (int i = 0; i < count; i++) {
        sendto(node->sockfd, msg, sizeof(gossip_msg_t), 0, (struct sockaddr*)&targets[i], sizeof(struct sockaddr_in));
    }
}

void* listener_thread_func(void* arg) {
    node_t *node = (node_t*)arg;
    gossip_msg_t msg;
    struct sockaddr_in sender;
    socklen_t len = sizeof(sender);

    while (node->running) {
        ssize_t rec = recvfrom(node->sockfd, &msg, sizeof(gossip_msg_t), 0, (struct sockaddr*)&sender, &len);
        if (rec <= 0) continue;

        // Automatically learn about the sender (Peer Discovery)
        membership_add(&node->membership, sender);

        pthread_mutex_lock(&node->lock);
        int already_seen = 0;
        for (int i = 0; i < node->seen_count && i < MAX_SEEN_MSGS; i++) {
            if (strcmp(node->seen_ids[i], msg.msg_id) == 0) {
                already_seen = 1;
                break;
            }
        }

        if (!already_seen) {
            printf("\n[Received] ID: %s | Content: %s\n> ", msg.msg_id, msg.payload);
            fflush(stdout);
            
            strncpy(node->seen_ids[node->seen_count % MAX_SEEN_MSGS], msg.msg_id, ID_LEN);
            node->seen_count++;
            pthread_mutex_unlock(&node->lock);

            relay_gossip(node, &msg, &sender);
        } else {
            pthread_mutex_unlock(&node->lock);
        }
    }
    return NULL;
}

void node_cleanup(node_t *node) {
    node->running = 0;
    close(node->sockfd);
    pthread_join(node->listener_thread, NULL);
    pthread_mutex_destroy(&node->lock);
}