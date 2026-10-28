// Orchid Sitter supervisor (mechanism E): starts every daemon, restarts it with
// backoff when it dies, and stops them all on SIGTERM/SIGINT.
//   supervisor [-t HB_TIMEOUT_S] -c NAME=PATH [ARGS...] [-c ...]
// Each -c command is split on spaces/tabs with no quoting, so PATH and ARGS
// cannot contain spaces.
// Exit status: 0 clean shutdown, 1 configuration error, 2 setup/runtime failure.
#include "child.h"

#include "../common/log.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXIT_CONFIG 1
#define EXIT_SETUP 2
#define SHUTDOWN_GRACE_MS 5000
#define HB_TIMEOUT_DEFAULT_S 30 // enforced from commit 3 on

enum { TAG_SIGNAL = 100, TAG_TIMER = 101 }; // epoll tags; children use 0..CHILD_MAX-1

struct sup {
    struct child children[CHILD_MAX];
    int nchildren;
    int epfd, sigfd, timerfd;
    bool shutting_down;
    uint64_t kill_deadline_ms; // 0 = no SIGKILL pending
    unsigned hb_timeout_s;
};

static bool parse_args(struct sup *s, int argc, char **argv)
{
    int opt;
    char *end;

    while ((opt = getopt(argc, argv, ":t:c:")) != -1) {
        switch (opt) {
        case 't': {
            errno = 0;
            unsigned long t = strtoul(optarg, &end, 10);
            if (errno != 0 || *end != '\0' || t == 0 || t > 3600) {
                log_msg(LVL_ERROR, "event=config_error reason=bad_timeout value=%s", optarg);
                return false;
            }
            s->hb_timeout_s = (unsigned)t;
            break;
        }
        case 'c':
            if (s->nchildren == CHILD_MAX) {
                log_msg(LVL_ERROR, "event=config_error reason=too_many_children max=%d",
                        CHILD_MAX);
                return false;
            }
            if (!child_parse(&s->children[s->nchildren], optarg))
                return false;
            // Invariant: names of children[0..i) differ from the new child's name.
            for (int i = 0; i < s->nchildren; i++) {
                if (strcmp(s->children[i].name, s->children[s->nchildren].name) == 0) {
                    log_msg(LVL_ERROR, "event=config_error reason=duplicate_name name=%s",
                            s->children[i].name);
                    return false;
                }
            }
            s->nchildren++;
            break;
        default:
            log_msg(LVL_ERROR, "event=config_error reason=bad_option option=-%c", optopt);
            return false;
        }
    }
    if (optind != argc) {
        log_msg(LVL_ERROR, "event=config_error reason=unexpected_argument arg=%s", argv[optind]);
        return false;
    }
    if (s->nchildren == 0) {
        log_msg(LVL_ERROR, "event=config_error reason=no_children");
        return false;
    }
    return true;
}

// Arm the one-shot timer for the earliest pending deadline, or disarm it.
static void arm_timer(struct sup *s)
{
    uint64_t due = s->kill_deadline_ms;

    // Invariant: due is the earliest deadline among those examined (0 = none).
    for (int i = 0; i < s->nchildren; i++) {
        const struct child *c = &s->children[i];
        if (c->state == CHILD_WAITING && (due == 0 || c->next_start_ms < due))
            due = c->next_start_ms;
    }
    struct itimerspec its = {0};
    its.it_value.tv_sec = (time_t)(due / 1000u);
    its.it_value.tv_nsec = (long)(due % 1000u) * 1000000L;
    if (timerfd_settime(s->timerfd, TFD_TIMER_ABSTIME, &its, NULL) != 0)
        log_msg(LVL_ERROR, "event=timer_arm_failed errno=%d", errno);
}

static bool any_running(const struct sup *s)
{
    for (int i = 0; i < s->nchildren; i++)
        if (s->children[i].state == CHILD_RUNNING)
            return true;
    return false;
}

static void reap_children(struct sup *s)
{
    int status;

    // Invariant: every child reaped so far has been passed to child_reaped.
    for (;;) {
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid == 0)
            return;
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            if (errno != ECHILD)
                log_msg(LVL_ERROR, "event=waitpid_failed errno=%d", errno);
            return;
        }
        int i = 0;
        while (i < s->nchildren && s->children[i].pid != pid)
            i++;
        if (i == s->nchildren) {
            log_msg(LVL_WARN, "event=unknown_child_reaped pid=%d", (int)pid);
            continue;
        }
        child_reaped(&s->children[i], s->epfd, status, now_ms(), s->shutting_down);
    }
}

static void begin_shutdown(struct sup *s, int signo)
{
    if (s->shutting_down)
        return;
    s->shutting_down = true;
    s->kill_deadline_ms = now_ms() + SHUTDOWN_GRACE_MS;
    log_msg(LVL_INFO, "event=shutdown_begin signal=%d grace_ms=%d", signo, SHUTDOWN_GRACE_MS);
    for (int i = 0; i < s->nchildren; i++) {
        struct child *c = &s->children[i];
        if (c->state == CHILD_WAITING) {
            c->state = CHILD_STOPPED;
        } else if (c->state == CHILD_RUNNING && kill(c->pid, SIGTERM) != 0) {
            log_msg(LVL_ERROR, "event=kill_failed name=%s pid=%d signal=%d errno=%d", c->name,
                    (int)c->pid, SIGTERM, errno);
        }
    }
}

static void on_signalfd(struct sup *s)
{
    struct signalfd_siginfo si;

    // Invariant: every signal read so far has been acted on.
    for (;;) {
        ssize_t n = read(s->sigfd, &si, sizeof si);
        if (n != (ssize_t)sizeof si) {
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && errno != EAGAIN)
                log_msg(LVL_ERROR, "event=signalfd_read_failed errno=%d", errno);
            return;
        }
        if (si.ssi_signo == SIGCHLD)
            reap_children(s); // pending SIGCHLDs coalesce, so reap everything
        else
            begin_shutdown(s, (int)si.ssi_signo);
    }
}

static void on_timer(struct sup *s)
{
    uint64_t expirations;
    if (read(s->timerfd, &expirations, sizeof expirations) < 0 && errno != EAGAIN)
        log_msg(LVL_WARN, "event=timerfd_read_failed errno=%d", errno);

    uint64_t now = now_ms();
    if (s->kill_deadline_ms != 0 && now >= s->kill_deadline_ms) {
        s->kill_deadline_ms = 0;
        for (int i = 0; i < s->nchildren; i++) {
            struct child *c = &s->children[i];
            if (c->state != CHILD_RUNNING)
                continue;
            log_msg(LVL_WARN, "event=shutdown_kill name=%s pid=%d", c->name, (int)c->pid);
            if (kill(c->pid, SIGKILL) != 0)
                log_msg(LVL_ERROR, "event=kill_failed name=%s pid=%d signal=%d errno=%d",
                        c->name, (int)c->pid, SIGKILL, errno);
        }
    }
    for (int i = 0; !s->shutting_down && i < s->nchildren; i++) {
        struct child *c = &s->children[i];
        if (c->state == CHILD_WAITING && now >= c->next_start_ms)
            child_spawn(c, (uint32_t)i, s->epfd, now);
    }
}

int main(int argc, char **argv)
{
    static struct sup s; // static: zeroed, and too big to want on the stack
    int status = EXIT_SETUP;
    sigset_t mask;

    log_init(2, "supervisor");
    s.epfd = s.sigfd = s.timerfd = -1;
    s.hb_timeout_s = HB_TIMEOUT_DEFAULT_S;
    if (!parse_args(&s, argc, argv))
        return EXIT_CONFIG;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        log_msg(LVL_ERROR, "event=setup_failed step=sigprocmask errno=%d", errno);
        goto out;
    }
    s.sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (s.sigfd < 0) {
        log_msg(LVL_ERROR, "event=setup_failed step=signalfd errno=%d", errno);
        goto out;
    }
    s.timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (s.timerfd < 0) {
        log_msg(LVL_ERROR, "event=setup_failed step=timerfd_create errno=%d", errno);
        goto out;
    }
    s.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (s.epfd < 0) {
        log_msg(LVL_ERROR, "event=setup_failed step=epoll_create1 errno=%d", errno);
        goto out;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = TAG_SIGNAL};
    if (epoll_ctl(s.epfd, EPOLL_CTL_ADD, s.sigfd, &ev) != 0) {
        log_msg(LVL_ERROR, "event=setup_failed step=epoll_add_signalfd errno=%d", errno);
        goto out;
    }
    ev.data.u32 = TAG_TIMER;
    if (epoll_ctl(s.epfd, EPOLL_CTL_ADD, s.timerfd, &ev) != 0) {
        log_msg(LVL_ERROR, "event=setup_failed step=epoll_add_timerfd errno=%d", errno);
        goto out;
    }

    log_msg(LVL_INFO, "event=supervisor_start pid=%d children=%d hb_timeout_s=%u",
            (int)getpid(), s.nchildren, s.hb_timeout_s);
    uint64_t now = now_ms();
    for (int i = 0; i < s.nchildren; i++)
        child_spawn(&s.children[i], (uint32_t)i, s.epfd, now);
    arm_timer(&s);

    // Invariant: every child is RUNNING, WAITING with the timer armed for it,
    // or STOPPED; the loop ends once shutdown has reaped every child.
    while (!(s.shutting_down && !any_running(&s))) {
        struct epoll_event evs[CHILD_MAX + 2];
        int n = epoll_wait(s.epfd, evs, CHILD_MAX + 2, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            // Children get SIGTERM from PR_SET_PDEATHSIG when we exit.
            log_msg(LVL_ERROR, "event=epoll_wait_failed errno=%d", errno);
            goto out;
        }
        for (int i = 0; i < n; i++) {
            uint32_t tag = evs[i].data.u32;
            if (tag == TAG_SIGNAL)
                on_signalfd(&s);
            else if (tag == TAG_TIMER)
                on_timer(&s);
            else if (tag < (uint32_t)s.nchildren)
                child_drain_heartbeat(&s.children[tag], s.epfd);
        }
        arm_timer(&s);
    }
    log_msg(LVL_INFO, "event=shutdown_complete");
    status = 0;

out:
    for (int i = 0; i < s.nchildren; i++)
        if (s.children[i].hb_fd >= 0)
            close(s.children[i].hb_fd);
    if (s.epfd >= 0)
        close(s.epfd);
    if (s.timerfd >= 0)
        close(s.timerfd);
    if (s.sigfd >= 0)
        close(s.sigfd);
    return status;
}
