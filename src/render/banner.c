#include "clay/render.h"

#include "clay/box.h"
#include "clay/color.h"
#include "clay/str.h"
#include "clay/term.h"

#include <stdio.h>

void clay_banner(const char *name, const char *version, const char *tagline) {
    ClayStr line;
    clay_str_init(&line);
    clay_str_printf(&line, "%s%s%s", clay_color(CLAY_ORANGE), name, clay_color(CLAY_RESET));
    clay_str_printf(&line, " %sv%s%s", clay_color(CLAY_WHITE), version, clay_color(CLAY_RESET));
    clay_str_printf(&line, " %s\xe2\x80\x94 %s%s", clay_color(CLAY_GRAY), tagline, clay_color(CLAY_RESET));

    ClayBoxStyle style = clay_box_style_default();
    style.border = &CLAY_BORDER_SQUARE;
    style.color = CLAY_GRAY;
    style.text_color = NULL; /* line is already pre-colored per segment */
    style.padding_x = 2;
    style.indent = 2;

    fputc('\n', stdout);
    clay_box_line(line.data, &style);
    fputc('\n', stdout);

    clay_str_free(&line);
}
