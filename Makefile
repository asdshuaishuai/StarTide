CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS+= -Icore -Icli -MMD -MP
LDFLAGS ?=

# ONNX Runtime powers the local vector path (one .onnx file, no Python).
# When it is missing the engine still builds: sh_onnx.h compiles to stubs
# and find/embed degrade to the lexical lists.
ORT_PREFIX ?= $(shell brew --prefix onnxruntime 2>/dev/null)
ifneq ($(wildcard $(ORT_PREFIX)/include/onnxruntime/onnxruntime_c_api.h),)
ORT_CPPFLAGS := -I$(ORT_PREFIX)/include
ORT_LDFLAGS  := -L$(ORT_PREFIX)/lib -lonnxruntime
else
ORT_CPPFLAGS := -DSKILLCTL_NO_ONNX
ORT_LDFLAGS  :=
endif
CPPFLAGS += $(ORT_CPPFLAGS)
LDFLAGS  += $(ORT_LDFLAGS)

# llama.cpp is the GGUF runtime: it brings the byte-level BPE tokeniser and
# last-token pooling that decoder embedding models (Qwen3/F2LLM) need, so no
# tokeniser has to be written here. Absent, the header compiles to stubs.
LLAMA_PREFIX ?= $(shell brew --prefix llama.cpp 2>/dev/null)
GGML_PREFIX  ?= $(shell brew --prefix ggml 2>/dev/null)
ifneq ($(wildcard $(LLAMA_PREFIX)/include/llama.h),)
LLAMA_CPPFLAGS := -I$(LLAMA_PREFIX)/include -I$(GGML_PREFIX)/include
LLAMA_LDFLAGS  := -L$(LLAMA_PREFIX)/lib -lllama -L$(GGML_PREFIX)/lib -lggml -lggml-base
else
LLAMA_CPPFLAGS := -DSKILLCTL_NO_LLAMA
LLAMA_LDFLAGS  :=
endif
CPPFLAGS += $(LLAMA_CPPFLAGS)
LDFLAGS  += $(LLAMA_LDFLAGS)

ifeq ($(OS),Windows_NT)
CORE_SRC := $(filter-out core/sh_fs_posix.c,$(wildcard core/*.c))
else
CORE_SRC := $(filter-out core/sh_fs_win32.c,$(wildcard core/*.c))
endif
CLI_SRC  := $(wildcard cli/*.c)
CORE_OBJ := $(CORE_SRC:.c=.o)
CLI_OBJ  := $(CLI_SRC:.c=.o)
BIN      := build/skillctl
APP      := StarTide.app

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

# ggml's headers repeat typedefs (C11); -Wall flags it under -std=c99.
core/sh_llama.o: CFLAGS += -Wno-typedef-redefinition

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
	$(CC) -std=c99 -g -O1 -fsanitize=address,undefined -Icore -Icli $(ORT_CPPFLAGS) $(LLAMA_CPPFLAGS) -Wno-typedef-redefinition \
		-o build/skillctl_asan $(CLI_SRC) $(CORE_SRC) $(ORT_LDFLAGS) $(LLAMA_LDFLAGS)

gui:
	cd gui && cargo build --release

# Cross-compile the engine for Windows x86_64 (requires mingw-w64).
cross-win:
	@mkdir -p build-win
	x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Icore \
		-o build-win/skillctl.exe \
		$(filter-out core/sh_fs_posix.c,$(CORE_SRC)) $(CLI_SRC)
	@echo "built build-win/skillctl.exe"

# Build the StarTide desktop app (gpui-kit shell over the C engine).
# Assembles the bundle in the repo root: StarTide.app.
app:
	cd gui && cargo build --release
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	@cp gui/target/release/startide $(APP)/Contents/MacOS/startide
	@cp gui/resources/Info.plist $(APP)/Contents/Info.plist
	@cp gui/resources/AppIcon.icns $(APP)/Contents/Resources/AppIcon.icns
	@echo "built ./$(APP): Contents/MacOS/startide, Contents/Info.plist, Contents/Resources/AppIcon.icns"

# Install locally: the bundle to /Applications, the CLI engine onto PATH.
# BIN_DIR is auto-detected (first user-writable dir in PATH); override with
# `make install BIN_DIR=...`.
BIN_DIR ?= $(shell for d in $(subst :, , $(PATH)); do [ -d "$$d" ] && [ -w "$$d" ] && echo "$$d" && break; done)
install: app $(BIN)
	@rm -rf /Applications/$(APP)
	cp -R $(APP) /Applications/$(APP)
	install -m 755 $(BIN) $(BIN_DIR)/skillctl
	@codesign --force --deep -s - /Applications/$(APP) 2>/dev/null || true
	@echo "installed: /Applications/$(APP)"
	@echo "installed: $(BIN_DIR)/skillctl  (BIN_DIR=$(BIN_DIR))"

uninstall:
	@rm -rf /Applications/$(APP)
	@rm -f $(BIN_DIR)/skillctl
	@echo "uninstalled"

clean:
	rm -rf build
	rm -rf $(APP)
	rm -f $(CORE_OBJ) $(CLI_OBJ) $(CORE_OBJ:.o=.d) $(CLI_OBJ:.o=.d)
