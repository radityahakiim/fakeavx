#!/bin/bash
# Compare the helper paths of the working tree with another build (default
# HEAD): the old build's translator + helper blob are the oracle. Every
# helper-path instruction in hb_insns.S runs on N random inputs (default
# 2000) in both builds, as for Penryn and as for an SSSE3-only host; the
# complete state afterwards (16 ymm, gprs, flags incl. DF/OF, MXCSR, 512
# bytes of memory) must be identical. Runs on any x86-64 CPU, no module
# needed.
#
#   tests/helper_oracle.sh [REF] [N]
set -eu
ref=${1:-HEAD}
n=${2:-2000}
top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

make -s -C "$top/tests" helperbench
git -C "$top" archive "$ref" | tar -x -C "$work"
# the same harness, built against the reference translator and blob
cp "$top/tests/helperbench.c" "$top/tests/hb_run.S" "$top/tests/hb_insns.S" "$work/tests/"
make -s -C "$work/helpers"
(cd "$work/tests" && ${CC:-cc} -O2 -w -march=core2 -o helperbench helperbench.c hb_run.S hb_insns.S \
	$(ls ../core/*.c))
rc=0
# dump: each instruction translated alone; rdump: the loops of hb_loops,
# each built as a region (borrowed registers, exits) and run 1-50 times;
# sdump: pcmpXstrX with every control byte, operand form and W (3072
# combinations x 40 inputs); gdump: gathers, every form and corner case
# (2048 combinations x 10 inputs)
for what in dump rdump sdump gdump; do
	case $what in sdump) k=40 ;; gdump) k=10 ;; *) k=$n ;; esac
	for mode in penryn ssse3; do
		if [ $mode = ssse3 ]; then e="HB_NOSSE41=1"; else e="HB_PENRYN=1"; fi
		tests=$(env $e "$top/tests/helperbench" $what "" 1 | wc -l)
		if cmp -s <(env $e "$work/tests/helperbench" $what "" "$k") \
			  <(env $e "$top/tests/helperbench" $what "" "$k") 2>/dev/null; then
			echo "$what, $mode host: identical to $ref ($tests x $k inputs)"
		else
			echo "$what, $mode host: DIFFERENT from $ref"
			diff <(env $e "$work/tests/helperbench" $what "" "$k") \
			     <(env $e "$top/tests/helperbench" $what "" "$k") |
				grep '^[<>]' | cut -d' ' -f1-3 | head -10
			rc=1
		fi
	done
done
exit $rc
