#!/bin/sh
# Runs inside the VM (see run-app.sh): the cpuid site list must survive
# save -> clear -> restore, and stale entries must be dropped.
T=/tmp/fakeavx-cpuid
P=/sys/module/fakeavx/parameters
/app > /dev/null; echo "first run (cpuid asked before any trap): exit $?"
/app > /dev/null; echo "second run: exit $?"
sh $T list
sh $T save /tmp/cache; echo "saved: $(grep -c . /tmp/cache) lines"
sh $T clear; echo "after clear: $(grep -c . /proc/fakeavx/sites) lines"
sh $T restore /tmp/cache
scans=$(cat $P/stat_scans)
/app > /dev/null; echo "run right after restore: exit $? (scans since restore: $(($(cat $P/stat_scans) - scans)))"
# a changed binary must not get the old sites
sh $T clear
touch -d '2001-01-01' /app
sh $T restore /tmp/cache
echo "after restore with changed file: $(grep -c . /proc/fakeavx/sites) lines"
