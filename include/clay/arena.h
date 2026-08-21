#ifndef CLAY_ARENA_H
#define CLAY_ARENA_H

#include <stddef.h>

/* Bump allocator. Grows by adding new blocks; individual allocations are
   never freed one by one, only the whole arena at once. */
typedef struct ClayArena ClayArena;

ClayArena *clay_arena_create(size_t block_size); /* 0 = sane default */
void *clay_arena_alloc(ClayArena *arena, size_t size);
char *clay_arena_strdup(ClayArena *arena, const char *s);
void clay_arena_reset(ClayArena *arena);   /* keeps blocks, rewinds offsets */
void clay_arena_destroy(ClayArena *arena);

#endif /* CLAY_ARENA_H */
