#ifndef CLAY_BELOW_H
#define CLAY_BELOW_H

#include <stddef.h>

typedef enum {
    CLAY_BELOW_NONE,     /* no icon */
    CLAY_BELOW_LOADING,  /* animated spinner */
    CLAY_BELOW_FINISHED, /* green check */
    CLAY_BELOW_IDLE      /* clock */
} ClayBelowState;

/* Registers a status module below the prompt, ordered by `index` (lower
   first). Re-adding an existing id only updates its index. New modules
   start with empty text, CLAY_BELOW_NONE, enabled. */
void clay_below_add(int index, const char *id);

/* No-ops if `id` was never added. */
void clay_below_set_text(const char *id, const char *content);
void clay_below_set_state(const char *id, ClayBelowState state);
void clay_below_set_enabled(const char *id, int enabled);
void clay_below_reorder(const char *id, int index);

/* Shows a fixed-width elapsed timer in place of a module's text. */
void clay_below_start_elapsed(const char *id);
void clay_below_stop_elapsed(const char *id);

/* Gates the background animator's redraws to when a prompt is actually
   being edited. Managed by clay_prompt_line. */
void clay_below_set_editing(int editing);

/* Redraws the prompt line (`input`) plus enabled modules as one block;
   the terminal cursor is placed at display column `cursor` (a byte
   offset into `input`; ANSI escapes in the prefix up to it don't count
   towards the column). Thread-safe. */
void clay_below_render(const char *input, size_t cursor);

/* Renders only the enabled modules on the current row, without a prompt.
   Used for live status below streamed output. */
void clay_below_render_status(void);

/* Keeps a status-only row below streamed text. insert_above reserves the
   first response row; call push_down before each streamed newline, then
   prepare_prompt when the stream completes. prepare_prompt leaves a blank
   separator row before the next prompt. */
void clay_below_status_insert_above(void);
void clay_below_status_push_down(void);
void clay_below_status_finish_output(void);
void clay_below_status_refresh_below(void);
void clay_below_status_prepare_prompt(void);

/* Erases the modules row and moves the cursor to a fresh line below it.
   The prompt line itself stays on screen as history. */
void clay_below_finish(void);

#endif /* CLAY_BELOW_H */
