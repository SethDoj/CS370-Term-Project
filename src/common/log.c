#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int log_fd = 2;
static const char *log_component = "?";

static const char *const level_names[] = {
    [LVL_DEBUG] = "DEBUG",
    [LVL_INFO] = "INFO",
    [LVL_WARN] = "WARN",
    [LVL_ERROR] = "ERROR",
};

void log_init(int fd, const char *component)
{
    log_fd = fd;
    log_component = component;
}

// Write all of buf, retrying on EINTR and short writes. Logging must never
// fail its caller, so errors are dropped.
static void write_all(const char *buf, size_t len)
{
    // Invariant: buf[0..len) is what remains to be written.
    while (len > 0) {
        ssize_t n = write(log_fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

void log_msg(enum log_level lvl, const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    struct timespec ts;
    struct tm tm;
    int saved_errno = errno;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || gmtime_r(&ts.tv_sec, &tm) == NULL) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        gmtime_r(&ts.tv_sec, &tm);
    }

    size_t len = strftime(line, sizeof line, "%Y-%m-%dT%H:%M:%S", &tm);
    int n = snprintf(line + len, sizeof line - len, ".%03ldZ %s %s ",
                     ts.tv_nsec / 1000000, level_names[lvl], log_component);
    len += (n > 0) ? (size_t)n : 0;

    // Leave one byte for '\n'; vsnprintf's NUL lands in that byte and is overwritten.
    if (len < sizeof line - 1) {
        va_list ap;
        va_start(ap, fmt);
        n = vsnprintf(line + len, sizeof line - len, fmt, ap);
        va_end(ap);
        if (n > 0)
            len += (size_t)n;
    }
    if (len > sizeof line - 1) {
        // Cut: overwrite the tail with a marker so the lost fields are visible.
        static const char mark[] = LOG_TRUNC_MARK;
        len = sizeof line - 1;
        memcpy(line + len - (sizeof mark - 1), mark, sizeof mark - 1);
    }
    line[len++] = '\n';

    write_all(line, len);
    errno = saved_errno;
}
