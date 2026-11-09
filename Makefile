obj-m := gpu_rebar_layout.o

KERNELRELEASE ?= $(shell uname -r)
KERNEL_BUILD_DIR ?= /lib/modules/$(KERNELRELEASE)/build

all:
	$(MAKE) -C $(KERNEL_BUILD_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_BUILD_DIR) M=$(CURDIR) clean
