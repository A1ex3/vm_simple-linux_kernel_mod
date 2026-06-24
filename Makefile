obj-m += vm_simple.o

vm_simple-y := kernel_vm_s.o jit_compiler_x86_64.o vm_functions.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean