#include "clay/array.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CLAY_ARRAY_INITIAL_CAP 8

void clay_array_init(ClayArray *a, size_t elem_size) {
    a->elem_size = elem_size;
    a->count = 0;
    a->cap = CLAY_ARRAY_INITIAL_CAP;
    a->data = elem_size <= SIZE_MAX / a->cap ? malloc(a->cap * a->elem_size) : NULL;
    if (!a->data) a->cap = 0;
}

void clay_array_free(ClayArray *a) {
    free(a->data);
    a->data = NULL;
    a->count = 0;
    a->cap = 0;
}

void clay_array_clear(ClayArray *a) {
    a->count = 0;
}

static int array_grow(ClayArray *a) {
    size_t new_cap = a->cap ? a->cap : CLAY_ARRAY_INITIAL_CAP;
    if (new_cap > SIZE_MAX / 2) return 0;
    new_cap *= 2;
    if (a->elem_size == 0 || new_cap > SIZE_MAX / a->elem_size) return 0;
    void *grown = realloc(a->data, new_cap * a->elem_size);
    if (!grown) return 0;
    a->data = grown;
    a->cap = new_cap;
    return 1;
}

void *clay_array_push(ClayArray *a) {
    if (!a || a->elem_size == 0) return NULL;
    if (a->count == a->cap && !array_grow(a)) return NULL;
    if (a->count > SIZE_MAX / a->elem_size) return NULL;
    void *slot = (unsigned char *)a->data + a->count * a->elem_size;
    a->count++;
    return slot;
}

void clay_array_push_val(ClayArray *a, const void *value) {
    void *slot = clay_array_push(a);
    if (!slot || !value) return;
    memcpy(slot, value, a->elem_size);
}

void *clay_array_get(ClayArray *a, size_t index) {
    if (!a || index >= a->count) return NULL;
    return (unsigned char *)a->data + index * a->elem_size;
}

void clay_array_insert(ClayArray *a, size_t index, const void *value) {
    if (!a || index > a->count || !value) return;
    if (!clay_array_push(a)) return; /* grow if needed, reserve the extra slot */
    unsigned char *base = (unsigned char *)a->data;
    size_t tail = a->count - 1 - index;
    if (tail > 0) {
        memmove(base + (index + 1) * a->elem_size, base + index * a->elem_size, tail * a->elem_size);
    }
    memcpy(base + index * a->elem_size, value, a->elem_size);
}

void clay_array_remove(ClayArray *a, size_t index) {
    if (!a || index >= a->count) return;
    unsigned char *base = (unsigned char *)a->data;
    size_t tail = a->count - index - 1;
    if (tail > 0) {
        memmove(base + index * a->elem_size, base + (index + 1) * a->elem_size, tail * a->elem_size);
    }
    a->count--;
}
