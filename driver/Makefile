# ---------------------------------------------------------------------------
# Kernel module build.
#
#   make                 build smart_meter_driver.ko against the running kernel
#   make KDIR=/lib/modules/5.15.0-generic/build
#                        cross-build against a specific kernel tree
#   make install         install into /lib/modules/$(uname -r)/extra
#   make load            insmod + create /dev/smart_meter (needs root)
#   make unload          rmmod
#   make status          dmesg tail + show the sysfs/procfs counters
#   make clean
# ---------------------------------------------------------------------------

obj-m += smart_meter_driver.o

# Kernel build tree.  Override on the command line for cross compilation.
KDIR ?= /lib/modules/$(shell uname -r)/build

# Our shared ABI header lives one level up.
EXTRA_CFLAGS += -I$(src)/../include

# Uncomment to see every command kbuild runs.
# V = 1

.PHONY: all modules clean install uninstall load unload status check

all: modules

modules clean install uninstall:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

# Insmod the freshly built module and create the device node explicitly, so
# the sequence works even on a system where udev has not run yet.
load: modules
	@echo "insmod smart_meter_driver.ko"
	@sudo insmod smart_meter_driver.ko $(MODULE_ARGS)
	@sudo chmod 0666 /dev/smart_meter 2>/dev/null || true
	@ls -l /dev/smart_meter

unload:
	@sudo rmmod smart_meter_driver
	@echo "module removed"

status:
	@echo "--- dmesg ---"
	@dmesg | tail -n 20
	@echo "--- /proc/smart_meter ---"
	@cat /proc/smart_meter 2>/dev/null || echo "(module not loaded)"
	@echo "--- sysfs ---"
	@for f in /sys/kernel/smart_meter/*; do printf '%s = ' "$$f"; cat "$$f"; done 2>/dev/null

check:
	@test -c /dev/smart_meter && echo "OK: /dev/smart_meter exists" || \
		{ echo "FAIL: /dev/smart_meter missing - is the module loaded?"; exit 1; }
