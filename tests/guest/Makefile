CC ?= gcc
all: guest_test cpuid_test unpatched_test
unpatched_test: unpatched_test.c
	$(CC) -O2 -fPIE -pie -o $@ unpatched_test.c
cpuid_test: cpuid_test.c
	$(CC) -O2 -fPIE -msse4.2 -mpopcnt -static-pie -o $@ cpuid_test.c
ref.o: kern.c kern.h
	$(CC) -O2 -fPIE -mno-avx -DPFX=ref -c -o $@ kern.c
avx.o: kern.c kern.h
	$(CC) -O3 -fPIE -mavx2 -mpopcnt -DPFX=avx -DUSE_AVX -c -o $@ kern.c
guest_test: guest_test.c ref.o avx.o kern.h
	$(CC) -O2 -fPIE -mavx2 -mpopcnt -static-pie -o $@ guest_test.c ref.o avx.o -lpthread -lm
clean:
	rm -f *.o guest_test cpuid_test unpatched_test
