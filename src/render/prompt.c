#include "clay/prompt.h"

#include "clay/array.h"
#include "clay/below.h"
#include "clay/color.h"
#include "clay/str.h"
#include "clay/term.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads a line into a growable ClayStr, hands ownership to the caller.
   NULL only on EOF with nothing read. */
static char *read_line_dynamic(void) {
    ClayStr s;
    clay_str_init(&s);

    int c;
    int any = 0;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        any = 1;
        clay_str_push_char(&s, (char)c);
    }

    if (c == EOF && !any) {
        clay_str_free(&s);
        return NULL;
    }
    return s.data;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static ClayArray g_history; /* char*, oldest first */
static int g_history_ready = 0;
static int g_last_line_interrupted = 0;

static void ensure_history(void) {
    if (!g_history_ready) {
        clay_array_init(&g_history, sizeof(char *));
        g_history_ready = 1;
    }
}

static void push_history(const char *line) {
    if (!line || line[0] == '\0') return;
    ensure_history();
    if (g_history.count > 0) {
        char *last = *(char **)clay_array_get(&g_history, g_history.count - 1);
        if (strcmp(last, line) == 0) return;
    }
    char *copy = strdup(line);
    clay_array_push_val(&g_history, &copy);
}

size_t clay_prompt_history_count(void) {
    ensure_history();
    return g_history.count;
}

const char *clay_prompt_history_get(size_t index) {
    ensure_history();
    if (index >= g_history.count) return NULL;
    return *(char **)clay_array_get(&g_history, index);
}

void clay_prompt_history_clear(void) {
    ensure_history();
    for (size_t i = 0; i < g_history.count; i++) {
        free(*(char **)clay_array_get(&g_history, i));
    }
    clay_array_clear(&g_history);
}

static char *fallback_prompt_line(void) {
    printf("%s%s>%s ", clay_color(CLAY_GREEN), clay_color(CLAY_BOLD), clay_color(CLAY_RESET));
    fflush(stdout);
    char *line = read_line_dynamic();
    if (line) push_history(line);
    return line;
}

/* A burst of CLAY_KEY_CHAR reads that arrived without a gap collapses into
   one of these instead of being inserted char by char. `start`/`end` are
   the cyan placeholder's byte range in the editor buffer; `text` is the
   real pasted content it stands for. Blocks are kept sorted by `start`
   ascending, matching their order in the buffer. */
#define CLAY_PASTE_MIN_CHARS 4

typedef struct {
    size_t start;
    size_t end;
    char *text;
} ClayPasteBlock;

static void paste_blocks_free(ClayArray *blocks) {
    for (size_t i = 0; i < blocks->count; i++) {
        ClayPasteBlock *b = clay_array_get(blocks, i);
        free(b->text);
    }
    clay_array_clear(blocks);
}

/* Shifts every block at/after `at` by `delta` bytes - keeps the mapping
   correct after an insert or delete elsewhere in the buffer. */
static void paste_blocks_shift(ClayArray *blocks, size_t at, long delta) {
    for (size_t i = 0; i < blocks->count; i++) {
        ClayPasteBlock *b = clay_array_get(blocks, i);
        if (b->start >= at) {
            b->start = (size_t)((long)b->start + delta);
            b->end = (size_t)((long)b->end + delta);
        }
    }
}

/* Index of the block strictly covering byte `idx` (start < idx < end), or
   (size_t)-1. Used to keep the cursor from ever resting inside a
   placeholder - it can only sit before or after one, never mid-escape. */
static size_t paste_block_covering(ClayArray *blocks, size_t idx) {
    for (size_t i = 0; i < blocks->count; i++) {
        ClayPasteBlock *b = clay_array_get(blocks, i);
        if (idx > b->start && idx < b->end) return i;
    }
    return (size_t)-1;
}

/* Index of the block occupying byte `idx` (start <= idx < end), or
   (size_t)-1. Used by backspace: deleting any byte of a placeholder,
   including its trailing ']', removes the whole block. */
static size_t paste_block_at(ClayArray *blocks, size_t idx) {
    for (size_t i = 0; i < blocks->count; i++) {
        ClayPasteBlock *b = clay_array_get(blocks, i);
        if (idx >= b->start && idx < b->end) return i;
    }
    return (size_t)-1;
}

/* Inserts a "[Pasted N chars]" placeholder at *cursor, records the range
   it occupies, and advances *cursor past it. Takes ownership of `text`. */
static void paste_block_insert(ClayStr *buf, ClayArray *blocks, size_t *cursor, char *text, size_t len) {
    ClayStr placeholder;
    clay_str_init(&placeholder);
    clay_str_printf(&placeholder, "%s[Pasted %zu chars]%s", clay_color(CLAY_CYAN), len, clay_color(CLAY_RESET));

    size_t insert_at = 0;
    while (insert_at < blocks->count) {
        ClayPasteBlock *b = clay_array_get(blocks, insert_at);
        if (b->start >= *cursor) break;
        insert_at++;
    }

    paste_blocks_shift(blocks, *cursor, (long)placeholder.len);
    clay_str_insert_n(buf, *cursor, placeholder.data, placeholder.len);

    ClayPasteBlock block;
    block.start = *cursor;
    block.end = *cursor + placeholder.len;
    block.text = text;
    clay_array_insert(blocks, insert_at, &block);

    *cursor = block.end;
    clay_str_free(&placeholder);
}

/* Deletes the byte before *cursor. If it belongs to a paste placeholder,
   the whole block goes at once (wherever it sits, not just at the tail)
   instead of chewing through the placeholder text one byte at a time. */
static void prompt_backspace(ClayStr *buf, ClayArray *blocks, size_t *cursor) {
    if (*cursor == 0) return;

    size_t bi = paste_block_at(blocks, *cursor - 1);
    if (bi != (size_t)-1) {
        ClayPasteBlock *b = clay_array_get(blocks, bi);
        size_t start = b->start, len = b->end - b->start;
        free(b->text);
        clay_array_remove(blocks, bi);
        clay_str_remove_n(buf, start, len);
        paste_blocks_shift(blocks, start, -(long)len);
        *cursor = start;
        return;
    }

    clay_str_remove_n(buf, *cursor - 1, 1);
    paste_blocks_shift(blocks, *cursor, -1);
    (*cursor)--;
}

/* Moves *cursor one byte left/right, jumping clean over a paste
   placeholder instead of stepping into the middle of it. */
static void prompt_cursor_left(ClayStr *buf, ClayArray *blocks, size_t *cursor) {
    (void)buf;
    if (*cursor == 0) return;
    (*cursor)--;
    size_t bi = paste_block_covering(blocks, *cursor);
    if (bi != (size_t)-1) *cursor = ((ClayPasteBlock *)clay_array_get(blocks, bi))->start;
}

static void prompt_cursor_right(ClayStr *buf, ClayArray *blocks, size_t *cursor) {
    if (*cursor >= buf->len) return;
    (*cursor)++;
    size_t bi = paste_block_covering(blocks, *cursor);
    if (bi != (size_t)-1) *cursor = ((ClayPasteBlock *)clay_array_get(blocks, bi))->end;
}

/* Expands placeholders back into their real pasted text to build the line
   actually submitted. Caller frees the result. */
static char *paste_blocks_resolve(const ClayStr *buf, ClayArray *blocks) {
    ClayStr out;
    clay_str_init(&out);

    size_t block_idx = 0;
    size_t i = 0;
    while (i < buf->len) {
        if (block_idx < blocks->count) {
            ClayPasteBlock *b = clay_array_get(blocks, block_idx);
            if (i == b->start) {
                clay_str_push(&out, b->text);
                i = b->end;
                block_idx++;
                continue;
            }
        }
        clay_str_push_char(&out, buf->data[i]);
        i++;
    }

    return out.data;
}

/* Line editor: chars insert at the cursor, backspace deletes before it,
   left/right move it (jumping over paste placeholders whole), up/down
   recall history. A run of CLAY_PASTE_MIN_CHARS+ chars that arrive back
   to back (no blocking wait between them) is treated as a paste: it
   collapses into a single cyan "[Pasted N chars]" placeholder via
   paste_block_insert, so a fast typist isn't mistaken for one. */
static char *interactive_prompt_line(void) {
    ClayStr buf;
    clay_str_init(&buf);

    ClayArray blocks;
    clay_array_init(&blocks, sizeof(ClayPasteBlock));

    size_t cursor = 0;
    int history_pos = -1; /* -1 = editing a fresh line, not browsing history */
    int got_eof = 0;
    int interrupted = 0;

    int pending_valid = 0; /* a key already read while scanning a burst, not yet handled */
    ClayKey pending_key = CLAY_KEY_CHAR;
    char pending_ch = 0;

    clay_term_raw_enable();
    clay_below_set_editing(1);
    clay_below_render(buf.data, cursor);

    for (;;) {
        char ch = 0;
        ClayKey key;
        if (pending_valid) {
            key = pending_key;
            ch = pending_ch;
            pending_valid = 0;
        } else {
            key = clay_term_read_key(&ch);
        }

        if (key == CLAY_KEY_ENTER) {
            break;
        } else if (key == CLAY_KEY_INTERRUPT) {
            interrupted = 1;
            break;
        } else if (key == CLAY_KEY_INTERRUPT || key == CLAY_KEY_EOF) {
            got_eof = 1;
            break;
        } else if (key == CLAY_KEY_BACKSPACE) {
            prompt_backspace(&buf, &blocks, &cursor);
            history_pos = -1;
        } else if (key == CLAY_KEY_LEFT) {
            prompt_cursor_left(&buf, &blocks, &cursor);
        } else if (key == CLAY_KEY_RIGHT) {
            prompt_cursor_right(&buf, &blocks, &cursor);
        } else if (key == CLAY_KEY_UP) {
            size_t count = clay_prompt_history_count();
            if (count > 0) {
                history_pos = (history_pos == -1) ? (int)count - 1 : (history_pos > 0 ? history_pos - 1 : 0);
                clay_str_clear(&buf);
                clay_str_push(&buf, clay_prompt_history_get((size_t)history_pos));
                paste_blocks_free(&blocks);
                cursor = buf.len;
            }
        } else if (key == CLAY_KEY_DOWN) {
            if (history_pos != -1) {
                size_t count = clay_prompt_history_count();
                if ((size_t)(history_pos + 1) < count) {
                    history_pos++;
                    clay_str_clear(&buf);
                    clay_str_push(&buf, clay_prompt_history_get((size_t)history_pos));
                } else {
                    history_pos = -1;
                    clay_str_clear(&buf);
                }
                paste_blocks_free(&blocks);
                cursor = buf.len;
            }
        } else if (key == CLAY_KEY_CHAR) {
            ClayStr burst;
            clay_str_init(&burst);
            clay_str_push_char(&burst, ch);
            while (clay_term_input_pending()) {
                char nch = 0;
                ClayKey nkey = clay_term_read_key(&nch);
                if (nkey != CLAY_KEY_CHAR) {
                    pending_key = nkey;
                    pending_ch = nch;
                    pending_valid = 1;
                    break;
                }
                clay_str_push_char(&burst, nch);
            }

            if (burst.len >= CLAY_PASTE_MIN_CHARS) {
                paste_block_insert(&buf, &blocks, &cursor, burst.data, burst.len);
            } else {
                paste_blocks_shift(&blocks, cursor, (long)burst.len);
                clay_str_insert_n(&buf, cursor, burst.data, burst.len);
                cursor += burst.len;
                clay_str_free(&burst);
            }
            history_pos = -1;
        } else {
            continue;
        }

        clay_below_render(buf.data, cursor);
    }

    clay_below_set_editing(0);
    clay_below_finish();
    clay_term_raw_disable();

    if (interrupted) {
        g_last_line_interrupted = 1;
    }
    if (interrupted || (got_eof && buf.len == 0)) {
        clay_str_free(&buf);
        paste_blocks_free(&blocks);
        clay_array_free(&blocks);
        return NULL;
    }

    char *result = paste_blocks_resolve(&buf, &blocks);
    clay_str_free(&buf);
    paste_blocks_free(&blocks);
    clay_array_free(&blocks);

    push_history(result);
    return result;
}

char *clay_prompt_line(void) {
    g_last_line_interrupted = 0;
    if (!clay_term_is_interactive()) return fallback_prompt_line();
    char *line = interactive_prompt_line();
    if (!line && clay_term_take_interrupt()) g_last_line_interrupted = 1;
    return line;
}

int clay_prompt_was_interrupted(void) {
    return g_last_line_interrupted;
}

static void render_secret_line(const char *question, size_t len) {
    clay_term_clear_line();
    printf("%s%s%s  ", clay_color(CLAY_WHITE), question, clay_color(CLAY_RESET));
    for (size_t i = 0; i < len; i++) fputc('*', stdout);
    fflush(stdout);
}

static char *secret_fallback(const char *question) {
    printf("%s%s%s\n", clay_color(CLAY_WHITE), question, clay_color(CLAY_RESET));
    printf("%s%s>%s ", clay_color(CLAY_GREEN), clay_color(CLAY_BOLD), clay_color(CLAY_RESET));
    fflush(stdout);
    return read_line_dynamic();
}

char *clay_prompt_secret(const char *question) {
    if (!clay_term_is_interactive()) return secret_fallback(question);

    ClayStr buf;
    clay_str_init(&buf);
    int got_eof = 0;

    clay_term_raw_enable();
    render_secret_line(question, buf.len);

    for (;;) {
        char ch = 0;
        ClayKey key = clay_term_read_key(&ch);

        if (key == CLAY_KEY_ENTER) {
            break;
        } else if (key == CLAY_KEY_EOF) {
            got_eof = 1;
            break;
        } else if (key == CLAY_KEY_BACKSPACE) {
            if (buf.len > 0) buf.data[--buf.len] = '\0';
        } else if (key == CLAY_KEY_CHAR) {
            clay_str_push_char(&buf, ch);
        } else {
            continue;
        }

        render_secret_line(question, buf.len);
    }

    fputc('\n', stdout);
    clay_term_raw_disable();

    if (got_eof && buf.len == 0) {
        clay_str_free(&buf);
        return NULL;
    }
    return buf.data;
}

static void render_select_line(const char *question, const ClayChoice *options, int count, int selected) {
    clay_term_clear_line();
    printf("%s%s%s  ", clay_color(CLAY_WHITE), question, clay_color(CLAY_RESET));
    for (int i = 0; i < count; i++) {
        if (i == selected) {
            printf("%s%s[ %s ]%s ", clay_color(CLAY_BOLD), clay_color(CLAY_ORANGE), options[i].title, clay_color(CLAY_RESET));
        } else {
            printf("%s  %s  %s", clay_color(CLAY_GRAY), options[i].title, clay_color(CLAY_RESET));
        }
    }
    fflush(stdout);
}

static int select_fallback(const char *question, const ClayChoice *options, int count, int default_index) {
    printf("%s%s%s\n", clay_color(CLAY_WHITE), question, clay_color(CLAY_RESET));
    for (int i = 0; i < count; i++) {
        printf("  %s%d.%s %s%s", clay_color(CLAY_GRAY), i + 1, clay_color(CLAY_RESET), options[i].title,
               i == default_index ? "  (default)" : "");
        if (options[i].desc) printf("  %s%s%s", clay_color(CLAY_GRAY), options[i].desc, clay_color(CLAY_RESET));
        fputc('\n', stdout);
    }
    printf("%s%s>%s ", clay_color(CLAY_GREEN), clay_color(CLAY_BOLD), clay_color(CLAY_RESET));
    fflush(stdout);

    char *line = read_line_dynamic();
    if (!line) return default_index;

    char *answer = trim(line);
    char *endptr;
    long choice = strtol(answer, &endptr, 10);

    int result = default_index;
    if (*answer != '\0' && *endptr == '\0' && choice >= 1 && choice <= count) {
        result = (int)(choice - 1);
    }
    free(line);
    return result;
}

int clay_prompt_select(const char *question, const ClayChoice *options, int count, int default_index) {
    if (!clay_term_is_interactive()) {
        return select_fallback(question, options, count, default_index);
    }

    int selected = (default_index >= 0 && default_index < count) ? default_index : 0;
    int result = selected;

    clay_term_raw_enable();
    clay_term_hide_cursor();
    render_select_line(question, options, count, selected);

    for (;;) {
        ClayKey key = clay_term_read_key(NULL);
        if (key == CLAY_KEY_LEFT) {
            selected = (selected - 1 + count) % count;
        } else if (key == CLAY_KEY_RIGHT) {
            selected = (selected + 1) % count;
        } else if (key == CLAY_KEY_ENTER) {
            result = selected;
            break;
        } else if (key == CLAY_KEY_INTERRUPT || key == CLAY_KEY_EOF || key == CLAY_KEY_ESCAPE) {
            result = default_index;
            break;
        } else {
            continue;
        }
        render_select_line(question, options, count, selected);
    }

    clay_term_show_cursor();
    clay_term_raw_disable();
    fputc('\n', stdout);
    return result;
}

int clay_prompt_confirm(const char *question, int default_yes) {
    ClayChoice options[] = {{"Yes", NULL}, {"No", NULL}};
    return clay_prompt_select(question, options, 2, default_yes ? 0 : 1) == 0;
}

static int max_title_width(const ClayChoice *choices, int count) {
    int max_w = 0;
    for (int i = 0; i < count; i++) {
        int w = (int)clay_utf8_width(choices[i].title);
        if (w > max_w) max_w = w;
    }
    return max_w;
}

static void print_choice_row(const ClayChoice *choices, int count, int allow_custom, int index, int selected,
                              int title_width) {
    clay_term_clear_line();
    int is_custom_row = allow_custom && index == count;

    if (is_custom_row) {
        const char *label = "Type your own...";
        if (index == selected) {
            printf("%s%s\xe2\x9d\xaf %s%s", clay_color(CLAY_ORANGE), clay_color(CLAY_BOLD), label, clay_color(CLAY_RESET));
        } else {
            printf("  %s%s%s", clay_color(CLAY_DIM), label, clay_color(CLAY_RESET));
        }
        return;
    }

    const ClayChoice *choice = &choices[index];
    if (index == selected) {
        printf("%s%s\xe2\x9d\xaf %s%s", clay_color(CLAY_ORANGE), clay_color(CLAY_BOLD), choice->title, clay_color(CLAY_RESET));
    } else {
        printf("  %s%s%s", clay_color(CLAY_WHITE), choice->title, clay_color(CLAY_RESET));
    }

    if (choice->desc) {
        int pad = title_width - (int)clay_utf8_width(choice->title);
        for (int i = 0; i < pad; i++) fputc(' ', stdout);
        printf("  %s%s%s", clay_color(CLAY_GRAY), choice->desc, clay_color(CLAY_RESET));
    }
}

static int choice_fallback(const char *question, const ClayChoice *choices, int count,
                            int allow_custom, char **custom_out) {
    printf("%s%s%s\n", clay_color(CLAY_WHITE), question, clay_color(CLAY_RESET));
    int title_width = max_title_width(choices, count);
    for (int i = 0; i < count; i++) {
        printf("  %s%d.%s %s%s%s", clay_color(CLAY_GRAY), i + 1, clay_color(CLAY_RESET),
               clay_color(CLAY_WHITE), choices[i].title, clay_color(CLAY_RESET));
        if (choices[i].desc) {
            int pad = title_width - (int)clay_utf8_width(choices[i].title);
            for (int p = 0; p < pad; p++) fputc(' ', stdout);
            printf("  %s%s%s", clay_color(CLAY_GRAY), choices[i].desc, clay_color(CLAY_RESET));
        }
        fputc('\n', stdout);
    }
    if (allow_custom) {
        printf("  %s%d.%s %sType your own...%s\n", clay_color(CLAY_GRAY), count + 1, clay_color(CLAY_RESET),
               clay_color(CLAY_DIM), clay_color(CLAY_RESET));
    }
    printf("%s%s>%s ", clay_color(CLAY_GREEN), clay_color(CLAY_BOLD), clay_color(CLAY_RESET));
    fflush(stdout);

    char *line = read_line_dynamic();
    if (!line) return -1;

    char *answer = trim(line);
    char *endptr;
    long choice = strtol(answer, &endptr, 10);

    int result = -1;
    if (*answer != '\0' && *endptr == '\0' && choice >= 1 && choice <= count) {
        result = (int)(choice - 1);
    } else if (allow_custom && custom_out) {
        *custom_out = strdup(answer);
    }

    free(line);
    return result;
}

int clay_prompt_choice(const char *question, const ClayChoice *choices, int count,
                        int allow_custom, char **custom_out) {
    if (!clay_term_is_interactive()) {
        return choice_fallback(question, choices, count, allow_custom, custom_out);
    }

    int total_rows = count + (allow_custom ? 1 : 0);
    int selected = 0;
    int title_width = max_title_width(choices, count);
    int established = 1;

    printf("%s%s%s\n", clay_color(CLAY_WHITE), question, clay_color(CLAY_RESET));
    clay_term_raw_enable();
    clay_term_hide_cursor();

    print_choice_row(choices, count, allow_custom, 0, selected, title_width);
    for (int i = 1; i < total_rows; i++) {
        clay_term_row_enter(i, &established);
        print_choice_row(choices, count, allow_custom, i, selected, title_width);
    }
    fflush(stdout);

    int result = -1;
    int entering_custom = 0;

    for (;;) {
        ClayKey key = clay_term_read_key(NULL);
        if (key == CLAY_KEY_UP) {
            selected = (selected - 1 + total_rows) % total_rows;
        } else if (key == CLAY_KEY_DOWN) {
            selected = (selected + 1) % total_rows;
        } else if (key == CLAY_KEY_ENTER) {
            if (allow_custom && selected == count) entering_custom = 1;
            else result = selected;
            break;
        } else if (key == CLAY_KEY_INTERRUPT || key == CLAY_KEY_EOF || key == CLAY_KEY_ESCAPE) {
            break;
        } else {
            continue;
        }
        clay_term_cursor_up(total_rows - 1);
        fputc('\r', stdout);
        clay_term_clear_line();
        print_choice_row(choices, count, allow_custom, 0, selected, title_width);
        for (int i = 1; i < total_rows; i++) {
            clay_term_row_enter(i, &established);
            print_choice_row(choices, count, allow_custom, i, selected, title_width);
        }
        fflush(stdout);
    }

    fputc('\n', stdout);
    clay_term_show_cursor();
    clay_term_raw_disable();

    if (entering_custom) {
        printf("%s%s>%s ", clay_color(CLAY_GREEN), clay_color(CLAY_BOLD), clay_color(CLAY_RESET));
        fflush(stdout);
        char *line = read_line_dynamic();
        if (line && custom_out) {
            *custom_out = strdup(trim(line));
        }
        free(line);
    }

    return result;
}
