#!/bin/sh
# Boot a kernel in QEMU with a Penryn-class CPU (no AVX, no POPCNT), load
# fakeavx.ko and run the end-to-end test inside.
#
#   KERNEL=/path/to/bzImage QEMU=/path/to/qemu-system-x86_64 ./run.sh [module params]
#
# The module must have been built against the same kernel (make -C kmod KDIR=...).
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
: "${KERNEL:?set KERNEL to a bzImage}"
: "${QEMU:=qemu-system-x86_64}"
: "${CPU:=Penryn}"
: "${SMP:=4}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/root/bin" "$work/root/proc" "$work/root/sys" "$work/root/dev"
cp "$(command -v busybox)" "$work/root/bin/busybox"
cp "$top/kmod/fakeavx.ko" "$work/root/"
cp "${GUEST:-$top/tests/guest/guest_test}" "$work/root/guest_test"
cat > "$work/root/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sys /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
B=/bin/busybox
echo "=== cpu flags: \$(\$B grep -m1 -o -w -E 'sse4_1|avx|avx2|popcnt|xsave' /proc/cpuinfo | \$B tr '\n' ' ')"
echo "=== without fakeavx (expect SIGILL)"
/guest_test; echo "exit status \$?"
echo "=== insmod fakeavx.ko $*"
\$B insmod /fakeavx.ko $* || echo "INSMOD FAILED"
echo "=== opted out with FAKEAVX=0 (expect SIGILL)"
FAKEAVX=0 /guest_test; echo "exit status \$?"
echo "=== opted in"
FA_REPEATS=${REPEATS:-200} /guest_test; echo "exit status \$?"
/guest_test xgetbv; echo "exit status \$?"
echo "=== stats"
for f in /sys/module/fakeavx/parameters/stat_*; do echo "\$(\$B basename \$f) \$(\$B cat \$f)"; done
echo "=== rmmod"
\$B rmmod fakeavx && echo "rmmod ok"
\$B dmesg | \$B grep -i -E 'fakeavx|BUG|WARNING|Oops|RCU|sleeping|scheduling while' | \$B tail -30
echo "=== done"
\$B poweroff -f
INIT
chmod +x "$work/root/init"
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null | gzip -1) > "$work/initrd.gz"

"$QEMU" -accel tcg -cpu "$CPU" -smp "$SMP" -m 512 -nographic -no-reboot \
	-kernel "$KERNEL" -initrd "$work/initrd.gz" \
	-append "console=ttyS0 panic=-1 quiet loglevel=4 ${CMDLINE:-}"
