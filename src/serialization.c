#include "serialization.h"
#include <stdio.h>
#include <string.h>

int serialize_message(const gossip_msg_t *msg, char *buffer, size_t buf_size) {
    return snprintf(buffer, buf_size,
        "{"
        "\"version\":%d,"
        "\"msg_id\":\"%s\","
        "\"msg_type\":\"%s\","
        "\"sender_id\":\"%s\","
        "\"sender_addr\":\"%s\","
        "\"timestamp_ms\":%llu,"
        "\"ttl\":%d,"
        "\"payload\":%s"
        "}",
        msg->version,
        msg->msg_id,
        msg->msg_type,
        msg->sender_id,
        msg->sender_addr,
        (unsigned long long)msg->timestamp_ms,
        msg->ttl,
        msg->payload
    );
}


int deserialize_message(const char *buffer, gossip_msg_t *msg) {
    int items = sscanf(buffer,
        "{"
        "\"version\":%d,"
        "\"msg_id\":\"%[^\"]\","
        "\"msg_type\":\"%[^\"]\","
        "\"sender_id\":\"%[^\"]\","
        "\"sender_addr\":\"%[^\"]\","
        "\"timestamp_ms\":%llu,"
        "\"ttl\":%d,"
        "\"payload\":%[^\n}]"
        "}",
        &msg->version,
        msg->msg_id,
        msg->msg_type,
        msg->sender_id,
        msg->sender_addr,
        &msg->timestamp_ms,
        &msg->ttl,
        msg->payload
    );

    return (items >= 7) ? 0 : -1;
}
