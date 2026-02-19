#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h> // Required for close()

void print_usage() {
    printf("Usage: ./gossip_node -p <port> [-f <fanout>] [-t <ttl>] [-b <ip:port>]\n");
}

int main(int argc, char *argv[]) {
    node_t node = {0};
    int port = 0, fanout = 3, ttl = 5, peer_limit = 20;
    char boot_ip[64] = {0};
    int boot_port = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:f:t:b:")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'f': fanout = atoi(optarg); break;
            case 't': ttl = atoi(optarg); break;
            case 'b': 
                sscanf(optarg, "%[^:]:%d", boot_ip, &boot_port);
                break;
            default: print_usage(); return 1;
        }
    }

    if (port == 0) { print_usage(); return 1; }

    if (node_init(&node, port, fanout, ttl, peer_limit) != 0) {
        fprintf(stderr, "Failed to init node\n");
        return 1;
    }

    if (boot_port > 0) {
        node_bootstrap(&node, boot_ip, boot_port);
    }

    node_run(&node); // Starts the listener thread

    printf("Gossip Node started on port %d. Type 'msg <text>' to gossip.\n", port);
    char input[MSG_BUF_SIZE];
    while(fgets(input, MSG_BUF_SIZE, stdin)) {
        if(strncmp(input, "msg ", 4) == 0) {
            gossip_msg_t m;
            // Create a unique ID: port_timestamp
            snprintf(m.msg_id, ID_LEN, "%d_%ld", port, (long)time(NULL));
            m.ttl = node.ttl;
            strncpy(m.payload, input + 4, MSG_BUF_SIZE);
            
            // Add to our own seen set so we don't process it again
            pthread_mutex_lock(&node.lock);
            strcpy(node.seen_ids[node.seen_count % MAX_SEEN_MSGS], m.msg_id);
            node.seen_count++;
            pthread_mutex_unlock(&node.lock);

            relay_gossip(&node, &m, NULL);
        }
        printf("> ");
    }

    node_cleanup(&node);
    return 0;
}