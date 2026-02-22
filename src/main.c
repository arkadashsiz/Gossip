#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <arpa/inet.h>

node_t *global_node = NULL;

void handle_signal(int sig) {
    (void)sig;
    if (global_node) {
        global_node->running = 0;
        /* Unblock fgets() in the interactive loop by closing stdin */
        fclose(stdin);
    }
}

static struct option long_options[] = {
    {"port",          required_argument, 0, 'p'},
    {"fanout",        required_argument, 0, 'f'},
    {"ttl",           required_argument, 0, 't'},
    {"bootstrap",     required_argument, 0, 'b'},
    {"peer-limit",    required_argument, 0, 'l'},
    {"ping-interval", required_argument, 0, 'i'},
    {"peer-timeout",  required_argument, 0, 'o'},
    {"seed",          required_argument, 0, 's'},
    {"message",       required_argument, 0, 'm'},
    /* Hybrid Push-Pull */
    {"pull-interval", required_argument, 0, 'q'},
    {"max-ihave-ids", required_argument, 0, 'x'},
    /* PoW */
    {"pow-difficulty",required_argument, 0, 'k'},
    {0, 0, 0, 0}
};

void print_usage() {
    printf(
        "Usage: ./gossip_node -p <port> [options]\n"
        "Options:\n"
        "  -p, --port           <port>        Listen port (required)\n"
        "  -f, --fanout         <n>           Gossip fanout (default 3)\n"
        "  -t, --ttl            <n>           Message TTL (default 5)\n"
        "  -b, --bootstrap      <ip:port>     Bootstrap node address\n"
        "  -l, --peer-limit     <n>           Max peers (default 20)\n"
        "  -i, --ping-interval  <secs>        PING interval (default 2)\n"
        "  -o, --peer-timeout   <secs>        Peer timeout (default 6)\n"
        "  -s, --seed           <n>           RNG seed (default 42)\n"
        "  -m, --message        <text>        Auto-inject a GOSSIP message\n"
        "  -q, --pull-interval  <secs>        IHAVE broadcast interval (0=off, default 0)\n"
        "  -x, --max-ihave-ids  <n>           Max IDs per IHAVE (default 32)\n"
        "  -k, --pow-difficulty <n>           PoW leading-zero nibbles (0=off, default 0)\n"
    );
}

int main(int argc, char *argv[]) {
    char auto_message[MSG_BUF_SIZE] = {0};

    int port           = 0;
    int fanout         = 3;
    int ttl            = 5;
    int peer_limit     = 20;
    int ping_interval  = 2;
    int peer_timeout   = 6;
    int pull_interval  = 0;
    int max_ihave_ids  = 32;
    int pow_difficulty = 0;
    unsigned int seed  = 42;
    char boot_ip[64]   = {0};
    int  boot_port     = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "p:f:t:b:l:i:o:s:m:q:x:k:",
                              long_options, NULL)) != -1) {
        switch (opt) {
            case 'p': port           = atoi(optarg); break;
            case 'f': fanout         = atoi(optarg); break;
            case 't': ttl            = atoi(optarg); break;
            case 'b': sscanf(optarg, "%63[^:]:%d", boot_ip, &boot_port); break;
            case 'l': peer_limit     = atoi(optarg); break;
            case 'i': ping_interval  = atoi(optarg); break;
            case 'o': peer_timeout   = atoi(optarg); break;
            case 's': seed           = (unsigned int)atoi(optarg); break;
            case 'm': strncpy(auto_message, optarg, MSG_BUF_SIZE - 1); break;
            case 'q': pull_interval  = atoi(optarg); break;
            case 'x': max_ihave_ids  = atoi(optarg); break;
            case 'k': pow_difficulty = atoi(optarg); break;
            default:  print_usage(); return 1;
        }
    }

    if (port == 0) { print_usage(); return 1; }

    node_t node;
    if (node_init(&node, port, fanout, ttl, peer_limit,
                  ping_interval, peer_timeout, seed,
                  pull_interval, max_ihave_ids, pow_difficulty) != 0) {
        fprintf(stderr, "Failed to init node\n");
        return 1;
    }

    global_node = &node;
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    if (boot_port > 0) {
        node_bootstrap(&node, boot_ip, boot_port);
    }

    node_run(&node);
    printf("Gossip Node started on port %d\n", port);

    /* Brief delay so listener is ready before injecting */
    usleep(200000);

    /* --- Auto-inject message if provided --- */
    if (strlen(auto_message) > 0) {
        gossip_msg_t m;
        memset(&m, 0, sizeof(m));
        m.version = 1;
        snprintf(m.msg_id, ID_LEN, "%s_%llu",
                 node.node_id, (unsigned long long)current_time_ms());
        strcpy(m.msg_type,    "GOSSIP");
        strcpy(m.sender_id,   node.node_id);
        strcpy(m.sender_addr, node.self_addr);
        m.timestamp_ms = current_time_ms();
        m.ttl = node.ttl;
        snprintf(m.payload, MSG_BUF_SIZE,
                 "{ \"topic\": \"news\", \"data\": \"%s\" }",
                 auto_message);

        pthread_mutex_lock(&node.lock);
        mark_seen_public(&node, m.msg_id);
        pthread_mutex_unlock(&node.lock);

        /* store for IWANT */
        pthread_mutex_lock(&node.lock);
        /* use internal store helper indirectly via log_event trick – 
           actually call relay so it also gets stored in listener path.
           We directly store here: */
        {
            int idx = node.gossip_store_count % MAX_STORED_GOSSIP;
            strncpy(node.gossip_store[idx].msg_id, m.msg_id, ID_LEN - 1);
            serialize_message(&m, node.gossip_store[idx].serialized,
                              MAX_SERIALIZED_LEN);
            node.gossip_store_count++;
        }
        pthread_mutex_unlock(&node.lock);

        log_event(&node, "SEND", m.msg_type, m.msg_id);
        relay_gossip(&node, &m, NULL);
    }

    /* --- Interactive or non-interactive mode ---
     * We treat the node as interactive only if stdin is a tty AND
     * no -m flag was given (experiment/injector nodes are non-interactive).
     * Background processes launched by experiment.sh have stdin redirected
     * to /dev/null by the script, so isatty() returns 0 for them.       */
    if (isatty(STDIN_FILENO) && strlen(auto_message) == 0) {
        char input[MSG_BUF_SIZE];
        printf("> ");
        fflush(stdout);

        while (node.running && fgets(input, MSG_BUF_SIZE, stdin)) {
            /* Strip newline */
            input[strcspn(input, "\n")] = '\0';

            if (strncmp(input, "msg ", 4) == 0) {
                gossip_msg_t m;
                memset(&m, 0, sizeof(m));
                m.version = 1;
                snprintf(m.msg_id, ID_LEN, "%s_%llu",
                         node.node_id,
                         (unsigned long long)current_time_ms());
                strcpy(m.msg_type,    "GOSSIP");
                strcpy(m.sender_id,   node.node_id);
                strcpy(m.sender_addr, node.self_addr);
                m.timestamp_ms = current_time_ms();
                m.ttl = node.ttl;
                snprintf(m.payload, MSG_BUF_SIZE,
                         "{ \"topic\": \"news\", \"data\": \"%s\" }",
                         input + 4);

                pthread_mutex_lock(&node.lock);
                mark_seen_public(&node, m.msg_id);
                {
                    int idx = node.gossip_store_count % MAX_STORED_GOSSIP;
                    strncpy(node.gossip_store[idx].msg_id,
                            m.msg_id, ID_LEN - 1);
                    serialize_message(&m, node.gossip_store[idx].serialized,
                                      MAX_SERIALIZED_LEN);
                    node.gossip_store_count++;
                }
                pthread_mutex_unlock(&node.lock);

                log_event(&node, "SEND", m.msg_type, m.msg_id);
                relay_gossip(&node, &m, NULL);

            } else if (strcmp(input, "peers") == 0) {
                pthread_mutex_lock(&node.membership.lock);
                printf("Peers (%d):\n", node.membership.count);
                for (int i = 0; i < node.membership.count; i++) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &node.membership.list[i].addr.sin_addr,
                              ip, INET_ADDRSTRLEN);
                    printf("  %s:%d\n", ip,
                           ntohs(node.membership.list[i].addr.sin_port));
                }
                pthread_mutex_unlock(&node.membership.lock);
            } else if (strcmp(input, "quit") == 0 ||
                       strcmp(input, "exit") == 0) {
                break;
            } else if (strlen(input) > 0) {
                printf("Commands: msg <text> | peers | quit\n");
            }

            printf("> ");
            fflush(stdout);
        }
    } else {
        /* Non-interactive (experiment node or injector with -m flag).
         * sleep(1) is interrupted by SIGTERM on Linux, but we use a
         * short loop to be safe. */
        while (node.running) {
            struct timespec ts = {0, 100000000L}; /* 100 ms */
            nanosleep(&ts, NULL);
        }
    }

    node_cleanup(&node);
    return 0;
}
