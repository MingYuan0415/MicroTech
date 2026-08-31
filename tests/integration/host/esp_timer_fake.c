#include "esp_timer.h"

#include <time.h>

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_nsec / 1000LL;
}
