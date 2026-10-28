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

COMMON_OBJS  := $(BUILD)/common/log.o
SUP_OBJS     := $(BUILD)/supervisor/backoff.o $(BUILD)/supervisor/child.o
BINS         := $(BUILD)/supervisor/supervisor $(BUILD)/stub/fake_child
UNIT_TESTS   := $(BUILD)/tests/test_log $(BUILD)/tests/test_backoff
SCRIPT_TESTS := tests/test_fake_child.sh tests/test_supervisor.sh
SUP_RUNNER   ?=

.PHONY: all test asan memcheck picheck clean

all: $(BINS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/supervisor/supervisor: $(BUILD)/supervisor/supervisor.o $(SUP_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/stub/fake_child: $(BUILD)/stub/fake_child.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/tests/%: tests/%.c $(COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/tests/test_backoff: $(SUP_OBJS)

test: $(UNIT_TESTS) $(BINS)
	@for t in $(UNIT_TESTS); do echo "== $$t"; $(RUNNER) ./$$t || exit 1; done
	@for s in $(SCRIPT_TESTS); do echo "== $$s"; SUP_RUNNER="$(SUP_RUNNER)" sh $$s $(BUILD) || exit 1; done

asan:
	$(MAKE) BUILD=build-asan \
	    EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
	    EXTRA_LDFLAGS="-fsanitize=address,undefined" test

memcheck:
	$(MAKE) BUILD=build RUNNER="sh tests/valgrind.sh" SUP_RUNNER="sh tests/valgrind.sh" test

# Build and test on the Pi's own compiler: make picheck PI=user@host
picheck:
	@test -n "$(PI)" || { echo "usage: make picheck PI=user@host"; exit 1; }
	rsync -a --delete --exclude=build/ --exclude=build-asan/ --exclude=.git/ \
	    --exclude='milestone transcripts/' ./ $(PI):orchid-picheck/
	ssh $(PI) 'cd orchid-picheck && make clean test asan'

clean:
	rm -rf build build-asan
