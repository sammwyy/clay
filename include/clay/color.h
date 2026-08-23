#ifndef CLAY_COLOR_H
#define CLAY_COLOR_H

/* Text attributes */
#define CLAY_RESET     "\x1b[0m"
#define CLAY_BOLD      "\x1b[1m"
#define CLAY_DIM       "\x1b[2m"
#define CLAY_UNDERLINE "\x1b[4m"

/* Palette. clay's accent is orange; everything else stays neutral so the
   accent keeps standing out. True-color escapes, no 256-color fallback. */
#define CLAY_ORANGE "\x1b[38;2;255;140;66m"
#define CLAY_GRAY   "\x1b[38;2;158;158;168m"
#define CLAY_WHITE  "\x1b[38;2;242;242;247m"
#define CLAY_CYAN   "\x1b[38;2;102;204;204m"
#define CLAY_GREEN  "\x1b[38;2;96;200;120m"
#define CLAY_YELLOW "\x1b[38;2;230;190;80m"
#define CLAY_CORAL  "\x1b[38;2;240;138;120m"
#define CLAY_RED    "\x1b[38;2;225;95;95m"
#define CLAY_PINK   "\x1b[38;2;238;130;190m"

/* Icons (UTF-8) */
#define CLAY_ICON_DIAMOND "\xe2\x97\x86" /* ◆ */
#define CLAY_ICON_COMPLEX  "\xe2\x84\x82" /* ℂ */
#define CLAY_ICON_CHECK   "\xe2\x9c\x93" /* ✓ */
#define CLAY_ICON_CROSS   "\xe2\x9c\x97" /* ✗ */
#define CLAY_ICON_DOT     "\xc2\xb7"     /* · */
#define CLAY_ICON_ARROW   "\xe2\x86\x92" /* → */
#define CLAY_ICON_SLEEP   "\xf0\x9f\x92\xa4" /* 💤 */
#define CLAY_ICON_PROMPT  "\xe2\x80\xba" /* › */

#endif /* CLAY_COLOR_H */
