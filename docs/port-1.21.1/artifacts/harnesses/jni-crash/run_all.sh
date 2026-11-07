#!/bin/bash
# Re-runs every harness test against the UNMODIFIED origin/1.14.4_CCT main.cpp build (cclj_1144.dll),
# one JVM per test, and records output + exit codes in logs/run_all.txt.
W="<scratch>/work/code-audit"
cd "$W/jni"
OUT=logs/run_all.txt; : > $OUT
for t in strings first-yield-filter bios-error api-call unknown-java-type yield-function cyclic-map cyclic-table-roundtrip \
         "deep-table 40" "deep-table 2000" "deep-table 20000" abort-plain abort-pcall close-while-running abort-after-close; do
  echo "################ $t" >> $OUT
  timeout 60 java -XX:ErrorFile=NUL -cp out harness.Main "$W/lj/lua51.dll" "$W/src/cclj_1144.dll" $t >> $OUT 2>&1
  echo "exit=$?" >> $OUT
done
echo "################ gc-upvalue (variant DLL with luaopen_debug)" >> $OUT
timeout 60 java -XX:ErrorFile=NUL -cp out harness.Main "$W/lj/lua51.dll" "$W/src/cclj_1144_debug.dll" gc-upvalue >> $OUT 2>&1
echo "exit=$?" >> $OUT
