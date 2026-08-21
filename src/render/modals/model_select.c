#include "clay/model_select.h"

#include "clay/color.h"
#include "clay/str.h"
#include "clay/term.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_MODEL_MAX_ITEMS 64
#define CLAY_MODEL_VISIBLE_ROWS 6
#define CLAY_MODEL_FILTER_PREFIX "  Search: "

static int item_matches(const ClayModelItem *item, const char *filter) {
    if (!filter[0]) return 1;
    size_t flen = strlen(filter);
    for (const char *p = item->id; *p; p++) {
        size_t i = 0;
        while (i < flen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)filter[i])) i++;
        if (i == flen) return 1;
    }
    return 0;
}

static void render_tabs(const ClayModelProvider *providers, int count, int active) {
    for (int i = 0; i < count; i++) {
        if (i > 0) fputs("  ", stdout);
        if (i == active) {
            printf("%s%s[ %s ]%s", clay_color(CLAY_BOLD), clay_color(CLAY_ORANGE), providers[i].label, clay_color(CLAY_RESET));
        } else {
            printf("%s%s%s", clay_color(CLAY_GRAY), providers[i].label, clay_color(CLAY_RESET));
        }
    }
}

static void render_filter(const char *filter) {
    printf("%s%s%s", clay_color(CLAY_GRAY), CLAY_MODEL_FILTER_PREFIX, clay_color(CLAY_RESET));
    printf("%s%s%s", clay_color(CLAY_WHITE), filter, clay_color(CLAY_RESET));
}

static void render_item(const ClayModelItem *item, int selected) {
    if (selected) {
        printf("%s%s\xe2\x9d\xaf %s%s", clay_color(CLAY_ORANGE), clay_color(CLAY_BOLD), item->id, clay_color(CLAY_RESET));
    } else {
        printf("  %s%s%s", clay_color(CLAY_ORANGE), item->id, clay_color(CLAY_RESET));
    }
    if (item->desc) {
        printf("  %s%s%s", clay_color(CLAY_GRAY), item->desc, clay_color(CLAY_RESET));
    }
}

static void render_more_hint(int count, int above) {
    printf("  %s\xe2\x80\xa6 and %d more %s%s", clay_color(CLAY_GRAY), count, above ? "above" : "below", clay_color(CLAY_RESET));
}

/* Row layout: 0 = tabs, 1 = blank spacer, 2 = filter (cursor rests here
   between calls), 3.. = item area (hint-above, items, hint-below). */
ClayModelSelection clay_model_select(const ClayModelProvider *providers, int provider_count, int default_provider) {
    ClayModelSelection result;
    memset(&result, 0, sizeof(result));
    if (provider_count <= 0) return result;

    int active_tab = (default_provider >= 0 && default_provider < provider_count) ? default_provider : 0;

    ClayStr filter;
    clay_str_init(&filter);

    ClayModelItem cache[CLAY_MODEL_MAX_ITEMS];
    int cache_count = 0;
    int cached_tab = -1;

    int filtered[CLAY_MODEL_MAX_ITEMS];
    int filtered_count = 0;
    int selected = 0;
    int scroll = 0;

    int established = 1;
    int last_row_count = 0;
    int have_rendered = 0;

    clay_term_raw_enable();
    clay_term_hide_cursor();

    for (;;) {
        if (cached_tab != active_tab) {
            cache_count = providers[active_tab].fetch(providers[active_tab].ctx, cache, CLAY_MODEL_MAX_ITEMS);
            cached_tab = active_tab;
            selected = 0;
            scroll = 0;
        }

        filtered_count = 0;
        for (int i = 0; i < cache_count; i++) {
            if (item_matches(&cache[i], filter.data)) filtered[filtered_count++] = i;
        }
        if (selected >= filtered_count) selected = filtered_count > 0 ? filtered_count - 1 : 0;
        if (selected < 0) selected = 0;

        int need_above = scroll > 0;
        int capacity = CLAY_MODEL_VISIBLE_ROWS - (need_above ? 1 : 0);
        int need_below = (scroll + capacity) < filtered_count;
        if (need_below) capacity--;
        if (capacity < 1) capacity = 1;

        if (selected < scroll) scroll = selected;
        if (selected >= scroll + capacity) scroll = selected - capacity + 1;
        if (scroll < 0) scroll = 0;

        need_above = scroll > 0;
        int item_rows = filtered_count - scroll;
        if (item_rows > capacity) item_rows = capacity;
        if (item_rows < 0) item_rows = 0;
        need_below = (scroll + item_rows) < filtered_count;

        int total_rows = 3 + need_above + item_rows + need_below;

        if (have_rendered) clay_term_cursor_up(2);
        have_rendered = 1;
        fputc('\r', stdout);
        clay_term_clear_line();
        render_tabs(providers, provider_count, active_tab);

        int rows_to_visit = total_rows > last_row_count ? total_rows : last_row_count;
        for (int row = 1; row < rows_to_visit; row++) {
            clay_term_row_enter(row, &established);
            clay_term_clear_line();

            if (row == 2) {
                render_filter(filter.data);
            } else if (row > 2 && row < total_rows) {
                int slot = row - 3;
                if (need_above && slot == 0) {
                    render_more_hint(scroll, 1);
                } else {
                    int item_slot = need_above ? slot - 1 : slot;
                    if (item_slot < item_rows) {
                        int fi = filtered[scroll + item_slot];
                        render_item(&cache[fi], (scroll + item_slot) == selected);
                    } else {
                        render_more_hint(filtered_count - (scroll + item_rows), 0);
                    }
                }
            }
        }

        clay_term_cursor_up(rows_to_visit - 3);
        clay_term_cursor_col((int)clay_utf8_width(CLAY_MODEL_FILTER_PREFIX) + (int)clay_utf8_width(filter.data));
        last_row_count = total_rows;
        fflush(stdout);

        char ch = 0;
        ClayKey key = clay_term_read_key(&ch);
        if (key == CLAY_KEY_LEFT) {
            active_tab = (active_tab - 1 + provider_count) % provider_count;
        } else if (key == CLAY_KEY_RIGHT) {
            active_tab = (active_tab + 1) % provider_count;
        } else if (key == CLAY_KEY_UP) {
            if (selected > 0) selected--;
        } else if (key == CLAY_KEY_DOWN) {
            if (selected < filtered_count - 1) selected++;
        } else if (key == CLAY_KEY_BACKSPACE) {
            if (filter.len > 0) filter.data[--filter.len] = '\0';
        } else if (key == CLAY_KEY_CHAR) {
            clay_str_push_char(&filter, ch);
        } else if (key == CLAY_KEY_ENTER) {
            if (filtered_count > 0) {
                int fi = filtered[selected];
                result.model = strdup(cache[fi].id);
                result.provider = strdup(providers[active_tab].label);
                result.ok = 1;
            }
            break;
        } else if (key == CLAY_KEY_EOF || key == CLAY_KEY_ESCAPE) {
            break;
        }
    }

    clay_term_cursor_up(2);
    for (int row = 0; row < last_row_count; row++) {
        clay_term_clear_line();
        if (row + 1 < last_row_count) clay_term_cursor_down(1);
    }
    clay_term_cursor_up(last_row_count - 1);
    fputc('\r', stdout);

    clay_term_show_cursor();
    clay_term_raw_disable();
    clay_str_free(&filter);

    return result;
}

void clay_model_selection_free(ClayModelSelection *sel) {
    free(sel->provider);
    free(sel->model);
    sel->provider = NULL;
    sel->model = NULL;
    sel->ok = 0;
}
