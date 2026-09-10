# PathHide kernel module - standalone build
# Single file (pathhide.c) compiles to pathhide.ko

obj-m += pathhide.o

# Kernel source directory. Override with KDIR= or KERNELDIR=
KDIR ?= /lib/modules/$(shell uname -r)/build
KERNELDIR ?= $(KDIR)

PWD := $(shell pwd)

# Android GKI cross-compile: make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/gki-kernel
ARCH ?= $(shell uname -m)
CROSS_COMPILE ?=

ccflags-y += -Wno-unused-function -Wno-declaration-after-statement

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

# Deploy compiled ko into ksud embed directory (ksud embeds bin/aarch64/ksu.ko)
KSUD_DIR ?= $(abspath $(PWD)/../../userspace/ksud)
deploy: all
	mkdir -p $(KSUD_DIR)/bin/aarch64
	cp pathhide.ko $(KSUD_DIR)/bin/aarch64/ksu.ko
	@echo "Deployed pathhide.ko -> $(KSUD_DIR)/bin/aarch64/ksu.ko"

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: all clean deploy
