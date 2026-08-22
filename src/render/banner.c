#include "clay/render.h"

#include "clay/color.h"
#include "clay/term.h"

#include <stdio.h>

void clay_banner(const char *version) {
    printf("%s%s %slay%s %s%s%s %sv%s%s %s%s%s %sType /help for commands.%s\n\n", clay_color(CLAY_ORANGE),
           CLAY_ICON_DIAMOND, CLAY_ICON_COMPLEX, clay_color(CLAY_RESET), clay_color(CLAY_GRAY), CLAY_ICON_DOT,
           clay_color(CLAY_RESET), clay_color(CLAY_WHITE), version, clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
           CLAY_ICON_DOT, clay_color(CLAY_RESET), clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
}
