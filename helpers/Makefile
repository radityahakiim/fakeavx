# Builds the user-mode helper blob and blob.h (a C array of its bytes).
CC ?= gcc
BLOB_CFLAGS := -O2 -ffunction-sections -fdata-sections -march=core2 -mno-sse4.1 -mno-avx -ffreestanding -fno-builtin \
	-fPIE -fvisibility=hidden -fno-stack-protector -fcf-protection=none \
	-fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
	-fno-jump-tables -nostdinc -isystem $(shell $(CC) -print-file-name=include) -Wall

all: blob.h

entry.o: entry.S ../core/fakeavx.h
	$(CC) $(BLOB_CFLAGS) -D__ASSEMBLY__ -c -o $@ $<
sse41.o: ../core/optable.c ../core/fakeavx.h
	$(CC) $(BLOB_CFLAGS) -c -o $@ ../core/optable.c
helper.o: helper.c ../core/fakeavx.h
	$(CC) $(BLOB_CFLAGS) -c -o $@ $<
blob.elf: entry.o helper.o sse41.o blob.lds
	ld -static -nostdlib --gc-sections -T blob.lds -o $@ entry.o helper.o sse41.o
	@if readelf -S $@ | grep -E '\.(data|bss|got)' ; then echo "blob has writable data"; rm -f $@; exit 1; fi
blob.bin: blob.elf
	objcopy -O binary -j .text $< $@
blob.h: blob.bin
	( echo "/* generated from helpers/ - do not edit */"; \
	  echo "static const unsigned char fa_blob[] = {"; \
	  od -An -v -tx1 $< | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  echo "};" ) > $@
clean:
	rm -f *.o blob.elf blob.bin blob.h
