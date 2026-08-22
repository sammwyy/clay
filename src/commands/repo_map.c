#include "context.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Aider's repo-map ranks a symbol graph built with tree-sitter + PageRank.
   The parser isn't the valuable part - the ranking is. This gets most of
   the parsing value from ctags when it's installed (accurate, many
   languages), degrading to a handful of per-language line heuristics when
   it isn't - and replaces PageRank with a single-pass reference count
   (how often each defined name appears elsewhere in the tree), which is
   most of the ranking signal without building a graph. */

#define CLAY_REPO_MAP_MAX_RESULTS 150
#define CLAY_REPO_MAP_CTAGS_CAPTURE_LIMIT (2 * 1024 * 1024)
#define CLAY_REPO_MAP_MAX_SYMBOLS 4000

typedef struct {
    char *name;
    char *kind;
    char *file;
    int line;
    long rank;
} ClayRepoSymbol;

static char *dup_range(const char *start, size_t len) {
    char *copy = malloc(len + 1);
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static void next_field(const char *p, const char *end, const char **field_start, size_t *field_len,
                       const char **next) {
    while (p < end && *p == ' ') p++;
    *field_start = p;
    while (p < end && *p != ' ') p++;
    *field_len = (size_t)(p - *field_start);
    *next = p;
}

/* Parses `ctags -x` output: "name kind line file <source line...>" per
   line, whitespace-separated. Returns 1 (and appends to `symbols`) if the
   command ran and produced at least one entry, 0 if ctags isn't installed
   or the run failed - the caller falls back to the heuristic scan. */
static int try_ctags(ClayCommands *commands, const char *workspace_dir, ClayArray *symbols) {
    char *scratch_dir = clay_chat_scratch_dir(commands->chat);
    ClaySandboxConfig sandbox = {
        .mode = commands->sandbox_mode,
        .workspace_dir = workspace_dir,
        .scratch_dir = scratch_dir,
    };
    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;
    int truncated = 0;
    int rc = clay_sandbox_exec(
        &sandbox, "ctags -x -R --exclude=.git --exclude=node_modules --exclude=build --exclude=build-win .", &output,
        CLAY_REPO_MAP_CTAGS_CAPTURE_LIMIT, &exit_code, &truncated);
    free(scratch_dir);
    if (rc != 0 || exit_code != 0 || output.len == 0) {
        clay_str_free(&output);
        return 0;
    }

    const char *p = output.data;
    const char *end = output.data + output.len;
    while (p < end && symbols->count < CLAY_REPO_MAP_MAX_SYMBOLS) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;

        const char *name_start, *kind_start, *line_start, *file_start;
        size_t name_len, kind_len, line_len, file_len;
        const char *cursor = p;
        next_field(cursor, line_end, &name_start, &name_len, &cursor);
        next_field(cursor, line_end, &kind_start, &kind_len, &cursor);
        next_field(cursor, line_end, &line_start, &line_len, &cursor);
        next_field(cursor, line_end, &file_start, &file_len, &cursor);
        if (name_len > 0 && kind_len > 0 && line_len > 0 && file_len > 0) {
            char *line_str = dup_range(line_start, line_len);
            ClayRepoSymbol symbol = {dup_range(name_start, name_len), dup_range(kind_start, kind_len),
                                     dup_range(file_start, file_len), atoi(line_str), 0};
            free(line_str);
            clay_array_push_val(symbols, &symbol);
        }
        p = line_end < end ? line_end + 1 : end;
    }
    clay_str_free(&output);
    return symbols->count > 0;
}

static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Top-level (column 0) definitions only, matching the language conventions
   most files in that language actually follow - not a real parser. */
static int match_c(const char *line, const char **name_start, size_t *name_len, const char **kind) {
    if (strncmp(line, "#define ", 8) == 0) {
        const char *p = line + 8;
        const char *start = p;
        while (is_ident_char(*p)) p++;
        if (p == start) return 0;
        *name_start = start;
        *name_len = (size_t)(p - start);
        *kind = "macro";
        return 1;
    }
    const char *struct_kw = strstr(line, "struct ");
    if (struct_kw && (struct_kw == line || strncmp(line, "typedef struct", 14) == 0)) {
        const char *p = struct_kw + 7;
        const char *start = p;
        while (is_ident_char(*p)) p++;
        if (p > start) {
            *name_start = start;
            *name_len = (size_t)(p - start);
            *kind = "struct";
            return 1;
        }
    }
    if (strncmp(line, "enum ", 5) == 0) {
        const char *p = line + 5;
        const char *start = p;
        while (is_ident_char(*p)) p++;
        if (p > start) {
            *name_start = start;
            *name_len = (size_t)(p - start);
            *kind = "enum";
            return 1;
        }
    }
    /* Function definition: top-level, '(' before any ';', brace on the
       same line (this codebase's own K&R style, and common elsewhere). */
    const char *paren = strchr(line, '(');
    const char *semi = strchr(line, ';');
    size_t len = strlen(line);
    if (paren && (!semi || semi > paren) && len > 0 && line[len - 1] == '{') {
        const char *end = paren;
        while (end > line && *(end - 1) == ' ') end--;
        const char *start = end;
        while (start > line && is_ident_char(*(start - 1))) start--;
        if (start < end) {
            *name_start = start;
            *name_len = (size_t)(end - start);
            *kind = "function";
            return 1;
        }
    }
    return 0;
}

static int match_python(const char *line, const char **name_start, size_t *name_len, const char **kind) {
    const char *prefix;
    if (strncmp(line, "def ", 4) == 0) {
        prefix = line + 4;
        *kind = "function";
    } else if (strncmp(line, "class ", 6) == 0) {
        prefix = line + 6;
        *kind = "class";
    } else return 0;
    const char *start = prefix;
    const char *p = start;
    while (is_ident_char(*p)) p++;
    if (p == start) return 0;
    *name_start = start;
    *name_len = (size_t)(p - start);
    return 1;
}

static int match_js(const char *line, const char **name_start, size_t *name_len, const char **kind) {
    if (strncmp(line, "export default ", 15) == 0) line += 15;
    else if (strncmp(line, "export ", 7) == 0) line += 7;
    const char *prefix;
    if (strncmp(line, "async function ", 15) == 0) {
        prefix = line + 15;
        *kind = "function";
    } else if (strncmp(line, "function ", 9) == 0) {
        prefix = line + 9;
        *kind = "function";
    } else if (strncmp(line, "class ", 6) == 0) {
        prefix = line + 6;
        *kind = "class";
    } else if (strncmp(line, "const ", 6) == 0) {
        prefix = line + 6;
        *kind = "const";
    } else return 0;
    const char *start = prefix;
    const char *p = start;
    while (is_ident_char(*p) || *p == '$') p++;
    if (p == start) return 0;
    *name_start = start;
    *name_len = (size_t)(p - start);
    return 1;
}

static int match_go(const char *line, const char **name_start, size_t *name_len, const char **kind) {
    if (strncmp(line, "func ", 5) == 0) {
        const char *p = line + 5;
        if (*p == '(') {
            p = strchr(p, ')');
            if (!p) return 0;
            p++;
            while (*p == ' ') p++;
        }
        const char *start = p;
        while (is_ident_char(*p)) p++;
        if (p == start) return 0;
        *name_start = start;
        *name_len = (size_t)(p - start);
        *kind = "function";
        return 1;
    }
    if (strncmp(line, "type ", 5) == 0) {
        const char *p = line + 5;
        const char *start = p;
        while (is_ident_char(*p)) p++;
        if (p == start) return 0;
        *name_start = start;
        *name_len = (size_t)(p - start);
        *kind = "type";
        return 1;
    }
    return 0;
}

static int match_rust(const char *line, const char **name_start, size_t *name_len, const char **kind) {
    static const struct {
        const char *prefix;
        const char *kind;
    } PREFIXES[] = {
        {"pub fn ", "function"}, {"fn ", "function"}, {"pub struct ", "struct"}, {"struct ", "struct"},
        {"pub enum ", "enum"},   {"enum ", "enum"},   {"pub trait ", "trait"},   {"trait ", "trait"},
    };
    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
        size_t plen = strlen(PREFIXES[i].prefix);
        if (strncmp(line, PREFIXES[i].prefix, plen) != 0) continue;
        const char *start = line + plen;
        const char *p = start;
        while (is_ident_char(*p)) p++;
        if (p == start) return 0;
        *name_start = start;
        *name_len = (size_t)(p - start);
        *kind = PREFIXES[i].kind;
        return 1;
    }
    return 0;
}

static int match_line(const char *ext, const char *line, const char **name_start, size_t *name_len,
                      const char **kind) {
    if (line[0] == ' ' || line[0] == '\t' || line[0] == '\0') return 0; /* top-level only */
    if (strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0) return match_c(line, name_start, name_len, kind);
    if (strcmp(ext, "py") == 0) return match_python(line, name_start, name_len, kind);
    if (strcmp(ext, "js") == 0 || strcmp(ext, "jsx") == 0 || strcmp(ext, "mjs") == 0 || strcmp(ext, "ts") == 0 ||
        strcmp(ext, "tsx") == 0) {
        return match_js(line, name_start, name_len, kind);
    }
    if (strcmp(ext, "go") == 0) return match_go(line, name_start, name_len, kind);
    if (strcmp(ext, "rs") == 0) return match_rust(line, name_start, name_len, kind);
    return 0;
}

static void heuristic_scan(const char *workspace_dir, ClayArray *files, ClayArray *symbols) {
    for (size_t i = 0; i < files->count && symbols->count < CLAY_REPO_MAP_MAX_SYMBOLS; i++) {
        const char *rel = *(char **)clay_array_get(files, i);
        const char *ext = strrchr(rel, '.');
        ext = ext ? ext + 1 : "";
        if (strcmp(ext, "c") != 0 && strcmp(ext, "h") != 0 && strcmp(ext, "py") != 0 && strcmp(ext, "js") != 0 &&
            strcmp(ext, "jsx") != 0 && strcmp(ext, "mjs") != 0 && strcmp(ext, "ts") != 0 && strcmp(ext, "tsx") != 0 &&
            strcmp(ext, "go") != 0 && strcmp(ext, "rs") != 0) {
            continue;
        }

        ClayStr abs;
        clay_str_init(&abs);
        clay_str_printf(&abs, "%s/%s", workspace_dir, rel);
        FILE *file = fopen(abs.data, "rb");
        clay_str_free(&abs);
        if (!file) continue;

        ClayStr line;
        clay_str_init(&line);
        int line_no = 0;
        int ch;
        do {
            ch = fgetc(file);
            if (ch == '\n' || ch == EOF) {
                line_no++;
                const char *name_start;
                size_t name_len;
                const char *kind;
                if (match_line(ext, line.data, &name_start, &name_len, &kind)) {
                    ClayRepoSymbol symbol = {dup_range(name_start, name_len), strdup(kind), strdup(rel), line_no, 0};
                    clay_array_push_val(symbols, &symbol);
                }
                clay_str_clear(&line);
            } else if (ch != '\r') {
                clay_str_push_char(&line, (char)ch);
            }
        } while (ch != EOF && symbols->count < CLAY_REPO_MAP_MAX_SYMBOLS);
        clay_str_free(&line);
        fclose(file);
    }
}

static void free_counts(const char *key, void *value, void *ctx) {
    (void)key;
    (void)ctx;
    free(value);
}

/* Reference-count ranking: tokenize every scanned file once, bumping a
   counter for each identifier that matches a known symbol name. Symbols
   sharing a name share a rank - good enough to surface what's widely used
   without per-definition disambiguation. */
static void rank_symbols(const char *workspace_dir, ClayArray *files, ClayArray *symbols) {
    ClayMap *counts = clay_map_create();
    for (size_t i = 0; i < symbols->count; i++) {
        ClayRepoSymbol *symbol = clay_array_get(symbols, i);
        if (clay_map_get(counts, symbol->name)) continue;
        long *count = malloc(sizeof(long));
        *count = 0;
        clay_map_set(counts, symbol->name, count);
    }

    for (size_t i = 0; i < files->count; i++) {
        const char *rel = *(char **)clay_array_get(files, i);
        ClayStr abs;
        clay_str_init(&abs);
        clay_str_printf(&abs, "%s/%s", workspace_dir, rel);
        FILE *file = fopen(abs.data, "rb");
        clay_str_free(&abs);
        if (!file) continue;

        ClayStr token;
        clay_str_init(&token);
        int ch;
        do {
            ch = fgetc(file);
            if (is_ident_char(ch == EOF ? '\0' : (char)ch)) {
                clay_str_push_char(&token, (char)ch);
                continue;
            }
            if (token.len > 0) {
                long *count = clay_map_get(counts, token.data);
                if (count) (*count)++;
                clay_str_clear(&token);
            }
        } while (ch != EOF);
        clay_str_free(&token);
        fclose(file);
    }

    for (size_t i = 0; i < symbols->count; i++) {
        ClayRepoSymbol *symbol = clay_array_get(symbols, i);
        long *count = clay_map_get(counts, symbol->name);
        symbol->rank = count ? *count : 0;
    }

    clay_map_foreach(counts, free_counts, NULL);
    clay_map_destroy(counts);
}

static int compare_rank_desc(const void *a, const void *b) {
    const ClayRepoSymbol *symbol_a = a;
    const ClayRepoSymbol *symbol_b = b;
    if (symbol_b->rank != symbol_a->rank) return symbol_b->rank > symbol_a->rank ? 1 : -1;
    return strcmp(symbol_a->name, symbol_b->name);
}

ClayJson *clay_fs_tool_repo_map(const ClayJson *arguments, void *userdata) {
    (void)arguments;
    ClayCommands *commands = userdata;
    char *workspace_dir = clay_term_cwd();

    ClayArray symbols;
    clay_array_init(&symbols, sizeof(ClayRepoSymbol));
    const char *source = "ctags";
    if (!try_ctags(commands, workspace_dir, &symbols)) {
        source = "heuristic";
        ClayArray files;
        clay_array_init(&files, sizeof(char *));
        int truncated = 0;
        clay_fs_walk_files(workspace_dir, "", "*", &files, &truncated);
        heuristic_scan(workspace_dir, &files, &symbols);
        for (size_t i = 0; i < files.count; i++) free(*(char **)clay_array_get(&files, i));
        clay_array_free(&files);
    }

    ClayJson *result = clay_json_object();
    if (symbols.count == 0) {
        free(workspace_dir);
        clay_array_free(&symbols);
        clay_json_object_set(result, "ok", clay_json_bool(1));
        clay_json_object_set(result, "output",
                             clay_json_string("(no recognized top-level definitions found)"));
        clay_json_object_set(result, "output_truncated", clay_json_bool(0));
        return result;
    }

    ClayArray rank_files;
    clay_array_init(&rank_files, sizeof(char *));
    int rank_truncated = 0;
    clay_fs_walk_files(workspace_dir, "", "*", &rank_files, &rank_truncated);
    rank_symbols(workspace_dir, &rank_files, &symbols);
    for (size_t i = 0; i < rank_files.count; i++) free(*(char **)clay_array_get(&rank_files, i));
    clay_array_free(&rank_files);

    qsort(symbols.data, symbols.count, sizeof(ClayRepoSymbol), compare_rank_desc);

    size_t shown = symbols.count < CLAY_REPO_MAP_MAX_RESULTS ? symbols.count : CLAY_REPO_MAP_MAX_RESULTS;
    ClayStr out;
    clay_str_init(&out);
    for (size_t i = 0; i < shown; i++) {
        ClayRepoSymbol *symbol = clay_array_get(&symbols, i);
        clay_str_printf(&out, "%s:%d\t%s %s\t(refs: %ld)\n", symbol->file, symbol->line, symbol->kind, symbol->name,
                        symbol->rank);
    }
    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(out.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(symbols.count > shown));
    clay_json_object_set(result, "source", clay_json_string(source));
    clay_str_free(&out);

    for (size_t i = 0; i < symbols.count; i++) {
        ClayRepoSymbol *symbol = clay_array_get(&symbols, i);
        free(symbol->name);
        free(symbol->kind);
        free(symbol->file);
    }
    clay_array_free(&symbols);
    free(workspace_dir);
    return result;
}

ClayJson *clay_fs_tool_repo_map_schema(void) {
    ClayJson *properties = clay_json_object();
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", clay_json_array());
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}
