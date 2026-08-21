#ifndef CLAY_JSON_H
#define CLAY_JSON_H

#include "clay/str.h"

#include <stddef.h>

typedef enum {
    CLAY_JSON_NULL,
    CLAY_JSON_BOOL,
    CLAY_JSON_NUMBER,
    CLAY_JSON_STRING,
    CLAY_JSON_ARRAY,
    CLAY_JSON_OBJECT
} ClayJsonType;

typedef struct ClayJson ClayJson;

/* Constructors. Each returns a new, owned value. */
ClayJson *clay_json_null(void);
ClayJson *clay_json_bool(int value);
ClayJson *clay_json_number(double value);
ClayJson *clay_json_string(const char *value); /* copies value */
ClayJson *clay_json_array(void);
ClayJson *clay_json_object(void);

/* Mutators. Both take ownership of `value`. clay_json_object_set replaces
   an existing member under the same key, freeing the old value. */
void clay_json_array_push(ClayJson *array, ClayJson *value);
void clay_json_object_set(ClayJson *object, const char *key, ClayJson *value);

/* Recursively frees `value` and everything under it. No-op on NULL. */
void clay_json_free(ClayJson *value);

/* Deep copy, independently owned from `value`. */
ClayJson *clay_json_clone(const ClayJson *value);

/* Accessors. NULL/0/"" on a type mismatch or missing key/index. */
ClayJsonType clay_json_type(const ClayJson *value);
int clay_json_bool_value(const ClayJson *value);
double clay_json_number_value(const ClayJson *value);
const char *clay_json_string_value(const ClayJson *value);
size_t clay_json_array_count(const ClayJson *value);
ClayJson *clay_json_array_get(const ClayJson *value, size_t index); /* NULL out of range */
ClayJson *clay_json_object_get(const ClayJson *value, const char *key); /* NULL if missing */

/* Serializes to compact JSON, appended to `out`. */
void clay_json_stringify(const ClayJson *value, ClayStr *out);

/* Parses one JSON value starting at `text`; if `end_out` is non-NULL,
   sets *end_out to the first byte after it. NULL on a malformed document.
   Caller frees the result with clay_json_free. */
ClayJson *clay_json_parse(const char *text, const char **end_out);

#endif /* CLAY_JSON_H */
