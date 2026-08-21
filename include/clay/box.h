#ifndef CLAY_BOX_H
#define CLAY_BOX_H

typedef struct {
    const char *top_left;
    const char *top_right;
    const char *bottom_left;
    const char *bottom_right;
    const char *horizontal;
    const char *vertical;
} ClayBorder;

extern const ClayBorder CLAY_BORDER_ROUND;
extern const ClayBorder CLAY_BORDER_SQUARE;
extern const ClayBorder CLAY_BORDER_DOUBLE;

typedef struct {
    const ClayBorder *border;
    const char *color;      /* ANSI color applied to the border, NULL = none */
    const char *text_color; /* ANSI color applied to content lines, NULL = none */
    int padding_x;
    int indent;             /* left margin, in spaces, before the box */
    int width;               /* 0 = auto-size to the longest line */
} ClayBoxStyle;

ClayBoxStyle clay_box_style_default(void);

void clay_box(const char **lines, int line_count, const ClayBoxStyle *style);
void clay_box_line(const char *text, const ClayBoxStyle *style); /* single-line convenience */

#endif /* CLAY_BOX_H */
