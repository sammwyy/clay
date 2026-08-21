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

# test_openai.c has its own main() (see TEST_TARGET below) and is excluded
# from the main clay binary.
SRC      := $(filter-out src/test_openai.c,$(shell find src -name '*.c'))
OBJ      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)

TEST_SRC    := src/test_openai.c
TEST_OBJ    := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(TEST_SRC))
TEST_TARGET := $(BIN_DIR)/test_openai
# Everything test_openai.c needs except clay's own main().
LIB_OBJ     := $(filter-out $(BUILD_DIR)/main.o,$(OBJ))

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

.PHONY: all build build-win build_win test-openai run clean debug

all: build

build: $(TARGET)

$(TARGET): $(OBJ) | $(BIN_DIR)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

test-openai: $(TEST_TARGET)

$(TEST_TARGET): $(LIB_OBJ) $(TEST_OBJ) | $(BIN_DIR)
	$(CC) $(LIB_OBJ) $(TEST_OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c
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
	@./$(TARGET)

debug: CFLAGS += -g -O0 -DDEBUG
debug: clean build

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(BUILD_DIR_WIN) $(BIN_DIR_WIN)

-include $(DEP)
-include $(DEP_WIN)
