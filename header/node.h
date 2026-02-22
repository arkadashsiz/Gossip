#ifndef NODE_H
#define NODE_H

#include <pthread.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include "member.h"
#include "message.h"
#include "serialization.h"

#define MAX_SEEN_MSGS 2000

/* Store full gossip messages so we can respond to IWANT */
#define MAX_STORED_GOSSIP 500

typedef struct {
    char msg_id[ID_LEN];
    char serialized[MAX_SERIALIZED_LEN];   /* full wire-format for IWANT replies */
} stored_gossip_t;

typedef struct {
    char node_id[NODE_ID_LEN];      /* UUID string */
    char self_addr[ADDR_STR_LEN];   /* "127.0.0.1:8000" */

    int port;
    int fanout;
    int ttl;
    int running;
    int ping_interval;   /* seconds */
    int peer_timeout;    /* seconds */
    unsigned int seed;
    int sockfd;

    /* Hybrid Push-Pull parameters */
    int pull_interval;   /* seconds between IHAVE broadcasts (0 = disabled) */
    int max_ihave_ids;   /* max IDs per IHAVE message */

    /* Proof-of-Work */
    int pow_difficulty;  /* number of leading zero hex chars required (0 = disabled) */

    membership_t membership;

    char seen_ids[MAX_SEEN_MSGS][ID_LEN];
    int seen_count;

    /* Full-message store for IWANT */
    stored_gossip_t gossip_store[MAX_STORED_GOSSIP];
    int gossip_store_count;

    pthread_mutex_t lock;
    pthread_t listener_thread;
    pthread_t ping_thread;
    pthread_t pull_thread;   /* Hybrid Pull thread */

    FILE *log_file;
    uint64_t sent_messages;

} node_t;

/* Core Node Functions */
int node_init(node_t *node,
              int port, int fanout,
              int ttl, int peer_limit, int ping_interval, int peer_timeout,
              unsigned int seed, int pull_interval, int max_ihave_ids,
              int pow_difficulty);
void node_run(node_t *node);
void node_bootstrap(node_t *node, const char *boot_ip, int boot_port);
void node_cleanup(node_t *node);
uint64_t current_time_ms();

/* Internal Thread Logic */
void* listener_thread_func(void* arg);
void* ping_thread_func(void* arg);
void* pull_thread_func(void* arg);

/* Message Handlers */
void handle_hello(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_get_peers(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_peers_list(node_t *node, gossip_msg_t *msg);
void handle_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_ping(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_pong(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_ihave(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);
void handle_iwant(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender);

/* Helpers */
void relay_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *exclude);
void membership_remove_expired(node_t *node);
void log_event(node_t *node, const char *event, const char *msg_type,
               const char *msg_id);

/* PoW */
int  node_build_hello_payload(node_t *node, char *payload_buf, size_t buf_size);
int  node_verify_hello_pow(node_t *node, gossip_msg_t *msg);

/* Seen-set (lock must be held by caller) */
void mark_seen_public(node_t *node, const char *msg_id);

#endif
