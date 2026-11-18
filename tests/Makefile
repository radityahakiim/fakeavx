CC ?= gcc
CFLAGS := -O1 -g -Wall -Wno-unused-function
CORE := ../core/decode.c ../core/translate.c ../core/optable.c ../core/intdec.c ../core/region.c \
	../core/ssedec.c ../core/lendec.c ../core/divert.c

all: difftest
../helpers/blob.h: ../helpers/helper.c ../helpers/entry.S ../core/fakeavx.h
	$(MAKE) -C ../helpers
difftest: difftest.c run.S $(CORE) ../core/fakeavx.h ../helpers/blob.h
	$(CC) $(CFLAGS) -o $@ difftest.c run.S $(CORE)
# helper paths in an ordinary process (no module): helperbench time | dump
helperbench: helperbench.c hb_run.S hb_insns.S $(CORE) ../core/fakeavx.h ../helpers/blob.h
	$(CC) -O2 -g -Wall -Wno-unused-function -march=core2 -o $@ helperbench.c hb_run.S hb_insns.S $(CORE)
# same results as another build (default HEAD)? make helper-oracle [REF=...]
helper-oracle: helperbench
	./helper_oracle.sh $(or $(REF),HEAD)
fuzz: fuzz.c $(CORE) ../core/fakeavx.h
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ fuzz.c $(CORE)
regtest: regtest.c regsync.S $(CORE) ../core/fakeavx.h ../helpers/blob.h
	$(MAKE) -C guest ref.o avx.o
	$(CC) $(CFLAGS) -D__ASSEMBLY__ -c -o regsync.o regsync.S
	$(CC) $(CFLAGS) -no-pie -o $@ regtest.c regsync.o $(CORE) guest/ref.o guest/avx.o -lm
check: difftest fuzz regtest
	./regtest
	./difftest 300
	FA_FAR=1 ./difftest 100
	FA_NOSSE41=1 ./difftest 100
	./fuzz 3000000
# fa_find_divert() against objdump, on any x86-64 executable: make divert-check BIN=...
divert_check: divert_check.c $(CORE) ../core/fakeavx.h
	$(CC) $(CFLAGS) -o $@ divert_check.c $(CORE)
divert-check: divert_check
	objdump -d --insn-width=16 -j .text $(BIN) | awk -f divert_truth.awk > divert_truth.txt
	./divert_check $(BIN) divert_truth.txt
clean:
	rm -f difftest fuzz regtest divert_check divert_truth.txt helperbench
