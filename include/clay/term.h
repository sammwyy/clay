#ifndef CLAY_TERM_H
#define CLAY_TERM_H

#include "clay/str.h"
#include "clay/array.h"

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

/* Runs a shell command in the current directory, appending combined
   stdout/stderr to output up to output_limit. */
int clay_term_shell_exec(const char *command, ClayStr *output, size_t output_limit, int *exit_code,
                         int *output_truncated);
int clay_term_change_dir(const char *path);

/* Appends `value` to `out`, quoted so the shell clay_term_shell_exec/
   clay_sandbox_exec invokes it through treats it as one literal argument.
   Targets POSIX shells (sh/bash) - the shell clay uses everywhere sandbox
   mode is supported. */
void clay_term_shell_quote(ClayStr *out, const char *value);

/* Process-wide environment variable, inherited by every child process
   started afterward (clay_term_shell_exec, clay_sandbox_exec). Set value
   to NULL to unset. */
void clay_term_setenv(const char *name, const char *value);

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
    CLAY_KEY_NONE,
    CLAY_KEY_CHAR,
    CLAY_KEY_ENTER,
    CLAY_KEY_BACKSPACE,
    CLAY_KEY_UP,
    CLAY_KEY_DOWN,
    CLAY_KEY_LEFT,
    CLAY_KEY_RIGHT,
    CLAY_KEY_ESCAPE,
    CLAY_KEY_INTERRUPT,
    CLAY_KEY_EOF
} ClayKey;

/* Raw mode: no line buffering, no echo. Pair enable/disable around raw
   reads. */
void clay_term_raw_enable(void);
void clay_term_raw_disable(void);

/* True once after Ctrl-C/SIGINT is received; consumes that notification. */
int clay_term_take_interrupt(void);

/* Reads one key in raw mode; *ch_out (if set) receives the char for
   CLAY_KEY_CHAR. */
ClayKey clay_term_read_key(char *ch_out);
ClayKey clay_term_read_key_timeout(char *ch_out, int timeout_ms);
int clay_term_take_escape(void);

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

/* A long-lived child process wired up for line-based bidirectional
   communication over its stdin/stdout (its stderr is inherited, so
   diagnostics reach the terminal directly) - for a protocol like MCP's
   stdio transport, as opposed to clay_term_shell_exec's one-shot capture. */
typedef struct ClayProcess ClayProcess;

/* Starts `command` (found via PATH) with `argv` (NULL-terminated,
   argv[0] conventionally the program name). NULL on failure. */
ClayProcess *clay_term_process_start(const char *command, char *const argv[]);

/* Writes `len` bytes to the process's stdin. 0 on success, -1 on failure. */
int clay_term_process_write(ClayProcess *process, const char *data, size_t len);

/* Reads one line from the process's stdout, blocking until a newline or
   EOF; strips the trailing newline. Malloc'd; NULL on EOF or error. */
char *clay_term_process_read_line(ClayProcess *process);

/* Closes the process's pipes, terminates it if still running, and waits
   for it to exit. Frees `process`. */
void clay_term_process_stop(ClayProcess *process);

/* $HOME, or %USERPROFILE% on Windows. Malloc'd; NULL if unset. */
char *clay_term_home_dir(void);

/* Human-readable OS name/version, e.g. "Fedora Linux 41" or "Windows".
   Malloc'd, never NULL. */
char *clay_term_platform_name(void);

/* Current working directory, absolute. Malloc'd; NULL on failure. */
char *clay_term_cwd(void);

/* Creates one directory level (not recursive). 0 on success or if it
   already exists, nonzero on failure. */
int clay_term_mkdir(const char *path);
int clay_term_list_dir(const char *path, ClayArray *names); /* char *, caller frees entries; dirs only */
int clay_term_list_entries(const char *path, ClayArray *names); /* char *, caller frees entries; files and dirs */
int clay_term_is_dir(const char *path);
long long clay_term_file_modified_at(const char *path);
int clay_term_random_bytes(unsigned char *bytes, size_t count);

/* Restricts `path` to owner-only access (POSIX chmod 0600); no-op on
   Windows. For files holding secrets. */
void clay_term_restrict_file(const char *path);

#endif /* CLAY_TERM_H */
