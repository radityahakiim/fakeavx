#!/bin/sh
# Runs inside the VM (see run-app.sh), with /app = tests/guest/cpuid_test and
# EXTRA="tools/fakeavx-cpuid tests/guest/cpuid_plain tests/qemu/guest-cache-test.sh".
#
# - /app asks CPUID before executing anything unsupported: it must pass on
#   its very first run, because programs are examined before they start.
# - cpuid_plain contains nothing the CPU lacks: it must see the real CPU.
# - The list survives save -> clear -> restore, and stale entries are dropped.
T=/tmp/fakeavx-cpuid
P=/sys/module/fakeavx/parameters
/app > /tmp/out; echo "first run (cpuid asked before any trap): exit $?"; grep RESULT /tmp/out
/app > /dev/null; echo "second run: exit $?"
/tmp/cpuid_plain; echo "cpuid_plain: exit $?"
echo "exec scans: $(cat $P/stat_scans_exec), trap scans: $(cat $P/stat_scans)"
sh $T list
echo "--- list -a"
sh $T list -a
sh $T save /tmp/cache; echo "saved: $(grep -c . /tmp/cache) lines"
sh $T clear; echo "after clear: $(grep -c . /proc/fakeavx/sites) lines"
sh $T restore /tmp/cache
scans=$(($(cat $P/stat_scans) + $(cat $P/stat_scans_exec)))
/app > /dev/null; echo "run right after restore: exit $?"
/tmp/cpuid_plain > /dev/null; echo "cpuid_plain after restore: exit $?"
echo "scans since restore: $(($(cat $P/stat_scans) + $(cat $P/stat_scans_exec) - scans)) (expect 0)"
# a changed binary must not get the old sites
sh $T clear
echo 1 > $P/execscan
echo 0 > $P/autocpuid
touch -d '2001-01-01' /app
sh $T restore /tmp/cache
echo "after restore with changed /app: $(grep -c " /app\$" /proc/fakeavx/sites) /app lines (expect 0)"
echo 1 > $P/autocpuid
