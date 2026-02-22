#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>

#define ID_LEN 128
#define NODE_ID_LEN 64
#define ADDR_STR_LEN 64
#define MSG_TYPE_LEN 32
#define MSG_BUF_SIZE 8192

/* Wire buffer must be large enough for a fully serialized gossip_msg_t.
   With MSG_BUF_SIZE=8192 and all other fixed fields, ~9 KB is safe. */
#define MAX_SERIALIZED_LEN 10240

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
