#include "clay/render.h"

#include "clay/color.h"
#include "clay/term.h"

#include <stdio.h>

void clay_banner(const char *name, const char *version) {
    printf("%s%s%s %sv%s%s  %sType /help for view available commands.%s\n\n", clay_color(CLAY_ORANGE), name,
           clay_color(CLAY_RESET), clay_color(CLAY_WHITE), version, clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
           clay_color(CLAY_RESET));
}
