#include "clay/below.h"

#include "clay/array.h"
#include "clay/color.h"
#include "clay/str.h"
#include "clay/term.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAY_BELOW_MAX_MODULES 64
#define CLAY_BELOW_SPINNER_FRAMES 10
#define CLAY_BELOW_SEPARATOR_WIDTH 3 /* " · " */

typedef struct {
    char *id;
    ClayStr text;
    ClayBelowState state;
    ClayBelowAlign alignment;
    int enabled;
    int optional; /* dropped when the block would otherwise overflow */
    int index;
    int show_elapsed;
    struct timespec elapsed_start;
} ClayBelowModule;

static const char *SPINNER_FRAMES[CLAY_BELOW_SPINNER_FRAMES] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
    "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static ClayArray g_modules;
static int g_modules_ready = 0;

static ClayArray g_overlay; /* char*, transient rows after the status modules */
static int g_overlay_ready = 0;

static ClayStr g_last_input;
static int g_last_input_ready = 0;
static size_t g_last_cursor = 0;

static int g_last_line_count = 0;
static int g_max_rows_established = 1; /* rows below the prompt ever created via a real '\n' */
static int g_spinner_frame = 0;
static int g_editing = 0;
static int g_status_only = 0;

static pthread_t g_animator;
static int g_animator_started = 0;

static void ensure_modules(void) {
    if (!g_modules_ready) {
        clay_array_init(&g_modules, sizeof(ClayBelowModule));
        g_modules_ready = 1;
    }
}

static void ensure_overlay(void) {
    if (!g_overlay_ready) {
        clay_array_init(&g_overlay, sizeof(char *));
        g_overlay_ready = 1;
    }
}

static void clear_overlay(void) {
    ensure_overlay();
    for (size_t i = 0; i < g_overlay.count; i++) free(*(char **)clay_array_get(&g_overlay, i));
    clay_array_clear(&g_overlay);
}

static void ensure_last_input(void) {
    if (!g_last_input_ready) {
        clay_str_init(&g_last_input);
        g_last_input_ready = 1;
    }
}

static int safe_row_width(void) {
    int width = clay_term_width();
    /* Avoid using the terminal's final column: many terminals autowrap as
       soon as it is written, which makes a logical row become two rows. */
    return width > 1 ? width - 1 : 1;
}

static int below_indent_width(void) {
    int width = safe_row_width();
    return width >= 2 ? 2 : width;
}

static void print_below_indent(void) {
    for (int i = 0; i < below_indent_width(); i++) fputc(' ', stdout);
}

static void render_turn_separator(void) {
    fputs(clay_color(CLAY_GRAY), stdout);
    print_below_indent();
    clay_term_write_clipped("──────────────────────────────", safe_row_width() - below_indent_width());
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

static ClayBelowModule *find_module(const char *id) {
    for (size_t i = 0; i < g_modules.count; i++) {
        ClayBelowModule *m = clay_array_get(&g_modules, i);
        if (strcmp(m->id, id) == 0) return m;
    }
    return NULL;
}

static int has_loading_module(void) {
    for (size_t i = 0; i < g_modules.count; i++) {
        ClayBelowModule *m = clay_array_get(&g_modules, i);
        if (m->enabled && m->state == CLAY_BELOW_LOADING) return 1;
    }
    return 0;
}

/* Sorted, enabled-only view: fills `order` with indices into g_modules,
   ascending by .index, stable on ties. Small N, plain insertion sort. */
static int sorted_enabled_indices(int *order) {
    int count = 0;
    size_t n = g_modules.count;
    if (n > CLAY_BELOW_MAX_MODULES) n = CLAY_BELOW_MAX_MODULES;

    for (size_t i = 0; i < n; i++) {
        ClayBelowModule *m = clay_array_get(&g_modules, i);
        if (m->enabled) order[count++] = (int)i;
    }

    for (int i = 1; i < count; i++) {
        int key = order[i];
        ClayBelowModule *km = clay_array_get(&g_modules, (size_t)key);
        int j = i - 1;
        while (j >= 0) {
            ClayBelowModule *jm = clay_array_get(&g_modules, (size_t)order[j]);
            if (jm->index <= km->index) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
    return count;
}

/* Keep the established left group before the right group. The old renderer
   used this grouping whenever it fit on one row; retaining it makes the new
   two-row layout a reflow rather than a reordering of the status modules. */
static void group_aligned_indices(const int *order, int count, int *grouped) {
    int at = 0;
    for (int pass = 0; pass < 2; pass++) {
        ClayBelowAlign alignment = pass == 0 ? CLAY_BELOW_ALIGN_LEFT
                                             : CLAY_BELOW_ALIGN_RIGHT;
        for (int i = 0; i < count; i++) {
            ClayBelowModule *m = clay_array_get(&g_modules, (size_t)order[i]);
            if (m->alignment == alignment) grouped[at++] = order[i];
        }
    }
}

static int print_module_inline_limited(const ClayBelowModule *m, int available) {
    if (available <= 0) return 0;
    int written = 0;
    switch (m->state) {
        case CLAY_BELOW_LOADING:
            if (available - written < 2) return written;
            printf("%s%s%s ", clay_color(CLAY_YELLOW), SPINNER_FRAMES[g_spinner_frame], clay_color(CLAY_RESET));
            written += 2;
            break;
        case CLAY_BELOW_FINISHED:
            if (available - written < 2) return written;
            printf("%s%s%s ", clay_color(CLAY_GREEN), CLAY_ICON_CHECK, clay_color(CLAY_RESET));
            written += 2;
            break;
        case CLAY_BELOW_IDLE:
            if (available - written < 2) return written;
            printf("%s%s%s ", clay_color(CLAY_GRAY), CLAY_ICON_SLEEP, clay_color(CLAY_RESET));
            written += 2;
            break;
        case CLAY_BELOW_NONE:
        default:
            break;
    }
    if (m->show_elapsed) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double seconds = (double)(now.tv_sec - m->elapsed_start.tv_sec) +
                         (double)(now.tv_nsec - m->elapsed_start.tv_nsec) / 1e9;
        const char *color = m->state == CLAY_BELOW_LOADING ? CLAY_YELLOW : CLAY_GRAY;
        char elapsed[32];
        snprintf(elapsed, sizeof(elapsed), "%5.1fs", seconds);
        printf("%s", clay_color(color));
        written += (int)clay_term_write_clipped(elapsed, available - written);
        printf("%s", clay_color(CLAY_RESET));
    } else {
        printf("%s", clay_color(CLAY_GRAY));
        written += (int)clay_term_write_clipped(m->text.data, available - written);
        printf("%s", clay_color(CLAY_RESET));
    }
    return written;
}

static int module_display_width(const ClayBelowModule *m) {
    int width = 0;
    switch (m->state) {
        case CLAY_BELOW_LOADING:
            width += (int)clay_utf8_width(SPINNER_FRAMES[0]) + 1;
            break;
        case CLAY_BELOW_FINISHED:
            width += (int)clay_utf8_width(CLAY_ICON_CHECK) + 1;
            break;
        case CLAY_BELOW_IDLE:
            width += (int)clay_utf8_width(CLAY_ICON_SLEEP) + 1;
            break;
        case CLAY_BELOW_NONE:
        default:
            break;
    }
    if (m->show_elapsed)
        width += 5; /* "%5.1fs" */
    else
        width += (int)clay_utf8_width(m->text.data);
    return width;
}

static void print_module_separator(void) {
    printf(" %s%s%s ", clay_color(CLAY_GRAY), CLAY_ICON_DOT,
           clay_color(CLAY_RESET));
}

/* Whether every module in `order` lands inside `max_rows` rows. */
static int modules_fit(const int *order, int count, int max_rows) {
    if (count == 0) return 1;
    int capacity = safe_row_width() - below_indent_width();
    if (capacity <= 0) return 0;
    int rows = 1;
    int used = 0;
    for (int i = 0; i < count; i++) {
        int module_width = module_display_width(clay_array_get(&g_modules, (size_t)order[i]));
        if (module_width > capacity) return 0;
        int needed = module_width + (used ? CLAY_BELOW_SEPARATOR_WIDTH : 0);
        if (used && used + needed > capacity) {
            if (rows == max_rows) return 0;
            rows++;
            needed = module_width;
            used = 0;
        }
        used += needed;
    }
    return 1;
}

/* Optional modules are a nicety, not information the block must carry:
   they only survive when everything still fits on one row. */
static int drop_optional_modules(int *order, int count) {
    if (modules_fit(order, count, 1)) return count;
    int kept = 0;
    for (int i = 0; i < count; i++) {
        ClayBelowModule *m = clay_array_get(&g_modules, (size_t)order[i]);
        if (!m->optional) order[kept++] = order[i];
    }
    return kept;
}

static int module_row_count(const int *order, int count, int max_rows) {
    if (count == 0 || max_rows <= 0) return 0;
    int capacity = safe_row_width() - below_indent_width();
    if (capacity <= 0) return 0;
    int rows = 1;
    int used = 0;
    for (int i = 0; i < count; i++) {
        int module_width = module_display_width(clay_array_get(&g_modules, (size_t)order[i]));
        int needed = module_width + (used ? CLAY_BELOW_SEPARATOR_WIDTH : 0);
        if (used && used + needed > capacity) {
            if (rows == max_rows) break;
            rows++;
            used = 0;
            needed = module_width;
        }
        if (module_width > capacity) break;
        used += needed;
    }
    return rows;
}

/* Fits whole modules greedily without ever relying on terminal wrapping.
   A prompt can spend two rows on status, while the streamed-output status
   remains one row because its cursor choreography deliberately reserves a
   single row beneath the response.

   start_row is this block's first physical row (matching the row numbering
   clay_term_row_enter uses elsewhere in this file). Every row after the
   first must go through clay_term_row_enter too: a bare '\n' scrolls the
   whole screen when that row already exists below row 0, which is exactly
   the double-scroll bug clay_term_row_enter exists to avoid. */
static int render_modules(const int *order, int count, int max_rows, int start_row) {
    if (count == 0 || max_rows <= 0) return 0;

    int capacity = safe_row_width() - below_indent_width();
    if (capacity <= 0) return 0;
    int starts[2] = {0, 0};
    int ends[2] = {0, 0};
    int rows = 1;
    int used = 0;
    int truncated = 0;
    starts[0] = 0;

    for (int i = 0; i < count; i++) {
        int module_width = module_display_width(clay_array_get(&g_modules, (size_t)order[i]));
        int needed = module_width + (used ? CLAY_BELOW_SEPARATOR_WIDTH : 0);
        if (used && used + needed > capacity) {
            ends[rows - 1] = i;
            if (rows == max_rows) {
                truncated = 1;
                break;
            }
            starts[rows] = i;
            used = 0;
            rows++;
            needed = module_width;
        }
        if (module_width > capacity) {
            ends[rows - 1] = i + 1;
            if (i + 1 < count) truncated = 1;
            break;
        }
        used += needed;
        ends[rows - 1] = i + 1;
    }

    for (int row = 0; row < rows; row++) {
        if (row > 0) {
            clay_term_row_enter(start_row + row, &g_max_rows_established);
            clay_term_clear_line();
        }
        print_below_indent();
        int row_used = 0;
        for (int i = starts[row]; i < ends[row]; i++) {
            if (row_used) {
                print_module_separator();
                row_used += CLAY_BELOW_SEPARATOR_WIDTH;
            }
            ClayBelowModule *m = clay_array_get(&g_modules, (size_t)order[i]);
            row_used += print_module_inline_limited(m, capacity - row_used);
        }
        if (truncated && row + 1 == rows && row_used + 2 <= capacity)
            fputs(" …", stdout);
    }
    return rows;
}

/* Cursor rests at row 0 col 0 between calls. Rows that already exist use
   cursor-down (never scrolls); new rows use a real '\n' (scrolls if row 0
   is at the bottom). Using '\n' on an existing row double-scrolls and
   walks the block up the screen. */
static void render_locked(void) {
    ensure_modules();
    ensure_last_input();
    ensure_overlay();

    fputc('\r', stdout);
    clay_term_clear_line();
    /* Some terminals (notably tmux) reflow previously-written rows when the
       window is resized - wrapping or unwrapping lines that were written at
       the old width - without the app's involvement. That desyncs the
       cursor-relative row bookkeeping below and leaves stale fragments on
       screen. Erasing everything from the cursor to the end of the screen
       before redrawing guarantees no such fragment can survive a redraw. */
    fputs("\x1b[0J", stdout);
    int prompt_prefix = safe_row_width() >= 2 ? 2 : 1;
    printf("%s%s%s%s", clay_color(CLAY_GREEN), clay_color(CLAY_BOLD),
           CLAY_ICON_PROMPT, clay_color(CLAY_RESET));
    if (prompt_prefix == 2) fputc(' ', stdout);

    /* The editor is deliberately a one-row viewport. A long input used to
       wrap into the status/overlay area, so subsequent redraws cleared or
       cursor-positioned the wrong physical rows. Keep the cursor in view by
       scrolling the beginning away and mark that with an ellipsis. */
    int input_space = safe_row_width() - prompt_prefix;
    size_t cursor = g_last_cursor < g_last_input.len ? g_last_cursor : g_last_input.len;
    char saved = g_last_input.data[cursor];
    g_last_input.data[cursor] = '\0';
    size_t start = 0;
    int omitted = 0;
    while ((int)clay_utf8_width(g_last_input.data + start) > input_space - 1 &&
           start < cursor) {
        unsigned char byte = (unsigned char)g_last_input.data[start];
        if (byte == 0x1b) {
            start++;
            if (g_last_input.data[start] == '[') {
                start++;
                while (g_last_input.data[start] &&
                       ((unsigned char)g_last_input.data[start] < 0x40 ||
                        (unsigned char)g_last_input.data[start] > 0x7e)) start++;
                if (g_last_input.data[start]) start++;
            } else if (g_last_input.data[start] == ']') {
                start++;
                while (g_last_input.data[start] && g_last_input.data[start] != '\a' &&
                       !(g_last_input.data[start] == 0x1b && g_last_input.data[start + 1] == '\\')) start++;
                if (g_last_input.data[start] == '\a') start++;
                else if (g_last_input.data[start] == 0x1b) start += 2;
            } else if (g_last_input.data[start]) {
                start++;
            }
        } else if ((byte & 0xE0) == 0xC0) {
            start += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            start += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            start += 4;
        } else {
            start++;
        }
        omitted = 1;
    }
    int before_cursor = (int)clay_utf8_width(g_last_input.data + start);
    g_last_input.data[cursor] = saved;
    if (omitted && input_space > 0) fputs("…", stdout);
    clay_term_write_clipped(g_last_input.data + start,
                            input_space - (omitted ? 1 : 0));

    int order[CLAY_BELOW_MAX_MODULES];
    int count = drop_optional_modules(order, sorted_enabled_indices(order));
    int grouped_order[CLAY_BELOW_MAX_MODULES];
    group_aligned_indices(order, count, grouped_order);
    int status_rows = module_row_count(grouped_order, count, 2);
    int overlay_start = 1 + status_rows;
    int total_now = overlay_start + (int)g_overlay.count;

    int rows_to_visit = total_now > g_last_line_count ? total_now : g_last_line_count;

    for (int row = 1; row < rows_to_visit;) {
        clay_term_row_enter(row, &g_max_rows_established);
        clay_term_clear_line();

        if (count > 0 && row == 1) {
            int rendered_rows = render_modules(grouped_order, count, 2, row);
            if (rendered_rows > 1 && g_max_rows_established < row + rendered_rows)
                g_max_rows_established = row + rendered_rows;
            row += rendered_rows;
            continue;
        } else if (row >= overlay_start && row < total_now) {
            clay_term_write_clipped(*(char **)clay_array_get(&g_overlay,
                                      (size_t)(row - overlay_start)), safe_row_width());
        }
        row++;
    }

    if (rows_to_visit > 1) clay_term_cursor_up(rows_to_visit - 1);

    clay_term_cursor_col(prompt_prefix + (omitted && input_space > 0 ? 1 : 0) + before_cursor);

    g_last_line_count = total_now;
    fflush(stdout);
}

static void render_status_locked(void) {
    ensure_modules();

    fputc('\r', stdout);
    clay_term_clear_line();

    int order[CLAY_BELOW_MAX_MODULES];
    int count = drop_optional_modules(order, sorted_enabled_indices(order));
    int grouped_order[CLAY_BELOW_MAX_MODULES];
    group_aligned_indices(order, count, grouped_order);
    render_modules(grouped_order, count, 1, 0);

    fputc('\r', stdout);
    g_last_line_count = 1;
    fflush(stdout);
}

static void *animator_loop(void *arg) {
    (void)arg;
    for (;;) {
        clay_term_sleep_ms(80);
        pthread_mutex_lock(&g_lock);
        if (g_editing && has_loading_module()) {
            g_spinner_frame = (g_spinner_frame + 1) % CLAY_BELOW_SPINNER_FRAMES;
            if (g_status_only) render_status_locked();
            else render_locked();
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void ensure_animator(void) {
    if (!g_animator_started) {
        g_animator_started = 1;
        pthread_create(&g_animator, NULL, animator_loop, NULL);
        pthread_detach(g_animator);
    }
}

void clay_below_add(int index, const char *id) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();

    ClayBelowModule *existing = find_module(id);
    if (existing) {
        existing->index = index;
    } else {
        ClayBelowModule m;
        m.id = strdup(id);
        clay_str_init(&m.text);
        m.state = CLAY_BELOW_NONE;
        m.alignment = CLAY_BELOW_ALIGN_LEFT;
        m.enabled = 1;
        m.optional = 0;
        m.index = index;
        m.show_elapsed = 0;
        clay_array_push_val(&g_modules, &m);
    }

    ensure_animator();
    pthread_mutex_unlock(&g_lock);
}

void clay_below_set_text(const char *id, const char *content) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) {
        clay_str_clear(&m->text);
        clay_str_push(&m->text, content);
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_set_state(const char *id, ClayBelowState state) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) m->state = state;
    pthread_mutex_unlock(&g_lock);
}

void clay_below_set_enabled(const char *id, int enabled) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) m->enabled = enabled;
    pthread_mutex_unlock(&g_lock);
}

void clay_below_reorder(const char *id, int index) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) m->index = index;
    pthread_mutex_unlock(&g_lock);
}

void clay_below_set_alignment(const char *id, ClayBelowAlign alignment) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) m->alignment = alignment;
    pthread_mutex_unlock(&g_lock);
}

void clay_below_set_optional(const char *id, int optional) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) m->optional = optional != 0;
    pthread_mutex_unlock(&g_lock);
}

void clay_below_start_elapsed(const char *id) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) {
        clock_gettime(CLOCK_MONOTONIC, &m->elapsed_start);
        m->show_elapsed = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_stop_elapsed(const char *id) {
    pthread_mutex_lock(&g_lock);
    ensure_modules();
    ClayBelowModule *m = find_module(id);
    if (m) m->show_elapsed = 0;
    pthread_mutex_unlock(&g_lock);
}

void clay_below_set_editing(int editing) {
    pthread_mutex_lock(&g_lock);
    g_editing = editing;
    pthread_mutex_unlock(&g_lock);
}

void clay_below_set_overlay(const char *const *rows, size_t count) {
    pthread_mutex_lock(&g_lock);
    clear_overlay();
    for (size_t i = 0; i < count; i++) {
        char *copy = strdup(rows[i] ? rows[i] : "");
        clay_array_push_val(&g_overlay, &copy);
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_render(const char *input, size_t cursor) {
    pthread_mutex_lock(&g_lock);
    ensure_last_input();
    clay_str_clear(&g_last_input);
    clay_str_push(&g_last_input, input);
    g_last_cursor = cursor;
    g_status_only = 0;
    render_locked();
    pthread_mutex_unlock(&g_lock);
}

void clay_below_clear_screen(void) {
    pthread_mutex_lock(&g_lock);
    fputs("\x1b[2J\x1b[H", stdout);
    g_last_line_count = 0;
    g_max_rows_established = 1;
    g_status_only = 0;
    fflush(stdout);
    pthread_mutex_unlock(&g_lock);
}

void clay_below_render_status(void) {
    pthread_mutex_lock(&g_lock);
    g_status_only = 1;
    render_status_locked();
    pthread_mutex_unlock(&g_lock);
}

/* '\n' scrolls correctly at the terminal's last row; insert-line silently drops content there instead. */
void clay_below_status_insert_above(void) {
    pthread_mutex_lock(&g_lock);
    if (g_status_only) {
        fputc('\r', stdout);
        clay_term_clear_line();
        fputc('\n', stdout);
        render_status_locked();
        clay_term_cursor_up(1);
        fputc('\r', stdout);
        fflush(stdout);
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_status_push_down(void) {
    pthread_mutex_lock(&g_lock);
    if (g_status_only) {
        clay_term_cursor_down(1);
        fputc('\r', stdout);
        clay_term_clear_line();
        fputc('\n', stdout);
        render_status_locked();
        clay_term_cursor_up(2);
        fputc('\r', stdout);
        fflush(stdout);
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_status_finish_output(void) {
    pthread_mutex_lock(&g_lock);
    if (g_status_only) {
        clay_term_cursor_down(1);
        fputc('\r', stdout);
        clay_term_clear_line();
        g_status_only = 0;
        g_last_line_count = 0;
        fflush(stdout);
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_status_refresh_below(void) {
    pthread_mutex_lock(&g_lock);
    if (g_status_only) {
        clay_term_cursor_down(1);
        render_status_locked();
        clay_term_cursor_up(1);
        fflush(stdout);
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_status_prepare_prompt(void) {
    pthread_mutex_lock(&g_lock);
    if (g_status_only) {
        clay_term_cursor_col(0);
        clay_term_cursor_down(1);
        fputc('\r', stdout);
        clay_term_clear_line();
        fputc('\n', stdout); /* breathing room after the assistant block */
        render_turn_separator(); /* separator row between turns */
        fputc('\n', stdout); /* row 0 of the next prompt */
        fputc('\n', stdout); /* row 1 of the next prompt, reserved up front */
        clay_term_cursor_up(1);
        fputc('\r', stdout);
        g_status_only = 0;
        g_last_line_count = 0;
        g_max_rows_established = 2;
        fflush(stdout);
    }
    pthread_mutex_unlock(&g_lock);
}

void clay_below_finish(void) {
    pthread_mutex_lock(&g_lock);
    clear_overlay();
    if (g_status_only) {
        fputc('\r', stdout);
        clay_term_clear_line();
        fflush(stdout);
        g_status_only = 0;
        g_last_line_count = 0;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* Prompt row stays as history; modules row is ephemeral, erase it. */
    if (g_last_line_count > 1) {
        for (int i = 1; i < g_last_line_count; i++) {
            clay_term_cursor_down(1);
            fputc('\r', stdout);
            clay_term_clear_line();
        }
    } else {
        fputc('\n', stdout);
    }
    fflush(stdout);
    g_last_line_count = 0;
    g_max_rows_established = 1;
    ensure_last_input();
    clay_str_clear(&g_last_input);
    g_last_cursor = 0;
    pthread_mutex_unlock(&g_lock);
}
