#ifndef CLAY_TERM_H
#define CLAY_TERM_H

#include <stddef.h>

/* Enables ANSI escapes on Windows consoles; no-op elsewhere. Call once
   at startup. */
void clay_term_init(void);

void clay_term_hide_cursor(void);
void clay_term_show_cursor(void);
void clay_term_clear_line(void);
void clay_term_cursor_up(int n);
void clay_term_cursor_down(int n);
void clay_term_cursor_col(int col); /* 0-based column, absolute */
void clay_term_sleep_ms(int ms);

int clay_term_width(void);
int clay_term_supports_color(void); /* terminal capability: isatty + not "dumb" */
int clay_term_is_interactive(void); /* stdin and stdout are both a real tty */

/* On/off switch for clay_color(). Starts enabled unless NO_COLOR is set. */
void clay_term_set_color_enabled(int enabled);
int clay_term_color_enabled(void);

/* Returns `code` if colors are enabled, else "". Wrap every ANSI
   constant passed to printf/fputs with this. */
const char *clay_color(const char *code);

/* Display columns in a UTF-8 string; skips ANSI escapes, no wide-glyph
   handling. */
size_t clay_utf8_width(const char *s);

/* Prints `text` as an OSC 8 hyperlink to `url`; plain text if the
   terminal doesn't support hyperlinks. */
void clay_term_hyperlink(const char *url, const char *text);

/* Same, but resolves `path` to a file:// URI when it exists; the
   clickable label stays `path`. */
void clay_term_hyperlink_file(const char *path);

typedef enum {
    CLAY_KEY_CHAR,
    CLAY_KEY_ENTER,
    CLAY_KEY_BACKSPACE,
    CLAY_KEY_UP,
    CLAY_KEY_DOWN,
    CLAY_KEY_LEFT,
    CLAY_KEY_RIGHT,
    CLAY_KEY_ESCAPE,
    CLAY_KEY_EOF
} ClayKey;

/* Raw mode: no line buffering, no echo. Pair enable/disable around raw
   reads. */
void clay_term_raw_enable(void);
void clay_term_raw_disable(void);

/* Reads one key in raw mode; *ch_out (if set) receives the char for
   CLAY_KEY_CHAR. */
ClayKey clay_term_read_key(char *ch_out);

/* True if another byte is already sitting in stdin's input buffer, i.e.
   a follow-up read won't block. A paste lands in that buffer all at
   once, while typed keystrokes trickle in with a real gap between them
   - this is what lets a caller tell the two apart without bracketed
   paste mode. */
int clay_term_input_pending(void);

/* Moves the cursor down one row into `row` (row 1, 2, ... counted from a
   caller-tracked row 0). Uses cursor-down if the row already exists
   (*established > row, never scrolls), or a real '\n' if it's new
   territory (*established <= row, scrolls if needed; bumps
   *established). Used to redraw multi-row live widgets without walking
   them up the screen when pinned to the bottom of the terminal. */
void clay_term_row_enter(int row, int *established);

#endif /* CLAY_TERM_H */
