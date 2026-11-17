CC ?= gcc
CFLAGS := -O1 -g -Wall -Wno-unused-function
CORE := ../core/decode.c ../core/translate.c ../core/optable.c ../core/intdec.c ../core/region.c

all: difftest
../helpers/blob.h: ../helpers/helper.c ../helpers/entry.S ../core/fakeavx.h
	$(MAKE) -C ../helpers
difftest: difftest.c run.S $(CORE) ../core/fakeavx.h ../helpers/blob.h
	$(CC) $(CFLAGS) -o $@ difftest.c run.S $(CORE)
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
clean:
	rm -f difftest fuzz regtest
