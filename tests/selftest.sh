#!/bin/bash
# fakeavx self-test: build everything, run every test this machine can run,
# and write a report.
#
#   tests/selftest.sh [--quick] [--ref REF] [--out DIR]
#                     [--no-module] [--no-bench] [--no-http]
#
#   --quick      fewer inputs everywhere (~5 min instead of ~15)
#   --ref REF    build to compare the helper paths with (default HEAD; with
#                an unmodified tree that comparison is trivially identical,
#                so pass e.g. --ref HEAD~1 to compare with the previous commit)
#   --out DIR    where the report goes (default reports/)
#   --no-module  skip everything that needs the fakeavx module loaded
#   --no-bench   skip bench/fakeavx-bench.ts (~3 min)
#   --no-http    skip the Bun fetch() measurement
#
# Writes DIR/selftest-YYYYmmdd-HHMMSS.txt (the report) and a directory of the
# same name with every step's full log. Exit status 1 if any step failed.
#
# Needs: gcc, make, binutils, python3; kernel headers to build the module;
# bun for the benchmark and the HTTP test; gdb (optional) to count helper
# calls per HTTP request. Installing the module is not part of this: build,
# `sudo make install`, reload (from a terminal not running under fakeavx),
# then run this.
set -u
export LC_ALL=C

top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top" || exit 2

quick=0 ref=HEAD outdir="$top/reports" do_module=1 do_bench=1 do_http=1
while [ $# -gt 0 ]; do
	case "$1" in
	--quick) quick=1 ;;
	--ref) ref=$2; shift ;;
	--out) outdir=$2; shift ;;
	--no-module) do_module=0 ;;
	--no-bench) do_bench=0 ;;
	--no-http) do_http=0 ;;
	-h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

if [ $quick = 1 ]; then
	N_ORACLE=300 N_DUMP=300 N_PAIR=10 N_FUZZ=500000 N_HTTP=1000 N_COUNT=100 BENCH_ARGS=--quick
else
	N_ORACLE=2000 N_DUMP=2000 N_PAIR=50 N_FUZZ=3000000 N_HTTP=2000 N_COUNT=300 BENCH_ARGS=
fi

stamp=$(date +%Y%m%d-%H%M%S)
mkdir -p "$outdir" || exit 2
logs="$outdir/selftest-$stamp"
report="$outdir/selftest-$stamp.txt"
mkdir -p "$logs" || exit 2
start_iso=$(date '+%Y-%m-%d %H:%M:%S')
t_start=$(date +%s)

# ------------------------------------------------------------------ steps

names=() states=() details=() durations=()
nfail=0

# record NAME STATE DETAIL SECONDS
record() {
	names+=("$1"); states+=("$2"); details+=("$3"); durations+=("$4")
	[ "$2" = FAIL ] && nfail=$((nfail + 1))
	printf '  %-36s %-5s %s\n' "$1" "$2" "$3"
}

# run NAME CMD...: run with output to $logs/NAME.log; sets rc, log, secs
run() {
	local name=$1
	shift
	log="$logs/$name.log"
	local t0=$(date +%s)
	"$@" >"$log" 2>&1
	rc=$?
	secs=$(( $(date +%s) - t0 ))
}

have() { command -v "$1" >/dev/null 2>&1; }

P=/sys/module/fakeavx/parameters
STATS="slow fast translated regions regions_borrowing patched patched_short unpatched diverted unsupported"
stats() { for s in $STATS; do printf '%s=%s ' "$s" "$(cat "$P/stat_$s" 2>/dev/null || echo 0)"; done; }

module_loaded=0
[ -d "$P" ] && module_loaded=1
[ $do_module = 1 ] && [ $module_loaded = 0 ] && do_module=0 && module_note="not loaded: module tests skipped"
stats_start=$(stats)

echo "fakeavx self-test, logs in $logs"

# ------------------------------------------------------------------ build
echo "build"
run build-helpers make -C helpers
[ $rc = 0 ] && record "build: helper blob" PASS "helpers/blob.bin" $secs || record "build: helper blob" FAIL "see build-helpers.log" $secs
run build-module make module
if [ $rc = 0 ]; then record "build: kernel module" PASS "kmod/fakeavx.ko" $secs
else record "build: kernel module" FAIL "see build-module.log (kernel headers?)" $secs; fi
run build-tests make -C tests helperbench fuzz difftest
[ $rc = 0 ] && record "build: test programs" PASS "helperbench fuzz difftest" $secs || record "build: test programs" FAIL "see build-tests.log" $secs
run build-prof make prof
[ $rc = 0 ] && record "build: fakeavx-prof" PASS "" $secs || record "build: fakeavx-prof" FAIL "see build-prof.log" $secs
run build-guest make guest
[ $rc = 0 ] && record "build: guest tests" PASS "" $secs || record "build: guest tests" FAIL "see build-guest.log" $secs

HB=tests/helperbench

# ------------------------------------------------------------------ userspace
echo "userspace (translator and helper blob in an ordinary process)"
if git -C "$top" diff --quiet "$ref" -- core helpers 2>/dev/null; then
	same_as_ref=" (core/ and helpers/ unchanged from $ref: trivially identical)"
else
	same_as_ref=""
fi
run oracle tests/helper_oracle.sh "$ref" "$N_ORACLE"
ident=$(grep -c "identical to" "$log")
diffs=$(grep -c "DIFFERENT" "$log")
if [ $rc = 0 ] && [ "$ident" -gt 0 ]; then
	record "oracle vs $ref" PASS "$ident/6 comparisons identical$same_as_ref" $secs
else
	record "oracle vs $ref" FAIL "$diffs comparison(s) DIFFERENT, see oracle.log" $secs
fi

for mode in penryn ssse3; do
	if [ $mode = ssse3 ]; then e="HB_NOSSE41=1"; else e="HB_PENRYN=1"; fi
	run "vexpair-$mode" env $e "$HB" vexpair "$N_PAIR"
	line=$(tail -1 "$log")
	mm=$(sed -n 's/.*cases.*), \([0-9]*\) mismatches.*/\1/p' "$log" | tail -1)
	if [ $rc = 0 ] && [ "${mm:-x}" = 0 ]; then
		record "VEX vs legacy pcmpXstrX ($mode)" PASS "$(echo "$line" | sed 's/^vexpair: //')" $secs
	else
		record "VEX vs legacy pcmpXstrX ($mode)" FAIL "$line" $secs
	fi
done

run fuzz tests/fuzz "$N_FUZZ"
[ $rc = 0 ] && record "fuzz (ASan/UBSan)" PASS "$N_FUZZ inputs" $secs || record "fuzz (ASan/UBSan)" FAIL "exit $rc, see fuzz.log" $secs

run timing-strhttp "$HB" strhttp
la6=$(awk '$1=="0x4" && $2==6 {print $3}' "$log"); la16=$(awk '$1=="0x4" && $2==16 {print $3}' "$log")
record "time: picohttpparser pcmpestri" INFO "ranges la=6: ${la6:-?} cycles, la=16: ${la16:-?} cycles" $secs
run timing-strcost "$HB" strcost
worst=$(awk 'NR>2 {for (i=4;i<=6;i++) if ($i>m) {m=$i; w=$1" "$2" "$3}} END {print m" ("w")"}' "$log")
record "time: pcmpXstrX, all modes" INFO "worst case $worst cycles" $secs
run timing-helpers "$HB" time
sel=$(awk '$1 ~ /^(pcmpgtq|vpermd_y|vpmaskmovd_ld_y|vpgatherdd_y|pcmpistri|crc32q|popcnt64)$/ {printf "%s %.0f, ", $1, $2}' "$log")
record "time: helper paths" INFO "${sel%, } cycles" $secs

# ------------------------------------------------------------------ module
if [ $do_module = 1 ]; then
	echo "module (code executed natively, translated by the loaded fakeavx)"
	ko=$(modinfo -n fakeavx 2>/dev/null)
	ident_note="?" ident_state=WARN
	if [ -n "$ko" ] && [ -f "$ko" ] && [ -f kmod/fakeavx.ko ]; then
		objcopy -O binary -j .text "$ko" "$logs/installed.text" 2>/dev/null
		objcopy -O binary -j .text kmod/fakeavx.ko "$logs/tree.text" 2>/dev/null
		if cmp -s "$logs/installed.text" "$logs/tree.text"; then
			ident_note="installed module = this tree's build"
			ident_state=PASS
		else
			ident_note="installed module is NOT this tree's build (sudo make install, reload)"
			ident_state=WARN
		fi
		inst=$(stat -c %Y "$ko")
		load_line=$(journalctl -k -b --no-pager -o short-unix 2>/dev/null | grep "fakeavx: loaded" | tail -1)
		load=${load_line%%.*}
		if [ -n "$load" ] && [ "$load" -ge "$inst" ] 2>/dev/null; then
			ident_note="$ident_note; loaded $(date -d "@$load" '+%H:%M:%S'), after the install"
		elif [ -n "$load" ]; then
			ident_note="$ident_note; but loaded BEFORE the install: reload it"
			ident_state=WARN
		fi
	else
		ident_state=WARN
		ident_note="cannot compare (no installed module file or no tree build)"
	fi
	record "module identity" "$ident_state" "$ident_note" 0

	run mvexpair "$HB" mvexpair "$N_PAIR"
	mm=$(sed -n 's/.*, \([0-9]*\) mismatches/\1/p' "$log" | tail -1)
	[ $rc = 0 ] && [ "${mm:-x}" = 0 ] && record "VEX vs legacy, under the module" PASS "$(tail -1 "$log" | sed 's/^mvexpair (under the module): //')" $secs \
		|| record "VEX vs legacy, under the module" FAIL "$(tail -1 "$log")" $secs

	t0=$(date +%s)
	cmp <("$HB" dump "" "$N_DUMP") <("$HB" mdump "" "$N_DUMP") >"$logs/mdump.log" 2>&1
	rc1=$?
	cmp <("$HB" rdump "" "$N_DUMP") <("$HB" mrdump "" "$N_DUMP") >>"$logs/mdump.log" 2>&1
	rc2=$?
	secs=$(( $(date +%s) - t0 ))
	nins=$("$HB" dump "" 1 | wc -l); nloop=$("$HB" rdump "" 1 | wc -l)
	if [ $rc1 = 0 ] && [ $rc2 = 0 ]; then
		record "module vs userspace translator" PASS "$nins instructions + $nloop loops x $N_DUMP inputs identical" $secs
	else
		record "module vs userspace translator" FAIL "differs: see mdump.log" $secs
	fi

	for g in guest_test unpatched_test; do
		run "guest-$g" "tests/guest/$g"
		r=$(grep RESULT "$log" | tail -1)
		[ $rc = 0 ] && record "guest: $g" PASS "$r" $secs || record "guest: $g" FAIL "${r:-exit $rc}" $secs
	done
	# cpuid_test: the first run of a new file only teaches fakeavx its cpuid sites
	tests/guest/cpuid_test >/dev/null 2>&1
	run guest-cpuid_test tests/guest/cpuid_test
	r=$(grep RESULT "$log" | tail -1)
	[ $rc = 0 ] && record "guest: cpuid_test" PASS "$r" $secs || record "guest: cpuid_test" FAIL "${r:-exit $rc}" $secs

	run prof tools/prof/fakeavx-prof --check tools/prof/golden.txt
	s=$(grep -E "^[0-9]+ tests" "$log" | tail -1)
	failed=$(echo "$s" | sed -n 's/.*compared, \([0-9]*\) FAILED.*/\1/p')
	trapping=$(echo "$s" | sed -n 's/.* \([0-9]*\) still trapping.*/\1/p')
	faulted=$(echo "$s" | sed -n 's/.* \([0-9]*\) faulted.*/\1/p')
	if [ "${failed:-x}" = 0 ] && [ "${trapping:-x}" = 0 ] && [ "${faulted:-x}" = 0 ]; then
		record "fakeavx-prof --check" PASS "$s" $secs
	else
		record "fakeavx-prof --check" FAIL "${s:-exit $rc, see prof.log}" $secs
	fi

	# an instruction fakeavx rejects (FMA) must die with SIGILL and be logged
	printf '.text\n.globl main\nmain:\n\tvfmadd231ps %%ymm2, %%ymm1, %%ymm0\n\txor %%eax, %%eax\n\tret\n.section .note.GNU-stack,"",@progbits\n' >"$logs/selftest-fma.S"
	if gcc -o "$logs/selftest-fma" "$logs/selftest-fma.S" 2>"$logs/fma-build.log"; then
		since=$(date '+%Y-%m-%d %H:%M:%S')
		# in a subshell: its "Illegal instruction" notice goes to /dev/null, no core file
		( ulimit -c 0; "$logs/selftest-fma"; exit $? ) >/dev/null 2>&1
		frc=$?
		sleep 1 2>/dev/null || python3 -c 'import time; time.sleep(1)'
		fline=$(journalctl -k --no-pager --since "$since" 2>/dev/null | grep "selftest-fma.*unsupported instruction" | tail -1)
		if [ $frc = 132 ] && [ -n "$fline" ]; then
			record "unsupported instruction logged" PASS "SIGILL and: ${fline#*kernel: }" 1
		elif [ $frc = 132 ]; then
			record "unsupported instruction logged" WARN "SIGILL, but no log line found (journal readable?)" 1
		else
			record "unsupported instruction logged" FAIL "expected SIGILL (132), got exit $frc" 1
		fi
	fi

	if [ $do_bench = 1 ] && have bun; then
		echo "  (benchmark: a few minutes)"
		run bench bun bench/fakeavx-bench.ts $BENCH_ARGS --json "$logs/bench.json"
		last=$(grep -E "all results identical|CHECK FAILURE" "$log" | tail -1)
		killed=$(grep -c "KILLED" "$log")
		[ $rc = 0 ] && record "benchmark (bun bench/fakeavx-bench.ts)" PASS "$last" $secs \
			|| record "benchmark (bun bench/fakeavx-bench.ts)" FAIL "${last:-exit $rc}; $killed kernel(s) KILLED" $secs
	elif [ $do_bench = 1 ]; then
		record "benchmark" SKIP "bun not found" 0
	fi

	if [ $do_http = 1 ] && have bun; then
		t0=$(date +%s)
		port=$((18200 + RANDOM % 700))
		PORT=$port bun bench/http/server.ts >"$logs/http-server.log" 2>&1 &
		sp=$!
		for i in $(seq 1 50); do grep -q ready "$logs/http-server.log" 2>/dev/null && break; sleep 0.2; done
		for i in 1 2 3; do PORT=$port bun bench/http/client.ts "$N_HTTP" >>"$logs/http.log" 2>&1; done
		kill $sp 2>/dev/null; wait $sp 2>/dev/null
		best=$(python3 -I -c 'import json,sys
r=[json.loads(l) for l in open(sys.argv[1]) if l.startswith("{")]
b=max(r,key=lambda x:x["rps"]) if r else None
print("%.0f req/s, %.1f us/request (best of %d runs x %s requests)"%(b["rps"],b["usPerReq"],len(r),b["n"]) if b else "")' "$logs/http.log")
		secs=$(( $(date +%s) - t0 ))
		[ -n "$best" ] && record "Bun fetch() round trip" INFO "$best" $secs || record "Bun fetch() round trip" FAIL "no result, see http.log" $secs
		if have gdb; then
			t0=$(date +%s)
			port=$((port + 1))
			PORT=$port bun bench/http/server.ts >"$logs/http-server2.log" 2>&1 &
			sp=$!
			for i in $(seq 1 50); do grep -q ready "$logs/http-server2.log" 2>/dev/null && break; sleep 0.2; done
			STOP=1 PORT=$port FAKEAVX_BLOB="$top/helpers/blob.bin" timeout 900 \
				gdb -q -batch -x bench/http/count-helpers.py --args "$(command -v bun)" bench/http/client.ts "$N_COUNT" \
				>"$logs/http-count.log" 2>&1
			kill $sp 2>/dev/null; wait $sp 2>/dev/null
			secs=$(( $(date +%s) - t0 ))
			pc=$(awk '/COUNT:/ && /sse4-legacy map3 op 0x6[0-3]/ {s+=$2} END {print s+0}' "$logs/http-count.log")
			other=$(awk '/COUNT:/ && $2 ~ /^[0-9]+$/ && !/map3 op 0x6[0-3]/ && !/  imm/ {d=$3; for (i=4;i<=NF;i++) d=d" "$i; printf "%s%s %.1f", sep, d, $2/'"$N_COUNT"'; sep=", "}' "$logs/http-count.log")
			if grep -q "COUNT: blob copies" "$logs/http-count.log"; then
				record "fetch(): helper calls per request" INFO "pcmpXstrX $(python3 -c "print(round($pc/$N_COUNT,1))"); other: ${other:-none}" $secs
			else
				record "fetch(): helper calls per request" WARN "gdb count failed, see http-count.log" $secs
			fi
		fi
	elif [ $do_http = 1 ]; then
		record "Bun fetch() round trip" SKIP "bun not found" 0
	fi

	# kernel log since the start of this run: no fakeavx errors
	journalctl -k --no-pager --since "$start_iso" 2>/dev/null | grep -i fakeavx >"$logs/kernel.log"
	nuns=$(grep "unsupported instruction" "$logs/kernel.log" | grep -vc "selftest-fma")
	nbad=$(grep -ciE "BUG|WARNING|Oops|general protection" "$logs/kernel.log")
	if [ "$nbad" = 0 ] && [ "$nuns" = 0 ]; then
		record "kernel log during the run" PASS "no fakeavx errors, no unsupported instructions (other than the FMA test)" 0
	else
		record "kernel log during the run" FAIL "$nbad error line(s), $nuns unsupported instruction(s): see kernel.log" 0
	fi
else
	record "module tests" SKIP "${module_note:---no-module}" 0
fi

# ------------------------------------------------------------------ report
t_total=$(( $(date +%s) - t_start ))
stats_end=$(stats)
{
	echo "fakeavx self-test report"
	echo "========================"
	echo
	printf '%-10s %s\n' date "$start_iso (took $((t_total / 60)) min $((t_total % 60)) s)"
	printf '%-10s %s\n' host "$(hostname) / $(uname -r)"
	printf '%-10s %s\n' cpu "$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"
	printf '%-10s %s\n' features "$(for f in ssse3 sse4_1 sse4_2 popcnt avx avx2 xsave; do grep -qw "$f" /proc/cpuinfo && printf '%s ' "$f"; done)"
	printf '%-10s %s\n' governor "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo ?), load $(cut -d' ' -f1-3 /proc/loadavg)"
	printf '%-10s %s\n' tree "$(git log -1 --format='%h %s' 2>/dev/null)$(git diff --quiet 2>/dev/null || echo ' (+ uncommitted changes)')"
	printf '%-10s %s\n' compared "with $ref ($(git log -1 --format=%h "$ref" 2>/dev/null))"
	if [ $module_loaded = 1 ]; then
		printf '%-10s %s\n' fakeavx "loaded, policy=$(cat $P/policy) patch=$(cat $P/patch)"
	else
		printf '%-10s %s\n' fakeavx "not loaded"
	fi
	printf '%-10s %s\n' tools "gcc $(gcc -dumpversion 2>/dev/null), bun $(bun --version 2>/dev/null || echo -), gdb $(gdb --version 2>/dev/null | head -1 | awk '{print $NF}' || echo -)"
	printf '%-10s %s\n' mode "$([ $quick = 1 ] && echo quick || echo full)"
	echo
	echo "Summary"
	echo "-------"
	npass=0 ninfo=0 nskip=0 nwarn=0
	for i in "${!names[@]}"; do
		case "${states[$i]}" in PASS) npass=$((npass+1));; INFO) ninfo=$((ninfo+1));; SKIP) nskip=$((nskip+1));; WARN) nwarn=$((nwarn+1));; esac
		printf '%-38s %-5s %5ss  %s\n' "${names[$i]}" "${states[$i]}" "${durations[$i]}" "${details[$i]}"
	done
	echo
	echo "Result: $npass passed, $nfail FAILED, $nwarn warnings, $ninfo measurements, $nskip skipped"
	[ $nfail = 0 ] && echo "        all checks passed" || echo "        FAILURES: see the logs named after each step"
	echo
	echo "Module counters (all processes) at start and end of the run"
	echo "-----------------------------------------------------------"
	echo "start: $stats_start"
	echo "end:   $stats_end"
	if [ -f "$logs/bench.log" ]; then
		echo
		echo "Benchmark (bench/fakeavx-bench.ts)"
		echo "----------------------------------"
		sed -n '/^1\. Native kernels/,$p' "$logs/bench.log"
	fi
	if [ -f "$logs/timing-strhttp.log" ]; then
		echo
		echo "pcmpXstrX, picohttpparser's form (helperbench strhttp)"
		echo "------------------------------------------------------"
		cat "$logs/timing-strhttp.log"
	fi
	if [ -f "$logs/http-count.log" ]; then
		echo
		echo "Helper calls during $N_COUNT fetch() requests (gdb)"
		echo "------------------------------------------------"
		grep "COUNT:" "$logs/http-count.log" | sed 's/^COUNT: //'
	fi
	echo
	echo "Logs: $logs/"
	ls "$logs" | sed 's/^/  /'
} >"$report"

echo
sed -n '/^Summary/,/^        /p' "$report"
echo
echo "report: $report"
[ $nfail = 0 ]
