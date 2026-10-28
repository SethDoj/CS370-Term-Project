# Orchid Sitter build. Run on Linux (WSL Fedora for development, the Pi for deploy).

# GCC < 14 (e.g. Raspberry Pi OS Bookworm's 12.x) has no -std=c23, only -std=c2x.
# Override with `make CSTD=...`. The choice is printed so a fallback is never silent;
# gcc >= 14 treats c2x as c23, so only `make picheck` proves the Pi's compiler accepts our code.
CSTD    := $(shell $(CC) -std=c23 -E -x c /dev/null >/dev/null 2>&1 && echo c23 || echo c2x)
$(info cc: $(CC) $(shell $(CC) -dumpfullversion) | std: -std=$(CSTD))
CFLAGS  := -std=$(CSTD) -D_GNU_SOURCE -Wall -Wextra -Werror -Wvla -O2 -g $(EXTRA_CFLAGS)
LDFLAGS := $(EXTRA_LDFLAGS)

BUILD   ?= build
RUNNER  ?=

COMMON_OBJS := $(addprefix $(BUILD)/common/,log.o crc.o i2c.o child.o record.o csvlog.o sampler.o)
SENSOR_OBJS := $(BUILD)/sensors/sht40.o $(BUILD)/sensors/ads1115.o
HUB_OBJS    := $(BUILD)/hub/hub_core.o
LIB_OBJS    := $(COMMON_OBJS) $(SENSOR_OBJS) $(HUB_OBJS)
BINS        := $(BUILD)/bin/hub $(BUILD)/bin/climate_sampler $(BUILD)/bin/soil_sampler
UNIT_TESTS  := $(addprefix $(BUILD)/tests/,test_log test_crc test_record test_csvlog \
               test_sht40 test_ads1115 test_hub)

.PHONY: all test asan memcheck picheck clean
.SECONDARY: # keep objects built by pattern-rule chains

all: $(BINS)

# Every object is rebuilt when any header changes: few files, no stale structs.
$(BUILD)/%.o: src/%.c $(wildcard src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/bin/hub: $(BUILD)/hub/hub.o $(HUB_OBJS) $(COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/bin/%_sampler: $(BUILD)/%_sampler.o $(SENSOR_OBJS) $(COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/tests/%: tests/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do echo "== $$t"; $(RUNNER) ./$$t || exit 1; done

asan:
	$(MAKE) BUILD=build-asan \
	    EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
	    EXTRA_LDFLAGS="-fsanitize=address,undefined" test

memcheck:
	$(MAKE) BUILD=build RUNNER="sh tests/valgrind.sh" test

# Build and test on the Pi's own compiler: make picheck PI=user@host
picheck:
	@test -n "$(PI)" || { echo "usage: make picheck PI=user@host"; exit 1; }
	rsync -a --delete --exclude=build/ --exclude=build-asan/ --exclude=.git/ \
	    --exclude='milestone transcripts/' ./ $(PI):orchid-picheck/
	ssh $(PI) 'cd orchid-picheck && make clean test asan'

clean:
	rm -rf build build-asan
