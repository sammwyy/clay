#include "clay/arena.h"

#include <stdlib.h>
#include <string.h>

typedef struct ClayArenaBlock {
    struct ClayArenaBlock *next;
    size_t size;
    size_t used;
    unsigned char data[];
} ClayArenaBlock;

struct ClayArena {
    ClayArenaBlock *head;
    size_t block_size;
};

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static ClayArenaBlock *arena_block_create(size_t size) {
    ClayArenaBlock *block = malloc(sizeof(ClayArenaBlock) + size);
    block->next = NULL;
    block->size = size;
    block->used = 0;
    return block;
}

ClayArena *clay_arena_create(size_t block_size) {
    ClayArena *arena = malloc(sizeof(ClayArena));
    arena->block_size = block_size > 0 ? block_size : 4096;
    arena->head = arena_block_create(arena->block_size);
    return arena;
}

void *clay_arena_alloc(ClayArena *arena, size_t size) {
    size = align_up(size, sizeof(void *));
    ClayArenaBlock *block = arena->head;
    if (block->used + size > block->size) {
        size_t new_size = size > arena->block_size ? size : arena->block_size;
        ClayArenaBlock *fresh = arena_block_create(new_size);
        fresh->next = block;
        arena->head = fresh;
        block = fresh;
    }
    void *ptr = block->data + block->used;
    block->used += size;
    return ptr;
}

char *clay_arena_strdup(ClayArena *arena, const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = clay_arena_alloc(arena, len);
    memcpy(copy, s, len);
    return copy;
}

void clay_arena_reset(ClayArena *arena) {
    for (ClayArenaBlock *block = arena->head; block; block = block->next) {
        block->used = 0;
    }
}

void clay_arena_destroy(ClayArena *arena) {
    ClayArenaBlock *block = arena->head;
    while (block) {
        ClayArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    free(arena);
}
