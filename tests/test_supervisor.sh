#!/bin/sh
# Integration tests for the supervisor (mechanism E), driven by fake_child.
# Usage: test_supervisor.sh BUILD_DIR
# SUP_RUNNER, if set, prefixes the supervisor command (make memcheck uses the
# valgrind wrapper). The kill -9 case always runs the bare binary, since a
# killed process cannot be leak-checked.
set -u
build=$(cd "${1:?usage: test_supervisor.sh BUILD_DIR}" && pwd) || exit 2
sup=$build/supervisor/supervisor
runner=${SUP_RUNNER:-}
tmp=$(mktemp -d) || exit 2
# -c commands are split on spaces, so the child must live at a path without
# them; the repo path may have some (e.g. "School Stuff"). mktemp uses /tmp.
fc=$tmp/fake_child
cp "$build/stub/fake_child" "$fc" || exit 2
fail=0
bgpid=
sup_pid=
sup_status=
case=setup

cleanup() {
    [ -n "$bgpid" ] && kill -9 "$bgpid" 2>/dev/null
    [ -n "$sup_pid" ] && kill -9 "$sup_pid" 2>/dev/null
    pkill -9 -f "^$fc --tag sup$$" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT

failed() {
    echo "FAIL [$case]: $*"
    fail=1
}

# count LOG PATTERN: number of matching lines (0 if none or no file).
count() {
    _c=$(grep -c -- "$2" "$1" 2>/dev/null)
    echo "${_c:-0}"
}

# wait_for LOG PATTERN [COUNT] [TENTHS]: wait until COUNT lines match.
wait_for() {
    _n=0
    # Invariant: _n tenths of a second have been spent waiting.
    while [ "$(count "$1" "$2")" -lt "${3:-1}" ]; do
        [ "$_n" -ge "${4:-100}" ] && return 1
        sleep 0.1
        _n=$((_n + 1))
    done
}

# start_sup LOG ARGS...: run "$r supervisor ARGS" in the background with two
# extra inherited fds (5, 7) that close_range must keep out of the children.
start_sup() {
    _log=$1
    shift
    $r "$sup" "$@" 2>"$_log" 5</dev/null 7</dev/null &
    bgpid=$!
    sup_pid=
    _n=0
    # Invariant: _n tenths of a second waited; stop early if the supervisor exited.
    until [ "$(count "$_log" "event=supervisor_start")" -ge 1 ]; do
        if [ "$_n" -ge 150 ] || ! kill -0 "$bgpid" 2>/dev/null; then
            failed "supervisor did not start"
            cat "$_log"
            wait "$bgpid"
            bgpid=
            return 1
        fi
        sleep 0.1
        _n=$((_n + 1))
    done
    sup_pid=$(sed -n 's/.*event=supervisor_start pid=\([0-9]*\).*/\1/p' "$_log")
}

# stop_sup: SIGTERM the supervisor and collect its exit status (15 s limit).
stop_sup() {
    kill -TERM "$sup_pid" 2>/dev/null
    _n=0
    # Invariant: _n tenths of a second have been spent waiting for exit.
    while kill -0 "$bgpid" 2>/dev/null && [ "$_n" -lt 150 ]; do
        sleep 0.1
        _n=$((_n + 1))
    done
    if kill -0 "$bgpid" 2>/dev/null; then
        failed "supervisor still running 15 s after SIGTERM"
        kill -9 "$sup_pid" "$bgpid" 2>/dev/null
    fi
    wait "$bgpid"
    sup_status=$?
    bgpid=
    sup_pid=
}

# child_pid LOG NAME: pid of the most recent start of child NAME.
child_pid() {
    sed -n "s/.*event=child_start name=$2 pid=\([0-9]*\).*/\1/p" "$1" | tail -n 1
}

# survivors TAG: pids of fake_child processes started with --tag TAG.
survivors() {
    pgrep -f "^$fc --tag $1( |\$)" | paste -sd' ' -
}

# --- 1: children start with exactly fds 0-3 ---------------------------------
case=1_start_fds
T=sup$$_1
log=$tmp/1.log
r=$runner
if start_sup "$log" -c "a=$fc --tag $T" -c "b=$fc --tag $T"; then
    wait_for "$log" "fake_child event=start" 2 150 || failed "children did not start"
    n=$(count "$log" "fake_child event=start .* fds=0,1,2,3$")
    if [ "$n" -ne 2 ]; then
        failed "want 2 children reporting exactly fds=0,1,2,3, got $n"
        grep "fake_child event=start" "$log"
    fi

    # --- 5: SIGTERM stops everything, exit 0 (same run) ---------------------
    case=5_sigterm
    stop_sup
    [ "$sup_status" -eq 0 ] || failed "exit status $sup_status, want 0"
    grep -q "event=shutdown_complete" "$log" || failed "no shutdown_complete line"
    left=$(survivors "$T")
    [ -z "$left" ] || failed "children survived shutdown: $left"
fi
[ "$fail" -eq 0 ] || cat "$log"

# --- 2: kill -9 of a child is logged and the child restarts -----------------
case=2_kill_child
T=sup$$_2
log=$tmp/2.log
r=$runner
if start_sup "$log" -c "a=$fc --tag $T"; then
    wait_for "$log" "event=child_start name=a" 1 150 || failed "child a did not start"
    old=$(child_pid "$log" a)
    kill -9 "$old"
    wait_for "$log" "event=child_exit name=a pid=$old signal=9" 1 50 ||
        failed "kill -9 of pid $old not logged"
    wait_for "$log" "event=child_start name=a" 2 50 || failed "a not restarted within 5 s"
    new=$(child_pid "$log" a)
    [ "$new" != "$old" ] || failed "restarted child has the old pid $old"
    stop_sup
    [ "$sup_status" -eq 0 ] || failed "exit status $sup_status, want 0"
fi

# --- 3: a crash loop backs off 1 s, 2 s, 4 s --------------------------------
case=3_backoff
T=sup$$_3
log=$tmp/3.log
r=$runner
if start_sup "$log" -c "c=$fc --tag $T --exit-after 0 --exit-code 3"; then
    wait_for "$log" "event=child_restart_scheduled name=c" 3 150 ||
        failed "fewer than 3 restarts scheduled"
    delays=$(sed -n 's/.*event=child_restart_scheduled name=c delay_ms=\([0-9]*\).*/\1/p' "$log" |
        head -n 3 | paste -sd, -)
    [ "$delays" = "1000,2000,4000" ] || failed "delays '$delays', want 1000,2000,4000"
    grep -q "event=child_exit name=c pid=[0-9]* code=3" "$log" || failed "exit code 3 not logged"
    stop_sup
    [ "$sup_status" -eq 0 ] || failed "exit status $sup_status, want 0"
fi

# --- 4: exit 1 (config error) and exit 0 (clean stop) are not restarted ------
case=4_no_restart
T=sup$$_4
log=$tmp/4.log
r=$runner
if start_sup "$log" -c "d=$fc --tag $T --exit-after 0 --exit-code 1" \
    -c "e=$fc --tag $T --exit-after 0 --exit-code 0" -c "f=$fc --tag $T"; then
    wait_for "$log" "event=child_config_error name=d action=not_restarted" 1 150 ||
        failed "exit 1 not logged as config error"
    wait_for "$log" "event=child_stopped name=e" 1 150 || failed "exit 0 not logged as stopped"
    sleep 2
    [ "$(count "$log" "event=child_start name=d ")" -eq 1 ] || failed "d (exit 1) was restarted"
    [ "$(count "$log" "event=child_start name=e ")" -eq 1 ] || failed "e (exit 0) was restarted"
    kill -0 "$sup_pid" 2>/dev/null || failed "supervisor died"
    stop_sup
    [ "$sup_status" -eq 0 ] || failed "exit status $sup_status, want 0"
fi

# --- 6: kill -9 of the supervisor leaves no children (PR_SET_PDEATHSIG) -----
# --ignore-sigpipe: otherwise the children's next heartbeat write into the dead
# supervisor's pipe kills them by SIGPIPE, and this would pass without PDEATHSIG.
case=6_kill_supervisor
T=sup$$_6
log=$tmp/6.log
r=
if start_sup "$log" -c "a=$fc --tag $T --ignore-sigpipe" -c "b=$fc --tag $T --ignore-sigpipe"; then
    wait_for "$log" "fake_child event=start" 2 150 || failed "children did not start"
    kill -9 "$sup_pid"
    wait "$bgpid"
    bgpid=
    sup_pid=
    n=0
    # Invariant: n tenths of a second spent waiting for the children to die.
    while [ -n "$(survivors "$T")" ] && [ "$n" -lt 20 ]; do
        sleep 0.1
        n=$((n + 1))
    done
    left=$(survivors "$T")
    if [ -n "$left" ]; then
        failed "children survived kill -9 of the supervisor: $left"
        kill -9 $left 2>/dev/null
    fi
fi

# --- 7: bad configuration exits 1 -------------------------------------------
case=7_config
r=$runner
for spec in none noequals missing dup badopt; do
    log=$tmp/7_$spec.log
    case $spec in
    none) $r "$sup" 2>"$log" ;;
    noequals) $r "$sup" -c "$fc" 2>"$log" ;;
    missing) $r "$sup" -c "x=/nonexistent/prog" 2>"$log" ;;
    dup) $r "$sup" -c "a=$fc" -c "a=$fc" 2>"$log" ;;
    badopt) $r "$sup" -q -c "a=$fc" 2>"$log" ;;
    esac
    status=$?
    [ "$status" -eq 1 ] || { failed "$spec: exit status $status, want 1"; cat "$log"; }
done

# --- 8: a child ignoring SIGTERM is SIGKILLed after the 5 s grace -----------
case=8_shutdown_kill
T=sup$$_8
log=$tmp/8.log
r=$runner
if start_sup "$log" -c "g=$fc --tag $T --ignore-term"; then
    wait_for "$log" "fake_child event=start" 1 150 || failed "child did not start"
    stop_sup
    [ "$sup_status" -eq 0 ] || failed "exit status $sup_status, want 0"
    grep -q "event=shutdown_kill name=g" "$log" || failed "no shutdown_kill line for g"
    left=$(survivors "$T")
    [ -z "$left" ] || failed "child survived: $left"
fi

[ "$fail" -eq 0 ] && echo "test_supervisor: all checks passed"
exit "$fail"
