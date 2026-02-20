#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <time.h>

static struct option long_options[] = {
    {"port", required_argument, 0, 'p'},
    {"fanout", required_argument, 0, 'f'},
    {"ttl", required_argument, 0, 't'},
    {"bootstrap", required_argument, 0, 'b'},
    {"peer-limit", required_argument, 0, 'l'},
    {"ping-interval", required_argument, 0, 'i'},
    {"peer-timeout", required_argument, 0, 'o'},
    {"seed", required_argument, 0, 's'},
    {0, 0, 0, 0}
};


void print_usage() {
    printf("Usage: ./gossip_node -p <port> [-f <fanout>] [-t <ttl>] [-b <ip:port>]\n");
}

int main(int argc, char *argv[]) {

    node_t node = {0};

    int port = 0, fanout = 3, ttl = 5, peer_limit = 20;
    char boot_ip[64] = {0};
    int boot_port = 0;
    int ping_interval = 2;
    int peer_timeout = 6;
    unsigned int seed = 42;
    int opt;
    while ((opt = getopt_long(argc, argv, "p:f:t:b:l:i:o:s:",
                              long_options, NULL)) != -1) {

        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'f': fanout = atoi(optarg); break;
            case 't': ttl = atoi(optarg); break;
            case 'b':
                sscanf(optarg, "%[^:]:%d", boot_ip, &boot_port);
                break;
            case 'l': peer_limit = atoi(optarg); break;
            case 'i': ping_interval = atoi(optarg); break;
            case 'o': peer_timeout = atoi(optarg); break;
            case 's': seed = (unsigned int)atoi(optarg); break;
            default:
                print_usage();
                return 1;
        }
    }

    if (port == 0) {
        print_usage();
        return 1;
    }

    if (node_init(&node,
              port,
              fanout,
              ttl,
              peer_limit,
              ping_interval,
              peer_timeout,
              seed) != 0)
 {
        fprintf(stderr, "Failed to init node\n");
        return 1;
    }

    if (boot_port > 0) {
        node_bootstrap(&node, boot_ip, boot_port);
    }

    node_run(&node);

    printf("Gossip Node started on port %d\n", port);
    printf("> ");

    char input[MSG_BUF_SIZE];

    while (fgets(input, MSG_BUF_SIZE, stdin)) {

        if (strncmp(input, "msg ", 4) == 0) {

            gossip_msg_t m;

            m.version = 1;

            snprintf(m.msg_id, ID_LEN, "%s_%llu",
                     node.node_id,
                     (unsigned long long)current_time_ms());

            strcpy(m.msg_type, "GOSSIP");

            strcpy(m.sender_id, node.node_id);
            strcpy(m.sender_addr, node.self_addr);

            m.timestamp_ms = current_time_ms();
            m.ttl = node.ttl;

            snprintf(m.payload, MSG_BUF_SIZE,
                     "{ \"topic\": \"news\", \"data\": \"%s\" }",
                     input + 4);

            // Add to seen set
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
