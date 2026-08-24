#include "clay/skill.h"

#include "clay/json.h"
#include "clay/str.h"
#include "clay/storage.h"
#include "clay/term.h"

#include <stdlib.h>
#include <string.h>

static char *index_path(void) {
    return clay_storage_path("skills/index.json");
}

#define CLAY_SKILL_MANIFEST_LIMIT (1024 * 1024)
#define CLAY_SKILL_GIT_OUTPUT_LIMIT (16 * 1024)

static int looks_like_git_source(const char *path) {
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0 ||
        strncmp(path, "git://", 6) == 0 || strncmp(path, "ssh://", 6) == 0 ||
        strncmp(path, "file://", 7) == 0 || strncmp(path, "git@", 4) == 0)
        return 1;
    size_t len = strlen(path);
    return len > 4 && strcmp(path + len - 4, ".git") == 0;
}

/* Slug for the clone's directory name under ~/.clay/skills/sources - not
   the skill's own name (that comes from SKILL.md's frontmatter, parsed
   after the clone lands). Malloc'd. */
static char *git_url_slug(const char *url) {
    const char *base = strrchr(url, '/');
    base = base ? base + 1 : url;
    size_t len = strlen(base);
    if (len > 4 && strcmp(base + len - 4, ".git") == 0) len -= 4;
    ClayStr slug;
    clay_str_init(&slug);
    for (size_t i = 0; i < len; i++) {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) clay_str_push_char(&slug, c);
        else if (c >= 'A' && c <= 'Z') clay_str_push_char(&slug, (char)(c - 'A' + 'a'));
        else clay_str_push_char(&slug, '-');
    }
    if (slug.len == 0) clay_str_push(&slug, "skill");
    return slug.data;
}

/* Clones `url` into ~/.clay/skills/sources/<slug> with real `git clone`
   (git-shelled-out, same as clay/checkpoint.h - no libgit2), or updates it
   with `git pull --ff-only` if that clone already exists. Malloc'd
   absolute path to the local clone; NULL if git failed. */
static char *git_sync(const char *url) {
    if (clay_storage_ensure_dir("skills/sources") != 0) return NULL;
    char *sources_dir = clay_storage_path("skills/sources");
    if (!sources_dir) return NULL;
    char *slug = git_url_slug(url);
    ClayStr dest;
    clay_str_init(&dest);
    clay_str_printf(&dest, "%s/%s", sources_dir, slug);
    free(sources_dir);
    free(slug);

    ClayStr command;
    clay_str_init(&command);
    if (clay_term_is_dir(dest.data)) {
        clay_str_push(&command, "git -C ");
        clay_term_shell_quote(&command, dest.data);
        clay_str_push(&command, " pull --ff-only --quiet");
    } else {
        clay_str_push(&command, "git clone --depth 1 --quiet ");
        clay_term_shell_quote(&command, url);
        clay_str_push_char(&command, ' ');
        clay_term_shell_quote(&command, dest.data);
    }

    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;
    int truncated = 0;
    int rc = clay_term_shell_exec(command.data, &output, CLAY_SKILL_GIT_OUTPUT_LIMIT, &exit_code, &truncated);
    clay_str_free(&command);
    clay_str_free(&output);
    if (rc != 0 || exit_code != 0) {
        clay_str_free(&dest);
        return NULL;
    }
    return dest.data;
}
#define CLAY_SKILL_FILE_LIMIT (4 * 1024 * 1024)

static ClayJson *load_manifest(void) {
    char *path = index_path();
    ClayJson *root = path ? clay_storage_read_json(path, CLAY_SKILL_MANIFEST_LIMIT) : NULL;
    free(path);
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
    if (clay_storage_ensure_dir("skills") != 0) {
        clay_json_free(root);
        return -1;
    }
    char *path = index_path();
    if (!path) {
        clay_json_free(root);
        return -1;
    }
    int ok = clay_storage_write_json_atomic_private(path, root) == 0;
    free(path);
    clay_json_free(root);
    return ok ? 0 : -1;
}

static ClayJson *find_entry(ClayJson *entries, const char *name) {
    for (size_t i = 0; i < clay_json_array_count(entries); i++) {
        ClayJson *entry = clay_json_array_get(entries, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(entry, "name")), name) == 0) return entry;
    }
    return NULL;
}

int clay_skill_valid_name(const char *name) {
    if (!name || !*name) return 0;
    size_t len = strlen(name);
    if (len > 64) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

/* Splits `text` (a SKILL.md's contents) into its "---"-delimited YAML
   frontmatter and body. Only the flat "key: value" lines this format
   actually needs are parsed - no general YAML. NULL name/description if
   the frontmatter is missing or doesn't set them; body always points into
   `text` (not a copy). */
static void parse_frontmatter(const char *text, char **name_out, char **description_out, const char **body_out) {
    *name_out = NULL;
    *description_out = NULL;
    *body_out = text;

    if (strncmp(text, "---", 3) != 0) return;
    const char *cursor = text + 3;
    while (*cursor == '\r') cursor++;
    if (*cursor != '\n') return;
    cursor++;

    const char *end = strstr(cursor, "\n---");
    if (!end) return;

    const char *line = cursor;
    while (line < end) {
        const char *newline = memchr(line, '\n', (size_t)(end - line));
        size_t line_len = newline ? (size_t)(newline - line) : (size_t)(end - line);
        const char *colon = memchr(line, ':', line_len);
        if (colon) {
            size_t key_len = (size_t)(colon - line);
            const char *value = colon + 1;
            size_t value_len = line_len - key_len - 1;
            while (value_len > 0 && *value == ' ') { value++; value_len--; }
            while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\r')) value_len--;
            char **target = NULL;
            if (key_len == 4 && strncmp(line, "name", 4) == 0) target = name_out;
            else if (key_len == 11 && strncmp(line, "description", 11) == 0) target = description_out;
            if (target) {
                free(*target);
                *target = malloc(value_len + 1);
                memcpy(*target, value, value_len);
                (*target)[value_len] = '\0';
            }
        }
        line = newline ? newline + 1 : end;
    }

    const char *body = end + 4; /* past "\n---" */
    while (*body == '\r') body++;
    if (*body == '\n') body++;
    *body_out = body;
}

/* Resolves `path` (a skill directory or a direct SKILL.md path) to the
   directory holding SKILL.md and that file's malloc'd content. 0 on
   success. */
static int resolve_skill_md(const char *path, char **dir_out, char **content_out) {
    ClayStr candidate;
    clay_str_init(&candidate);
    size_t len = strlen(path);
    if (len >= 8 && strcmp(path + len - 8, "SKILL.md") == 0) {
        clay_str_push(&candidate, path);
    } else {
        clay_str_push(&candidate, path);
        if (len > 0 && path[len - 1] != '/') clay_str_push_char(&candidate, '/');
        clay_str_push(&candidate, "SKILL.md");
    }

    ClayStr content;
    if (clay_storage_read_limited(candidate.data, CLAY_SKILL_FILE_LIMIT, &content) != 0) {
        clay_str_free(&candidate);
        return -1;
    }

    ClayStr dir;
    clay_str_init(&dir);
    clay_str_push_n(&dir, candidate.data, candidate.len - strlen("SKILL.md"));
    if (dir.len > 0 && dir.data[dir.len - 1] == '/') dir.data[--dir.len] = '\0';
    clay_str_free(&candidate);

    *dir_out = dir.data;
    *content_out = content.data;
    return 0;
}

int clay_skill_install(const char *path, const char *name_override) {
    char *cloned_dir = NULL;
    if (looks_like_git_source(path)) {
        cloned_dir = git_sync(path);
        if (!cloned_dir) return -1;
        path = cloned_dir;
    }

    char *dir = NULL;
    char *content = NULL;
    int resolved = resolve_skill_md(path, &dir, &content);
    free(cloned_dir);
    if (resolved != 0) return -1;

    /* Store an absolute path - relative to the cwd at install time is
       meaningless once clay_skill_read runs in a later session. Already
       absolute for a fresh clone (under ~/.clay), but a local path or a
       subdirectory picked from an existing clone might not be. */
    char *absolute_dir = clay_term_resolve_path(dir);
    if (absolute_dir) {
        free(dir);
        dir = absolute_dir;
    }

    char *fm_name = NULL;
    char *fm_description = NULL;
    const char *body;
    parse_frontmatter(content, &fm_name, &fm_description, &body);

    const char *name = (name_override && *name_override) ? name_override : fm_name;
    int ok = name && clay_skill_valid_name(name);
    if (ok) {
        ClayJson *root = load_manifest();
        ClayJson *entries = clay_json_object_get(root, "entries");
        ClayJson *entry = find_entry(entries, name);
        if (!entry) {
            entry = clay_json_object();
            clay_json_array_push(entries, entry);
        }
        clay_json_object_set(entry, "name", clay_json_string(name));
        clay_json_object_set(entry, "description", clay_json_string(fm_description ? fm_description : ""));
        clay_json_object_set(entry, "path", clay_json_string(dir));
        if (!clay_json_object_get(entry, "enabled")) clay_json_object_set(entry, "enabled", clay_json_bool(1));
        ok = save_manifest(root) == 0;
    }

    free(dir);
    free(content);
    free(fm_name);
    free(fm_description);
    return ok ? 0 : -1;
}

int clay_skill_remove(const char *name) {
    if (!clay_skill_valid_name(name)) return -1;
    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    for (size_t i = 0; i < clay_json_array_count(entries); i++) {
        ClayJson *entry = clay_json_array_get(entries, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(entry, "name")), name) == 0) {
            clay_json_array_remove(entries, i);
            break;
        }
    }
    return save_manifest(root);
}

int clay_skill_set_enabled(const char *name, int enabled) {
    if (!clay_skill_valid_name(name)) return -1;
    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    ClayJson *entry = find_entry(entries, name);
    if (!entry) {
        clay_json_free(root);
        return -1;
    }
    clay_json_object_set(entry, "enabled", clay_json_bool(enabled));
    return save_manifest(root);
}

char *clay_skill_index(void) {
    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    ClayStr out;
    clay_str_init(&out);
    for (size_t i = 0; i < clay_json_array_count(entries); i++) {
        ClayJson *entry = clay_json_array_get(entries, i);
        ClayJson *enabled = clay_json_object_get(entry, "enabled");
        if (enabled && clay_json_type(enabled) == CLAY_JSON_BOOL && !clay_json_bool_value(enabled)) continue;
        clay_str_printf(&out, "- %s: %s\n", clay_json_string_value(clay_json_object_get(entry, "name")),
                        clay_json_string_value(clay_json_object_get(entry, "description")));
    }
    clay_json_free(root);
    return out.data;
}

char *clay_skill_read(const char *name) {
    if (!clay_skill_valid_name(name)) return NULL;
    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    ClayJson *entry = find_entry(entries, name);
    if (!entry) {
        clay_json_free(root);
        return NULL;
    }
    ClayJson *enabled = clay_json_object_get(entry, "enabled");
    if (enabled && clay_json_type(enabled) == CLAY_JSON_BOOL && !clay_json_bool_value(enabled)) {
        clay_json_free(root);
        return NULL;
    }
    char *dir = strdup(clay_json_string_value(clay_json_object_get(entry, "path")));
    clay_json_free(root);

    char *resolved_dir = NULL;
    char *content = NULL;
    if (resolve_skill_md(dir, &resolved_dir, &content) != 0) {
        free(dir);
        return NULL;
    }
    free(dir);

    char *fm_name = NULL;
    char *fm_description = NULL;
    const char *body;
    parse_frontmatter(content, &fm_name, &fm_description, &body);

    ClayStr out;
    clay_str_init(&out);
    clay_str_printf(&out, "Skill directory: %s\n\n%s", resolved_dir, body);

    free(resolved_dir);
    free(content);
    free(fm_name);
    free(fm_description);
    return out.data;
}

void clay_skill_list(ClayArray *out) {
    clay_array_init(out, sizeof(ClaySkillEntry));
    ClayJson *root = load_manifest();
    ClayJson *entries = clay_json_object_get(root, "entries");
    for (size_t i = 0; i < clay_json_array_count(entries); i++) {
        ClayJson *entry = clay_json_array_get(entries, i);
        ClayJson *enabled = clay_json_object_get(entry, "enabled");
        ClaySkillEntry item;
        item.name = strdup(clay_json_string_value(clay_json_object_get(entry, "name")));
        item.description = strdup(clay_json_string_value(clay_json_object_get(entry, "description")));
        item.path = strdup(clay_json_string_value(clay_json_object_get(entry, "path")));
        item.enabled = !(enabled && clay_json_type(enabled) == CLAY_JSON_BOOL && !clay_json_bool_value(enabled));
        clay_array_push_val(out, &item);
    }
    clay_json_free(root);
}

void clay_skill_entries_free(ClayArray *entries) {
    for (size_t i = 0; i < entries->count; i++) {
        ClaySkillEntry *entry = clay_array_get(entries, i);
        free(entry->name);
        free(entry->description);
        free(entry->path);
    }
    clay_array_free(entries);
}
