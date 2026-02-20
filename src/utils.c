#include "utils.h"
#include <sys/time.h>
#include <stdio.h>
uint64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}
