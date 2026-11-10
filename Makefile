CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS+= -Icore -MMD -MP
LDFLAGS ?=

ifeq ($(OS),Windows_NT)
CORE_SRC := $(filter-out core/sh_fs_posix.c,$(wildcard core/*.c))
else
CORE_SRC := $(filter-out core/sh_fs_win32.c,$(wildcard core/*.c))
endif
CLI_SRC  := $(wildcard cli/*.c)
CORE_OBJ := $(CORE_SRC:.c=.o)
CLI_OBJ  := $(CLI_SRC:.c=.o)
BIN      := build/skillctl

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
LDFLAGS += -Wl,-rpath,@executable_path/../lib
endif

.PHONY: all clean test asan e2e asan-smoke gui app cross-win

e2e: $(BIN)
	./tests/e2e.sh

asan-smoke: asan
	SKILLCTL_HOME=/tmp/skillctl-asan-gate ./build/skillctl_asan doctor
	@SKILLCTL_HOME=/tmp/skillctl-asan-gate ./build/skillctl_asan status --json >/dev/null
	@echo "asan smoke ok"

all: $(BIN)

$(BIN): $(CORE_OBJ) $(CLI_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Header dependencies, so a changed struct never leaves a stale object behind.
-include $(CORE_OBJ:.o=.d) $(CLI_OBJ:.o=.d)

test: build/test_core
	./build/test_core

build/test_core: tests/test_core.c $(CORE_OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_core.c $(CORE_OBJ) $(LDFLAGS)

asan:
	@mkdir -p build
	$(CC) -std=c99 -g -O1 -fsanitize=address,undefined -Icore -o build/skillctl_asan \
		$(CLI_SRC) $(CORE_SRC)

gui:
	cd app && cargo build --release

# Cross-compile the engine for Windows x86_64 (requires mingw-w64).
cross-win:
	@mkdir -p build-win
	x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Icore \
		-o build-win/skillctl.exe \
		$(filter-out core/sh_fs_posix.c,$(CORE_SRC)) $(CLI_SRC)
	@echo "built build-win/skillctl.exe"

# Build the StarTide desktop app (Tauri shell over the C engine).
app:
	cd app && cargo build --release
	@echo "built app/target/release/startide (run it next to the skillctl build)"

clean:
	rm -rf build
	rm -f $(CORE_OBJ) $(CLI_OBJ) $(CORE_OBJ:.o=.d) $(CLI_OBJ:.o=.d)
