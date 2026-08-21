#include "clay/box.h"

#include "clay/color.h"
#include "clay/term.h"

#include <stdio.h>

const ClayBorder CLAY_BORDER_ROUND = {
    "\xe2\x95\xad", "\xe2\x95\xae", "\xe2\x95\xb0", "\xe2\x95\xaf",
    "\xe2\x94\x80", "\xe2\x94\x82"
};

const ClayBorder CLAY_BORDER_SQUARE = {
    "\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94", "\xe2\x94\x98",
    "\xe2\x94\x80", "\xe2\x94\x82"
};

const ClayBorder CLAY_BORDER_DOUBLE = {
    "\xe2\x95\x94", "\xe2\x95\x97", "\xe2\x95\x9a", "\xe2\x95\x9d",
    "\xe2\x95\x90", "\xe2\x95\x91"
};

ClayBoxStyle clay_box_style_default(void) {
    ClayBoxStyle style;
    style.border = &CLAY_BORDER_SQUARE;
    style.color = CLAY_ORANGE;
    style.text_color = NULL;
    style.padding_x = 2;
    style.indent = 2;
    style.width = 0;
    return style;
}

static void print_repeated(const char *s, int times) {
    for (int i = 0; i < times; i++) fputs(s, stdout);
}

void clay_box(const char **lines, int line_count, const ClayBoxStyle *style_in) {
    ClayBoxStyle style = style_in ? *style_in : clay_box_style_default();
    const ClayBorder *b = style.border ? style.border : &CLAY_BORDER_SQUARE;

    int max_len = 0;
    for (int i = 0; i < line_count; i++) {
        int w = (int)clay_utf8_width(lines[i]);
        if (w > max_len) max_len = w;
    }
    int content_width = style.width > max_len ? style.width : max_len;

    char indent[64] = {0};
    for (int i = 0; i < style.indent && i < 63; i++) indent[i] = ' ';

    /* top border */
    fputs(indent, stdout);
    if (style.color) fputs(clay_color(style.color), stdout);
    fputs(b->top_left, stdout);
    print_repeated(b->horizontal, content_width + style.padding_x * 2);
    fputs(b->top_right, stdout);
    if (style.color) fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);

    /* content */
    for (int i = 0; i < line_count; i++) {
        int w = (int)clay_utf8_width(lines[i]);
        int pad_right = content_width - w;

        fputs(indent, stdout);
        if (style.color) fputs(clay_color(style.color), stdout);
        fputs(b->vertical, stdout);
        if (style.color) fputs(clay_color(CLAY_RESET), stdout);
        print_repeated(" ", style.padding_x);
        if (style.text_color) fputs(clay_color(style.text_color), stdout);
        fputs(lines[i], stdout);
        if (style.text_color) fputs(clay_color(CLAY_RESET), stdout);
        print_repeated(" ", pad_right + style.padding_x);
        if (style.color) fputs(clay_color(style.color), stdout);
        fputs(b->vertical, stdout);
        if (style.color) fputs(clay_color(CLAY_RESET), stdout);
        fputc('\n', stdout);
    }

    /* bottom border */
    fputs(indent, stdout);
    if (style.color) fputs(clay_color(style.color), stdout);
    fputs(b->bottom_left, stdout);
    print_repeated(b->horizontal, content_width + style.padding_x * 2);
    fputs(b->bottom_right, stdout);
    if (style.color) fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

void clay_box_line(const char *text, const ClayBoxStyle *style) {
    clay_box(&text, 1, style);
}
