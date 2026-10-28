// Stub child for supervisor tests, not part of the product. Writes a 1-byte
// heartbeat to fd 3 every second, reports its open fds at start (to prove the
// supervisor's fd hygiene), and exits on cue.
//   fake_child [--tag T] [--exit-after SECONDS] [--exit-code CODE]
#include "../common/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HB_FD 3
#define FD_SCAN_MAX 64

// Parse a non-negative int; false on junk or overflow.
static bool parse_int(const char *s, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 1000000)
        return false;
    *out = v;
    return true;
}

// Write the open fds as "0,1,2,3" (ascending; "+more" if any fd >= FD_SCAN_MAX).
static void list_fds(char *buf, size_t cap)
{
    bool open_fd[FD_SCAN_MAX] = {false};
    bool more = false;
    size_t len = 0;

    buf[0] = '\0';
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) {
        snprintf(buf, cap, "unknown");
        return;
    }
    struct dirent *e;
    // Invariant: open_fd marks every fd seen so far, except the one opendir used.
    while ((e = readdir(d)) != NULL) {
        long fd;
        if (!parse_int(e->d_name, &fd) || fd == dirfd(d))
            continue;
        if (fd < FD_SCAN_MAX)
            open_fd[fd] = true;
        else
            more = true;
    }
    closedir(d);

    for (int fd = 0; fd < FD_SCAN_MAX && len < cap; fd++) {
        if (open_fd[fd]) {
            int n = snprintf(buf + len, cap - len, "%s%d", len ? "," : "", fd);
            len += (n > 0) ? (size_t)n : 0;
        }
    }
    if (more && len < cap)
        snprintf(buf + len, cap - len, "+more");
}

int main(int argc, char **argv)
{
    const char *tag = "-";
    long exit_after = -1; // seconds; -1 = run until killed
    long exit_code = 0;

    log_init(2, "fake_child");
    for (int i = 1; i < argc; i++) {
        bool has_val = i + 1 < argc;
        if (strcmp(argv[i], "--tag") == 0 && has_val) {
            tag = argv[++i];
        } else if (strcmp(argv[i], "--exit-after") == 0 && has_val &&
                   parse_int(argv[i + 1], &exit_after)) {
            i++;
        } else if (strcmp(argv[i], "--exit-code") == 0 && has_val &&
                   parse_int(argv[i + 1], &exit_code) && exit_code <= 255) {
            i++;
        } else {
            log_msg(LVL_ERROR, "event=config_error reason=bad_option arg=%s", argv[i]);
            return 1;
        }
    }
    if (fcntl(HB_FD, F_GETFD) == -1) {
        log_msg(LVL_ERROR, "event=config_error reason=no_heartbeat_fd fd=%d", HB_FD);
        return 1;
    }

    char fds[128];
    list_fds(fds, sizeof fds);
    log_msg(LVL_INFO, "event=start pid=%d tag=%s fds=%s", (int)getpid(), tag, fds);

    // Invariant: t seconds have elapsed and t heartbeats have been attempted.
    for (long t = 0;; t++) {
        if (exit_after >= 0 && t >= exit_after) {
            log_msg(LVL_INFO, "event=exit code=%ld", exit_code);
            return (int)exit_code;
        }
        // EAGAIN (pipe full) just drops this heartbeat; the supervisor is behind.
        if (write(HB_FD, "h", 1) < 0 && errno != EAGAIN)
            log_msg(LVL_WARN, "event=heartbeat_failed errno=%d", errno);
        sleep(1);
    }
}
