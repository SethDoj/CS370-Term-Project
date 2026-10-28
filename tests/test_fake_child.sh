#!/bin/sh
# Tests for the supervisor's stub child: fd report, heartbeats, exit codes.
# Usage: test_fake_child.sh BUILD_DIR
set -u
bin=${1:?usage: test_fake_child.sh BUILD_DIR}/stub/fake_child
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
fail=0

# fd 3 is a file standing in for the heartbeat pipe. Whatever else the caller
# leaked (WSL can leave /dev/ptmx on fd 5) is inherited too, so the fd report
# is checked against the kernel's own list in /proc/<pid>/fd, not a fixed set.
# The exact "0,1,2,3" guarantee belongs to the supervisor (close_range) and is
# asserted in the supervisor tests.
"$bin" --tag "t$$" --exit-after 3 --exit-code 3 3>"$tmp/hb" 2>"$tmp/log" &
pid=$!
tries=0
# Invariant: tries * 0.1 s spent waiting for the start line.
until grep -q "event=start" "$tmp/log" 2>/dev/null || [ "$tries" -ge 20 ]; do
    sleep 0.1
    tries=$((tries + 1))
done
actual=$(ls /proc/"$pid"/fd 2>/dev/null | sort -n | paste -sd, -)
reported=$(sed -n 's/.*event=start .* fds=\([0-9,+a-z]*\)$/\1/p' "$tmp/log")
wait "$pid"
status=$?

[ "$status" -eq 3 ] || { echo "FAIL: exit status $status, want 3"; fail=1; }
[ -n "$actual" ] || { echo "FAIL: could not read /proc/$pid/fd"; fail=1; }
[ "$reported" = "$actual" ] ||
    { echo "FAIL: reported fds '$reported' != /proc/$pid/fd '$actual'"; cat "$tmp/log"; fail=1; }
case ",$reported," in
*,3,*) ;;
*) echo "FAIL: heartbeat fd 3 missing from report '$reported'"; fail=1 ;;
esac
hb=$(wc -c <"$tmp/hb")
[ "$hb" -ge 2 ] || { echo "FAIL: $hb heartbeat bytes, want >= 2"; fail=1; }

# No fd 3 means it was not started by the supervisor: config error, exit 1.
"$bin" --exit-after 0 3>&- 2>"$tmp/log2"
status=$?
[ "$status" -eq 1 ] || { echo "FAIL: without fd 3 exit status $status, want 1"; fail=1; }

# Unknown option is also a config error.
"$bin" --bogus 3>"$tmp/hb2" 2>"$tmp/log3"
status=$?
[ "$status" -eq 1 ] || { echo "FAIL: unknown option exit status $status, want 1"; fail=1; }

[ "$fail" -eq 0 ] && echo "test_fake_child: all checks passed"
exit "$fail"
