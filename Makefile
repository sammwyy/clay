CC      ?= cc
STD     := -std=gnu11
WARN    := -Wall -Wextra
OPT     := -O2

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
OBJ      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)

TEST_SRC    := tests/test_openai.c
TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_SRC))
TEST_TARGET := $(BIN_DIR)/test_openai
# Everything test_openai.c needs except clay's own main().
LIB_OBJ     := $(filter-out $(BUILD_DIR)/main.o,$(OBJ))

CLI_TEST_SRC    := tests/test_cli.c
CLI_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(CLI_TEST_SRC))
CLI_TEST_TARGET := $(BIN_DIR)/test_cli

CHAT_TEST_SRC    := tests/test_chat.c
CHAT_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(CHAT_TEST_SRC))
CHAT_TEST_TARGET := $(BIN_DIR)/test_chat

UUID_TEST_SRC    := tests/test_uuid.c
UUID_TEST_OBJ    := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(UUID_TEST_SRC))
UUID_TEST_TARGET := $(BIN_DIR)/test_uuid

# Windows cross-build via mingw-w64. With CURL_LINK=dynamic, clay.exe needs
# libcurl-4.dll and its own dependency DLLs alongside it.
CC_WIN      ?= x86_64-w64-mingw32-gcc
CFLAGS_WIN  := $(STD) $(WARN) $(OPT) -Iinclude -MMD -MP $(CURL_CFLAGS)
LDFLAGS_WIN := -lpthread $(CURL_LDFLAGS)

BUILD_DIR_WIN := build-win
BIN_DIR_WIN   := bin-win
TARGET_WIN    := $(BIN_DIR_WIN)/clay.exe

OBJ_WIN := $(patsubst src/%.c,$(BUILD_DIR_WIN)/%.o,$(SRC))
DEP_WIN := $(OBJ_WIN:.o=.d)

.PHONY: all build build-win build_win test-openai test-cli test-chat test-uuid run compress compress-win clean debug

all: build

build: $(TARGET)

$(TARGET): $(OBJ) | $(BIN_DIR)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

test-openai: $(TEST_TARGET)

$(TEST_TARGET): $(LIB_OBJ) $(TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(TEST_OBJ) -o $@ $(LDFLAGS)

test-cli: $(CLI_TEST_TARGET)

$(CLI_TEST_TARGET): $(LIB_OBJ) $(CLI_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(CLI_TEST_OBJ) -o $@ $(LDFLAGS)

test-chat: $(CHAT_TEST_TARGET)

$(CHAT_TEST_TARGET): $(LIB_OBJ) $(CHAT_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(CHAT_TEST_OBJ) -o $@ $(LDFLAGS)

test-uuid: $(UUID_TEST_TARGET)

$(UUID_TEST_TARGET): $(LIB_OBJ) $(UUID_TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(UUID_TEST_OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

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
	upx --best --lzma $(TARGET)

compress-win: build-win
	@upx -d $(TARGET_WIN) >/dev/null 2>&1 || true
	upx --best --lzma $(TARGET_WIN)

debug: CFLAGS += -g -O0 -DDEBUG
debug: clean build

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(BUILD_DIR_WIN) $(BIN_DIR_WIN)

-include $(DEP)
-include $(DEP_WIN)
