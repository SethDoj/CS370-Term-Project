#include "child.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEARTBEAT_FD 3

static int heartbeat_on;

int child_init(void)
{
    struct stat st;

    if (fstat(HEARTBEAT_FD, &st) == 0 && S_ISFIFO(st.st_mode)) {
        heartbeat_on = 1;
        // A full pipe means a stuck supervisor; drop beats rather than block.
        int fl = fcntl(HEARTBEAT_FD, F_GETFL);
        if (fl < 0 || fcntl(HEARTBEAT_FD, F_SETFL, fl | O_NONBLOCK) < 0)
            log_msg(LVL_WARN, "event=heartbeat_nonblock_failed err=%s", strerror(errno));
        // Keep the heartbeat pipe out of anything we exec.
        if (fcntl(HEARTBEAT_FD, F_SETFD, FD_CLOEXEC) < 0)
            log_msg(LVL_WARN, "event=heartbeat_cloexec_failed err=%s", strerror(errno));
    }

    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
        log_msg(LVL_ERROR, "event=pdeathsig_failed err=%s", strerror(errno));
        return -1;
    }
    // The parent may have died before prctl took effect.
    if (getppid() == 1)
        log_msg(LVL_WARN, "event=orphaned_at_start");
    return 0;
}

void child_heartbeat(void)
{
    static const char beat = 'h';
    ssize_t n;

    if (!heartbeat_on)
        return;
    do
        n = write(HEARTBEAT_FD, &beat, 1);
    while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN) {
        log_msg(LVL_WARN, "event=heartbeat_lost err=%s", strerror(errno));
        heartbeat_on = 0;
    }
}
