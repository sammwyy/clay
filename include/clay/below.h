#ifndef CLAY_BELOW_H
#define CLAY_BELOW_H

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

/* Gates the background animator's redraws to when a prompt is actually
   being edited. Managed by clay_prompt_line. */
void clay_below_set_editing(int editing);

/* Redraws the prompt line (`input`) plus enabled modules as one block;
   cursor ends at the end of `input`. Thread-safe. */
void clay_below_render(const char *input);

/* Erases the modules row and moves the cursor to a fresh line below it.
   The prompt line itself stays on screen as history. */
void clay_below_finish(void);

#endif /* CLAY_BELOW_H */
