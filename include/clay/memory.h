#ifndef CLAY_MEMORY_H
#define CLAY_MEMORY_H

#include "clay/array.h"

/* Long-term memory: ~/.clay/memory/<slug>.md holds each entry's content;
   ~/.clay/memory/index.json is the manifest (slug, type, summary,
   updated_at) used to build the index. Survives across every chat, unlike
   ClayChat's per-conversation journal. */

/* Lowercase letters, digits, and hyphens only, 1-64 chars. */
int clay_memory_valid_slug(const char *slug);

/* Writes/overwrites the entry's content and its manifest record. 0 on
   success, -1 on an invalid slug or I/O failure. */
int clay_memory_write(const char *slug, const char *type, const char *summary, const char *content);

/* Malloc'd entry content, or NULL if the slug is invalid or the entry
   doesn't exist. Caller frees. */
char *clay_memory_read(const char *slug);

/* Removes the entry's file and manifest record. 0 on success (including
   if it didn't already exist), -1 on an invalid slug or I/O failure. */
int clay_memory_delete(const char *slug);

/* Malloc'd index text, one "- slug [type]: summary" line per manifest
   entry; empty string if there are none. Caller frees. */
char *clay_memory_index(void);

typedef struct {
    char *slug;
    char *type;
    char *summary;
    long long updated_at;
} ClayMemoryEntry;

/* Every entry in the manifest, in write order. Caller frees with
   clay_memory_entries_free. */
void clay_memory_list(ClayArray *entries);
void clay_memory_entries_free(ClayArray *entries);

#endif /* CLAY_MEMORY_H */
