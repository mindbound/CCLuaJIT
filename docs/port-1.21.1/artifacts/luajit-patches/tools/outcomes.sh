#!/bin/bash
# usage: outcomes.sh <results-dir>  -> normalized per-case outcomes (timings, line numbers, paths removed) for diffing
R=$1
norm() { tr -d '\r' | sed -E 's/latency=[0-9.]+ ?[mu]s//; s/\(ran [0-9.]+ ms\)//; s/\]:[0-9]+:/]:L:/g; s/ +$//'; }
echo "## preempt"
awk '/^CASE |^== /{c=$0} /RESULT/{print c " => " $0}' "$R/preempt.txt" | norm | sed -E 's/(msg=).*(interrupted|hard abort).*/\1\2/'
echo "## pharness"
awk '/^CASE /{c=$0} /RESULT/{print c " => " $2 " " $3}' "$R/pharness.txt" | norm
echo "## cstack"
sed -n '/^SUMMARY/,$p' "$R/cstack.txt" | tr -d '\r' | awk 'NR>1{print $1, $2}'
echo "## compat"
tr -d '\r' < "$R/compat.txt" | grep -av "no file"
