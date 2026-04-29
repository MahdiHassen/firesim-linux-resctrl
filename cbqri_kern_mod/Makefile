# Out-of-tree build of cbqri_bandwidth_driver against the kernel tree
# shipped in this repo. Override KDIR if you want to build against a
# different kernel.

REPO_ROOT     := $(abspath $(CURDIR)/..)
KDIR          ?= $(REPO_ROOT)/kernel
ARCH          ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

obj-m := cbqri_bandwidth_driver.o

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(CURDIR) clean
