#include "clay/array.h"

#include <stdlib.h>
#include <string.h>

#define CLAY_ARRAY_INITIAL_CAP 8

void clay_array_init(ClayArray *a, size_t elem_size) {
    a->elem_size = elem_size;
    a->count = 0;
    a->cap = CLAY_ARRAY_INITIAL_CAP;
    a->data = malloc(a->cap * a->elem_size);
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

static void array_grow(ClayArray *a) {
    a->cap *= 2;
    a->data = realloc(a->data, a->cap * a->elem_size);
}

void *clay_array_push(ClayArray *a) {
    if (a->count == a->cap) array_grow(a);
    void *slot = (unsigned char *)a->data + a->count * a->elem_size;
    a->count++;
    return slot;
}

void clay_array_push_val(ClayArray *a, const void *value) {
    void *slot = clay_array_push(a);
    memcpy(slot, value, a->elem_size);
}

void *clay_array_get(ClayArray *a, size_t index) {
    return (unsigned char *)a->data + index * a->elem_size;
}
