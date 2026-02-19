#ifndef NODE_H
#define NODE_H

#include <pthread.h>
#include <netinet/in.h>
#include "member.h"  // Include our new membership management

#define MAX_SEEN_MSGS 1000
#define MSG_BUF_SIZE 1024
#define ID_LEN 64

/**
 * gossip_msg_t: The standard packet structure for Phase 2.
 * This is what gets sent over the wire via UDP.
 */
typedef struct {
    char msg_id[ID_LEN];    // Unique ID (e.g., "nodePort_sequence")
    int ttl;                // Time-To-Live (Phase 1 req)
    char payload[MSG_BUF_SIZE];
} gossip_msg_t;

/**
 * node_t: The central state of your Gossip Node.
 */
typedef struct {
    // Configuration
    int port;
    int fanout;
    int ttl;
    int running;
    
    // Networking
    int sockfd;
    
    // Membership Management (Updated Step)
    membership_t membership; 
    
    // Seen-Set (Phase 2 req: prevent infinite loops)
    char seen_ids[MAX_SEEN_MSGS][ID_LEN];
    int seen_count;
    
    // Concurrency
    pthread_mutex_t lock;
    pthread_t listener_thread;
} node_t;

// Core Node Functions
int  node_init(node_t *node, int port, int fanout, int ttl, int peer_limit);
void node_run(node_t *node);
void node_bootstrap(node_t *node, const char *boot_ip, int boot_port);
void node_cleanup(node_t *node);

// Internal Thread Logic
void* listener_thread_func(void* arg);
void handle_incoming_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void relay_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *exclude);

#endif