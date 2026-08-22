#include "clay/model_select.h"

#include "clay/color.h"
#include "clay/prompt.h"
#include "clay/str.h"
#include "clay/term.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_MODEL_VISIBLE_ROWS 7
#define CLAY_MODEL_SEARCH_PREFIX "Search: "

typedef struct {
    ClayArray items; /* ClayModelItem, strings borrowed from the provider */
    int fetched;
    int fetch_rc;
} ClayModelTabCache;

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

static int provider_label_width(const ClayModelProvider *providers, int count) {
    int width = 0;
    for (int i = 0; i < count; i++) {
        int label_width = (int)clay_utf8_width(providers[i].label);
        if (label_width > width) width = label_width;
    }
    return width + 1;
}

static void render_header(const ClayModelProvider *provider, int label_width, const char *filter) {
    int padding = label_width - (int)clay_utf8_width(provider->label);
    int left_padding = padding / 2;
    int right_padding = padding - left_padding;
    printf("%s%s< ", clay_color(CLAY_BOLD), clay_color(CLAY_ORANGE));
    for (int i = 0; i < left_padding; i++) fputc(' ', stdout);
    fputs(provider->label, stdout);
    for (int i = 0; i < right_padding; i++) fputc(' ', stdout);
    printf(" >%s  %s%s%s", clay_color(CLAY_RESET), clay_color(CLAY_GRAY), CLAY_MODEL_SEARCH_PREFIX,
           clay_color(CLAY_RESET));
    printf("%s%s%s", clay_color(CLAY_WHITE), filter, clay_color(CLAY_RESET));
}

static void render_item(const ClayModelItem *item, int selected) {
    if (selected) {
        printf("  %s›%s %s%s%s", clay_color(CLAY_ORANGE), clay_color(CLAY_RESET), clay_color(CLAY_BOLD), item->id,
               clay_color(CLAY_RESET));
    } else {
        printf("    %s%s%s", clay_color(CLAY_GRAY), item->id, clay_color(CLAY_RESET));
    }
    if (item->desc) {
        printf("  %s%s%s", clay_color(CLAY_GRAY), item->desc, clay_color(CLAY_RESET));
    }
}

static void render_list_header(void) {
    printf("  %sModels  %s↑↓ navigate · Enter select%s", clay_color(CLAY_DIM), clay_color(CLAY_DIM),
           clay_color(CLAY_RESET));
}

static void render_list_count(int first, int last, int total) {
    printf("  %s%d–%d of %d models%s", clay_color(CLAY_DIM), first, last, total, clay_color(CLAY_RESET));
}

static ClayModelSelection model_select_fallback(const ClayModelProvider *providers, int provider_count,
                                                int default_provider) {
    ClayModelSelection result;
    memset(&result, 0, sizeof(result));
    if (default_provider < 0 || default_provider >= provider_count) default_provider = 0;

    ClayChoice *provider_choices = calloc((size_t)provider_count, sizeof(ClayChoice));
    for (int i = 0; i < provider_count; i++) provider_choices[i].title = providers[i].label;
    int active = clay_prompt_choice("Select provider:", provider_choices, provider_count, 0, NULL);
    free(provider_choices);
    if (active < 0) return result;
    if (active >= provider_count) active = default_provider;

    ClayArray items;
    clay_array_init(&items, sizeof(ClayModelItem));
    int fetch_rc = providers[active].fetch(providers[active].ctx, &items);
    if (fetch_rc != 0) {
        printf("%sCould not retrieve models from %s.%s\n", clay_color(CLAY_RED), providers[active].label,
               clay_color(CLAY_RESET));
        clay_array_free(&items);
        return result;
    }
    if (items.count == 0) {
        printf("%sNo models available.%s\n", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
        clay_array_free(&items);
        return result;
    }

    ClayChoice *model_choices = calloc(items.count, sizeof(ClayChoice));
    for (size_t i = 0; i < items.count; i++) {
        ClayModelItem *item = clay_array_get(&items, i);
        model_choices[i].title = item->id;
        model_choices[i].desc = item->desc;
    }
    int selected = clay_prompt_choice("Select model:", model_choices, (int)items.count, 0, NULL);
    free(model_choices);
    if (selected >= 0) {
        ClayModelItem *item = clay_array_get(&items, (size_t)selected);
        result.provider = strdup(providers[active].id);
        result.model = strdup(item->id);
        result.ok = 1;
    }
    clay_array_free(&items);
    return result;
}

/* Row layout: 0 = active provider and search input, 1 = list title,
   2..8 = fixed item area, 9 = range/count. The cursor rests on row 0
   between redraws. The header is intentionally the only part that differs
   from command completion: it carries the provider tabs and search field. */
ClayModelSelection clay_model_select(const ClayModelProvider *providers, int provider_count, int default_provider) {
    ClayModelSelection result;
    memset(&result, 0, sizeof(result));
    if (provider_count <= 0) return result;
    if (!clay_term_is_interactive()) return model_select_fallback(providers, provider_count, default_provider);

    int active_tab = (default_provider >= 0 && default_provider < provider_count) ? default_provider : 0;

    ClayStr filter;
    clay_str_init(&filter);

    ClayModelTabCache *caches = calloc((size_t)provider_count, sizeof(ClayModelTabCache));
    for (int i = 0; i < provider_count; i++) clay_array_init(&caches[i].items, sizeof(ClayModelItem));

    ClayArray filtered;
    clay_array_init(&filtered, sizeof(int));
    int selected = 0;
    int scroll = 0;

    int established = 1;
    int shown_tab = -1;
    int label_width = provider_label_width(providers, provider_count);
    int header_width = label_width + 4;
    int search_col = header_width + 2 + (int)clay_utf8_width(CLAY_MODEL_SEARCH_PREFIX);

    clay_term_raw_enable();
    clay_term_hide_cursor();

    for (;;) {
        ClayModelTabCache *cache = &caches[active_tab];
        if (shown_tab != active_tab) {
            selected = 0;
            scroll = 0;
            shown_tab = active_tab;
        }
        if (!cache->fetched) {
            cache->fetch_rc = providers[active_tab].fetch(providers[active_tab].ctx, &cache->items);
            cache->fetched = 1;
        }

        clay_array_clear(&filtered);
        for (size_t i = 0; i < cache->items.count; i++) {
            ClayModelItem *item = clay_array_get(&cache->items, i);
            if (item_matches(item, filter.data)) {
                int index = (int)i;
                clay_array_push_val(&filtered, &index);
            }
        }
        int filtered_count = (int)filtered.count;
        if (selected >= filtered_count) selected = filtered_count > 0 ? filtered_count - 1 : 0;
        if (selected < 0) selected = 0;

        /* Match command completion's viewport: the first model starts at
           the top, the active one stays near the middle while possible, and
           it moves down only after reaching the final scroll position. */
        int middle = CLAY_MODEL_VISIBLE_ROWS / 2;
        int max_scroll = filtered_count - CLAY_MODEL_VISIBLE_ROWS;
        if (max_scroll < 0) max_scroll = 0;
        scroll = selected - middle;
        if (scroll < 0) scroll = 0;
        if (scroll > max_scroll) scroll = max_scroll;

        int item_rows = filtered_count - scroll;
        if (item_rows > CLAY_MODEL_VISIBLE_ROWS) item_rows = CLAY_MODEL_VISIBLE_ROWS;
        if (item_rows < 0) item_rows = 0;
        int empty = filtered_count == 0;
        int total_rows = CLAY_MODEL_VISIBLE_ROWS + 3;

        fputc('\r', stdout);
        clay_term_clear_line();
        render_header(&providers[active_tab], label_width, filter.data);

        for (int row = 1; row < total_rows; row++) {
            clay_term_row_enter(row, &established);
            clay_term_clear_line();

            if (row == 1) {
                render_list_header();
            } else if (row == 2 && empty) {
                if (cache->fetch_rc != 0) {
                    printf("  %sCould not retrieve models from %s.%s", clay_color(CLAY_RED), providers[active_tab].label,
                           clay_color(CLAY_RESET));
                } else {
                    printf("  %sNo models available.%s", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
                }
            } else if (!empty && row >= 2 && row < item_rows + 2) {
                int item_row = row - 2;
                int fi = *(int *)clay_array_get(&filtered, (size_t)(scroll + item_row));
                render_item(clay_array_get(&cache->items, (size_t)fi), (scroll + item_row) == selected);
            } else if (row == total_rows - 1 && filtered_count > CLAY_MODEL_VISIBLE_ROWS) {
                render_list_count(scroll + 1, scroll + item_rows, filtered_count);
            }
        }

        clay_term_cursor_up(total_rows - 1);
        clay_term_cursor_col(search_col + (int)clay_utf8_width(filter.data));
        fflush(stdout);

        char ch = 0;
        ClayKey key = clay_term_read_key(&ch);
        if (key == CLAY_KEY_LEFT) {
            active_tab = (active_tab - 1 + provider_count) % provider_count;
        } else if (key == CLAY_KEY_RIGHT) {
            active_tab = (active_tab + 1) % provider_count;
        } else if (key == CLAY_KEY_UP) {
            if (filtered_count > 0) selected = (selected - 1 + filtered_count) % filtered_count;
        } else if (key == CLAY_KEY_DOWN) {
            if (filtered_count > 0) selected = (selected + 1) % filtered_count;
        } else if (key == CLAY_KEY_BACKSPACE) {
            if (filter.len > 0) filter.data[--filter.len] = '\0';
        } else if (key == CLAY_KEY_CHAR) {
            clay_str_push_char(&filter, ch);
        } else if (key == CLAY_KEY_ENTER) {
            if (filtered_count > 0) {
                int fi = *(int *)clay_array_get(&filtered, (size_t)selected);
                ClayModelItem *item = clay_array_get(&cache->items, (size_t)fi);
                result.model = strdup(item->id);
                result.provider = strdup(providers[active_tab].id);
                result.ok = 1;
            }
            break;
        } else if (key == CLAY_KEY_INTERRUPT || key == CLAY_KEY_EOF || key == CLAY_KEY_ESCAPE) {
            break;
        }
    }

    int total_rows = CLAY_MODEL_VISIBLE_ROWS + 3;
    for (int row = 0; row < total_rows; row++) {
        clay_term_clear_line();
        if (row + 1 < total_rows) clay_term_cursor_down(1);
    }
    clay_term_cursor_up(total_rows - 1);
    fputc('\r', stdout);

    clay_term_show_cursor();
    clay_term_raw_disable();
    clay_str_free(&filter);
    clay_array_free(&filtered);
    for (int i = 0; i < provider_count; i++) clay_array_free(&caches[i].items);
    free(caches);

    return result;
}

void clay_model_selection_free(ClayModelSelection *sel) {
    free(sel->provider);
    free(sel->model);
    sel->provider = NULL;
    sel->model = NULL;
    sel->ok = 0;
}
