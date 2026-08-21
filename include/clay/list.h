#ifndef CLAY_LIST_H
#define CLAY_LIST_H

/* One colored piece of text inside a composed line. NULL color = default fg. */
typedef struct {
    const char *text;
    const char *color;
} ClaySegment;

/* Prints segments back to back on one line, each in its own color. */
void clay_segments_println(const ClaySegment *segments, int count);

/* Prints "◆ clay  <message>" - the standard response line prefix. */
void clay_say(const char *fmt, ...);

/* Same as clay_say, but the message itself is drawn in `color`. */
void clay_sayc(const char *color, const char *fmt, ...);

/* Prints "◆ clay  <message>" meant to head a list of steps that follow. */
void clay_list_header(const char *fmt, ...);

/* Prints "N. <verb> <target> (<info>)", indented under a clay_list_header.
   `verb` is white, `target` is cyan (optionally a clickable file link),
   `info` is dim gray inside parens. Pass info = NULL to omit it. */
void clay_list_step(int index, const char *verb, const char *target, const char *info, int link);

/* Prints a plain bullet item ("  · text"), for simple unordered lists. */
void clay_list_bullet(const char *fmt, ...);

#endif /* CLAY_LIST_H */
