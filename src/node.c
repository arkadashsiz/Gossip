#include "node.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <uuid/uuid.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

/* =========================================================
 * Helpers
 * ========================================================= */

static void send_msg(node_t *node, gossip_msg_t *msg,
                     struct sockaddr_in *dest) {
    char buf[MAX_SERIALIZED_LEN];
    int len = serialize_message(msg, buf, sizeof(buf));
    if (len <= 0) return;
    sendto(node->sockfd, buf, (size_t)len, 0,
           (struct sockaddr *)dest, sizeof(struct sockaddr_in));
    node->sent_messages++;
    log_event(node, "SEND", msg->msg_type, msg->msg_id);
}

/* Mark a msg_id as seen.  Returns 1 if it was already seen, 0 if new. */
static int mark_seen(node_t *node, const char *msg_id) {
    int limit = (node->seen_count < MAX_SEEN_MSGS)
                ? node->seen_count : MAX_SEEN_MSGS;
    for (int i = 0; i < limit; i++) {
        if (strcmp(node->seen_ids[i % MAX_SEEN_MSGS], msg_id) == 0)
            return 1;
    }
    strcpy(node->seen_ids[node->seen_count % MAX_SEEN_MSGS], msg_id);
    node->seen_count++;
    return 0;
}

/* Store the serialized form of a gossip message for later IWANT replies */
static void store_gossip(node_t *node, gossip_msg_t *msg) {
    int idx = node->gossip_store_count % MAX_STORED_GOSSIP;
    strncpy(node->gossip_store[idx].msg_id, msg->msg_id, ID_LEN - 1);
    serialize_message(msg, node->gossip_store[idx].serialized,
                      MAX_SERIALIZED_LEN);
    node->gossip_store_count++;
}

/* Look up stored gossip by msg_id.  Returns pointer or NULL. */
static stored_gossip_t *find_stored(node_t *node, const char *msg_id) {
    int total = (node->gossip_store_count < MAX_STORED_GOSSIP)
                ? node->gossip_store_count : MAX_STORED_GOSSIP;
    for (int i = 0; i < total; i++) {
        if (strcmp(node->gossip_store[i].msg_id, msg_id) == 0)
            return &node->gossip_store[i];
    }
    return NULL;
}


int node_build_hello_payload(node_t *node, char *payload_buf, size_t buf_size) {
    if (node->pow_difficulty <= 0) {
        snprintf(payload_buf, buf_size,
                 "{ \"capabilities\": [\"udp\", \"json\"] }");
        return 0;
    }

    /* Mine a nonce */
    unsigned long nonce = 0;
    char digest[65];
    pow_mine(node->node_id, node->pow_difficulty, &nonce, digest);

    snprintf(payload_buf, buf_size,
             "{ \"capabilities\": [\"udp\", \"json\"], "
             "\"pow\": { \"hash_alg\": \"sha256\", "
             "\"difficulty_k\": %d, "
             "\"nonce\": %lu, "
             "\"digest_hex\": \"%s\" } }",
             node->pow_difficulty, nonce, digest);
    return 0;
}

int node_verify_hello_pow(node_t *node, gossip_msg_t *msg) {
    if (node->pow_difficulty <= 0) return 1;  /* PoW disabled */

    /* Parse nonce and sender_id from payload.
     * Payload format: { ... "pow": { ... "nonce": NNN, "digest_hex": "HHH" } } */
    unsigned long nonce = 0;
    char *p = strstr(msg->payload, "\"nonce\":");
    if (!p) return 0;
    p += 8;
    while (*p == ' ') p++;
    nonce = strtoul(p, NULL, 10);

    char digest[65];
    int ok = pow_check(msg->sender_id, nonce, node->pow_difficulty, digest);
    if (!ok) {
        fprintf(stderr, "[PoW] HELLO from %s rejected (bad PoW)\n",
                msg->sender_addr);
    }
    return ok;
}

/* Public wrapper (caller must hold node->lock) */
void mark_seen_public(node_t *node, const char *msg_id) {
    mark_seen(node, msg_id);
}


int node_init(node_t *node,
              int port, int fanout, int ttl, int peer_limit,
              int ping_interval, int peer_timeout, unsigned int seed,
              int pull_interval, int max_ihave_ids, int pow_difficulty) {

    memset(node, 0, sizeof(*node));

    srand(seed);

    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, node->node_id);

    snprintf(node->self_addr, ADDR_STR_LEN, "127.0.0.1:%d", port);
    node->port           = port;
    node->fanout         = fanout;
    node->ttl            = ttl;
    node->ping_interval  = ping_interval;
    node->peer_timeout   = peer_timeout;
    node->seed           = seed;
    node->running        = 1;
    node->seen_count     = 0;
    node->gossip_store_count = 0;
    node->pull_interval  = pull_interval;
    node->max_ihave_ids  = (max_ihave_ids > 0) ? max_ihave_ids : 32;
    node->pow_difficulty = pow_difficulty;

    char log_name[64];
    snprintf(log_name, sizeof(log_name), "node_%d.log", port);
    node->log_file = fopen(log_name, "w");
    if (!node->log_file) { perror("log file"); return -1; }

    node->sent_messages = 0;
    pthread_mutex_init(&node->lock, NULL);
    membership_init(&node->membership, peer_limit);

    node->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (node->sockfd < 0) return -1;

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family      = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port        = htons(port);

    int opt = 1;
    setsockopt(node->sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Wake up recvfrom every 500 ms so the listener thread can check running */
    struct timeval tv = {0, 500000};
    setsockopt(node->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(node->sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("bind"); return -1;
    }

    return 0;
}

void node_run(node_t *node) {
    pthread_create(&node->listener_thread, NULL, listener_thread_func, node);
    pthread_create(&node->ping_thread,     NULL, ping_thread_func,     node);
    if (node->pull_interval > 0)
        pthread_create(&node->pull_thread, NULL, pull_thread_func,     node);
}

void node_bootstrap(node_t *node, const char *boot_ip, int boot_port) {
    struct sockaddr_in boot_addr;
    memset(&boot_addr, 0, sizeof(boot_addr));
    boot_addr.sin_family = AF_INET;
    boot_addr.sin_port   = htons(boot_port);
    inet_pton(AF_INET, boot_ip, &boot_addr.sin_addr);

    membership_add(&node->membership, boot_addr);

    /* --- HELLO --- */
    gossip_msg_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.version = 1;
    snprintf(hello.msg_id, ID_LEN, "HELLO_%s", node->node_id);
    strcpy(hello.msg_type,   "HELLO");
    strcpy(hello.sender_id,  node->node_id);
    strcpy(hello.sender_addr, node->self_addr);
    hello.timestamp_ms = current_time_ms();
    hello.ttl = 1;
    node_build_hello_payload(node, hello.payload, MSG_BUF_SIZE);
    send_msg(node, &hello, &boot_addr);

    /* --- GET_PEERS --- */
    gossip_msg_t get;
    memset(&get, 0, sizeof(get));
    get.version = 1;
    snprintf(get.msg_id, ID_LEN, "GET_%llu",
             (unsigned long long)current_time_ms());
    strcpy(get.msg_type,   "GET_PEERS");
    strcpy(get.sender_id,  node->node_id);
    strcpy(get.sender_addr, node->self_addr);
    get.timestamp_ms = current_time_ms();
    get.ttl = 1;
    snprintf(get.payload, MSG_BUF_SIZE, "{ \"max_peers\": 20 }");
    send_msg(node, &get, &boot_addr);
}

void node_cleanup(node_t *node) {
    node->running = 0;
    close(node->sockfd);
    pthread_join(node->listener_thread, NULL);
    pthread_join(node->ping_thread, NULL);
    if (node->pull_interval > 0)
        pthread_join(node->pull_thread, NULL);
    pthread_mutex_destroy(&node->lock);
    if (node->log_file) fclose(node->log_file);
}


void relay_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *exclude) {
    if (msg->ttl <= 0) return;

    gossip_msg_t relay = *msg;
    relay.ttl--;

    struct sockaddr_in targets[MAX_PEERS];
    int count = membership_get_random(&node->membership, targets,
                                      node->fanout, exclude);
    for (int i = 0; i < count; i++) {
        send_msg(node, &relay, &targets[i]);
    }
}

/* =========================================================
 * Listener thread
 * ========================================================= */

void* listener_thread_func(void *arg) {
    node_t *node = (node_t *)arg;

    struct sockaddr_in sender;
    socklen_t len = sizeof(sender);
    char recv_buf[MAX_SERIALIZED_LEN];

    while (node->running) {
        ssize_t rec = recvfrom(node->sockfd, recv_buf,
                               sizeof(recv_buf) - 1, 0,
                               (struct sockaddr *)&sender, &len);
        if (rec <= 0) continue;
        recv_buf[rec] = '\0';

        gossip_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (deserialize_message(recv_buf, &msg) != 0) continue;

        if      (strcmp(msg.msg_type, "HELLO")      == 0) handle_hello(node, &msg, &sender);
        else if (strcmp(msg.msg_type, "GET_PEERS")  == 0) handle_get_peers(node, &msg, &sender);
        else if (strcmp(msg.msg_type, "PEERS_LIST") == 0) handle_peers_list(node, &msg);
        else if (strcmp(msg.msg_type, "GOSSIP")     == 0) handle_gossip(node, &msg, &sender);
        else if (strcmp(msg.msg_type, "PING")        == 0) handle_ping(node, &msg, &sender);
        else if (strcmp(msg.msg_type, "PONG")        == 0) handle_pong(node, &msg, &sender);
        else if (strcmp(msg.msg_type, "IHAVE")       == 0) handle_ihave(node, &msg, &sender);
        else if (strcmp(msg.msg_type, "IWANT")       == 0) handle_iwant(node, &msg, &sender);
    }
    return NULL;
}

/* =========================================================
 * Message Handlers
 * ========================================================= */

void handle_hello(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {
    /* Validate PoW before accepting the peer */
    if (!node_verify_hello_pow(node, msg)) return;

    membership_add(&node->membership, *sender);
    printf("[HELLO] from %s\n> ", msg->sender_addr);

    /* Respond with our peer list */
    handle_get_peers(node, msg, sender);
}

void handle_get_peers(node_t *node, gossip_msg_t *msg,
                      struct sockaddr_in *sender) {
    gossip_msg_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.version = 1;
    snprintf(reply.msg_id, ID_LEN, "PEERS_%llu",
             (unsigned long long)current_time_ms());
    strcpy(reply.msg_type,    "PEERS_LIST");
    strcpy(reply.sender_id,   node->node_id);
    strcpy(reply.sender_addr, node->self_addr);
    reply.timestamp_ms = current_time_ms();
    reply.ttl = 1;

    /* Build JSON array */
    char peers_json[MSG_BUF_SIZE] = "[";
    pthread_mutex_lock(&node->membership.lock);
    for (int i = 0; i < node->membership.count; i++) {
        char entry[192];
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &node->membership.list[i].addr.sin_addr,
                  ip, INET_ADDRSTRLEN);
        int p = ntohs(node->membership.list[i].addr.sin_port);
        snprintf(entry, sizeof(entry),
                 "{\"addr\":\"%s:%d\"}%s",
                 ip, p,
                 (i < node->membership.count - 1) ? "," : "");
        strncat(peers_json, entry,
                sizeof(peers_json) - strlen(peers_json) - 1);
    }
    pthread_mutex_unlock(&node->membership.lock);
    strncat(peers_json, "]",
            sizeof(peers_json) - strlen(peers_json) - 1);

    snprintf(reply.payload, MSG_BUF_SIZE,
             "{ \"peers\": %s }", peers_json);

    send_msg(node, &reply, sender);
}

void handle_peers_list(node_t *node, gossip_msg_t *msg) {
    /* Simple parser – tolerates our own PEERS_LIST format */
    char *ptr = strstr(msg->payload, "addr\":\"");
    while (ptr) {
        ptr += 7;
        char ip[64] = {0};
        int  port   = 0;
        if (sscanf(ptr, "%63[^:]:%d", ip, &port) == 2) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            inet_pton(AF_INET, ip, &addr.sin_addr);
            membership_add(&node->membership, addr);
        }
        ptr = strstr(ptr + 1, "addr\":\"");
    }
}

void handle_gossip(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {
    pthread_mutex_lock(&node->lock);

    if (mark_seen(node, msg->msg_id)) {
        /* Already seen – drop */
        pthread_mutex_unlock(&node->lock);
        return;
    }

    /* New message */
    printf("\n[GOSSIP] %s from %s\n> ", msg->payload, msg->sender_addr);
    log_event(node, "RECEIVE", msg->msg_type, msg->msg_id);
    store_gossip(node, msg);

    pthread_mutex_unlock(&node->lock);

    relay_gossip(node, msg, sender);
}

void handle_ping(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {
    membership_add(&node->membership, *sender);

    gossip_msg_t pong;
    memset(&pong, 0, sizeof(pong));
    pong.version = 1;
    snprintf(pong.msg_id, ID_LEN, "PONG_%llu",
             (unsigned long long)current_time_ms());
    strcpy(pong.msg_type,    "PONG");
    strcpy(pong.sender_id,   node->node_id);
    strcpy(pong.sender_addr, node->self_addr);
    pong.timestamp_ms = current_time_ms();
    pong.ttl = 1;
    snprintf(pong.payload, MSG_BUF_SIZE,
             "{ \"reply_to\": \"%s\" }", msg->msg_id);
    send_msg(node, &pong, sender);
}

void handle_pong(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {
    (void)msg;
    membership_add(&node->membership, *sender);
}

/* ---- Hybrid Push-Pull ---- */

void handle_ihave(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {
    /*
     * Parse the "ids" array from the payload and collect any IDs we
     * haven't seen yet, then send IWANT if there are any.
     */
    char want_ids[MSG_BUF_SIZE] = "";
    int  want_count = 0;

    char *p = strstr(msg->payload, "\"ids\":");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;  /* skip '[' */

    while (*p && *p != ']') {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\n') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p == '"') {
            p++;
            char id[ID_LEN] = {0};
            int i = 0;
            while (*p && *p != '"' && i < ID_LEN - 1)
                id[i++] = *p++;
            if (*p == '"') p++;

            /* Check if we already have it */
            pthread_mutex_lock(&node->lock);
            int have = 0;
            int limit = (node->seen_count < MAX_SEEN_MSGS)
                        ? node->seen_count : MAX_SEEN_MSGS;
            for (int j = 0; j < limit; j++) {
                if (strcmp(node->seen_ids[j % MAX_SEEN_MSGS], id) == 0) {
                    have = 1; break;
                }
            }
            pthread_mutex_unlock(&node->lock);

            if (!have) {
                /* Append to want list */
                if (want_count > 0)
                    strncat(want_ids, ",",
                            sizeof(want_ids) - strlen(want_ids) - 1);
                char quoted[ID_LEN + 4];
                snprintf(quoted, sizeof(quoted), "\"%s\"", id);
                strncat(want_ids, quoted,
                        sizeof(want_ids) - strlen(want_ids) - 1);
                want_count++;
            }
        } else {
            p++;
        }
    }

    if (want_count == 0) return;

    gossip_msg_t iwant;
    memset(&iwant, 0, sizeof(iwant));
    iwant.version = 1;
    snprintf(iwant.msg_id, ID_LEN, "IWANT_%llu",
             (unsigned long long)current_time_ms());
    strcpy(iwant.msg_type,    "IWANT");
    strcpy(iwant.sender_id,   node->node_id);
    strcpy(iwant.sender_addr, node->self_addr);
    iwant.timestamp_ms = current_time_ms();
    iwant.ttl = 1;
    snprintf(iwant.payload, MSG_BUF_SIZE,
             "{ \"ids\": [%s] }", want_ids);
    send_msg(node, &iwant, sender);
}

void handle_iwant(node_t *node, gossip_msg_t *msg, struct sockaddr_in *sender) {
    /*
     * Parse the requested IDs and send back the full GOSSIP messages
     * from our store.
     */
    char *p = strstr(msg->payload, "\"ids\":");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;

    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',' || *p == '\n') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p == '"') {
            p++;
            char id[ID_LEN] = {0};
            int i = 0;
            while (*p && *p != '"' && i < ID_LEN - 1)
                id[i++] = *p++;
            if (*p == '"') p++;

            pthread_mutex_lock(&node->lock);
            stored_gossip_t *sg = find_stored(node, id);
            if (sg) {
                /* Send the raw serialized gossip directly */
                sendto(node->sockfd, sg->serialized,
                       strlen(sg->serialized), 0,
                       (struct sockaddr *)sender,
                       sizeof(struct sockaddr_in));
                node->sent_messages++;
                log_event(node, "SEND", "GOSSIP", id);
            }
            pthread_mutex_unlock(&node->lock);
        } else {
            p++;
        }
    }
}

/* =========================================================
 * Ping thread
 * ========================================================= */

void membership_remove_expired(node_t *node) {
    pthread_mutex_lock(&node->membership.lock);
    uint64_t now = current_time_ms();

    for (int i = 0; i < node->membership.count; ) {
        uint64_t last = node->membership.list[i].last_seen;
        if ((now - last) > (uint64_t)(node->peer_timeout) * 1000) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &node->membership.list[i].addr.sin_addr,
                      ip, INET_ADDRSTRLEN);
            int port = ntohs(node->membership.list[i].addr.sin_port);
            printf("[Peer Removed] %s:%d timed out\n> ", ip, port);
            /* Swap with last */
            node->membership.list[i] =
                node->membership.list[node->membership.count - 1];
            node->membership.count--;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&node->membership.lock);
}

void* ping_thread_func(void *arg) {
    node_t *node = (node_t *)arg;

    while (node->running) {
        sleep((unsigned)node->ping_interval);
        if (!node->running) break;

        struct sockaddr_in targets[MAX_PEERS];
        int count = membership_get_random(&node->membership, targets,
                                          node->fanout, NULL);
        for (int i = 0; i < count; i++) {
            gossip_msg_t ping;
            memset(&ping, 0, sizeof(ping));
            ping.version = 1;
            snprintf(ping.msg_id, ID_LEN, "PING_%llu",
                     (unsigned long long)current_time_ms());
            strcpy(ping.msg_type,    "PING");
            strcpy(ping.sender_id,   node->node_id);
            strcpy(ping.sender_addr, node->self_addr);
            ping.timestamp_ms = current_time_ms();
            ping.ttl = 1;
            snprintf(ping.payload, MSG_BUF_SIZE,
                     "{ \"ping_id\": \"%s\" }", ping.msg_id);
            send_msg(node, &ping, &targets[i]);
        }

        membership_remove_expired(node);
    }
    return NULL;
}

/* =========================================================
 * Hybrid Pull thread – broadcasts IHAVE periodically
 * ========================================================= */

void* pull_thread_func(void *arg) {
    node_t *node = (node_t *)arg;

    while (node->running) {
        sleep((unsigned)node->pull_interval);
        if (!node->running) break;

        /* Collect up to max_ihave_ids recent message IDs */
        pthread_mutex_lock(&node->lock);
        int total = (node->seen_count < MAX_SEEN_MSGS)
                    ? node->seen_count : MAX_SEEN_MSGS;
        int limit = (total < node->max_ihave_ids)
                    ? total : node->max_ihave_ids;

        char ids_json[MSG_BUF_SIZE] = "";
        /* Take the most recent 'limit' IDs */
        int start = (node->seen_count >= MAX_SEEN_MSGS)
                    ? node->seen_count % MAX_SEEN_MSGS
                    : 0;
        int collected = 0;
        for (int i = 0; i < MAX_SEEN_MSGS && collected < limit; i++) {
            int idx = (start + MAX_SEEN_MSGS - i - 1) % MAX_SEEN_MSGS;
            if (node->seen_ids[idx][0] == '\0') continue;
            if (collected > 0)
                strncat(ids_json, ",",
                        sizeof(ids_json) - strlen(ids_json) - 1);
            char q[ID_LEN + 4];
            snprintf(q, sizeof(q), "\"%s\"", node->seen_ids[idx]);
            strncat(ids_json, q, sizeof(ids_json) - strlen(ids_json) - 1);
            collected++;
        }
        pthread_mutex_unlock(&node->lock);

        if (collected == 0) continue;

        gossip_msg_t ihave;
        memset(&ihave, 0, sizeof(ihave));
        ihave.version = 1;
        snprintf(ihave.msg_id, ID_LEN, "IHAVE_%llu",
                 (unsigned long long)current_time_ms());
        strcpy(ihave.msg_type,    "IHAVE");
        strcpy(ihave.sender_id,   node->node_id);
        strcpy(ihave.sender_addr, node->self_addr);
        ihave.timestamp_ms = current_time_ms();
        ihave.ttl = 1;
        snprintf(ihave.payload, MSG_BUF_SIZE,
                 "{ \"ids\": [%s], \"max_ids\": %d }",
                 ids_json, node->max_ihave_ids);

        struct sockaddr_in targets[MAX_PEERS];
        int count = membership_get_random(&node->membership, targets,
                                          node->fanout, NULL);
        for (int i = 0; i < count; i++) {
            send_msg(node, &ihave, &targets[i]);
        }
    }
    return NULL;
}

/* =========================================================
 * Logging
 * ========================================================= */

void log_event(node_t *node, const char *event,
               const char *msg_type, const char *msg_id) {
    uint64_t now = current_time_ms();
    fprintf(node->log_file, "%llu,%s,%s,%s\n",
            (unsigned long long)now, event, msg_type, msg_id);
    fflush(node->log_file);
}
