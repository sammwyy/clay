#include "clay/storage.h"

#include "clay/json.h"
#include "clay/term.h"

#include <stdlib.h>
#include <string.h>

char *clay_storage_root(void) {
    char *home = clay_term_home_dir();
    if (!home) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay", home);
    free(home);
    return path.data;
}

char *clay_storage_path(const char *relative) {
    char *root = clay_storage_root();
    if (!root) return NULL;
    if (!relative || !*relative) return root;

    ClayStr path;
    clay_str_init(&path);
    clay_str_push(&path, root);
    if (relative[0] != '/') clay_str_push_char(&path, '/');
    clay_str_push(&path, relative);
    free(root);
    return path.data;
}

int clay_storage_ensure_dir(const char *relative) {
    char *root = clay_storage_root();
    if (!root || clay_term_mkdir(root) != 0) {
        free(root);
        return -1;
    }
    if (!relative || !*relative) {
        free(root);
        return 0;
    }

    ClayStr path;
    clay_str_init(&path);
    clay_str_push(&path, root);
    free(root);
    const char *start = relative;
    for (const char *p = relative;; p++) {
        if (*p != '/' && *p != '\0') continue;
        if (p > start) {
            clay_str_push_char(&path, '/');
            clay_str_push_n(&path, start, (size_t)(p - start));
            if (clay_term_mkdir(path.data) != 0) {
                clay_str_free(&path);
                return -1;
            }
        }
        if (*p == '\0') break;
        start = p + 1;
    }
    clay_str_free(&path);
    return 0;
}

int clay_storage_read_limited(const char *path, size_t max_bytes,
                              ClayStr *out) {
    return clay_term_read_file(path, max_bytes, out);
}

int clay_storage_write_atomic_private(const char *path, const void *data,
                                      size_t len) {
    return clay_term_write_file_atomic(path, data, len);
}

ClayJson *clay_storage_read_json(const char *path, size_t max_bytes) {
    ClayStr text;
    if (clay_storage_read_limited(path, max_bytes, &text) != 0) return NULL;
    ClayJson *value = clay_json_parse(text.data, NULL);
    clay_str_free(&text);
    return value;
}

int clay_storage_write_json_atomic_private(const char *path,
                                           const ClayJson *value) {
    ClayStr text;
    clay_str_init(&text);
    clay_json_stringify(value, &text);
    int rc = clay_storage_write_atomic_private(path, text.data, text.len);
    clay_str_free(&text);
    return rc;
}
