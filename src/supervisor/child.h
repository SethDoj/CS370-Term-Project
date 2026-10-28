// One supervised child: its command, process state and restart policy.
#ifndef ORCHID_CHILD_H
#define ORCHID_CHILD_H

#include "backoff.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define CHILD_MAX 8
#define CHILD_ARGV_MAX 8 // including the terminating NULL
#define CHILD_CMD_MAX 256
#define CHILD_NAME_MAX 32
#define HB_FD 3 // heartbeat pipe in every child (CLAUDE.md child contract)

enum child_state {
    CHILD_STOPPED, // not running and not coming back
    CHILD_RUNNING,
    CHILD_WAITING, // dead, restart due at next_start_ms
};

struct child {
    char name[CHILD_NAME_MAX];
    char cmd[CHILD_CMD_MAX];     // argv strings point into this buffer
    char *argv[CHILD_ARGV_MAX];
    enum child_state state;
    pid_t pid;                   // -1 unless RUNNING
    int hb_fd;                   // heartbeat read end, -1 if none
    uint64_t started_ms;         // CLOCK_MONOTONIC
    uint64_t next_start_ms;      // valid while WAITING
    uint64_t heartbeats;
    unsigned attempt;
    struct backoff bo;
};

// Milliseconds on CLOCK_MONOTONIC.
uint64_t now_ms(void);

// Parse "NAME=PATH [ARGS...]" into c. Logs and returns false on a bad spec.
bool child_parse(struct child *c, const char *spec);

// Fork and exec c with its heartbeat pipe on fd 3; the read end is watched in
// epfd under tag idx. If spawning fails, c is scheduled for a retry instead.
void child_spawn(struct child *c, uint32_t idx, int epfd, uint64_t now);

// Record c's exit (a waitpid status) and decide whether it comes back.
void child_reaped(struct child *c, int epfd, int status, uint64_t now, bool shutting_down);

// Read pending heartbeats from c's pipe; closes the pipe on EOF.
void child_drain_heartbeat(struct child *c, int epfd);

#endif
