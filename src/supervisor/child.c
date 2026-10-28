#include "child.h"

#include "../common/log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // cannot fail for CLOCK_MONOTONIC
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

bool child_parse(struct child *c, const char *spec)
{
    memset(c, 0, sizeof *c);
    c->state = CHILD_STOPPED;
    c->pid = -1;
    c->hb_fd = -1;
    backoff_init(&c->bo);

    const char *eq = strchr(spec, '=');
    size_t name_len = eq ? (size_t)(eq - spec) : 0;
    if (name_len == 0 || name_len >= sizeof c->name) {
        log_msg(LVL_ERROR, "event=config_error reason=bad_child_spec spec=\"%s\"", spec);
        return false;
    }
    memcpy(c->name, spec, name_len);
    c->name[name_len] = '\0';

    if (strlen(eq + 1) >= sizeof c->cmd) {
        log_msg(LVL_ERROR, "event=config_error reason=command_too_long name=%s", c->name);
        return false;
    }
    strcpy(c->cmd, eq + 1);

    int argc = 0;
    char *save = NULL;
    // Invariant: argv[0..argc) are the words of cmd split so far.
    for (char *w = strtok_r(c->cmd, " \t", &save); w != NULL; w = strtok_r(NULL, " \t", &save)) {
        if (argc == CHILD_ARGV_MAX - 1) {
            log_msg(LVL_ERROR, "event=config_error reason=too_many_args name=%s max=%d",
                    c->name, CHILD_ARGV_MAX - 1);
            return false;
        }
        c->argv[argc++] = w;
    }
    c->argv[argc] = NULL;
    if (argc == 0) {
        log_msg(LVL_ERROR, "event=config_error reason=no_command name=%s", c->name);
        return false;
    }
    if (access(c->argv[0], X_OK) != 0) {
        log_msg(LVL_ERROR, "event=config_error reason=not_executable name=%s path=%s errno=%d",
                c->name, c->argv[0], errno);
        return false;
    }
    return true;
}

// Runs in the forked child: set up the contract's environment and exec.
__attribute__((noreturn)) static void exec_child(const struct child *c, int hb_wr, pid_t parent)
{
    const char *step;
    sigset_t none;

    step = "prctl";
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0)
        goto fail;
    // The supervisor may have died before prctl took effect; nobody would signal us.
    if (getppid() != parent)
        _exit(0);

    // The supervisor blocks signals for its signalfd; a blocked mask survives exec.
    step = "sigprocmask";
    sigemptyset(&none);
    if (sigprocmask(SIG_SETMASK, &none, NULL) != 0)
        goto fail;

    // dup2 onto itself is a no-op that would leave O_CLOEXEC set, so clear it instead.
    step = "heartbeat_fd";
    if (hb_wr == HB_FD) {
        if (fcntl(HB_FD, F_SETFD, 0) != 0)
            goto fail;
    } else if (dup2(hb_wr, HB_FD) < 0) {
        goto fail;
    }

    step = "close_range";
    if (close_range(HB_FD + 1, ~0U, 0) != 0)
        goto fail;

    step = "execv";
    execv(c->argv[0], c->argv);
fail:
    log_msg(LVL_ERROR, "event=exec_failed name=%s step=%s errno=%d", c->name, step, errno);
    _exit(127);
}

static void schedule_restart(struct child *c, uint64_t now, uint64_t uptime)
{
    uint32_t delay = backoff_next(&c->bo, uptime);
    c->state = CHILD_WAITING;
    c->next_start_ms = now + delay;
    log_msg(LVL_INFO, "event=child_restart_scheduled name=%s delay_ms=%u", c->name,
            (unsigned)delay);
}

static void close_heartbeat(struct child *c, int epfd)
{
    if (c->hb_fd < 0)
        return;
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, c->hb_fd, NULL) != 0)
        log_msg(LVL_WARN, "event=epoll_del_failed name=%s errno=%d", c->name, errno);
    close(c->hb_fd);
    c->hb_fd = -1;
}

void child_spawn(struct child *c, uint32_t idx, int epfd, uint64_t now)
{
    int p[2];
    pid_t parent = getpid();

    c->attempt++;
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) {
        log_msg(LVL_ERROR, "event=spawn_failed name=%s step=pipe2 errno=%d", c->name, errno);
        schedule_restart(c, now, 0);
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        log_msg(LVL_ERROR, "event=spawn_failed name=%s step=fork errno=%d", c->name, errno);
        close(p[0]);
        close(p[1]);
        schedule_restart(c, now, 0);
        return;
    }
    if (pid == 0)
        exec_child(c, p[1], parent);

    close(p[1]);
    c->hb_fd = p[0];
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = idx};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->hb_fd, &ev) != 0) {
        // The child still runs; its heartbeats just go unread.
        log_msg(LVL_ERROR, "event=heartbeat_unwatched name=%s errno=%d", c->name, errno);
        close(c->hb_fd);
        c->hb_fd = -1;
    }
    c->pid = pid;
    c->state = CHILD_RUNNING;
    c->started_ms = now;
    log_msg(LVL_INFO, "event=child_start name=%s pid=%d attempt=%u", c->name, (int)pid,
            c->attempt);
}

void child_reaped(struct child *c, int epfd, int status, uint64_t now, bool shutting_down)
{
    unsigned long long uptime = now - c->started_ms;

    close_heartbeat(c, epfd);
    if (WIFEXITED(status))
        log_msg(LVL_INFO, "event=child_exit name=%s pid=%d code=%d uptime_ms=%llu", c->name,
                (int)c->pid, WEXITSTATUS(status), uptime);
    else
        log_msg(LVL_WARN, "event=child_exit name=%s pid=%d signal=%d uptime_ms=%llu", c->name,
                (int)c->pid, WTERMSIG(status), uptime);
    c->pid = -1;

    if (shutting_down) {
        c->state = CHILD_STOPPED;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
        c->state = CHILD_STOPPED;
        log_msg(LVL_ERROR, "event=child_config_error name=%s action=not_restarted", c->name);
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        c->state = CHILD_STOPPED;
        log_msg(LVL_INFO, "event=child_stopped name=%s action=not_restarted", c->name);
    } else {
        schedule_restart(c, now, uptime);
    }
}

void child_drain_heartbeat(struct child *c, int epfd)
{
    char buf[64];

    // Invariant: every byte read so far has been counted as one heartbeat.
    while (c->hb_fd >= 0) {
        ssize_t n = read(c->hb_fd, buf, sizeof buf);
        if (n > 0) {
            c->heartbeats += (uint64_t)n;
        } else if (n == 0) {
            close_heartbeat(c, epfd); // child closed it or died; SIGCHLD reports which
        } else if (errno == EINTR) {
            continue;
        } else {
            if (errno != EAGAIN)
                log_msg(LVL_WARN, "event=heartbeat_read_failed name=%s errno=%d", c->name, errno);
            return;
        }
    }
}
