# Build against the running kernel, or: make KDIR=/path/to/kernel/build
KDIR ?= /lib/modules/$(shell uname -r)/build

all: ../helpers/blob.h
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
../helpers/blob.h:
	$(MAKE) -C ../helpers
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
