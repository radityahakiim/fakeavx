# fakeavx - top-level Makefile
#
#   make                build the helper blob and the kernel module
#   make KDIR=/path     ... against a specific kernel build tree
#   make load / unload  insmod / rmmod the freshly built module (not persistent)
#   make install        permanent installation (scripts/install.sh, needs root)
#   make uninstall      remove it again
#   make guest          build the test programs for the AVX-less machine
#   make prof           build tools/prof/fakeavx-prof, the per-instruction profiler
#   make check          core tests; needs a machine with real AVX2
#   make selftest       build and run every test this machine can run, write a
#                       report to reports/ (tests/selftest.sh; QUICK=1 for ~5 min)
#   make kernel-compat  compile the module against kernels 6.1 ... 7.4 from
#                       kernel.org (tests/kernel-compat.sh; downloads ~1 GB)
#   make vmtest         end-to-end test in QEMU; needs KERNEL=bzImage built
#                       from the tree the module was built against (KDIR)
#   make clean

KDIR ?= /lib/modules/$(shell uname -r)/build
SUDO ?= sudo

.PHONY: all module helpers load unload install uninstall guest prof check vmtest clean help

all: module

helpers:
	$(MAKE) -C helpers

module: helpers
	$(MAKE) -C kmod KDIR=$(KDIR)

load: module
	-$(SUDO) rmmod fakeavx 2>/dev/null
	$(SUDO) insmod kmod/fakeavx.ko

unload:
	$(SUDO) rmmod fakeavx

install:
	$(SUDO) scripts/install.sh

uninstall:
	$(SUDO) scripts/uninstall.sh

guest:
	$(MAKE) -C tests/guest

prof:
	$(MAKE) -C tools/prof

check: helpers
	$(MAKE) -C tests check

selftest:
	tests/selftest.sh $(if $(QUICK),--quick)

kernel-compat:
	tests/kernel-compat.sh

vmtest: module guest
	tests/qemu/run.sh

clean:
	-$(MAKE) -C kmod KDIR=$(KDIR) clean
	$(MAKE) -C helpers clean
	$(MAKE) -C tests clean
	$(MAKE) -C tests/guest clean
	$(MAKE) -C tools/prof clean

help:
	@sed -n '3,17p' Makefile | sed 's/^# \{0,1\}//'
