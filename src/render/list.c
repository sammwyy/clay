#include "clay/list.h"

#include "clay/color.h"
#include "clay/str.h"
#include "clay/term.h"

#include <stdarg.h>
#include <stdio.h>

/* Keeps ordinary list items visually separate from assistant messages. */
#define CLAY_INDENT "  "

static ClayStr g_thinking;
static int g_thinking_ready = 0;
static int g_thinking_streaming = 0;
static int g_thinking_expanded = 0;
static int g_thinking_rows = 1;
static int g_thinking_col = 0;
static double g_thinking_seconds = 0;
static int g_response_line_start = 1;

static void print_prefix(void);

static void ensure_thinking(void) {
    if (!g_thinking_ready) {
        clay_str_init(&g_thinking);
        g_thinking_ready = 1;
    }
}

static void print_thinking_summary(double seconds) {
    printf("  %sReasoning finished in %.1fs%s\n",
           clay_color(CLAY_GRAY), seconds, clay_color(CLAY_RESET));
}

static void print_prefix(void) {
    printf("%s%s%s  ", clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND,
           clay_color(CLAY_RESET));
}

void clay_segments_println(const ClaySegment *segments, int count) {
    for (int i = 0; i < count; i++) {
        if (segments[i].color) fputs(clay_color(segments[i].color), stdout);
        fputs(segments[i].text, stdout);
        if (segments[i].color) fputs(clay_color(CLAY_RESET), stdout);
    }
    fputc('\n', stdout);
}

void clay_say(const char *fmt, ...) {
    print_prefix();
    fputs(clay_color(CLAY_GRAY), stdout);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

void clay_sayc(const char *color, const char *fmt, ...) {
    print_prefix();
    fputs(clay_color(color), stdout);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

void clay_response_begin(void) {
    fputc('\n', stdout);
    fputs(clay_color(CLAY_WHITE), stdout);
    g_response_line_start = 1;
    fflush(stdout);
}

void clay_response_write(const char *text) {
    if (!text || !*text) return;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (g_response_line_start && *p != '\n') {
            fputs("  ", stdout);
            g_response_line_start = 0;
        }
        fputc(*p, stdout);
        if (*p == '\n')
            g_response_line_start = 1;
        else if (*p != '\r')
            g_response_line_start = 0;
    }
    fflush(stdout);
}

void clay_response_end(void) {
    fputs(clay_color(CLAY_RESET), stdout);
    fflush(stdout);
}

int clay_response_prefix_width(void) {
    return 2;
}

void clay_turn_header(const char *model) {
    printf("%s%s%s %sAgent%s %s(%s)%s\n",
           clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND,
           clay_color(CLAY_RESET), clay_color(CLAY_WHITE),
           clay_color(CLAY_RESET), clay_color(CLAY_CORAL),
           model && *model ? model : "model", clay_color(CLAY_RESET));
}

void clay_thinking_begin(void) {
    ensure_thinking();
    clay_str_clear(&g_thinking);
    g_thinking_streaming = 1;
    g_thinking_expanded = 0;
    g_thinking_rows = 1;
    g_thinking_col = clay_response_prefix_width() +
                     (int)clay_utf8_width("Thinking: ");
    fputs("  ", stdout);
    fputs(clay_color(CLAY_GRAY), stdout);
    fputs("Thinking: ", stdout);
    fflush(stdout);
}

void clay_thinking_write(const char *text) {
    if (!text || !*text) return;
    ensure_thinking();
    clay_str_push(&g_thinking, text);
    int width = clay_term_width();
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        if (*p == '\n') {
            g_thinking_rows++;
            g_thinking_col = 0;
            p++;
            continue;
        }
        if (*p == '\r') {
            g_thinking_col = 0;
            p++;
            continue;
        }
        if (g_thinking_col >= width) {
            g_thinking_rows++;
            g_thinking_col = 0;
        }
        g_thinking_col++;
        if ((*p & 0xE0) == 0xC0)
            p += 2;
        else if ((*p & 0xF0) == 0xE0)
            p += 3;
        else if ((*p & 0xF8) == 0xF0)
            p += 4;
        else
            p++;
    }
    fputs(text, stdout);
    fflush(stdout);
}

void clay_thinking_finish(double seconds) {
    ensure_thinking();
    if (!g_thinking_streaming) return;
    g_thinking_seconds = seconds;
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
    clay_term_cursor_up(g_thinking_rows);
    for (int i = 0; i < g_thinking_rows; i++) {
        clay_term_clear_line();
        if (i + 1 < g_thinking_rows) clay_term_cursor_down(1);
    }
    if (g_thinking_rows > 1) clay_term_cursor_up(g_thinking_rows - 1);
    print_thinking_summary(seconds);
    g_thinking_streaming = 0;
    g_thinking_expanded = 0;
    fflush(stdout);
}

void clay_thinking_restore(const char *text, double seconds) {
    ensure_thinking();
    clay_str_clear(&g_thinking);
    clay_str_push(&g_thinking, text ? text : "");
    g_thinking_seconds = seconds;
    g_thinking_streaming = 0;
    g_thinking_expanded = 0;
    if (g_thinking.len) print_thinking_summary(seconds);
}

void clay_thinking_forget(void) {
    ensure_thinking();
    clay_str_clear(&g_thinking);
    g_thinking_streaming = 0;
    g_thinking_expanded = 0;
    g_thinking_seconds = 0;
}

int clay_thinking_can_toggle(void) {
    return g_thinking_ready && g_thinking.len > 0 &&
           !g_thinking_streaming && !g_thinking_expanded;
}

void clay_thinking_toggle(void) {
    ensure_thinking();
    if (!clay_thinking_can_toggle()) return;
    if (g_thinking_seconds > 0) {
        clay_sayc(CLAY_GRAY, "Reasoning (%.1fs):\n%s",
                  g_thinking_seconds, g_thinking.data);
    } else {
        clay_sayc(CLAY_GRAY, "Reasoning:\n%s", g_thinking.data);
    }
    g_thinking_expanded = 1;
    fflush(stdout);
}

void clay_list_header(const char *fmt, ...) {
    print_prefix();
    fputs(clay_color(CLAY_GRAY), stdout);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

void clay_list_step(int index, const char *verb, const char *target, const char *info, int link) {
    printf("%s%s%d.%s %s%s%s ", CLAY_INDENT, clay_color(CLAY_GRAY), index, clay_color(CLAY_RESET),
           clay_color(CLAY_WHITE), verb, clay_color(CLAY_RESET));

    fputs(clay_color(CLAY_CYAN), stdout);
    if (link) {
        clay_term_hyperlink_file(target);
    } else {
        fputs(target, stdout);
    }
    fputs(clay_color(CLAY_RESET), stdout);

    if (info) {
        printf(" %s(%s)%s", clay_color(CLAY_GRAY), info, clay_color(CLAY_RESET));
    }
    fputc('\n', stdout);
}

void clay_list_bullet(const char *fmt, ...) {
    printf("%s%s%s%s ", CLAY_INDENT, clay_color(CLAY_GRAY), CLAY_ICON_DOT, clay_color(CLAY_RESET));
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputc('\n', stdout);
}

void clay_tool_output_line(const char *fmt, ...) {
    printf("    %s%s%s ", clay_color(CLAY_GRAY), CLAY_ICON_DOT,
           clay_color(CLAY_RESET));
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputc('\n', stdout);
}
