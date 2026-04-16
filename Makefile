KDIR    := /lib/modules/$(shell uname -r)/build
CC      := gcc
CFLAGS  := -Wall -Wextra -g -D_GNU_SOURCE -I.

USER_TARGETS := engine workload_cpu workload_io

obj-m := monitor.o

.PHONY: all user kernel clean

all: user kernel

user: $(USER_TARGETS)

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ engine.c -lpthread

workload_cpu: workload_cpu.c
	$(CC) $(CFLAGS) -o $@ $

workload_io: workload_io.c
	$(CC) $(CFLAGS) -o $@ $

kernel:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm -f $(USER_TARGETS)
	$(MAKE) -C $(KDIR) M=$(PWD) clean
