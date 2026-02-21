#ifndef NODE_H
#define NODE_H

#include <pthread.h>
#include <netinet/in.h>
#include <stdint.h>
#include "member.h"
#include "message.h"
#include "serialization.h"
#define MAX_SEEN_MSGS 1000


/**
 * node_t: The central state of your Gossip Node.
 */

typedef struct {
    char node_id[NODE_ID_LEN];      // UUID string
    char self_addr[ADDR_STR_LEN];   // "127.0.0.1:8000"
    
    int port;
    int fanout;
    int ttl;
    int running;
    int ping_interval;   // seconds
    int peer_timeout;    // seconds
    unsigned int seed;
    int sockfd;

    membership_t membership;

    char seen_ids[MAX_SEEN_MSGS][ID_LEN];
    int seen_count;

    pthread_mutex_t lock;
    pthread_t listener_thread;
    pthread_t ping_thread;
} node_t;

// Core Node Functions
int node_init(node_t *node,
              int port, int fanout,
              int ttl, int peer_limit, int ping_interval, int peer_timeout, unsigned int seed);
void node_run(node_t *node);
void node_bootstrap(node_t *node, const char *boot_ip, int boot_port);
void node_cleanup(node_t *node);
uint64_t current_time_ms();
// Internal Thread Logic
void* listener_thread_func(void* arg);
void handle_hello(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_get_peers(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_peers_list(node_t *node, gossip_msg_t *msg);
void handle_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_incoming_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void relay_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *exclude);
void* ping_thread_func(void* arg);
void handle_ping(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_pong(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void membership_remove_expired(node_t *node);

#endif