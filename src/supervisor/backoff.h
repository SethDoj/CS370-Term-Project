// Restart delay for a crashed child: 1 s doubling, capped at 60 s, and reset
// to 1 s once the child had stayed up 5 minutes (CLAUDE.md child contract).
#ifndef ORCHID_BACKOFF_H
#define ORCHID_BACKOFF_H

#include <stdint.h>

#define BACKOFF_INITIAL_MS 1000u
#define BACKOFF_MAX_MS 60000u
#define BACKOFF_RESET_UPTIME_MS 300000u

struct backoff {
    uint32_t next_ms; // delay the next restart will use
};

void backoff_init(struct backoff *bo);

// Delay before restarting a child that died after uptime_ms; advances bo.
uint32_t backoff_next(struct backoff *bo, uint64_t uptime_ms);

#endif
