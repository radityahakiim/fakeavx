#!/bin/sh
# Warm-up test (kmod/main.c, fa_warm_up()) in the VM: fakeavx-prof --check
# runs cold, again in the same boot (the module knows the sites), and after
# "fakeavx-cpuid save", a module reload (as at a reboot) and "restore". Every
# run must be bit-exact; the later ones should warm up instead of trapping.
#
#   KERNEL=bzImage QEMU=qemu-system-x86_64 [CPU=core2duo] ./warm-test.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
extra=$(mktemp -d)
trap 'rm -rf "$extra"' EXIT
mkdir "$extra/warm"
cp "$top/tools/fakeavx-cpuid" "$top/tools/prof/golden.txt" "$extra/warm/"
cat > "$extra/warm/warm.sh" <<'GUEST'
P=/sys/module/fakeavx/parameters
st() { cat $P/stat_$1; }
run() {
	a=$(st slow); b=$(st ns_slow); w=$(st warmed); nw=$(st ns_warm); sy=$(st syncs); sk=$(st syncs_skipped)
	/app -r 3 -t 0.3 --check /tmp/warm/golden.txt > /tmp/out.$1 2>&1
	echo "$1: $(grep -oE '[0-9]+ tests; check: [0-9]+ compared, [0-9]+ FAILED' /tmp/out.$1)"
	echo "   slow-path entries $(( $(st slow) - a )), slow path $(( ($(st ns_slow) - b) / 1000000 )) ms (warm-up $(( ($(st ns_warm) - nw) / 1000000 )) ms, $(( $(st warmed) - w )) sites), IPI rounds $(( $(st syncs) - sy )), skipped $(( $(st syncs_skipped) - sk ))"
}
run cold
run same-boot
sh /tmp/warm/fakeavx-cpuid save /tmp/cache
echo "saved: $(grep -c '^warm:' /tmp/cache) warm-up sites"
rmmod fakeavx && insmod /fakeavx.ko
sh /tmp/warm/fakeavx-cpuid restore /tmp/cache
run after-restore
echo "dropped as stale: $(st warm_stale)"
GUEST
EXTRA="$extra/warm" "$here/run-app.sh" "$top/tools/prof/fakeavx-prof" "sh /tmp/warm/warm.sh" |
	grep -E "^(cold|same-boot|after-restore|   slow|saved|fakeavx: [0-9]+ cached|dropped)|BUG|WARNING|Oops"
