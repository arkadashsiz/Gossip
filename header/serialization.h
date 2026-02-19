#ifndef SERIALIZATION_H
#define SERIALIZATION_H

#include "node.h"

// Max size for the string representation of a message
#define MAX_SERIALIZED_LEN 2048

int serialize_message(const gossip_msg_t *msg, char *buffer, size_t buf_size);
int deserialize_message(const char *buffer, gossip_msg_t *msg);

#endif