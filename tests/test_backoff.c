// Unit tests for src/supervisor/backoff.c: doubling, cap, and uptime reset.
#include "../src/supervisor/backoff.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

int main(void)
{
    struct backoff bo;
    backoff_init(&bo);

    // A crash loop (uptime 0) doubles from 1 s and stops at the 60 s cap.
    const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        CHECK(backoff_next(&bo, 0) == want[i]);

    // Just under 5 minutes up is still a crash loop: stays capped.
    CHECK(backoff_next(&bo, BACKOFF_RESET_UPTIME_MS - 1) == 60000);

    // 5 minutes up resets the sequence to 1 s, then it doubles again.
    CHECK(backoff_next(&bo, BACKOFF_RESET_UPTIME_MS) == 1000);
    CHECK(backoff_next(&bo, 0) == 2000);

    // A fresh child's first restart is 1 s regardless of uptime.
    backoff_init(&bo);
    CHECK(backoff_next(&bo, 10) == 1000);

    if (failures == 0)
        printf("test_backoff: all checks passed\n");
    return failures != 0;
}
