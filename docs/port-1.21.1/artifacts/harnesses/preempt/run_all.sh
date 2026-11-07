#!/bin/bash
# Usage: run_all.sh <variant> <method>   -> runs every case in its own process
cd "$(dirname "$0")"
v=$1; m=$2
for c in $(./harness-$v.exe $m error list | tr -d '\r'); do
  timeout 10 ./harness-$v.exe $m error $c
  rc=$?
  [ $rc -eq 124 ] && echo "  RESULT TIMEOUT (process killed after 10 s)"
done
