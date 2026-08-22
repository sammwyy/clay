CC      ?= cc
STD     := -std=gnu11
WARN    := -Wall -Wextra
OPT     := -O2
STRIP   ?= strip
STRIP_WIN ?= x86_64-w64-mingw32-strip

# CURL_LINK=dynamic (default) or =static. Static needs a real libcurl.a
# plus its own static deps (openssl, zlib, ...), which Fedora's repos
# don't fully provide for either target as of this writing.
CURL_LINK ?= dynamic
ifeq ($(CURL_LINK),static)
CURL_CFLAGS  := -DCURL_STATICLIB
CURL_LDFLAGS := -Wl,-Bstatic -lcurl -Wl,-Bdynamic
else
CURL_CFLAGS  :=
CURL_LDFLAGS := -lcurl
endif

CFLAGS  ?= $(STD) $(WARN) $(OPT) -Iinclude -MMD -MP $(CURL_CFLAGS)
LDFLAGS ?= -lpthread $(CURL_LDFLAGS)

BUILD_DIR := build
BIN_DIR   := bin
TARGET    := $(BIN_DIR)/clay

# Test files live in tests/ and have their own main().
SRC      := $(shell find src -name '*.c')
# src/sandbox/ has one file per OS - each target only builds its own.
SRC_NIX  := $(filter-out src/sandbox/win32.c,$(SRC))
SRC_WIN  := $(filter-out src/sandbox/linux.c,$(SRC))
OBJ      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC_NIX))
DEP      := $(OBJ:.o=.d)

# Size-oriented release build. It deliberately disables LTO so it remains
# portable across toolchains while still removing unreachable sections.
BUILD_DIR_RELEASE := build-release
BIN_DIR_RELEASE   := bin-release
TARGET_RELEASE    := $(BIN_DIR_RELEASE)/clay
OBJ_RELEASE       := $(patsubst src/%.c,$(BUILD_DIR_RELEASE)/%.o,$(SRC_NIX))
DEP_RELEASE       := $(OBJ_RELEASE:.o=.d)
RELEASE_CFLAGS    := $(STD) $(WARN) -Os -fno-lto -ffunction-sections -fdata-sections -Iinclude -MMD -MP $(CURL_CFLAGS)
RELEASE_LDFLAGS   := -fno-lto -Wl,--gc-sections -Wl,--as-needed -s -lpthread $(CURL_LDFLAGS)

TEST_SRC    := tests/test_openai.c
TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_SRC))
TEST_TARGET := $(BIN_DIR)/test_openai
# Everything test_openai.c needs except clay's own main().
LIB_OBJ     := $(filter-out $(BUILD_DIR)/main.o,$(OBJ))

CLI_TEST_SRC    := tests/test_cli.c
CLI_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(CLI_TEST_SRC))
CLI_TEST_TARGET := $(BIN_DIR)/test_cli

COMMAND_TEST_SRC    := tests/test_command.c
COMMAND_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(COMMAND_TEST_SRC))
COMMAND_TEST_TARGET := $(BIN_DIR)/test_command

CHAT_TEST_SRC    := tests/test_chat.c
CHAT_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(CHAT_TEST_SRC))
CHAT_TEST_TARGET := $(BIN_DIR)/test_chat

UUID_TEST_SRC    := tests/test_uuid.c
UUID_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(UUID_TEST_SRC))
UUID_TEST_TARGET := $(BIN_DIR)/test_uuid

MEMORY_TEST_SRC    := tests/test_memory.c
MEMORY_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(MEMORY_TEST_SRC))
MEMORY_TEST_TARGET := $(BIN_DIR)/test_memory

SANDBOX_TEST_SRC    := tests/test_sandbox.c
SANDBOX_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(SANDBOX_TEST_SRC))
SANDBOX_TEST_TARGET := $(BIN_DIR)/test_sandbox

FS_TOOLS_TEST_SRC    := tests/test_fs_tools.c
FS_TOOLS_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(FS_TOOLS_TEST_SRC))
FS_TOOLS_TEST_TARGET := $(BIN_DIR)/test_fs_tools

CHECKPOINT_TEST_SRC    := tests/test_checkpoint.c
CHECKPOINT_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(CHECKPOINT_TEST_SRC))
CHECKPOINT_TEST_TARGET := $(BIN_DIR)/test_checkpoint

PERMISSIONS_TEST_SRC    := tests/test_permissions.c
PERMISSIONS_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(PERMISSIONS_TEST_SRC))
PERMISSIONS_TEST_TARGET := $(BIN_DIR)/test_permissions

CONTEXT_COMPACT_TEST_SRC    := tests/test_context_compact.c
CONTEXT_COMPACT_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(CONTEXT_COMPACT_TEST_SRC))
CONTEXT_COMPACT_TEST_TARGET := $(BIN_DIR)/test_context_compact

PROJECT_INSTRUCTIONS_TEST_SRC    := tests/test_project_instructions.c
PROJECT_INSTRUCTIONS_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(PROJECT_INSTRUCTIONS_TEST_SRC))
PROJECT_INSTRUCTIONS_TEST_TARGET := $(BIN_DIR)/test_project_instructions

TODOWRITE_TEST_SRC    := tests/test_todowrite.c
TODOWRITE_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TODOWRITE_TEST_SRC))
TODOWRITE_TEST_TARGET := $(BIN_DIR)/test_todowrite

PROCESS_TEST_SRC    := tests/test_process.c
PROCESS_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(PROCESS_TEST_SRC))
PROCESS_TEST_TARGET := $(BIN_DIR)/test_process

MCP_TEST_SRC    := tests/test_mcp.c
MCP_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(MCP_TEST_SRC))
MCP_TEST_TARGET := $(BIN_DIR)/test_mcp

REPO_MAP_TEST_SRC    := tests/test_repo_map.c
REPO_MAP_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(REPO_MAP_TEST_SRC))
REPO_MAP_TEST_TARGET := $(BIN_DIR)/test_repo_map

ENV_BLOCK_TEST_SRC    := tests/test_environment_block.c
ENV_BLOCK_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(ENV_BLOCK_TEST_SRC))
ENV_BLOCK_TEST_TARGET := $(BIN_DIR)/test_environment_block

COMPACT_TEST_SRC    := tests/test_compact.c
COMPACT_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(COMPACT_TEST_SRC))
COMPACT_TEST_TARGET := $(BIN_DIR)/test_compact

CODEX_TEST_SRC    := tests/test_openai_codex.c
CODEX_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(CODEX_TEST_SRC))
CODEX_TEST_TARGET := $(BIN_DIR)/test_openai_codex

GROK_TEST_SRC    := tests/test_grok.c
GROK_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(GROK_TEST_SRC))
GROK_TEST_TARGET := $(BIN_DIR)/test_grok

UNDO_TEST_SRC    := tests/test_undo.c
UNDO_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(UNDO_TEST_SRC))
UNDO_TEST_TARGET := $(BIN_DIR)/test_undo

STORAGE_TEST_SRC    := tests/test_storage.c
STORAGE_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(STORAGE_TEST_SRC))
STORAGE_TEST_TARGET := $(BIN_DIR)/test_storage

# Windows cross-build via mingw-w64. With CURL_LINK=dynamic, clay.exe needs
# libcurl-4.dll and its own dependency DLLs alongside it.
CC_WIN      ?= x86_64-w64-mingw32-gcc
CFLAGS_WIN  := $(STD) $(WARN) $(OPT) -Iinclude -MMD -MP $(CURL_CFLAGS)
LDFLAGS_WIN := -lpthread -lws2_32 -lshell32 -ladvapi32 $(CURL_LDFLAGS)

BUILD_DIR_WIN := build-win
BIN_DIR_WIN   := bin-win
TARGET_WIN    := $(BIN_DIR_WIN)/clay.exe

OBJ_WIN := $(patsubst src/%.c,$(BUILD_DIR_WIN)/%.o,$(SRC_WIN))
DEP_WIN := $(OBJ_WIN:.o=.d)

UNIT_TEST_TARGETS := $(CLI_TEST_TARGET) $(COMMAND_TEST_TARGET) $(CHAT_TEST_TARGET) \
	$(UUID_TEST_TARGET) $(MEMORY_TEST_TARGET) $(SANDBOX_TEST_TARGET) \
	$(FS_TOOLS_TEST_TARGET) $(CHECKPOINT_TEST_TARGET) $(PERMISSIONS_TEST_TARGET) \
	$(CONTEXT_COMPACT_TEST_TARGET) $(PROJECT_INSTRUCTIONS_TEST_TARGET) \
	$(TODOWRITE_TEST_TARGET) $(PROCESS_TEST_TARGET) $(MCP_TEST_TARGET) \
	$(REPO_MAP_TEST_TARGET) $(ENV_BLOCK_TEST_TARGET) $(COMPACT_TEST_TARGET) \
	$(CODEX_TEST_TARGET) $(GROK_TEST_TARGET) $(UNDO_TEST_TARGET) $(STORAGE_TEST_TARGET)

.PHONY: all build release build-win build_win test test-openai test-openai-codex test-grok test-cli test-command test-chat test-uuid test-memory test-sandbox test-fs-tools test-checkpoint test-permissions test-context-compact test-project-instructions test-todowrite test-process test-mcp test-repo-map test-environment-block test-compact test-undo test-storage completions man-pages run compress compress-release compress-win clean debug

all: build

build: $(TARGET)

release: $(TARGET_RELEASE)

$(TARGET_RELEASE): $(OBJ_RELEASE) | $(BIN_DIR_RELEASE)
	$(CC) $(OBJ_RELEASE) -o $@ $(RELEASE_LDFLAGS)

$(BUILD_DIR_RELEASE)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RELEASE_CFLAGS) -c $< -o $@

$(TARGET): $(OBJ) | $(BIN_DIR)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

test: $(UNIT_TEST_TARGETS)
	@set -e; for test_binary in $^; do ./$$test_binary; done

test-openai: $(TEST_TARGET)
	@./$<

test-openai-codex: $(CODEX_TEST_TARGET)
	@./$<

test-grok: $(GROK_TEST_TARGET)
	@./$<

$(CODEX_TEST_TARGET): $(LIB_OBJ) $(CODEX_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(CODEX_TEST_OBJ) -o $@ $(LDFLAGS)

$(GROK_TEST_TARGET): $(LIB_OBJ) $(GROK_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(GROK_TEST_OBJ) -o $@ $(LDFLAGS)

test-undo: $(UNDO_TEST_TARGET)
	@./$<

$(UNDO_TEST_TARGET): $(LIB_OBJ) $(UNDO_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(UNDO_TEST_OBJ) -o $@ $(LDFLAGS)

test-storage: $(STORAGE_TEST_TARGET)
	@./$<

$(STORAGE_TEST_TARGET): $(LIB_OBJ) $(STORAGE_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(STORAGE_TEST_OBJ) -o $@ $(LDFLAGS)

$(TEST_TARGET): $(LIB_OBJ) $(TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(TEST_OBJ) -o $@ $(LDFLAGS)

test-cli: $(CLI_TEST_TARGET)
	@./$<

$(CLI_TEST_TARGET): $(LIB_OBJ) $(CLI_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(CLI_TEST_OBJ) -o $@ $(LDFLAGS)

test-command: $(COMMAND_TEST_TARGET)
	@./$<

$(COMMAND_TEST_TARGET): $(LIB_OBJ) $(COMMAND_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(COMMAND_TEST_OBJ) -o $@ $(LDFLAGS)

test-chat: $(CHAT_TEST_TARGET)
	@./$<

$(CHAT_TEST_TARGET): $(LIB_OBJ) $(CHAT_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(CHAT_TEST_OBJ) -o $@ $(LDFLAGS)

test-uuid: $(UUID_TEST_TARGET)
	@./$<

$(UUID_TEST_TARGET): $(LIB_OBJ) $(UUID_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(UUID_TEST_OBJ) -o $@ $(LDFLAGS)

test-memory: $(MEMORY_TEST_TARGET)
	@./$<

$(MEMORY_TEST_TARGET): $(LIB_OBJ) $(MEMORY_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(MEMORY_TEST_OBJ) -o $@ $(LDFLAGS)

test-sandbox: $(SANDBOX_TEST_TARGET)
	@./$<

$(SANDBOX_TEST_TARGET): $(LIB_OBJ) $(SANDBOX_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(SANDBOX_TEST_OBJ) -o $@ $(LDFLAGS)

test-fs-tools: $(FS_TOOLS_TEST_TARGET)
	@./$<

$(FS_TOOLS_TEST_TARGET): $(LIB_OBJ) $(FS_TOOLS_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(FS_TOOLS_TEST_OBJ) -o $@ $(LDFLAGS)

test-checkpoint: $(CHECKPOINT_TEST_TARGET)
	@./$<

$(CHECKPOINT_TEST_TARGET): $(LIB_OBJ) $(CHECKPOINT_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(CHECKPOINT_TEST_OBJ) -o $@ $(LDFLAGS)

test-permissions: $(PERMISSIONS_TEST_TARGET)
	@./$<

$(PERMISSIONS_TEST_TARGET): $(LIB_OBJ) $(PERMISSIONS_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(PERMISSIONS_TEST_OBJ) -o $@ $(LDFLAGS)

test-context-compact: $(CONTEXT_COMPACT_TEST_TARGET)
	@./$<

$(CONTEXT_COMPACT_TEST_TARGET): $(LIB_OBJ) $(CONTEXT_COMPACT_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(CONTEXT_COMPACT_TEST_OBJ) -o $@ $(LDFLAGS)

test-project-instructions: $(PROJECT_INSTRUCTIONS_TEST_TARGET)
	@./$<

$(PROJECT_INSTRUCTIONS_TEST_TARGET): $(LIB_OBJ) $(PROJECT_INSTRUCTIONS_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(PROJECT_INSTRUCTIONS_TEST_OBJ) -o $@ $(LDFLAGS)

test-todowrite: $(TODOWRITE_TEST_TARGET)
	@./$<

$(TODOWRITE_TEST_TARGET): $(LIB_OBJ) $(TODOWRITE_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(TODOWRITE_TEST_OBJ) -o $@ $(LDFLAGS)

test-process: $(PROCESS_TEST_TARGET)
	@./$<

$(PROCESS_TEST_TARGET): $(LIB_OBJ) $(PROCESS_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(PROCESS_TEST_OBJ) -o $@ $(LDFLAGS)

test-mcp: $(MCP_TEST_TARGET)
	@./$<

$(MCP_TEST_TARGET): $(LIB_OBJ) $(MCP_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(MCP_TEST_OBJ) -o $@ $(LDFLAGS)

test-repo-map: $(REPO_MAP_TEST_TARGET)
	@./$<

$(REPO_MAP_TEST_TARGET): $(LIB_OBJ) $(REPO_MAP_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(REPO_MAP_TEST_OBJ) -o $@ $(LDFLAGS)

test-environment-block: $(ENV_BLOCK_TEST_TARGET)
	@./$<

$(ENV_BLOCK_TEST_TARGET): $(LIB_OBJ) $(ENV_BLOCK_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(ENV_BLOCK_TEST_OBJ) -o $@ $(LDFLAGS)

test-compact: $(COMPACT_TEST_TARGET)
	@./$<

$(COMPACT_TEST_TARGET): $(LIB_OBJ) $(COMPACT_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(COMPACT_TEST_OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

$(BIN_DIR_RELEASE):
	mkdir -p $@

COMPLETION_FILES := completions/clay.bash completions/clay.zsh completions/clay.fish

completions: $(COMPLETION_FILES)

man-pages: man/clay.1

build-win: $(TARGET_WIN)
build_win: build-win

$(TARGET_WIN): $(OBJ_WIN) | $(BIN_DIR_WIN)
	$(CC_WIN) $(OBJ_WIN) -o $@ $(LDFLAGS_WIN)

$(BUILD_DIR_WIN)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC_WIN) $(CFLAGS_WIN) -c $< -o $@

$(BIN_DIR_WIN):
	mkdir -p $@

run: build
	@mkdir -p .playground
	@./$(TARGET) --cwd .playground

compress: build
	@upx -d $(TARGET) >/dev/null 2>&1 || true
	$(STRIP) --strip-unneeded $(TARGET)
	upx --best --lzma $(TARGET)

compress-release: release
	@upx -d $(TARGET_RELEASE) >/dev/null 2>&1 || true
	$(STRIP) --strip-unneeded $(TARGET_RELEASE)
	upx --best --lzma $(TARGET_RELEASE)

compress-win: build-win
	@upx -d $(TARGET_WIN) >/dev/null 2>&1 || true
	$(STRIP_WIN) --strip-unneeded $(TARGET_WIN)
	upx --best --lzma $(TARGET_WIN)

debug: CFLAGS += -g -O0 -DDEBUG
debug: clean build

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(BUILD_DIR_WIN) $(BIN_DIR_WIN) $(BUILD_DIR_RELEASE) $(BIN_DIR_RELEASE)

-include $(DEP)
-include $(DEP_WIN)
-include $(DEP_RELEASE)
