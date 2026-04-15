obj-m += src/monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Kernel build: expose include/ for monitor_ioctl.h
ccflags-y := -I$(PWD)/include

# If you want host-built workload binaries to run directly inside an Alpine
# rootfs, you can override this with WORKLOAD_LDFLAGS=-static when your
# toolchain supports it.
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine workloads/cpu_hog workloads/memory_hog workloads/io_pulse

all: $(USER_TARGETS) module

ci: WORKLOAD_LDFLAGS =
ci: $(USER_TARGETS)

module: monitor.ko

engine: src/engine.c include/monitor_ioctl.h
	gcc -O2 -Wall -Wextra -Iinclude -o engine src/engine.c -lpthread

workloads/cpu_hog: workloads/cpu_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o workloads/cpu_hog workloads/cpu_hog.c

workloads/memory_hog: workloads/memory_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o workloads/memory_hog workloads/memory_hog.c

workloads/io_pulse: workloads/io_pulse.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o workloads/io_pulse workloads/io_pulse.c

monitor.ko: src/monitor.c include/monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f engine workloads/cpu_hog workloads/memory_hog workloads/io_pulse
	rm -f src/*.o src/*.mod src/*.mod.c src/*.symvers src/*.order
	rm -f *.o *.mod *.mod.c *.symvers *.order
	rm -f *.log
	rm -rf logs
	rm -f /tmp/mini_runtime.sock

.PHONY: all ci module clean
