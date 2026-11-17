# objdump -d --insn-width=16 output -> one line per short instruction that
# may be fakeavx's (VEX, or 0F B8 / 0F 38 / 0F 3A): its address, then the
# addresses of the 24 instructions before it, nearest first. See divert_check.c.
BEGIN { FS = "\t" }
/^ +[0-9a-f]+:\t/ {
	a = $1; sub(/^ +/, "", a); sub(/:$/, "", a)
	b = $2; gsub(/ +$/, "", b); n = split(b, x, " ")
	if ((n == 3 || n == 4) && (x[1] == "c4" || x[1] == "c5" || b ~ /0f (b8|38|3a)/)) {
		line = a
		for (i = 0; i < 24; i++) { j = (k - 1 - i + 48) % 24; if (r[j] != "") line = line " " r[j] }
		print line
	}
	r[k % 24] = a; k++
}
