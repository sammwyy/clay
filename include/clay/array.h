#ifndef CLAY_ARRAY_H
#define CLAY_ARRAY_H

#include <stddef.h>

/* Growable, type-erased dynamic array. Doubles capacity on growth. */
typedef struct {
    void *data;
    size_t count;
    size_t cap;
    size_t elem_size;
} ClayArray;

void clay_array_init(ClayArray *a, size_t elem_size);
void clay_array_free(ClayArray *a);
void clay_array_clear(ClayArray *a);
void *clay_array_push(ClayArray *a);              /* returns pointer to a new, uninitialized slot */
void clay_array_push_val(ClayArray *a, const void *value);
void *clay_array_get(ClayArray *a, size_t index);
void clay_array_insert(ClayArray *a, size_t index, const void *value); /* shifts elements at/after index right */
void clay_array_remove(ClayArray *a, size_t index);                   /* shifts elements after index left */

#endif /* CLAY_ARRAY_H */
