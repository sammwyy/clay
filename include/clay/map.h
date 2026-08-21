#ifndef CLAY_MAP_H
#define CLAY_MAP_H

#include <stddef.h>

/* String-keyed hash map, separate chaining, doubles its bucket count past
   a 0.75 load factor. Values are opaque pointers owned by the caller; the
   map only owns its key copies. */
typedef struct ClayMap ClayMap;
typedef void (*ClayMapVisitor)(const char *key, void *value, void *ctx);

ClayMap *clay_map_create(void);
void clay_map_destroy(ClayMap *map);
void clay_map_set(ClayMap *map, const char *key, void *value);
void *clay_map_get(ClayMap *map, const char *key);
int clay_map_remove(ClayMap *map, const char *key); /* 1 if a key was removed */
size_t clay_map_count(ClayMap *map);
void clay_map_foreach(ClayMap *map, ClayMapVisitor visitor, void *ctx);

#endif /* CLAY_MAP_H */
