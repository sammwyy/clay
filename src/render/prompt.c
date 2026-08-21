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

/* Append-only line editor: chars append, backspace deletes last, up/down
   recall history. No mid-line cursor movement; left/right ignored. */
static char *interactive_prompt_line(void) {
    ClayStr buf;
    clay_str_init(&buf);

    int history_pos = -1; /* -1 = editing a fresh line, not browsing history */
    int got_eof = 0;

    clay_term_raw_enable();
    clay_below_set_editing(1);
    clay_below_render(buf.data);

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
            history_pos = -1;
        } else if (key == CLAY_KEY_UP) {
            size_t count = clay_prompt_history_count();
            if (count > 0) {
                history_pos = (history_pos == -1) ? (int)count - 1 : (history_pos > 0 ? history_pos - 1 : 0);
                clay_str_clear(&buf);
                clay_str_push(&buf, clay_prompt_history_get((size_t)history_pos));
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
            }
        } else if (key == CLAY_KEY_CHAR) {
            clay_str_push_char(&buf, ch);
            history_pos = -1;
        } else {
            continue;
        }

        clay_below_render(buf.data);
    }

    clay_below_set_editing(0);
    clay_below_finish();
    clay_term_raw_disable();

    if (got_eof && buf.len == 0) {
        clay_str_free(&buf);
        return NULL;
    }

    push_history(buf.data);
    return buf.data;
}

char *clay_prompt_line(void) {
    if (!clay_term_is_interactive()) return fallback_prompt_line();
    return interactive_prompt_line();
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
        } else if (key == CLAY_KEY_EOF || key == CLAY_KEY_ESCAPE) {
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
            printf("%s%s\xe2\x9d\xaf %s%s\n", clay_color(CLAY_ORANGE), clay_color(CLAY_BOLD), label, clay_color(CLAY_RESET));
        } else {
            printf("  %s%s%s\n", clay_color(CLAY_DIM), label, clay_color(CLAY_RESET));
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
    fputc('\n', stdout);
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

    printf("%s%s%s\n", clay_color(CLAY_WHITE), question, clay_color(CLAY_RESET));
    clay_term_raw_enable();
    clay_term_hide_cursor();
    for (int i = 0; i < total_rows; i++) print_choice_row(choices, count, allow_custom, i, selected, title_width);

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
        } else if (key == CLAY_KEY_EOF || key == CLAY_KEY_ESCAPE) {
            break;
        } else {
            continue;
        }
        clay_term_cursor_up(total_rows);
        for (int i = 0; i < total_rows; i++) print_choice_row(choices, count, allow_custom, i, selected, title_width);
    }

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
