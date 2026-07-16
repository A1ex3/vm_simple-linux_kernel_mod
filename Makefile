obj-m += vm_simple.o
vm_simple-y := $(patsubst %.c,%.o,$(wildcard *.c))

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

SIGN_KEY := /etc/pki/akmods/private/private_key.priv
SIGN_CERT := /etc/pki/akmods/certs/public_key.der

SIGN_FILE := $(KDIR)/scripts/sign-file

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@if [ -f $(SIGN_KEY) ] && [ -f $(SIGN_CERT) ]; then \
		echo "Signing module..."; \
		$(SIGN_FILE) sha256 $(SIGN_KEY) $(SIGN_CERT) vm_simple.ko; \
	else \
		echo "Warning: Signing keys not found. Module will not be signed."; \
	fi

install:
	insmod vm_simple.ko

uninstall:
	rmmod vm_simple.ko

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean