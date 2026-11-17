#!/bin/sh
# Install fakeavx permanently: the module is loaded at every boot, rebuilt for
# new kernels (if DKMS is available), and the list of known cpuid sites
# survives reboots.
#
#   sudo scripts/install.sh              install and start
#   sudo scripts/install.sh --no-dkms    do not use DKMS even if present
#
# With DESTDIR=/some/dir only the files are laid out there (for packaging or
# inspection); nothing is built into the running system.
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
ver=$(cat "$top/VERSION")
D=${DESTDIR:-}
use_dkms=1
[ "$1" = "--no-dkms" ] && use_dkms=0
command -v dkms > /dev/null 2>&1 || use_dkms=0

if [ -z "$D" ] && [ "$(id -u)" != 0 ]; then
	echo "run as root (sudo $0)" >&2
	exit 1
fi
if ! grep -qw ssse3 /proc/cpuinfo; then
	echo "this CPU has no SSSE3; fakeavx needs a Core 2 or later" >&2
	[ -z "$D" ] && exit 1
fi
if [ -z "$D" ] && grep -qw avx /proc/cpuinfo; then
	echo "this CPU already has AVX; the module would refuse to load" >&2
	exit 1
fi

say() { echo "==> $*"; }

# --- tools, units, configuration
say "installing tools and systemd units"
install -Dm755 "$top/tools/fakeavx-cpuid" "$D/usr/local/bin/fakeavx-cpuid"
install -Dm755 "$top/tools/fakeavx-census" "$D/usr/local/bin/fakeavx-census"
for u in fakeavx.service fakeavx-save.service fakeavx-save.timer; do
	install -Dm644 "$top/scripts/$u" "$D/etc/systemd/system/$u"
done
install -d "$D/etc/modules-load.d" "$D/etc/modprobe.d" "$D/var/cache/fakeavx"
echo fakeavx > "$D/etc/modules-load.d/fakeavx.conf"
if [ ! -f "$D/etc/modprobe.d/fakeavx.conf" ]; then
	cat > "$D/etc/modprobe.d/fakeavx.conf" <<CONF
# fakeavx module options. Defaults: every process is handled (set FAKEAVX=0
# in a program's environment to leave it alone), instructions are patched.
#options fakeavx policy=0      # only programs started with FAKEAVX=1
#options fakeavx patch=0       # trap on every execution (slow; debugging)
#options fakeavx autocpuid=0   # do not look for cpuid instructions automatically
CONF
fi

# --- the module
if [ "$use_dkms" = 1 ]; then
	src=$D/usr/src/fakeavx-$ver
	say "copying sources to $src for DKMS"
	rm -rf "$src"
	install -d "$src"
	cp -r "$top/core" "$top/helpers" "$top/kmod" "$src/"
	sed "s/@VERSION@/$ver/" "$top/dkms.conf" > "$src/dkms.conf"
	find "$src" \( -name '*.o' -o -name '*.ko' -o -name '*.mod*' -o -name '.*.cmd' -o \
		-name 'blob.elf' -o -name 'blob.bin' -o -name 'blob.h' -o -name 'modules.order' -o \
		-name 'Module.symvers' \) -delete
	if [ -z "$D" ]; then
		for old in $(dkms status fakeavx 2> /dev/null | sed -n 's|^fakeavx/\([^,:]*\).*|\1|p' | sort -u); do
			[ "$old" = "$ver" ] || dkms remove "fakeavx/$old" --all > /dev/null 2>&1 || true
		done
		dkms remove "fakeavx/$ver" --all > /dev/null 2>&1 || true
		say "building and installing with DKMS (rebuilt automatically for new kernels)"
		dkms add "fakeavx/$ver"
		dkms install "fakeavx/$ver"
	fi
else
	kver=${KVER:-$(uname -r)}
	if [ -z "$D" ]; then
		say "building the module for kernel $kver"
		make -C "$top/helpers"
		make -C "$top/kmod" KDIR="/lib/modules/$kver/build"
	fi
	if [ -f "$top/kmod/fakeavx.ko" ]; then
		install -Dm644 "$top/kmod/fakeavx.ko" "$D/lib/modules/$kver/updates/fakeavx.ko"
	fi
	[ -z "$D" ] && depmod -a "$kver"
	echo "note: without DKMS, run this script again after every kernel update"
fi

[ -n "$D" ] && { say "files laid out under $D"; exit 0; }

# --- start it now
say "loading the module"
if [ -d /sys/module/fakeavx ]; then
	/usr/local/bin/fakeavx-cpuid save 2> /dev/null || true
	systemctl stop fakeavx.service 2> /dev/null || true
	rmmod fakeavx || echo "could not unload the old module; reboot to use the new one"
fi
modprobe fakeavx || true
systemctl daemon-reload
systemctl enable --now fakeavx.service fakeavx-save.timer

if [ -d /sys/module/fakeavx ]; then
	say "fakeavx is running"
else
	echo "the module did not load; see 'dmesg | tail'" >&2
	exit 1
fi
if grep -qw xsave /proc/cpuinfo && ! grep -qw noxsave /proc/cmdline; then
	echo "note: programs that ask XGETBV whether AVX is enabled will only get a yes"
	echo "      if the kernel is booted with the 'noxsave' parameter."
fi
