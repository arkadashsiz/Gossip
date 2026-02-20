#include "member.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

void membership_init(membership_t *m, int limit) {
    m->count = 0;
    m->limit = (limit > MAX_PEERS) ? MAX_PEERS : limit;
    pthread_mutex_init(&m->lock, NULL);
}

int membership_add(membership_t *m, struct sockaddr_in addr) {

    pthread_mutex_lock(&m->lock);

    for (int i = 0; i < m->count; i++) {

        if (m->list[i].addr.sin_port == addr.sin_port &&
            m->list[i].addr.sin_addr.s_addr == addr.sin_addr.s_addr) {

            m->list[i].last_seen = current_time_ms();
            pthread_mutex_unlock(&m->lock);
            return 0;
        }
    }

    if (m->count < m->limit) {

        m->list[m->count].addr = addr;
        m->list[m->count].last_seen = current_time_ms();
        m->count++;

        pthread_mutex_unlock(&m->lock);
        return 1;
    }

    pthread_mutex_unlock(&m->lock);
    return -1;
}


int membership_get_random(membership_t *m, struct sockaddr_in *targets, int count, struct sockaddr_in *exclude) {
    pthread_mutex_lock(&m->lock);
    if (m->count == 0) {
        pthread_mutex_unlock(&m->lock);
        return 0;
    }

    int found = 0;
    // Simple shuffle-based selection
    int indices[MAX_PEERS];
    for(int i=0; i<m->count; i++) indices[i] = i;
    for(int i=m->count-1; i>0; i--) {
        int j = rand() % (i+1);
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }

    for (int i = 0; i < m->count && found < count; i++) {
        struct sockaddr_in *candidate = &m->list[indices[i]].addr;
        if (exclude && candidate->sin_port == exclude->sin_port && 
            candidate->sin_addr.s_addr == exclude->sin_addr.s_addr) continue;
        
        targets[found++] = *candidate;
    }
    pthread_mutex_unlock(&m->lock);
    return found;
}