#ifndef CLAY_STORAGE_H
#define CLAY_STORAGE_H

#include "clay/str.h"

#include <stddef.h>

typedef struct ClayJson ClayJson;

/* Paths and files under the private ~/.clay storage tree. Returned paths are
   malloc'd. `relative` uses '/' separators and must be an internal path, not
   user input. */
char *clay_storage_root(void);
char *clay_storage_path(const char *relative);

/* Creates ~/.clay and every directory in `relative`. An empty relative path
   only creates the root. */
int clay_storage_ensure_dir(const char *relative);

/* Consistent bounded read and owner-private atomic write helpers. */
int clay_storage_read_limited(const char *path, size_t max_bytes,
                              ClayStr *out);
int clay_storage_write_atomic_private(const char *path, const void *data,
                                      size_t len);

/* Bounded JSON file helpers. Returned values are owned by the caller. */
ClayJson *clay_storage_read_json(const char *path, size_t max_bytes);
int clay_storage_write_json_atomic_private(const char *path,
                                           const ClayJson *value);

#endif /* CLAY_STORAGE_H */
