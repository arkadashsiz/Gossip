#ifndef SERIALIZATION_H
#define SERIALIZATION_H

#include "message.h"

typedef unsigned long size_t;
int serialize_message(const gossip_msg_t *msg, char *buffer, size_t buf_size);
int deserialize_message(const char *buffer, gossip_msg_t *msg);

#endif
