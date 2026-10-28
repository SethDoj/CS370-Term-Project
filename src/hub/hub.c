// hub: accepts sampler connections on a SOCK_SEQPACKET Unix socket and appends
// every record to its sensor's CSV file. Single-threaded epoll loop.
#include "../common/child.h"
#include "../common/log.h"
#include "../common/record.h"
#include "../common/sampler.h"
#include "hub_core.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PEERS 8
#define HEARTBEAT_S 5

struct hub_loop {
    int ep, listen_fd, sig_fd, beat_fd;
    int peers[MAX_PEERS]; // -1 = free slot
};

static int add_fd(int ep, int fd)
{
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };

    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
        log_msg(LVL_ERROR, "event=epoll_add_failed fd=%d err=%s", fd, strerror(errno));
        return -1;
    }
    return 0;
}

// Bind and listen on path, removing a stale socket left by a crashed hub.
static int listen_on(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct stat st;
    int fd;

    if (strlen(path) >= sizeof addr.sun_path) {
        log_msg(LVL_ERROR, "event=config_error why=sock_path_too_long");
        return -1;
    }
    strcpy(addr.sun_path, path);
    // Only ever unlink a socket, never a file someone mistyped as -s.
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            log_msg(LVL_ERROR, "event=config_error why=sock_path_not_socket path=%s", path);
            return -1;
        }
        if (unlink(path) != 0) {
            log_msg(LVL_ERROR, "event=unlink_failed path=%s err=%s", path, strerror(errno));
            return -1;
        }
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        log_msg(LVL_ERROR, "event=socket_failed err=%s", strerror(errno));
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, MAX_PEERS) != 0) {
        log_msg(LVL_ERROR, "event=listen_failed path=%s err=%s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void accept_peer(struct hub_loop *L)
{
    int fd = accept4(L->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

    if (fd < 0) {
        if (errno != EAGAIN && errno != EINTR)
            log_msg(LVL_WARN, "event=accept_failed err=%s", strerror(errno));
        return;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (L->peers[i] < 0) {
            if (add_fd(L->ep, fd) != 0)
                break;
            L->peers[i] = fd;
            log_msg(LVL_INFO, "event=peer_connected fd=%d", fd);
            return;
        }
    }
    log_msg(LVL_WARN, "event=peer_refused fd=%d why=no_slot", fd);
    close(fd);
}

static void drop_peer(struct hub_loop *L, int fd, const char *why)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (L->peers[i] == fd)
            L->peers[i] = -1;
    if (epoll_ctl(L->ep, EPOLL_CTL_DEL, fd, NULL) != 0)
        log_msg(LVL_WARN, "event=epoll_del_failed fd=%d err=%s", fd, strerror(errno));
    close(fd);
    log_msg(LVL_WARN, "event=peer_lost fd=%d why=%s", fd, why);
}

// Read every queued message from a peer; drop the peer on EOF or error.
static void serve_peer(struct hub_loop *L, struct hub *h, int fd, uint32_t events)
{
    // One byte bigger than a record so an oversized message shows as bad_len.
    unsigned char buf[sizeof(struct record) + 1];

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            hub_handle_msg(h, buf, (size_t)n);
            continue;
        }
        if (n == 0) {
            drop_peer(L, fd, "eof");
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN)
            break;
        log_msg(LVL_WARN, "event=recv_failed fd=%d err=%s", fd, strerror(errno));
        drop_peer(L, fd, "recv_error");
        return;
    }
    if (events & (EPOLLHUP | EPOLLERR))
        drop_peer(L, fd, "hup");
}

static void usage(void)
{
    fprintf(stderr, "usage: hub [-s %s] [-d data_dir]\n", HUB_SOCK_DEFAULT);
}

int main(int argc, char **argv)
{
    const char *sock_path = HUB_SOCK_DEFAULT;
    const char *dir = ".";
    struct hub h = { .fd = { -1, -1, -1 } };
    struct hub_loop L = { .ep = -1, .listen_fd = -1, .sig_fd = -1, .beat_fd = -1 };
    int ret = EXIT_RESTART;
    int opt;
    sigset_t mask;

    for (int i = 0; i < MAX_PEERS; i++)
        L.peers[i] = -1;

    log_init(2, "hub");
    while ((opt = getopt(argc, argv, "s:d:")) != -1) {
        switch (opt) {
        case 's':
            sock_path = optarg;
            break;
        case 'd':
            dir = optarg;
            break;
        default:
            goto bad_arg;
        }
    }
    if (optind != argc)
        goto bad_arg;

    if (child_init() != 0)
        return EXIT_RESTART;

    // Storage or socket we cannot set up is a configuration problem.
    ret = EXIT_CONFIG;
    if (hub_open(&h, dir) != 0)
        goto out;
    L.listen_fd = listen_on(sock_path);
    if (L.listen_fd < 0)
        goto out;

    ret = EXIT_RESTART;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        log_msg(LVL_ERROR, "event=sigprocmask_failed err=%s", strerror(errno));
        goto out;
    }
    L.sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (L.sig_fd < 0) {
        log_msg(LVL_ERROR, "event=signalfd_failed err=%s", strerror(errno));
        goto out;
    }
    L.beat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    struct itimerspec its = { .it_interval = { HEARTBEAT_S, 0 }, .it_value = { HEARTBEAT_S, 0 } };
    if (L.beat_fd < 0 || timerfd_settime(L.beat_fd, 0, &its, NULL) != 0) {
        log_msg(LVL_ERROR, "event=timerfd_failed err=%s", strerror(errno));
        goto out;
    }
    L.ep = epoll_create1(EPOLL_CLOEXEC);
    if (L.ep < 0) {
        log_msg(LVL_ERROR, "event=epoll_create_failed err=%s", strerror(errno));
        goto out;
    }
    if (add_fd(L.ep, L.listen_fd) != 0 || add_fd(L.ep, L.sig_fd) != 0 ||
        add_fd(L.ep, L.beat_fd) != 0)
        goto out;

    child_heartbeat();
    log_msg(LVL_INFO, "event=start sock=%s dir=%s", sock_path, dir);

    for (;;) {
        struct epoll_event evs[MAX_PEERS + 3];
        int n = epoll_wait(L.ep, evs, MAX_PEERS + 3, -1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LVL_ERROR, "event=epoll_wait_failed err=%s", strerror(errno));
            goto out;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;

            if (fd == L.sig_fd) {
                struct signalfd_siginfo si;
                if (read(L.sig_fd, &si, sizeof si) == (ssize_t)sizeof si)
                    log_msg(LVL_INFO, "event=stop signal=%u", si.ssi_signo);
                ret = EXIT_CLEAN;
                goto out;
            } else if (fd == L.beat_fd) {
                uint64_t ticks;
                if (read(L.beat_fd, &ticks, sizeof ticks) < 0 && errno != EAGAIN)
                    log_msg(LVL_WARN, "event=timer_read_failed err=%s", strerror(errno));
                child_heartbeat();
            } else if (fd == L.listen_fd) {
                accept_peer(&L);
            } else {
                serve_peer(&L, &h, fd, evs[i].events);
            }
        }
    }

out:
    for (int i = 0; i < MAX_PEERS; i++)
        if (L.peers[i] >= 0)
            close(L.peers[i]);
    if (L.ep >= 0)
        close(L.ep);
    if (L.beat_fd >= 0)
        close(L.beat_fd);
    if (L.sig_fd >= 0)
        close(L.sig_fd);
    if (L.listen_fd >= 0) {
        close(L.listen_fd);
        if (unlink(sock_path) != 0)
            log_msg(LVL_WARN, "event=unlink_failed path=%s err=%s", sock_path, strerror(errno));
    }
    hub_close(&h);
    return ret;

bad_arg:
    log_msg(LVL_ERROR, "event=config_error why=bad_arguments");
    usage();
    return EXIT_CONFIG;
}
