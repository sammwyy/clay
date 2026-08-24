#ifndef CLAY_TERM_H
#define CLAY_TERM_H

#include "clay/array.h"
#include "clay/str.h"

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

/* Notifies the user when an interactive session needs attention. */
void clay_term_notify(const char *title, const char *message);

/* Runs a shell command in the current directory, appending combined
   stdout/stderr to output up to output_limit. */
int clay_term_shell_exec(const char *command, ClayStr *output,
                         size_t output_limit, int *exit_code,
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
int clay_term_supports_color(
    void); /* terminal capability: isatty + not "dumb" */
int clay_term_is_interactive(void); /* stdin and stdout are both a real tty */
/* Forces interactive terminal behavior off for one-shot/scripted runs,
   even when the process itself is attached to a tty. */
void clay_term_set_noninteractive(int noninteractive);

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
  CLAY_KEY_CYCLE_SANDBOX,
  CLAY_KEY_THINK_TOGGLE,
  CLAY_KEY_CLEAR_SCREEN,
  CLAY_KEY_HISTORY_SEARCH,
  CLAY_KEY_ESCAPE,
  CLAY_KEY_INTERRUPT,
  CLAY_KEY_EOF
} ClayKey;

/* Raw mode: no line buffering, no echo. Pair enable/disable around raw
   reads. */
void clay_term_raw_enable(void);
void clay_term_raw_disable(void);
int clay_term_take_think_toggle(void);

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

/* Writes a file through a sibling temporary file and atomically replaces the
   destination. The temporary file is removed on failure. */
int clay_term_write_file_atomic(const char *path, const void *data, size_t len);

/* Reads at most max_bytes into a NUL-terminated string. max_bytes == 0 means
   unlimited; exceeding a non-zero limit returns -1 with errno == EFBIG. */
int clay_term_read_file(const char *path, size_t max_bytes, ClayStr *out);
int clay_term_list_dir(
    const char *path,
    ClayArray *names); /* char *, caller frees entries; dirs only */
int clay_term_list_entries(
    const char *path,
    ClayArray *names); /* char *, caller frees entries; files and dirs */
int clay_term_is_dir(const char *path);
long long clay_term_file_modified_at(const char *path);
int clay_term_random_bytes(unsigned char *bytes, size_t count);

/* Opens a URL using the operating system's normal browser association.
   This never routes through a shell. */
int clay_term_open_browser(const char *url);

/* A tiny loopback-only HTTP listener for short-lived OAuth callbacks. */
typedef struct ClayTermHttpServer ClayTermHttpServer;
ClayTermHttpServer *clay_term_http_server_create(unsigned short port);
/* The bound port (useful when create(0) asks the OS for an ephemeral port). */
unsigned short clay_term_http_server_port(const ClayTermHttpServer *server);
/* 1 = request received, 0 = timed out, -1 = listener error. */
int clay_term_http_server_receive(ClayTermHttpServer *server, ClayStr *request,
                                  int timeout_ms);
int clay_term_http_server_reply(ClayTermHttpServer *server, int status,
                                const char *body);
/* `extra_headers` must contain complete CRLF-terminated header lines and is
   intended for fixed, trusted headers such as a narrow OAuth CORS policy. */
int clay_term_http_server_reply_with_headers(ClayTermHttpServer *server,
                                             int status, const char *body,
                                             const char *extra_headers);
void clay_term_http_server_destroy(ClayTermHttpServer *server);

/* Restricts `path` to owner-only access (POSIX chmod 0600); no-op on
   Windows. For files holding secrets. */
void clay_term_restrict_file(const char *path);

#endif /* CLAY_TERM_H */
