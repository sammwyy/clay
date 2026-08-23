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

static void render_turn_separator(void) {
    fputs(clay_color(CLAY_GRAY), stdout);
    fputs("  ──────────────────────────────", stdout);
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

static void print_module_inline(const ClayBelowModule *m) {
    switch (m->state) {
        case CLAY_BELOW_LOADING:
            printf("%s%s%s ", clay_color(CLAY_YELLOW), SPINNER_FRAMES[g_spinner_frame], clay_color(CLAY_RESET));
            break;
        case CLAY_BELOW_FINISHED:
            printf("%s%s%s ", clay_color(CLAY_GREEN), CLAY_ICON_CHECK, clay_color(CLAY_RESET));
            break;
        case CLAY_BELOW_IDLE:
            printf("%s%s%s ", clay_color(CLAY_GRAY), CLAY_ICON_SLEEP, clay_color(CLAY_RESET));
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
        printf("%s%5.1fs%s", clay_color(color), seconds, clay_color(CLAY_RESET));
    } else {
        printf("%s%s%s", clay_color(CLAY_GRAY), m->text.data, clay_color(CLAY_RESET));
    }
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

static int module_group_width(const int *order, int start, int count) {
    int width = 0;
    for (int i = start; i < start + count; i++) {
        if (i > start) width += CLAY_BELOW_SEPARATOR_WIDTH;
        width += module_display_width(
            clay_array_get(&g_modules, (size_t)order[i]));
    }
    return width;
}

static void print_module_separator(void) {
    printf(" %s%s%s ", clay_color(CLAY_GRAY), CLAY_ICON_DOT,
           clay_color(CLAY_RESET));
}

static void print_module_group(const int *order, int start, int count) {
    for (int i = start; i < start + count; i++) {
        if (i > start) print_module_separator();
        print_module_inline(clay_array_get(&g_modules, (size_t)order[i]));
    }
}

/* Modules opt into the right-hand group; their configured index still
   controls their order within that group. */
static void print_modules_aligned(const int *order, int count) {
    int left_order[CLAY_BELOW_MAX_MODULES];
    int right_order[CLAY_BELOW_MAX_MODULES];
    int left_count = 0;
    int right_count = 0;
    for (int i = 0; i < count; i++) {
        ClayBelowModule *m = clay_array_get(&g_modules, (size_t)order[i]);
        if (m->alignment == CLAY_BELOW_ALIGN_RIGHT)
            right_order[right_count++] = order[i];
        else
            left_order[left_count++] = order[i];
    }

    if (left_count == 0 || right_count == 0) {
        print_module_group(order, 0, count);
        return;
    }

    int left_width = module_group_width(left_order, 0, left_count);
    int right_width = module_group_width(right_order, 0, right_count);
    int separator_width = CLAY_BELOW_SEPARATOR_WIDTH;
    if (2 + left_width + separator_width + right_width > clay_term_width()) {
        print_module_group(order, 0, count);
        return;
    }

    print_module_group(left_order, 0, left_count);
    print_module_separator();
    print_module_group(right_order, 0, right_count);
}

static int module_group_offset(const int *order, int count, int target) {
    int offset = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0) offset += CLAY_BELOW_SEPARATOR_WIDTH;
        if (order[i] == target) return offset;
        offset += module_display_width(
            clay_array_get(&g_modules, (size_t)order[i]));
    }
    return -1;
}

static int status_display_column(const int *order, int count) {
    int status_order = -1;
    int left_order[CLAY_BELOW_MAX_MODULES];
    int right_order[CLAY_BELOW_MAX_MODULES];
    int left_count = 0;
    int right_count = 0;
    for (int i = 0; i < count; i++) {
        ClayBelowModule *m = clay_array_get(&g_modules, (size_t)order[i]);
        if (strcmp(m->id, "status") == 0) {
            status_order = order[i];
        }
        if (m->alignment == CLAY_BELOW_ALIGN_RIGHT)
            right_order[right_count++] = order[i];
        else
            left_order[left_count++] = order[i];
    }
    if (status_order < 0) return -1;

    int width = clay_term_width();
    if (left_count > 0 && right_count > 0) {
        int left_width = module_group_width(left_order, 0, left_count);
        int right_width = module_group_width(right_order, 0, right_count);
        int separator_width = CLAY_BELOW_SEPARATOR_WIDTH;
        if (2 + left_width + separator_width + right_width <= width) {
            int offset = status_order >= 0
                             ? module_group_offset(right_order, right_count,
                                                   status_order)
                             : -1;
            if (offset >= 0)
                return 2 + left_width + separator_width + offset;
            offset = module_group_offset(left_order, left_count, status_order);
            if (offset >= 0) return 2 + offset;
        }
    }

    int column = 2;
    int offset = module_group_offset(order, count, status_order);
    if (offset >= 0) column += offset;
    return column;
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
    printf("%s%s%s%s %s", clay_color(CLAY_GREEN), clay_color(CLAY_BOLD),
           CLAY_ICON_PROMPT, clay_color(CLAY_RESET), g_last_input.data);

    int order[CLAY_BELOW_MAX_MODULES];
    int count = sorted_enabled_indices(order);
    int overlay_start = count > 0 ? 2 : 1;
    int total_now = overlay_start + (int)g_overlay.count;

    int rows_to_visit = total_now > g_last_line_count ? total_now : g_last_line_count;

    for (int row = 1; row < rows_to_visit; row++) {
        clay_term_row_enter(row, &g_max_rows_established);
        clay_term_clear_line();

        if (count > 0 && row == 1) {
            fputs("  ", stdout);
            print_modules_aligned(order, count);
        } else if (row >= overlay_start && row < total_now) {
            fputs(*(char **)clay_array_get(&g_overlay, (size_t)(row - overlay_start)), stdout);
        }
    }

    if (rows_to_visit > 1) clay_term_cursor_up(rows_to_visit - 1);

    size_t cursor = g_last_cursor < g_last_input.len ? g_last_cursor : g_last_input.len;
    char saved = g_last_input.data[cursor];
    g_last_input.data[cursor] = '\0';
    int col = (int)clay_utf8_width(g_last_input.data);
    g_last_input.data[cursor] = saved;
    clay_term_cursor_col(2 + col);

    g_last_line_count = total_now;
    fflush(stdout);
}

static void render_status_locked(void) {
    ensure_modules();

    fputc('\r', stdout);
    clay_term_clear_line();
    fputs("  ", stdout);

    int order[CLAY_BELOW_MAX_MODULES];
    int count = sorted_enabled_indices(order);
    print_modules_aligned(order, count);

    fputc('\r', stdout);
    g_last_line_count = 1;
    fflush(stdout);
}

static int status_module_loading(void) {
    ClayBelowModule *status = find_module("status");
    return status && status->enabled && status->state == CLAY_BELOW_LOADING;
}

static void render_status_spinner_locked(void) {
    ensure_modules();
    int order[CLAY_BELOW_MAX_MODULES];
    int count = sorted_enabled_indices(order);
    int status_column = status_display_column(order, count);
    ClayBelowModule *status = find_module("status");
    if (!status || status_column < 0) {
        render_status_locked();
        return;
    }
    /* The spinner and timer are fixed-width, so update only that cell. This
       preserves the cursor row and avoids wrapping the whole status line. */
    clay_term_cursor_col(status_column);
    print_module_inline(status);
    clay_term_cursor_col(0);
    fflush(stdout);
}

static void *animator_loop(void *arg) {
    (void)arg;
    for (;;) {
        clay_term_sleep_ms(80);
        pthread_mutex_lock(&g_lock);
        if (g_editing && has_loading_module()) {
            g_spinner_frame = (g_spinner_frame + 1) % CLAY_BELOW_SPINNER_FRAMES;
            if (g_status_only && status_module_loading()) render_status_spinner_locked();
            else if (g_status_only) render_status_locked();
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
