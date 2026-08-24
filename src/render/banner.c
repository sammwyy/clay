#include "clay/render.h"

#include "clay/color.h"
#include "clay/term.h"

#include <stdio.h>

void clay_banner(const char *version) {
    printf("%s%s%s  %sClay%s  %sv%s%s\n",
           clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND,
           clay_color(CLAY_RESET), clay_color(CLAY_WHITE),
           clay_color(CLAY_RESET), clay_color(CLAY_GRAY), version,
           clay_color(CLAY_RESET));
    printf("%s  %s/help%s for commands  %s·%s  %sCtrl+O%s reasoning\n",
           clay_color(CLAY_GRAY), clay_color(CLAY_CYAN),
           clay_color(CLAY_GRAY), clay_color(CLAY_GRAY),
           clay_color(CLAY_GRAY), clay_color(CLAY_CYAN),
           clay_color(CLAY_GRAY));
    fputc('\n', stdout);
}
