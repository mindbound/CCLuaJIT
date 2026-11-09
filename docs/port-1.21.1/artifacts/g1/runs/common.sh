# Sourced by the G1 scripts. step <name> <cmd...>: prints the step, runs it, prints its exit code.
step() { local name=$1; shift; echo "=== $name ($(date +%H:%M:%S))"; "$@"; local rc=$?; echo "=== $name exit: $rc"; return 0; }
