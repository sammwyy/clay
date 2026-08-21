#include "context.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_FS_READ_LIMIT (256 * 1024)
#define CLAY_FS_GREP_LIMIT (64 * 1024)      /* shown inline to the model */
#define CLAY_FS_GREP_CAPTURE_LIMIT (4 * 1024 * 1024) /* captured, for the scratch dump */
#define CLAY_FS_GLOB_MAX_MATCHES 500
#define CLAY_FS_READ_SUGGESTION_LIMIT 5

/* Resolves `path` (relative to the workspace, or absolute) to a normalized
   absolute path in `abs_out`. Returns 0 if the result stays inside
   `workspace_dir`, -1 if it would escape it. */
static int resolve_workspace_path(const char *workspace_dir, const char *path, ClayStr *abs_out) {
    ClayStr joined;
    clay_str_init(&joined);
    if (path[0] == '/') clay_str_push(&joined, path);
    else {
        clay_str_push(&joined, workspace_dir);
        clay_str_push_char(&joined, '/');
        clay_str_push(&joined, path);
    }

    char *dup = strdup(joined.data);
    clay_str_free(&joined);
    ClayArray stack;
    clay_array_init(&stack, sizeof(char *));
    char *p = dup;
    while (*p == '/') p++;
    char *token = p;
    for (;; p++) {
        if (*p == '/' || *p == '\0') {
            int end = *p == '\0';
            *p = '\0';
            if (*token && strcmp(token, ".") != 0) {
                if (strcmp(token, "..") == 0) {
                    if (stack.count > 0) clay_array_remove(&stack, stack.count - 1);
                } else clay_array_push_val(&stack, &token);
            }
            if (end) break;
            token = p + 1;
        }
    }
    clay_str_init(abs_out);
    clay_str_push_char(abs_out, '/');
    for (size_t i = 0; i < stack.count; i++) {
        if (i > 0) clay_str_push_char(abs_out, '/');
        clay_str_push(abs_out, *(char **)clay_array_get(&stack, i));
    }
    clay_array_free(&stack);
    free(dup);

    size_t wlen = strlen(workspace_dir);
    int contained = strncmp(abs_out->data, workspace_dir, wlen) == 0 &&
                    (abs_out->data[wlen] == '/' || abs_out->data[wlen] == '\0');
    return contained ? 0 : -1;
}

static int read_whole_file(const char *path, ClayStr *out) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    clay_str_init(out);
    int ch;
    while ((ch = fgetc(file)) != EOF) clay_str_push_char(out, (char)ch);
    fclose(file);
    return 0;
}

static int write_whole_file(const char *path, const char *data, size_t len) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    size_t written = fwrite(data, 1, len, file);
    int ok = written == len && fclose(file) == 0;
    return ok ? 0 : -1;
}

/* Creates every directory above `abs_path`'s final component. */
static int make_parent_dirs(const char *abs_path) {
    const char *slash = strrchr(abs_path, '/');
    if (!slash || slash == abs_path) return 1;
    ClayStr partial;
    clay_str_init(&partial);
    const char *p = abs_path;
    if (*p == '/') {
        clay_str_push_char(&partial, '/');
        p++;
    }
    int ok = 1;
    while (p < slash) {
        const char *next = strchr(p, '/');
        if (!next || next > slash) next = slash;
        clay_str_push_n(&partial, p, (size_t)(next - p));
        if (partial.len > 0 && clay_term_mkdir(partial.data) != 0) {
            ok = 0;
            break;
        }
        p = next + 1;
        if (p <= slash) clay_str_push_char(&partial, '/');
    }
    clay_str_free(&partial);
    return ok;
}

/* Common path arg handling shared by read/write/edit. On success returns 0
   and fills abs_out/workspace_out (caller frees both); on failure fills
   *result_out with the error tool result to return immediately. */
static int resolve_arg_path(const ClayJson *arguments, ClayStr *abs_out, char **workspace_out, ClayJson **result_out) {
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));
    if (!path || !*path) {
        *result_out = clay_json_object();
        clay_json_object_set(*result_out, "ok", clay_json_bool(0));
        clay_json_object_set(*result_out, "error", clay_json_string("path is required"));
        return -1;
    }
    *workspace_out = clay_term_cwd();
    if (resolve_workspace_path(*workspace_out, path, abs_out) != 0) {
        clay_str_free(abs_out);
        free(*workspace_out);
        *workspace_out = NULL;
        *result_out = clay_json_object();
        clay_json_object_set(*result_out, "ok", clay_json_bool(0));
        clay_json_object_set(*result_out, "error", clay_json_string("path escapes the workspace"));
        return -1;
    }
    return 0;
}

ClayJson *clay_fs_tool_read(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    ClayStr abs;
    char *workspace_dir = NULL;
    ClayJson *result = NULL;
    if (resolve_arg_path(arguments, &abs, &workspace_dir, &result) != 0) return result;

    long offset = (long)clay_json_number_value(clay_json_object_get(arguments, "offset"));
    long limit = (long)clay_json_number_value(clay_json_object_get(arguments, "limit"));
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));

    result = clay_json_object();
    clay_json_object_set(result, "path", clay_json_string(path));
    FILE *file = fopen(abs.data, "rb");
    free(workspace_dir);
    clay_str_free(&abs);
    if (!file) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        ClayStr error;
        clay_str_init(&error);
        clay_str_push(&error, strerror(errno));
        if (errno == ENOENT) {
            const char *base_name = path;
            for (const char *p = path; *p; p++) {
                if (*p == '/') base_name = p + 1;
            }
            if (*base_name) {
                char *ws = clay_term_cwd();
                ClayStr suggest_pattern;
                clay_str_init(&suggest_pattern);
                clay_str_printf(&suggest_pattern, "*%s*", base_name);
                ClayArray matches;
                clay_array_init(&matches, sizeof(char *));
                int suggest_truncated = 0;
                clay_fs_walk_files(ws, "", suggest_pattern.data, &matches, &suggest_truncated);
                clay_str_free(&suggest_pattern);
                free(ws);
                if (matches.count > 0) {
                    clay_str_push(&error, " - did you mean: ");
                    size_t shown = matches.count < CLAY_FS_READ_SUGGESTION_LIMIT ? matches.count : CLAY_FS_READ_SUGGESTION_LIMIT;
                    for (size_t i = 0; i < shown; i++) {
                        if (i > 0) clay_str_push(&error, ", ");
                        clay_str_push(&error, *(char **)clay_array_get(&matches, i));
                    }
                    clay_str_push_char(&error, '?');
                }
                for (size_t i = 0; i < matches.count; i++) free(*(char **)clay_array_get(&matches, i));
                clay_array_free(&matches);
            }
        }
        clay_json_object_set(result, "error", clay_json_string(error.data));
        clay_str_free(&error);
        return result;
    }

    ClayStr formatted;
    clay_str_init(&formatted);
    ClayStr line;
    clay_str_init(&line);
    size_t line_no = 0;
    size_t emitted = 0;
    int truncated = 0;
    int at_line_start = 1;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (at_line_start) {
            line_no++;
            at_line_start = 0;
        }
        if (ch == '\n') {
            if (line_no > (size_t)(offset > 0 ? offset - 1 : 0) && (limit <= 0 || emitted < (size_t)limit)) {
                clay_str_printf(&formatted, "%5zu\t%s\n", line_no, line.data);
                emitted++;
            }
            clay_str_clear(&line);
            at_line_start = 1;
            if (formatted.len > CLAY_FS_READ_LIMIT) {
                truncated = 1;
                break;
            }
            continue;
        }
        clay_str_push_char(&line, (char)ch);
    }
    if (!truncated && !at_line_start && line.len > 0 &&
        line_no > (size_t)(offset > 0 ? offset - 1 : 0) && (limit <= 0 || emitted < (size_t)limit)) {
        clay_str_printf(&formatted, "%5zu\t%s\n", line_no, line.data);
    }
    clay_str_free(&line);
    fclose(file);

    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(formatted.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(truncated));
    clay_json_object_set(result, "total_lines", clay_json_number((double)line_no));
    clay_str_free(&formatted);
    return result;
}

ClayJson *clay_fs_tool_read_schema(void) {
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("File path, relative to the workspace root or absolute."));
    ClayJson *offset = clay_json_object();
    clay_json_object_set(offset, "type", clay_json_string("number"));
    clay_json_object_set(offset, "description", clay_json_string("1-based line number to start from. Omit to start at line 1."));
    ClayJson *limit = clay_json_object();
    clay_json_object_set(limit, "type", clay_json_string("number"));
    clay_json_object_set(limit, "description", clay_json_string("Max number of lines to return. Omit to read the whole file."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "offset", offset);
    clay_json_object_set(properties, "limit", limit);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("path"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

ClayJson *clay_fs_tool_write(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    ClayStr abs;
    char *workspace_dir = NULL;
    ClayJson *result = NULL;
    if (resolve_arg_path(arguments, &abs, &workspace_dir, &result) != 0) return result;
    free(workspace_dir);

    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));
    const char *content = clay_json_string_value(clay_json_object_get(arguments, "content"));
    if (!content) content = "";

    result = clay_json_object();
    clay_json_object_set(result, "path", clay_json_string(path));
    if (!make_parent_dirs(abs.data) || write_whole_file(abs.data, content, strlen(content)) != 0) {
        clay_str_free(&abs);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("could not write file"));
        return result;
    }
    clay_str_free(&abs);

    size_t lines = 1;
    for (const char *p = content; *p; p++) {
        if (*p == '\n') lines++;
    }
    ClayStr summary;
    clay_str_init(&summary);
    clay_str_printf(&summary, "Wrote %zu bytes (%zu lines).", strlen(content), lines);
    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(summary.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    clay_str_free(&summary);
    return result;
}

ClayJson *clay_fs_tool_write_schema(void) {
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description",
                         clay_json_string("File path, relative to the workspace root or absolute. Overwrites if it "
                                          "already exists; parent directories are created as needed."));
    ClayJson *content = clay_json_object();
    clay_json_object_set(content, "type", clay_json_string("string"));
    clay_json_object_set(content, "description", clay_json_string("Full file content to write."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "content", content);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("path"));
    clay_json_array_push(required, clay_json_string("content"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

static size_t count_occurrences(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t needle_len = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

/* Replaces the first occurrence of `needle` (or every occurrence, if
   replace_all) with `repl`, appending the result to `out`. */
static void apply_replacement(ClayStr *out, const char *haystack, const char *needle, const char *repl,
                              int replace_all) {
    size_t needle_len = strlen(needle);
    const char *p = haystack;
    int replaced_one = 0;
    for (;;) {
        const char *match = strstr(p, needle);
        if (!match || (!replace_all && replaced_one)) {
            clay_str_push(out, p);
            return;
        }
        clay_str_push_n(out, p, (size_t)(match - p));
        clay_str_push(out, repl);
        replaced_one = 1;
        p = match + needle_len;
    }
}

static void push_prefixed_lines(ClayStr *out, const char *text, const char *prefix) {
    const char *p = text;
    while (*p) {
        const char *newline = strchr(p, '\n');
        size_t len = newline ? (size_t)(newline - p) : strlen(p);
        clay_str_push(out, prefix);
        clay_str_push_n(out, p, len);
        clay_str_push_char(out, '\n');
        if (!newline) break;
        p = newline + 1;
    }
}

ClayJson *clay_fs_tool_edit(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    const char *old_string = clay_json_string_value(clay_json_object_get(arguments, "oldString"));
    const char *new_string = clay_json_string_value(clay_json_object_get(arguments, "newString"));
    int replace_all = clay_json_bool_value(clay_json_object_get(arguments, "replaceAll"));
    if (!new_string) new_string = "";
    if (!old_string || !*old_string) {
        ClayJson *result = clay_json_object();
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("oldString is required and must be non-empty"));
        return result;
    }

    ClayStr abs;
    char *workspace_dir = NULL;
    ClayJson *result = NULL;
    if (resolve_arg_path(arguments, &abs, &workspace_dir, &result) != 0) return result;
    free(workspace_dir);
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));

    result = clay_json_object();
    clay_json_object_set(result, "path", clay_json_string(path));
    ClayStr content;
    if (read_whole_file(abs.data, &content) != 0) {
        clay_str_free(&abs);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string(strerror(errno)));
        return result;
    }

    size_t occurrences = count_occurrences(content.data, old_string);
    if (occurrences == 0) {
        clay_str_free(&abs);
        clay_str_free(&content);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("oldString was not found in the file"));
        return result;
    }
    if (occurrences > 1 && !replace_all) {
        clay_str_free(&abs);
        clay_str_free(&content);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(
            result, "error",
            clay_json_string("oldString is not unique in the file - include more context, or pass replaceAll"));
        return result;
    }

    ClayStr updated;
    clay_str_init(&updated);
    apply_replacement(&updated, content.data, old_string, new_string, replace_all);
    clay_str_free(&content);

    if (write_whole_file(abs.data, updated.data, updated.len) != 0) {
        clay_str_free(&abs);
        clay_str_free(&updated);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("could not write file"));
        return result;
    }
    clay_str_free(&abs);
    clay_str_free(&updated);

    ClayStr diff;
    clay_str_init(&diff);
    push_prefixed_lines(&diff, old_string, "- ");
    push_prefixed_lines(&diff, new_string, "+ ");
    if (replace_all && occurrences > 1) clay_str_printf(&diff, "(%zu replacements)", occurrences);
    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(diff.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    clay_json_object_set(result, "replacements", clay_json_number((double)(replace_all ? occurrences : 1)));
    clay_str_free(&diff);
    return result;
}

ClayJson *clay_fs_tool_edit_schema(void) {
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("File path, relative to the workspace root or absolute."));
    ClayJson *old_string = clay_json_object();
    clay_json_object_set(old_string, "type", clay_json_string("string"));
    clay_json_object_set(
        old_string, "description",
        clay_json_string("Exact text to replace. Must match byte-for-byte, including whitespace. Include enough "
                         "surrounding context to make it unique in the file unless replaceAll is set."));
    ClayJson *new_string = clay_json_object();
    clay_json_object_set(new_string, "type", clay_json_string("string"));
    clay_json_object_set(new_string, "description", clay_json_string("Replacement text."));
    ClayJson *replace_all = clay_json_object();
    clay_json_object_set(replace_all, "type", clay_json_string("boolean"));
    clay_json_object_set(replace_all, "description",
                         clay_json_string("Replace every occurrence instead of requiring oldString to be unique."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "oldString", old_string);
    clay_json_object_set(properties, "newString", new_string);
    clay_json_object_set(properties, "replaceAll", replace_all);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("path"));
    clay_json_array_push(required, clay_json_string("oldString"));
    clay_json_array_push(required, clay_json_string("newString"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

void clay_fs_walk_files(const char *base_dir, const char *rel_prefix, const char *pattern, ClayArray *matches,
                      int *truncated) {
    if (*truncated || matches->count >= CLAY_FS_GLOB_MAX_MATCHES) {
        *truncated = 1;
        return;
    }
    ClayArray entries;
    if (clay_term_list_entries(base_dir, &entries) != 0) return;
    for (size_t i = 0; i < entries.count; i++) {
        char *name = *(char **)clay_array_get(&entries, i);
        if (strcmp(name, ".git") == 0) continue;

        ClayStr rel;
        clay_str_init(&rel);
        if (*rel_prefix) {
            clay_str_push(&rel, rel_prefix);
            clay_str_push_char(&rel, '/');
        }
        clay_str_push(&rel, name);

        ClayStr full;
        clay_str_init(&full);
        clay_str_push(&full, base_dir);
        clay_str_push_char(&full, '/');
        clay_str_push(&full, name);

        if (clay_term_is_dir(full.data)) {
            clay_fs_walk_files(full.data, rel.data, pattern, matches, truncated);
        } else if (clay_str_wildcard_match(pattern, rel.data)) {
            if (matches->count >= CLAY_FS_GLOB_MAX_MATCHES) *truncated = 1;
            else {
                char *copy = strdup(rel.data);
                clay_array_push_val(matches, &copy);
            }
        }
        clay_str_free(&rel);
        clay_str_free(&full);
    }
    for (size_t i = 0; i < entries.count; i++) free(*(char **)clay_array_get(&entries, i));
    clay_array_free(&entries);
}

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

ClayJson *clay_fs_tool_glob(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    const char *pattern = clay_json_string_value(clay_json_object_get(arguments, "pattern"));
    const char *base = clay_json_string_value(clay_json_object_get(arguments, "path"));
    if (!base || !*base) base = ".";
    ClayJson *result = clay_json_object();
    if (!pattern || !*pattern) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("pattern is required"));
        return result;
    }
    clay_json_object_set(result, "pattern", clay_json_string(pattern));

    char *workspace_dir = clay_term_cwd();
    ClayStr abs_base;
    if (resolve_workspace_path(workspace_dir, base, &abs_base) != 0) {
        free(workspace_dir);
        clay_str_free(&abs_base);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("path escapes the workspace"));
        return result;
    }
    free(workspace_dir);

    ClayArray matches;
    clay_array_init(&matches, sizeof(char *));
    int truncated = 0;
    clay_fs_walk_files(abs_base.data, "", pattern, &matches, &truncated);
    clay_str_free(&abs_base);

    if (matches.count > 0) qsort(matches.data, matches.count, sizeof(char *), compare_strings);
    ClayStr output;
    clay_str_init(&output);
    for (size_t i = 0; i < matches.count; i++) {
        clay_str_push(&output, *(char **)clay_array_get(&matches, i));
        clay_str_push_char(&output, '\n');
        free(*(char **)clay_array_get(&matches, i));
    }
    clay_array_free(&matches);

    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(output.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(truncated));
    clay_str_free(&output);
    return result;
}

ClayJson *clay_fs_tool_glob_schema(void) {
    ClayJson *pattern = clay_json_object();
    clay_json_object_set(pattern, "type", clay_json_string("string"));
    clay_json_object_set(
        pattern, "description",
        clay_json_string("Wildcard pattern matched against each file's path relative to `path` (e.g. \"*.c\", "
                         "\"src/*.c\", \"*/test_*.c\"). '*' matches any run of characters including '/'; '?' "
                         "matches one character."));
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("Directory to search from. Defaults to the workspace root."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "pattern", pattern);
    clay_json_object_set(properties, "path", path);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("pattern"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

ClayJson *clay_fs_tool_grep(const ClayJson *arguments, void *userdata) {
    ClayCommands *commands = userdata;
    const char *pattern = clay_json_string_value(clay_json_object_get(arguments, "pattern"));
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));
    const char *include = clay_json_string_value(clay_json_object_get(arguments, "glob"));
    if (!path || !*path) path = ".";
    ClayJson *result = clay_json_object();
    if (!pattern || !*pattern) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("pattern is required"));
        return result;
    }
    clay_json_object_set(result, "pattern", clay_json_string(pattern));

    ClayStr invocation;
    clay_str_init(&invocation);
    clay_str_push(&invocation, "grep -rn -I --binary-files=without-match -e ");
    clay_term_shell_quote(&invocation, pattern);
    if (include && *include) {
        clay_str_push(&invocation, " --include=");
        clay_term_shell_quote(&invocation, include);
    }
    clay_str_push_char(&invocation, ' ');
    clay_term_shell_quote(&invocation, path);

    char *workspace_dir = clay_term_cwd();
    char *scratch_dir = clay_chat_scratch_dir(commands->chat);
    ClaySandboxConfig sandbox = {
        .mode = commands->sandbox_mode,
        .access = commands->sandbox_access,
        .workspace_dir = workspace_dir,
        .scratch_dir = scratch_dir,
    };
    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;
    int truncated = 0;
    int rc = clay_sandbox_exec(&sandbox, invocation.data, &output, CLAY_FS_GREP_CAPTURE_LIMIT, &exit_code, &truncated);
    free(workspace_dir);
    free(scratch_dir);
    clay_str_free(&invocation);

    /* grep exits 1 for "no matches", which is a normal empty result, and
       >1 for a real error (bad pattern, missing path, ...). */
    int ok = rc == 0 && exit_code <= 1;
    clay_json_object_set(result, "ok", clay_json_bool(ok));
    if (exit_code == 1) {
        clay_json_object_set(result, "output", clay_json_string(""));
        clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    } else if (output.len > CLAY_FS_GREP_LIMIT) {
        char *scratch_path = clay_chat_dump_scratch(commands->chat, "grep", output.data);
        ClayStr preview;
        clay_str_init(&preview);
        clay_str_push_n(&preview, output.data, CLAY_FS_GREP_LIMIT);
        if (scratch_path) clay_str_printf(&preview, "\n... (%zu bytes total, full output at %s)", output.len, scratch_path);
        else clay_str_push(&preview, "\n... (truncated)");
        clay_json_object_set(result, "output", clay_json_string(preview.data));
        clay_json_object_set(result, "output_truncated", clay_json_bool(1));
        if (scratch_path) clay_json_object_set(result, "scratch_path", clay_json_string(scratch_path));
        clay_str_free(&preview);
        free(scratch_path);
    } else {
        clay_json_object_set(result, "output", clay_json_string(output.data));
        clay_json_object_set(result, "output_truncated", clay_json_bool(truncated));
    }
    if (!ok) clay_json_object_set(result, "error", clay_json_string(rc != 0 ? "failed to start grep" : "grep failed"));
    clay_str_free(&output);
    return result;
}

ClayJson *clay_fs_tool_grep_schema(void) {
    ClayJson *pattern = clay_json_object();
    clay_json_object_set(pattern, "type", clay_json_string("string"));
    clay_json_object_set(pattern, "description", clay_json_string("Regular expression to search for (grep -E syntax)."));
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("File or directory to search. Defaults to the workspace root."));
    ClayJson *glob = clay_json_object();
    clay_json_object_set(glob, "type", clay_json_string("string"));
    clay_json_object_set(glob, "description", clay_json_string("Only search files matching this glob, e.g. \"*.c\"."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "pattern", pattern);
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "glob", glob);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("pattern"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}
