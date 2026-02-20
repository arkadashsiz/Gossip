#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>

#define ID_LEN 128
#define NODE_ID_LEN 64
#define ADDR_STR_LEN 64
#define MSG_TYPE_LEN 32
#define MSG_BUF_SIZE 4096

typedef struct {
    int version;

    char msg_id[ID_LEN];
    char msg_type[MSG_TYPE_LEN];

    char sender_id[NODE_ID_LEN];
    char sender_addr[ADDR_STR_LEN];

    uint64_t timestamp_ms;

    int ttl;

    char payload[MSG_BUF_SIZE];
} gossip_msg_t;

#endif
