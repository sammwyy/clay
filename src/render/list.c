#include "clay/list.h"

#include "clay/color.h"
#include "clay/term.h"

#include <stdarg.h>
#include <stdio.h>

/* Aligns continuation lines under the text of a "◆ ℂlay  " prefix. */
#define CLAY_INDENT "         "

static void print_prefix(void) {
    printf("%s%s %slay%s  ", clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND, CLAY_ICON_COMPLEX, clay_color(CLAY_RESET));
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
    print_prefix();
    fputs(clay_color(CLAY_WHITE), stdout);
    fflush(stdout);
}

void clay_response_write(const char *text) {
    fputs(text, stdout);
    fflush(stdout);
}

void clay_response_end(void) {
    fputs(clay_color(CLAY_RESET), stdout);
    fflush(stdout);
}

int clay_response_prefix_width(void) {
    return (int)clay_utf8_width("\xe2\x97\x86 \xe2\x84\x82lay  ");
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
