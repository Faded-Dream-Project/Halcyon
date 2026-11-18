# Halcyon — Makefile
#
# C17, GCC 13+, Linux only. Links against an INSTALLED FDK (found via
# pkg-config; https://github.com/Faded-Dream-Project/FDK — FDK itself is
# never vendored into this repository).
#
#   make               debug build (ASan+UBSan): build/halcyon
#   make release       optimized build (-O2 -DNDEBUG)
#   make test          headless test suite (no display needed)
#   make test-display  display tier (window smoke test under Xvfb)
#   make clean
#
# Configuration is stamped in build/.config-*: switching debug <-> release
# forces a full rebuild (same discipline as FDK's build).

CC       ?= cc
BUILD_DIR := build

STD    := -std=c17
WARN   := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
          -Wmissing-prototypes -Wconversion -Wsign-conversion -Wcast-qual \
          -Wpointer-arith -Wundef -Wwrite-strings -Werror
FEATURE := -D_POSIX_C_SOURCE=200809L

DEBUG_FLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
REL_FLAGS   := -O2 -DNDEBUG

HAL_CONFIG ?= debug
CONFIG_FLAGS := $(if $(filter release,$(HAL_CONFIG)),$(REL_FLAGS),$(DEBUG_FLAGS))
CONFIG_STAMP := $(BUILD_DIR)/.config-$(HAL_CONFIG)

# ---- FDK (the only external dependency) ----

FDK_CFLAGS := $(shell pkg-config --cflags fdk 2>/dev/null)
FDK_LIBS   := $(shell pkg-config --libs fdk 2>/dev/null)
ifeq ($(strip $(FDK_LIBS)),)
$(error FDK not found via pkg-config. Build and install FDK first (github.com/Faded-Dream-Project/FDK, `make install`), and make sure PKG_CONFIG_PATH can see fdk.pc.)
endif

CFLAGS  := $(STD) $(WARN) $(FEATURE) $(CONFIG_FLAGS)
# rpath: binaries find a workspace-local FDK install without exported
# LD_LIBRARY_PATH (an /usr/local install makes the rpath a no-op).
# FDK_LIBDIR is nonempty by construction — the FDK check above already
# failed the build otherwise.
FDK_LIBDIR := $(shell pkg-config --variable=libdir fdk)
LDFLAGS := $(FDK_LIBS) -lm -Wl,-rpath,$(FDK_LIBDIR)

# ---- Sources ----
# src/platform is compiled with the FDK include path; every other module
# builds against Halcyon headers only (enforced: no other directory gets
# $(FDK_CFLAGS), so an accidental fdk include is a compile error there).

PLATFORM_SRC := $(wildcard src/platform/*.c)
OTHER_SRC    := $(filter-out src/main.c, $(filter-out $(PLATFORM_SRC), $(wildcard src/*.c src/*/*.c)))
MAIN_SRC     := src/main.c

PLATFORM_OBJ := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(PLATFORM_SRC))
OTHER_OBJ    := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(OTHER_SRC))
MAIN_OBJ     := $(BUILD_DIR)/obj/main.o

APP      := $(BUILD_DIR)/halcyon
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRC))

.PHONY: all release test test-display clean
all: $(APP)

release:
	$(MAKE) HAL_CONFIG=release all

# Config stamp: always runs, rewrites only on a real change; a switch
# invalidates every object (timestamps) and forces the full rebuild.
$(CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@cur=`cat $@ 2>/dev/null || echo none`; \
	if [ x"$$cur" != x"$(HAL_CONFIG)" ]; then \
		rm -f $(BUILD_DIR)/.config-*; \
		printf '%s\n' '$(HAL_CONFIG)' > $@; \
	fi
.PHONY: FORCE
FORCE:

# Only src/platform objects see FDK's include path (target-specific
# below): an #include "fdk/..." anywhere else is a compile error, which
# is the enforcement behind "all FDK calls live in src/platform/".

$(BUILD_DIR)/obj/%.o: src/%.c $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -Isrc $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/platform/%.o: src/platform/%.c $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -Isrc $(CFLAGS) $(FDK_CFLAGS) -MMD -MP -c $< -o $@

$(APP): $(MAIN_OBJ) $(PLATFORM_OBJ) $(OTHER_OBJ) Makefile
	$(CC) $(CONFIG_FLAGS) -o $@ $(filter %.o,$^) $(LDFLAGS)

# ---- Tests ----
# Every test links all module objects except main.o; each tests/*.c has
# its own main() and exits nonzero on any failed check.

$(BUILD_DIR)/tests/%: tests/%.c $(PLATFORM_OBJ) $(OTHER_OBJ) $(CONFIG_STAMP) Makefile
	@mkdir -p $(BUILD_DIR)/tests
	$(CC) -Isrc -Itests $(CFLAGS) $< $(PLATFORM_OBJ) $(OTHER_OBJ) -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	@fail=0; \
	for t in $(TEST_BIN); do \
		LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-fdk.supp ./$$t || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "== all headless tests passed =="; \
	else echo "== headless tests FAILED =="; exit 1; fi

# The display tier: the window smoke test under a throwaway Xvfb
# (started directly — xvfb-run needs xauth, which not every minimal
# image carries). Skips with a clear message when Xvfb is absent.
test-display: $(APP)
	@if command -v Xvfb >/dev/null 2>&1; then \
		disp=90; \
		while [ -e /tmp/.X$$disp-lock ] || [ -e /tmp/.X11-unix/X$$disp ]; do \
			disp=$$((disp + 1)); \
		done; \
		Xvfb :$$disp -screen 0 1280x800x24 >/dev/null 2>&1 & \
		xvfb_pid=$$!; \
		for i in 1 2 3 4 5 6 7 8 9 10; do \
			[ -e /tmp/.X11-unix/X$$disp ] && break; \
			sleep 0.2; \
		done; \
		DISPLAY=:$$disp LSAN_OPTIONS=suppressions=$(CURDIR)/tests/data/lsan-fdk.supp \
			$(realpath $(APP)) --smoke; \
		status=$$?; \
		kill $$xvfb_pid 2>/dev/null; \
		wait $$xvfb_pid 2>/dev/null; \
		exit $$status; \
	else \
		echo "test-display: Xvfb not found, skipping (install xvfb to run)"; \
	fi

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/obj/*.d $(BUILD_DIR)/obj/*/*.d)

# Benchmark: frame time at 10k/50k/100k triangles (docs/performance.md).
.PHONY: bench
# Clean -O2 objects (no sanitizers) so the numbers are honest.
BENCH_OBJ_DIR := build/bench-obj
bench: tests/bench.c $(shell find src -name '*.c' ! -name 'main.c')
	@mkdir -p $(BENCH_OBJ_DIR)
	@for f in $(shell find src -name '*.c' ! -name 'main.c'); do \
		o=$(BENCH_OBJ_DIR)/$$(echo $$f | sed 's|^src/||; s|/|_|g; s|\.c$$|.o|'); \
		cc -Isrc $(shell pkg-config --cflags fdk) -std=c17 -O2 -D_POSIX_C_SOURCE=200809L -c $$f -o $$o || exit 1; \
	done
	$(CC) -Isrc $(shell pkg-config --cflags fdk) -O2 tests/bench.c $(BENCH_OBJ_DIR)/*.o -o build/bench $(LDFLAGS)
	@echo "== benchmark build done (build/bench) =="
