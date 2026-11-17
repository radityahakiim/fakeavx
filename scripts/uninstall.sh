#!/bin/sh
# Remove what scripts/install.sh installed. With --purge the cpuid cache and
# the module options file go too.
set -e
[ "$(id -u)" = 0 ] || { echo "run as root (sudo $0)" >&2; exit 1; }

systemctl disable --now fakeavx-save.timer fakeavx.service 2> /dev/null || true
rmmod fakeavx 2> /dev/null || true
if command -v dkms > /dev/null 2>&1; then
	for v in $(dkms status fakeavx 2> /dev/null | sed -n 's|^fakeavx/\([^,:]*\).*|\1|p' | sort -u); do
		dkms remove "fakeavx/$v" --all || true
		rm -rf "/usr/src/fakeavx-$v"
	done
fi
rm -f /lib/modules/*/updates/fakeavx.ko
depmod -a
rm -f /etc/systemd/system/fakeavx.service /etc/systemd/system/fakeavx-save.service \
	/etc/systemd/system/fakeavx-save.timer /etc/modules-load.d/fakeavx.conf \
	/usr/local/bin/fakeavx-cpuid /usr/local/bin/fakeavx-census
systemctl daemon-reload
if [ "$1" = "--purge" ]; then
	rm -rf /var/cache/fakeavx /etc/modprobe.d/fakeavx.conf
fi
echo "fakeavx removed"
