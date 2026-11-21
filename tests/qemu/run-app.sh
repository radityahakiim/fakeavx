#!/bin/sh
# Run a dynamically linked program inside the Penryn VM with fakeavx loaded.
# The VM has no network device.
#
#   KERNEL=bzImage QEMU=qemu-system-x86_64 [CPU=core2duo] [RUNS=2] [SITES=file] [FAKEAVX=avx2] \
#       ./run-app.sh /path/to/binary 'guest command line'
#
# The binary is copied to /app in the guest together with the host's glibc.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
: "${KERNEL:?set KERNEL to a bzImage}"
: "${QEMU:=qemu-system-x86_64}"
bin=$1
cmd=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/root/bin" "$work/root/proc" "$work/root/sys" "$work/root/dev" \
	"$work/root/lib64" "$work/root/usr/lib" "$work/root/tmp" "$work/root/etc"
ln -s usr/lib "$work/root/lib"
cp "$(command -v busybox)" "$work/root/bin/busybox"
cp "$top/kmod/fakeavx.ko" "$work/root/"
cp "$bin" "$work/root/app"
chmod +x "$work/root/app"
for l in libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 ld-linux-x86-64.so.2 libgcc_s.so.1 libstdc++.so.6; do
	[ -e /usr/lib/$l ] && cp -L /usr/lib/$l "$work/root/usr/lib/"
done
cp -L /usr/lib/ld-linux-x86-64.so.2 "$work/root/lib64/"
[ -n "$EXTRA" ] && cp -r $EXTRA "$work/root/tmp/"
# SITES: output of "tools/fakeavx-cpuid scan" for the binary
[ -n "$SITES" ] && cp "$SITES" "$work/root/sites"
true
cat > "$work/root/init" <<INIT
#!/bin/busybox sh
B=/bin/busybox
\$B mount -t proc proc /proc
\$B mount -t sysfs sys /sys
\$B mount -t devtmpfs dev /dev 2>/dev/null
\$B mkdir -p /dev/shm; \$B mount -t tmpfs tmpfs /dev/shm
export HOME=/tmp PATH=/bin TMPDIR=/tmp
\$B --install -s /bin
\$B insmod /fakeavx.ko ${MODARGS:-} || echo "INSMOD FAILED"
if [ -f /sites ]; then
	n=0; while read off; do echo "\$off /app" > /sys/module/fakeavx/parameters/cpuid_site && n=\$((n+1)); done < /sites
	echo "=== \$n cpuid sites registered"
fi
cd /tmp
i=0; while [ \$i -lt ${RUNS:-1} ]; do i=\$((i+1))
echo "=== run \$i"
${FAKEAVX:+FAKEAVX=$FAKEAVX }$cmd; echo "exit status \$?"
done
echo "=== /proc/fakeavx/sites: \$(\$B grep -c . /proc/fakeavx/sites) lines"; \$B grep -v -e "^[0-9a-f]* " -e "^warm:" /proc/fakeavx/sites
echo "=== stats"
for f in /sys/module/fakeavx/parameters/stat_*; do echo "\$(\$B basename \$f) \$(\$B cat \$f)"; done
\$B dmesg | \$B grep -i -E 'fakeavx|BUG|WARNING|Oops|sleeping|scheduling while|segfault|traps:' | \$B tail -25
echo "=== done"
\$B poweroff -f
INIT
chmod +x "$work/root/init"
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null | gzip -1) > "$work/initrd.gz"

"$QEMU" -accel tcg -cpu "${CPU:-Penryn}" -smp "${SMP:-2}" -m "${MEM:-3072}" -nographic -no-reboot \
	-nic none -kernel "$KERNEL" -initrd "$work/initrd.gz" \
	-append "console=ttyS0 panic=-1 loglevel=4 ${CMDLINE:-}"
