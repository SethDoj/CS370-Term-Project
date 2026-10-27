// Unit tests for src/common/log.c: line shape, level names, truncation.
#include "../src/common/log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

// Read one log line written to the pipe; returns its length.
static size_t read_line(int fd, char *buf, size_t cap)
{
    ssize_t n = read(fd, buf, cap - 1);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
    return (size_t)n;
}

// "2026-10-27T15:04:05.123Z " is 25 bytes: check the timestamp shape.
static void check_timestamp(const char *s)
{
    CHECK(s[4] == '-' && s[7] == '-' && s[10] == 'T');
    CHECK(s[13] == ':' && s[16] == ':' && s[19] == '.');
    CHECK(s[23] == 'Z' && s[24] == ' ');
}

int main(void)
{
    int p[2];
    char buf[LOG_LINE_MAX * 2];

    if (pipe(p) != 0) {
        perror("pipe");
        return 1;
    }
    log_init(p[1], "test");

    log_msg(LVL_INFO, "event=start n=%d", 5);
    size_t n = read_line(p[0], buf, sizeof buf);
    CHECK(n > 25);
    check_timestamp(buf);
    CHECK(strcmp(buf + 25, "INFO test event=start n=5\n") == 0);

    log_msg(LVL_WARN, "event=w");
    read_line(p[0], buf, sizeof buf);
    CHECK(strcmp(buf + 25, "WARN test event=w\n") == 0);

    log_msg(LVL_ERROR, "event=e");
    read_line(p[0], buf, sizeof buf);
    CHECK(strcmp(buf + 25, "ERROR test event=e\n") == 0);

    // Prefix is 25 (timestamp) + "INFO test " (10) = 35 bytes, so a 476-byte
    // message fills the line exactly (35 + 476 + '\n' = 512) and is not marked.
    char big[LOG_LINE_MAX * 2];
    const size_t mark_len = strlen(LOG_TRUNC_MARK);
    memset(big, 'x', 476);
    big[476] = '\0';
    log_msg(LVL_INFO, "%s", big);
    n = read_line(p[0], buf, sizeof buf);
    CHECK(n == LOG_LINE_MAX);
    CHECK(buf[n - 2] == 'x' && buf[n - 1] == '\n');
    CHECK(strstr(buf, LOG_TRUNC_MARK) == NULL);

    // One byte more is cut to LOG_LINE_MAX and ends in the visible marker.
    memset(big, 'x', 477);
    big[477] = '\0';
    log_msg(LVL_INFO, "%s", big);
    n = read_line(p[0], buf, sizeof buf);
    CHECK(n == LOG_LINE_MAX);
    CHECK(n > mark_len && memcmp(buf + n - 1 - mark_len, LOG_TRUNC_MARK, mark_len) == 0);
    CHECK(buf[n - 1] == '\n');

    // A far-too-long message is marked the same way.
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    log_msg(LVL_INFO, "msg=%s", big);
    n = read_line(p[0], buf, sizeof buf);
    CHECK(n == LOG_LINE_MAX);
    CHECK(memcmp(buf + n - 1 - mark_len, LOG_TRUNC_MARK, mark_len) == 0);

    close(p[0]);
    close(p[1]);
    if (failures == 0)
        printf("test_log: all checks passed\n");
    return failures != 0;
}
