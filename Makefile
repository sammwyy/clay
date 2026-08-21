CC      ?= cc
STD     := -std=gnu11
WARN    := -Wall -Wextra
OPT     := -O2
CFLAGS  ?= $(STD) $(WARN) $(OPT) -Iinclude -MMD -MP
LDFLAGS ?= -lpthread

BUILD_DIR := build
BIN_DIR   := bin
TARGET    := $(BIN_DIR)/clay

SRC := $(shell find src -name '*.c')
OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

# Windows cross-build via mingw-w64 (needs the posix-threads variant for pthread.h)
CC_WIN      ?= x86_64-w64-mingw32-gcc
CFLAGS_WIN  := $(STD) $(WARN) $(OPT) -Iinclude -MMD -MP
LDFLAGS_WIN := -lpthread -static

BUILD_DIR_WIN := build-win
BIN_DIR_WIN   := bin-win
TARGET_WIN    := $(BIN_DIR_WIN)/clay.exe

OBJ_WIN := $(patsubst src/%.c,$(BUILD_DIR_WIN)/%.o,$(SRC))
DEP_WIN := $(OBJ_WIN:.o=.d)

.PHONY: all build build-win build_win run clean debug

all: build

build: $(TARGET)

$(TARGET): $(OBJ) | $(BIN_DIR)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

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
