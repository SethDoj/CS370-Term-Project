#include "sampler.h"

#include "child.h"
#include "log.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// Contract: a heartbeat at least every 10 s. Beat at half that for margin.
#define HEARTBEAT_S 5

struct loop_state {
    int hub_fd; // -1 while disconnected
    int hub_down_logged; // hub_lost already logged for this outage
    int dev_open;
    int degraded;
    struct timespec degraded_since; // CLOCK_MONOTONIC
    uint32_t seq;
};

static int64_t now_ns(clockid_t clk)
{
    struct timespec ts;

    if (clock_gettime(clk, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int make_timer(int period_s, long first_ns)
{
    struct itimerspec its = {
        .it_interval = { .tv_sec = period_s },
        .it_value = { .tv_sec = first_ns / 1000000000L, .tv_nsec = first_ns % 1000000000L },
    };
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

    if (fd < 0) {
        log_msg(LVL_ERROR, "event=timerfd_failed err=%s", strerror(errno));
        return -1;
    }
    if (timerfd_settime(fd, 0, &its, NULL) != 0) {
        log_msg(LVL_ERROR, "event=timerfd_settime_failed err=%s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// Consume a timerfd expiration so poll stops reporting it.
static void drain_timer(int fd)
{
    uint64_t n;

    if (read(fd, &n, sizeof n) < 0 && errno != EAGAIN)
        log_msg(LVL_WARN, "event=timer_read_failed err=%s", strerror(errno));
}

static void hub_disconnect(struct loop_state *st, const char *why, int err)
{
    if (st->hub_fd >= 0)
        close(st->hub_fd);
    st->hub_fd = -1;
    if (!st->hub_down_logged) {
        log_msg(LVL_WARN, "event=hub_lost why=%s err=%s", why, strerror(err));
        st->hub_down_logged = 1;
    }
}

static void hub_connect(struct loop_state *st, const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd;

    // Length was checked in sampler_run.
    strcpy(addr.sun_path, path);
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        hub_disconnect(st, "socket", errno);
        return;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        int err = errno;
        close(fd);
        hub_disconnect(st, "connect", err);
        return;
    }
    st->hub_fd = fd;
    st->hub_down_logged = 0;
    log_msg(LVL_INFO, "event=hub_connected sock=%s", path);
}

// Track OK <-> failing transitions so the log shows detect -> degrade -> recover.
static void note_status(struct loop_state *st, enum record_status status)
{
    int failing = (status == REC_IO_ERR || status == REC_CRC_ERR);
    struct timespec now = { 0 };

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        log_msg(LVL_WARN, "event=clock_failed err=%s", strerror(errno));
    if (failing && !st->degraded) {
        st->degraded = 1;
        st->degraded_since = now;
        log_msg(LVL_WARN, "event=sensor_degraded status=%s", record_status_name(status));
    } else if (!failing && st->degraded) {
        st->degraded = 0;
        log_msg(LVL_INFO, "event=sensor_recovered down_s=%lld",
                (long long)(now.tv_sec - st->degraded_since.tv_sec));
    }
    if (status == REC_PARTIAL)
        log_msg(LVL_WARN, "event=sensor_partial");
}

static void take_sample(const struct sampler *s, struct loop_state *st)
{
    struct record r = { 0 };
    enum record_status status = REC_IO_ERR;

    if (!st->dev_open && s->open(s->ctx) == 0)
        st->dev_open = 1;
    if (st->dev_open) {
        status = s->read(s->ctx, &r);
        if (status == REC_IO_ERR) {
            s->close(s->ctx);
            st->dev_open = 0;
        }
    }
    note_status(st, status);

    r.sensor_id = (uint8_t)s->sensor;
    r.status = (uint8_t)status;
    r.src_seq = ++st->seq;
    r.sample_time_ns = now_ns(CLOCK_REALTIME);
    record_seal(&r);

    if (st->hub_fd < 0)
        hub_connect(st, s->sock_path);
    if (st->hub_fd < 0)
        return; // record dropped; the gap in src_seq shows it
    if (send(st->hub_fd, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r)
        hub_disconnect(st, "send", errno);
}

int sampler_run(const struct sampler *s)
{
    struct loop_state st = { .hub_fd = -1 };
    int ret = EXIT_RESTART;
    int sig_fd = -1, beat_fd = -1, sample_fd = -1;
    sigset_t mask;

    if (strlen(s->sock_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        log_msg(LVL_ERROR, "event=config_error why=sock_path_too_long");
        return EXIT_CONFIG;
    }

    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        log_msg(LVL_ERROR, "event=sigprocmask_failed err=%s", strerror(errno));
        goto out;
    }
    sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sig_fd < 0) {
        log_msg(LVL_ERROR, "event=signalfd_failed err=%s", strerror(errno));
        goto out;
    }
    // First sample right away (1 ns: a zero it_value would disarm the timer).
    beat_fd = make_timer(HEARTBEAT_S, HEARTBEAT_S * 1000000000L);
    sample_fd = make_timer(s->interval_s, 1);
    if (beat_fd < 0 || sample_fd < 0)
        goto out;

    child_heartbeat();
    log_msg(LVL_INFO, "event=start interval_s=%d sock=%s", s->interval_s, s->sock_path);

    for (;;) {
        struct pollfd pfd[3] = {
            { .fd = sig_fd, .events = POLLIN },
            { .fd = beat_fd, .events = POLLIN },
            { .fd = sample_fd, .events = POLLIN },
        };

        if (poll(pfd, 3, -1) < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LVL_ERROR, "event=poll_failed err=%s", strerror(errno));
            goto out;
        }
        if (pfd[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sig_fd, &si, sizeof si) == (ssize_t)sizeof si)
                log_msg(LVL_INFO, "event=stop signal=%u", si.ssi_signo);
            ret = EXIT_CLEAN;
            goto out;
        }
        if (pfd[1].revents & POLLIN) {
            drain_timer(beat_fd);
            child_heartbeat();
        }
        if (pfd[2].revents & POLLIN) {
            drain_timer(sample_fd);
            take_sample(s, &st);
        }
    }

out:
    if (st.dev_open)
        s->close(s->ctx);
    if (st.hub_fd >= 0)
        close(st.hub_fd);
    if (sample_fd >= 0)
        close(sample_fd);
    if (beat_fd >= 0)
        close(beat_fd);
    if (sig_fd >= 0)
        close(sig_fd);
    return ret;
}
