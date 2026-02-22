#include "serialization.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

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
        msg->payload   /* payload must already be valid JSON */
    );
}

/*
 * Minimal hand-rolled deserializer.
 * We use a two-pass approach:
 *   1. Parse all scalar fields with sscanf up to "payload":
 *   2. Find the payload JSON value by scanning for the key and
 *      copying everything until the final closing '}'.
 *
 * This is deliberately simple and tolerates the JSON structure
 * produced by serialize_message().  It does NOT handle arbitrary JSON.
 */
int deserialize_message(const char *buffer, gossip_msg_t *msg) {
    /* Temporary holders for sscanf */
    unsigned long long ts = 0;

    int items = sscanf(buffer,
        "{"
        "\"version\":%d,"
        "\"msg_id\":\"%127[^\"]\","
        "\"msg_type\":\"%31[^\"]\","
        "\"sender_id\":\"%63[^\"]\","
        "\"sender_addr\":\"%63[^\"]\","
        "\"timestamp_ms\":%llu,"
        "\"ttl\":%d,",
        &msg->version,
        msg->msg_id,
        msg->msg_type,
        msg->sender_id,
        msg->sender_addr,
        &ts,
        &msg->ttl
    );

    if (items < 7) return -1;

    msg->timestamp_ms = (uint64_t)ts;

    /* Extract payload: find "payload": and copy the JSON value */
    const char *key = "\"payload\":";
    const char *p = strstr(buffer, key);
    if (!p) return -1;
    p += strlen(key);

    /* Copy payload up to the last '}' of the outer object */
    const char *end = buffer + strlen(buffer);
    /* Walk back from end to find the closing '}' */
    while (end > p && *end != '}') end--;
    if (end <= p) return -1;

    size_t payload_len = (size_t)(end - p);
    if (payload_len >= MSG_BUF_SIZE) payload_len = MSG_BUF_SIZE - 1;

    memcpy(msg->payload, p, payload_len);
    msg->payload[payload_len] = '\0';

    /* Trim trailing whitespace/newlines */
    size_t l = strlen(msg->payload);
    while (l > 0 && (msg->payload[l-1] == '\n' || msg->payload[l-1] == '\r' ||
                     msg->payload[l-1] == ' ')) {
        msg->payload[--l] = '\0';
    }

    return 0;
}
