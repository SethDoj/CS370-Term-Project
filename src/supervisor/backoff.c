#include "backoff.h"

void backoff_init(struct backoff *bo)
{
    bo->next_ms = BACKOFF_INITIAL_MS;
}

uint32_t backoff_next(struct backoff *bo, uint64_t uptime_ms)
{
    // A child that ran long enough was healthy; this crash starts a new sequence.
    if (uptime_ms >= BACKOFF_RESET_UPTIME_MS)
        bo->next_ms = BACKOFF_INITIAL_MS;

    uint32_t delay = bo->next_ms;
    bo->next_ms = (delay >= BACKOFF_MAX_MS / 2) ? BACKOFF_MAX_MS : delay * 2;
    return delay;
}
