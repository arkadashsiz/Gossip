#ifndef MEMBER_H
#define MEMBER_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>

#define MAX_PEERS 64

typedef struct {
    struct sockaddr_in addr;
    uint64_t last_seen;
} peer_info_t;

typedef struct {
    peer_info_t list[MAX_PEERS];
    int count;
    int limit;
    pthread_mutex_t lock;
} membership_t;

void membership_init(membership_t *m, int limit);
int membership_add(membership_t *m, struct sockaddr_in addr);
int membership_get_random(membership_t *m, struct sockaddr_in *targets, int count,
                          struct sockaddr_in *exclude);

#endif
