#!/bin/bash
# Compile-test the fakeavx kernel module against other kernel versions.
#
#   tests/kernel-compat.sh [VERSION...]     e.g. 6.1.192 6.12.118 7.3.5 7.4-rc3
#
# For each version: download the source from kernel.org (cached), configure
# x86_64_defconfig plus what the module needs (KPROBES, UPROBES, KVM for
# MMU_NOTIFIER, KALLSYMS), `make modules_prepare`, then build kmod/ against
# it. Only compiles: it says nothing about running.
#
# Work directory: $FAKEAVX_KTEST (default ~/.cache/fakeavx-ktest), about
# 1.5-2 GB per version (archives are deleted once extracted unless
# FAKEAVX_KTEST_KEEP_TARBALLS is set); `tests/kernel-compat.sh --clean` removes it.
# Prints one line per version and exits 1 if any failed; full logs are in
# the work directory.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
work=${FAKEAVX_KTEST:-$HOME/.cache/fakeavx-ktest}

if [ "${1:-}" = --clean ]; then
	rm -rf -- "$work"
	exit 0
fi
[ $# -gt 0 ] || set -- 6.1.192 6.6.162 6.12.118 6.18.64 7.2.15 7.3.5 7.4-rc3
mkdir -p "$work" || exit 2
jobs=$(nproc)

url_of() {
	case "$1" in
	*-rc*) echo "https://git.kernel.org/torvalds/t/linux-$1.tar.gz" ;;
	*) echo "https://cdn.kernel.org/pub/linux/kernel/v${1%%.*}.x/linux-$1.tar.xz" ;;
	esac
}

prepare() {	# version -> prepared tree in $work/linux-$v
	local v=$1 src="$work/linux-$v" url tarball
	[ -f "$src/.fakeavx-prepared" ] && return 0
	url=$(url_of "$v")
	tarball="$work/$(basename "$url")"
	if [ ! -s "$tarball" ]; then
		curl -sSfL -o "$tarball.part" "$url" && mv -- "$tarball.part" "$tarball" || return 1
	fi
	rm -rf -- "$src"
	if ! tar -xf "$tarball" -C "$work"; then
		# a truncated or damaged download (an interrupted earlier run): fetch it once more
		rm -rf -- "$src" "$tarball"
		curl -sSfL -o "$tarball.part" "$url" && mv -- "$tarball.part" "$tarball" || return 1
		tar -xf "$tarball" -C "$work" || return 1
	fi
	[ -d "$src" ] || return 1
	[ -n "${FAKEAVX_KTEST_KEEP_TARBALLS:-}" ] || rm -f -- "$tarball"	# extracted: not needed any more
	(
		cd "$src" &&
		make -s x86_64_defconfig &&
		scripts/config --enable KPROBES --enable UPROBES --enable UPROBE_EVENTS \
			--enable KALLSYMS --enable KVM --enable KVM_INTEL --enable MODULES \
			--disable WERROR &&
		make -s olddefconfig &&
		make -s -j"$jobs" modules_prepare
	) || return 1
	touch "$src/.fakeavx-prepared"
}

build() {	# version -> build kmod/ in a copy of the tree's sources
	local v=$1 src="$work/linux-$v" out="$work/build-$v"
	rm -rf -- "$out"
	mkdir -p "$out" && cp -r "$top/core" "$top/helpers" "$top/kmod" "$out/" || return 1
	rm -f -- "$out"/kmod/*.o "$out"/kmod/*.ko "$out"/kmod/.*.cmd
	make -s -C "$out/helpers" || return 1
	# modules_prepare leaves no Module.symvers: undefined-symbol checks are modpost warnings
	make -C "$out/kmod" KDIR="$src" KBUILD_MODPOST_WARN=1
}

rc=0
for v in "$@"; do
	log="$work/$v.log"
	printf '%-12s ' "$v"
	if ! prepare "$v" >"$log" 2>&1; then
		echo "SETUP FAILED (download or modules_prepare; see $log)"
		rc=1
		continue
	fi
	if build "$v" >>"$log" 2>&1 && [ -f "$work/build-$v/kmod/fakeavx.ko" ]; then
		echo "OK    fakeavx.ko built ($(grep -E 'warning:' "$log" | grep -cE '/(kmod|core|helpers)/') warnings in fakeavx's own files)"
	else
		echo "FAILED"
		grep -E 'error:|undefined' "$log" | sed 's/^/             /' | head -8
		rc=1
	fi
done
exit $rc
