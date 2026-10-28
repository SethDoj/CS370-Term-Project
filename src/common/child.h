// Child-contract helpers shared by every supervised daemon (see CLAUDE.md).
#ifndef ORCHID_CHILD_H
#define ORCHID_CHILD_H

// Exit codes the supervisor acts on.
#define EXIT_CLEAN 0
#define EXIT_CONFIG 1 // fatal configuration error: not restarted
#define EXIT_RESTART 2 // anything else unexpected: restart with backoff

// Die with the parent (PR_SET_PDEATHSIG) and enable heartbeats if fd 3 is a
// pipe. Run standalone (fd 3 not a pipe), heartbeats are silently skipped.
// Returns 0, or -1 if the death signal could not be set (logged).
int child_init(void);

// Write one heartbeat byte to fd 3 if heartbeats are enabled. Never blocks
// the caller for long and never fails it; a lost supervisor is logged once.
void child_heartbeat(void);

#endif
