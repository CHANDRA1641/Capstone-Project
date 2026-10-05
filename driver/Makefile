# Kbuild wrapper for the Sentinel kernel module.
#   make                 build against the running kernel's headers
#   make KDIR=/path      build against another kernel tree (e.g. a QEMU guest kernel)
#   make clean
KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m := sentinel_drv.o
ccflags-y += -I$(src)/../include

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
