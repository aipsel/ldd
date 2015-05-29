ifneq ($(KERNELRELEASE),)
	obj-m := hello.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

	OBJS = .*.cmd .tmp_versions *.o *.ko *.ko.* *.mod.c \
		modules.order Module.symvers
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	@rm -rf $(OBJS)

endif

