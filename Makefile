# ---------------------------------------------------------------------------
# Smart Meter - top level build
#
#   make            build the user-space application into build/app
#   make driver     build the kernel module (needs kernel headers)
#   make all        build both
#   make load       insmod the driver
#   make unload     rmmod the driver
#   make run        build and run the dashboard
#   make demo       run the 30 second non-interactive demo (no root needed)
#   make ipc-client build the POSIX IPC helper
#   make test       build and run the self test
#   make clean      remove build artefacts
# ---------------------------------------------------------------------------

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDLIBS   += -pthread -lrt

BUILD_DIR := build
BIN       := $(BUILD_DIR)/app
IPC_BIN   := $(BUILD_DIR)/sm-ipc-client
TEST_BIN  := $(BUILD_DIR)/selftest

APP_SRCS  := $(wildcard user_app/*.cpp)
APP_OBJS  := $(patsubst user_app/%.cpp,$(BUILD_DIR)/%.o,$(APP_SRCS))
APP_DEPS  := $(APP_OBJS:.o=.d)

# Everything except main(), so the self test can link the same objects.
LIB_OBJS  := $(filter-out $(BUILD_DIR)/main.o,$(APP_OBJS))

# Kernel module knobs: override on the command line.
PULSES_PER_HOUR ?= 3600
GPIO_PIN        ?= 0
SIMULATE        ?= 1

.PHONY: all app driver run demo load unload status clean help test ipc-client \
        format-check distclean

all: app ipc-client

app: $(BIN)

$(BIN): $(APP_OBJS) | $(BUILD_DIR)
	$(CXX) $(APP_OBJS) -o $@ $(LDLIBS)

$(BUILD_DIR)/%.o: user_app/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread -MMD -MP -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

ipc-client: $(IPC_BIN)

$(IPC_BIN): ipc/sm_ipc_client.c $(BUILD_DIR)/ipc_channel.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra $< \
		$(BUILD_DIR)/ipc_channel.o -o $@ -lrt

test: $(TEST_BIN)

$(TEST_BIN): tests/selftest.cpp $(LIB_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread $< $(LIB_OBJS) -o $@ $(LDLIBS)

# --- kernel module ---------------------------------------------------------

driver:
	$(MAKE) -C driver

load: driver
	sudo insmod driver/smart_meter_driver.ko simulate=$(SIMULATE) \
		pulses_per_hour=$(PULSES_PER_HOUR) gpio_pin=$(GPIO_PIN)
	sleep 1
	ls -l /dev/smart_meter

unload:
	sudo rmmod smart_meter_driver

status:
	@$(MAKE) -C driver status

# --- running ---------------------------------------------------------------

run: app
	./$(BIN) --source auto

demo: app
	./$(BIN) --demo

# --- housekeeping ----------------------------------------------------------

clean:
	@rm -rf $(BUILD_DIR)
	@if [ -d "$(KDIR)" ]; then $(MAKE) -C driver clean; else \
		echo "skipping driver clean: $(KDIR) not present"; fi

distclean: clean
	@rm -rf logs/*.csv exports

help:
	@echo "targets: all app driver load unload run demo test ipc-client clean"
	@echo "knobs  : PULSES_PER_HOUR=$(PULSES_PER_HOUR) GPIO_PIN=$(GPIO_PIN) SIMULATE=$(SIMULATE)"

-include $(APP_DEPS)
