# fakeavx-prof: per-instruction profiler and verifier (see fakeavx-prof -h)
#
# The C part is built for plain Core 2 so that nothing but the test code
# uses the instructions being measured.
CC ?= gcc
CFLAGS ?= -O2 -g -Wall
ARCH := -march=core2 -mtune=generic -mno-avx
CORE := ../../core/decode.c ../../core/translate.c ../../core/optable.c \
	../../core/intdec.c ../../core/region.c ../../core/lendec.c

all: fakeavx-prof

# prof.o first: the test code then sits before the C code in .text
fakeavx-prof: prof.S fakeavx-prof.c prof_tests.h prof.h $(CORE) ../../core/fakeavx.h
	$(CC) $(CFLAGS) $(ARCH) -o $@ prof.S fakeavx-prof.c $(CORE) -lm

clean:
	rm -f fakeavx-prof

.PHONY: all clean
