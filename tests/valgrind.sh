#!/bin/sh
# Run a program under valgrind and fail on memory errors, leaks, OR leaked fds.
# valgrind's --track-fds=yes only reports fds left open at exit; it does not
# change the exit code, so we grep its log for them.
log=$(mktemp) || exit 2
valgrind -q --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=all \
    --track-fds=yes --log-file="$log" "$@"
status=$?
cat "$log" >&2
if grep -q "Open file descriptor" "$log"; then
    echo "valgrind.sh: file descriptor leaked at exit" >&2
    status=1
fi
rm -f "$log"
exit $status
