#include "clay/map.h"

#include <stdlib.h>
#include <string.h>

typedef struct ClayMapNode {
    char *key;
    void *value;
    struct ClayMapNode *next;
} ClayMapNode;

struct ClayMap {
    ClayMapNode **buckets;
    size_t bucket_count;
    size_t count;
};

#define CLAY_MAP_INITIAL_BUCKETS 16
#define CLAY_MAP_MAX_LOAD 0.75

static unsigned long fnv1a(const char *s) {
    unsigned long hash = 2166136261UL;
    for (; *s; s++) {
        hash ^= (unsigned char)*s;
        hash *= 16777619UL;
    }
    return hash;
}

ClayMap *clay_map_create(void) {
    ClayMap *map = malloc(sizeof(ClayMap));
    map->bucket_count = CLAY_MAP_INITIAL_BUCKETS;
    map->buckets = calloc(map->bucket_count, sizeof(ClayMapNode *));
    map->count = 0;
    return map;
}

void clay_map_destroy(ClayMap *map) {
    for (size_t i = 0; i < map->bucket_count; i++) {
        ClayMapNode *node = map->buckets[i];
        while (node) {
            ClayMapNode *next = node->next;
            free(node->key);
            free(node);
            node = next;
        }
    }
    free(map->buckets);
    free(map);
}

static ClayMapNode **map_find_slot(ClayMapNode **buckets, size_t bucket_count, const char *key) {
    ClayMapNode **slot = &buckets[fnv1a(key) % bucket_count];
    while (*slot && strcmp((*slot)->key, key) != 0) slot = &(*slot)->next;
    return slot;
}

static void map_rehash(ClayMap *map) {
    size_t new_count = map->bucket_count * 2;
    ClayMapNode **new_buckets = calloc(new_count, sizeof(ClayMapNode *));

    for (size_t i = 0; i < map->bucket_count; i++) {
        ClayMapNode *node = map->buckets[i];
        while (node) {
            ClayMapNode *next = node->next;
            ClayMapNode **slot = &new_buckets[fnv1a(node->key) % new_count];
            node->next = *slot;
            *slot = node;
            node = next;
        }
    }

    free(map->buckets);
    map->buckets = new_buckets;
    map->bucket_count = new_count;
}

void clay_map_set(ClayMap *map, const char *key, void *value) {
    if ((double)(map->count + 1) / (double)map->bucket_count > CLAY_MAP_MAX_LOAD) {
        map_rehash(map);
    }

    ClayMapNode **slot = map_find_slot(map->buckets, map->bucket_count, key);
    if (*slot) {
        (*slot)->value = value;
        return;
    }

    ClayMapNode *node = malloc(sizeof(ClayMapNode));
    node->key = strdup(key);
    node->value = value;
    node->next = NULL;
    *slot = node;
    map->count++;
}

void *clay_map_get(ClayMap *map, const char *key) {
    ClayMapNode **slot = map_find_slot(map->buckets, map->bucket_count, key);
    return *slot ? (*slot)->value : NULL;
}

int clay_map_remove(ClayMap *map, const char *key) {
    ClayMapNode **slot = map_find_slot(map->buckets, map->bucket_count, key);
    if (!*slot) return 0;

    ClayMapNode *node = *slot;
    *slot = node->next;
    free(node->key);
    free(node);
    map->count--;
    return 1;
}

size_t clay_map_count(ClayMap *map) {
    return map->count;
}

void clay_map_foreach(ClayMap *map, ClayMapVisitor visitor, void *ctx) {
    for (size_t i = 0; i < map->bucket_count; i++) {
        for (ClayMapNode *node = map->buckets[i]; node; node = node->next) {
            visitor(node->key, node->value, ctx);
        }
    }
}
