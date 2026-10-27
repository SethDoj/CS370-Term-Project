# <Orchid Sitter>: team rules
## project
A Raspberry Pi 4 device that tells an orchid owner when to water. Inputs: soil moisture (capacitive probe via ADC), temperature/humidity (SHT40), light (VEML7700). Menu mechanisms: E (multi-process supervision) and D (crash-consistent storage). See PROBLEM.md, DESIGN.md. Current target (M3): both sensors -> real pipeline -> real storage, one mechanism live. Build order: supervisor (with stub children) -> samplers -> hub/storage -> analysis
## Commands
- Build: ‘make‘ Cross/deploy: ‘make deploy PI=pi@<host>‘
- Tests: ‘make test‘ Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
- Soak sanity: ‘make soakcheck‘ (1-hour miniature of the 48h run)
- A change is DONE only when build, test, and asan pass. Show output.
## Hard constraints
- The PRODUCT makes no network calls except serving its own LAN
interface. No LLM APIs, no cloud inference, no pretrained models.
The intelligence in analysis/ is ours. This is a graded boundary.
- Every daemon must be supervisable: clean exit codes, no orphaned
fds across restart, heartbeat within 60s of start.
- Every allocation checked; every syscall’s error path handled and
logged. The 48-hour soak is the test suite of last resort.
- NEVER weaken, skip, or delete a test to make the suite pass.
## Child contract (supervisor <-> every daemon)
- Heartbeat pipe is fd 3. Write one heartbeat at least every 10 s, first one within 10 s of start. Supervisor SIGKILLs a child silent for more than 30 s (overridable with -t in tests).
- Exit 0 = clean stop. Exit 1 = fatal CONFIG error (logged, NOT restarted). Any other exit or signal = restart with backoff (1 s doubling, cap 60 s, reset after 5 min up).
- Sensor I/O trouble (NACK, timeout, unplugged device) is NEVER exit 1. Mark the sensor degraded, keep heartbeating, retry, and log detect -> degrade -> recover. The 48h soak requires an injected unplug or kill -9 to be visible in the log.
- No fds leaked across restart. Children die if the supervisor dies (PR_SET_PDEATHSIG).
## Ownership
- TBD
## Style
- Systems core: C23, -Wall -Wextra -Werror, no VLAs.
goto-cleanup for multi-resource functions.
- Python permitted only under tools/ and ui/. No graded mechanism
may live there.
- Smallest diff that passes. Do not refactor unrelated code.
## Workflow
- Multi-file or algorithmic change: plan first, wait for approval.
- Hardware bugs: paste real evidence (dmesg, timing capture,
/proc/interrupts). No fixes proposed from a verbal description.