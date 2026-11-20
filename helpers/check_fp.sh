#!/bin/sh
# Build check for the helper blob: helper_entry no longer saves and restores
# MXCSR, so floating-point instructions (which set MXCSR's exception flags)
# may only be in the functions that do it themselves (helper.c: fp_cmp and
# fp_sse41 around do_cmp and do_sse41), and those may only be called from
# there. Fails if a code change or another compiler breaks that.
#
#   check_fp.sh blob.elf [allowed functions] [allowed callers]
elf=$1
allowed=${2:-"do_cmp do_sse41 round_f round_d"}
callers=${3:-"fa_helper_c fp_cmp fp_sse41 do_sse41"}

objdump -d --no-show-raw-insn "$elf" | awk -v allowed="$allowed" -v callers="$callers" '
BEGIN {
	n = split(allowed, a, " "); for (i = 1; i <= n; i++) ok[a[i]] = 1
	n = split(callers, c, " "); for (i = 1; i <= n; i++) caller[c[i]] = 1
	bad = 0
}
/^[0-9a-f]+ <.*>:$/ { f = $2; gsub(/[<>:]/, "", f); next }
# instructions that can set MXCSR exception flags
/\t(add|sub|mul|div|sqrt|min|max|hadd|hsub|addsub|dp|round|cmp[a-z]*)(ss|sd|ps|pd)[ \t]|\tu?comis[sd][ \t]|\tcvt[a-z0-9]*[ \t]/ {
	if (!(f in ok)) { print "check_fp: floating point outside the MXCSR-saving helpers, in " f ": " $0; bad = 1 }
}
/\t(call|jmp)[a-z]*[ \t].*<[^>+]*>$/ {
	t = $NF; sub(/^</, "", t); sub(/>$/, "", t)
	if ((t in ok) && !(f in caller)) { print "check_fp: " t " reached from " f; bad = 1 }
}
END { exit bad }'
