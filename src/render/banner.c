#include "clay/render.h"

#include "clay/color.h"
#include "clay/term.h"

#include <stdlib.h>
#include <stdio.h>

void clay_banner(const char *version) {
    printf("%s%s%s  %sClay%s  %sv%s%s  %s·%s  %s/help%s for commands  %s·%s  "
           "%sCtrl+O%s reasoning\n",
           clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND,
           clay_color(CLAY_RESET), clay_color(CLAY_WHITE),
           clay_color(CLAY_RESET), clay_color(CLAY_GRAY), version,
           clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
           clay_color(CLAY_RESET), clay_color(CLAY_CYAN),
           clay_color(CLAY_GRAY), clay_color(CLAY_GRAY),
           clay_color(CLAY_RESET), clay_color(CLAY_CYAN),
           clay_color(CLAY_GRAY));
    char *cwd = clay_term_display_cwd();
    printf("  %s%s%s\n", clay_color(CLAY_GRAY), cwd, clay_color(CLAY_RESET));
    free(cwd);
    fputc('\n', stdout);
}
