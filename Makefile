obj-m = labjack.o
KVERSION = $(shell uname -r)

all: 
	make -C /lib/modules/$(KVERSION)/build M=$(shell pwd) modules
clean:
	make -C /lib/modules/$(KVERSION)/build M=$(shell pwd) clean
	rm testb
	rm testc
tests:
	gcc -o testb testb.c
	gcc -o testc testc.c