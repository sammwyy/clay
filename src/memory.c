#include "clay/memory.h"

#include "clay/json.h"
#include "clay/str.h"
#include "clay/term.h"
#include "clay/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *clay_dir(void) {
    char *home = clay_term_home_dir();
    if (!home) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay", home);
    free(home);
    return path.data;
}

static char *memory_dir(void) {
    char *dir = clay_dir();
    if (!dir) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/memory", dir);
    free(dir);
    return path.data;
}

static int ensure_memory_dir(void) {
    char *dir = clay_dir();
    if (!dir || clay_term_mkdir(dir) != 0) {
        free(dir);
        return -1;
    }
    ClayStr mem;
    clay_str_init(&mem);
    clay_str_printf(&mem, "%s/memory", dir);
    free(dir);
    int rc = clay_term_mkdir(mem.data);
    clay_str_free(&mem);
    return rc;
}

static char *entry_path(const char *slug) {
    char *dir = memory_dir();
    if (!dir) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/%s.md", dir, slug);
    free(dir);
    return path.data;
}

static char *index_path(void) {
    char *dir = memory_dir();
    if (!dir) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/index.json", dir);
    free(dir);
    return path.data;
}

#define CLAY_MEMORY_FILE_LIMIT (4 * 1024 * 1024)

static char *read_file(const char *path) {
    ClayStr text;
    if (clay_term_read_file(path, CLAY_MEMORY_FILE_LIMIT, &text) != 0) return NULL;
    return text.data;
}

static ClayJson *load_manifest(void) {
    char *path = index_path();
    char *text = path ? read_file(path) : NULL;
    free(path);
    ClayJson *root = text ? clay_json_parse(text, NULL) : NULL;
    free(text);
    if (!root || clay_json_type(root) != CLAY_JSON_OBJECT ||
        clay_json_type(clay_json_object_get(root, "entries")) != CLAY_JSON_ARRAY) {
        clay_json_free(root);
        root = clay_json_object();
        clay_json_object_set(root, "entries", clay_json_array());
    }
    return root;
}

/* Takes ownership of root. */
static int save_manifest(ClayJson *root) {
    char *path = index_path();
    if (!path) {
        clay_json_free(root);
        return -1;
    }
    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(root, &body);
    clay_json_free(root);
    int ok = clay_term_write_file_atomic(path, body.data, body.len) == 0;
    free(path);
    clay_str_free(&body);
    return ok ? 0 : -1;
}

int clay_memory_valid_slug(const char *slug) {
    if (!slug || !*slug) return 0;
    size_t len = strlen(slug);
    if (len > 64) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = slug[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

int clay_memory_write(const char *slug, const char *type, const char *summary, const char *content) {
    if (!clay_memory_valid_slug(slug) || ensure_memory_dir() != 0) return -1;
    char *path = entry_path(slug);
    if (!path) return -1;
    const char *body = content ? content : "";
    size_t len = strlen(body);
    int rc = clay_term_write_file_atomic(path, body, len);
    free(path);
    if (rc != 0) return -1;

    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    ClayJson *entry = NULL;
    for (size_t i = 0; i < clay_json_array_count(entries); i++) {
        ClayJson *candidate = clay_json_array_get(entries, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(candidate, "slug")), slug) == 0) {
            entry = candidate;
            break;
        }
    }
    if (!entry) {
        entry = clay_json_object();
        clay_json_array_push(entries, entry);
    }
    clay_json_object_set(entry, "slug", clay_json_string(slug));
    clay_json_object_set(entry, "type", clay_json_string(type && *type ? type : "note"));
    clay_json_object_set(entry, "summary", clay_json_string(summary ? summary : ""));
    clay_json_object_set(entry, "updated_at", clay_json_number((double)clay_time_now()));
    return save_manifest(root);
}

char *clay_memory_read(const char *slug) {
    if (!clay_memory_valid_slug(slug)) return NULL;
    char *path = entry_path(slug);
    if (!path) return NULL;
    char *content = read_file(path);
    free(path);
    return content;
}

int clay_memory_delete(const char *slug) {
    if (!clay_memory_valid_slug(slug)) return -1;
    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    for (size_t i = 0; i < clay_json_array_count(entries); i++) {
        ClayJson *entry = clay_json_array_get(entries, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(entry, "slug")), slug) == 0) {
            clay_json_array_remove(entries, i);
            break;
        }
    }
    char *path = entry_path(slug);
    if (path) remove(path);
    free(path);
    return save_manifest(root);
}

char *clay_memory_index(void) {
    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    ClayStr out;
    clay_str_init(&out);
    for (size_t i = 0; i < clay_json_array_count(entries); i++) {
        ClayJson *entry = clay_json_array_get(entries, i);
        clay_str_printf(&out, "- %s [%s]: %s\n", clay_json_string_value(clay_json_object_get(entry, "slug")),
                        clay_json_string_value(clay_json_object_get(entry, "type")),
                        clay_json_string_value(clay_json_object_get(entry, "summary")));
    }
    clay_json_free(root);
    return out.data;
}

void clay_memory_list(ClayArray *entries) {
    clay_array_init(entries, sizeof(ClayMemoryEntry));
    ClayJson *root = load_manifest();
    ClayJson *json_entries = clay_json_object_get(root, "entries");
    for (size_t i = 0; i < clay_json_array_count(json_entries); i++) {
        ClayJson *entry = clay_json_array_get(json_entries, i);
        ClayMemoryEntry item;
        item.slug = strdup(clay_json_string_value(clay_json_object_get(entry, "slug")));
        item.type = strdup(clay_json_string_value(clay_json_object_get(entry, "type")));
        item.summary = strdup(clay_json_string_value(clay_json_object_get(entry, "summary")));
        item.updated_at = (long long)clay_json_number_value(clay_json_object_get(entry, "updated_at"));
        clay_array_push_val(entries, &item);
    }
    clay_json_free(root);
}

void clay_memory_entries_free(ClayArray *entries) {
    for (size_t i = 0; i < entries->count; i++) {
        ClayMemoryEntry *entry = clay_array_get(entries, i);
        free(entry->slug);
        free(entry->type);
        free(entry->summary);
    }
    clay_array_free(entries);
}
