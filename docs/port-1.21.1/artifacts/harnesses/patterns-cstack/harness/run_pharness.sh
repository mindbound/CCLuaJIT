#!/bin/bash
# usage: run_pharness.sh <exe> <modes...>
cd "$(dirname "$0")"
exe=$1; shift
export PH_DIR=$(cd .. && pwd)
for m in "$@"; do
  for c in $(./$exe $m list | tr -d '\r'); do
    timeout 60 ./$exe $m $c
    rc=$?
    [ $rc -eq 124 ] && echo "CASE $c [$m]" && echo "  RESULT TIMEOUT (killed after 60 s)"
    [ $rc -ne 0 ] && [ $rc -ne 124 ] && [ $rc -ne 3 ] && echo "  (exit code $rc)"
  done
done
