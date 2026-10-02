CC      ?= cc

BUILD   ?= build
OPT     ?= -O2
SAN     ?=

ifeq ($(SANITIZE),1)
  override BUILD := $(BUILD)-san
  OPT   := -O1
  SAN   := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
endif

WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
           -Wpointer-arith -Wwrite-strings -Wvla -Werror
CFLAGS  += -std=c11 $(OPT) -g $(WARN) -fno-strict-aliasing -D_GNU_SOURCE \
           -Iinclude -Isrc -MMD -MP $(SAN)
LDFLAGS += $(SAN)
LDLIBS  += -lm -lpthread

CORE_SRCS := $(wildcard src/core/*.c src/net/*.c src/tcp/*.c src/apps/*.c) src/stack.c
DAEMON_SRCS := src/main.c src/netif/tun.c
UNIT_SRCS := $(wildcard tests/unit/*.c)
SIM_SRCS  := tests/sim/sim.c tests/sim/simnet.c
TOOL_SRCS := tools/genbytes.c

obj = $(patsubst %.c,$(BUILD)/%.o,$(1))

CORE_OBJS := $(call obj,$(CORE_SRCS))

UNAME := $(shell uname -s)

.PHONY: all test unit sim clean fuzz

all: $(BUILD)/test_unit $(BUILD)/sim $(BUILD)/genbytes
ifeq ($(UNAME),Linux)
all: $(BUILD)/ustack
endif

$(BUILD)/ustack: $(CORE_OBJS) $(call obj,$(DAEMON_SRCS))
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_unit: $(CORE_OBJS) $(call obj,$(UNIT_SRCS) tests/sim/simnet.c)
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/sim: $(CORE_OBJS) $(call obj,$(SIM_SRCS))
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/genbytes: $(call obj,$(TOOL_SRCS) src/core/pattern.c)
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -Itests -c -o $@ $<

unit: $(BUILD)/test_unit
	./$(BUILD)/test_unit

sim: $(BUILD)/sim
	./$(BUILD)/sim --quick

test: unit sim

FUZZ_CC ?= clang
fuzz: $(CORE_SRCS) tests/fuzz/fuzz_stack.c tests/sim/simnet.c
	@mkdir -p build-fuzz
	$(FUZZ_CC) -std=c11 -O1 -g -D_GNU_SOURCE -fno-strict-aliasing -Iinclude -Isrc -Itests \
	    -fsanitize=fuzzer,address,undefined -o build-fuzz/fuzz_stack $^ -lm

clean:
	rm -rf build build-* 

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
