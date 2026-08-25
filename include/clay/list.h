#ifndef CLAY_LIST_H
#define CLAY_LIST_H

#include "clay/str.h"

/* One colored piece of text inside a composed line. NULL color = default fg. */
typedef struct {
    const char *text;
    const char *color;
} ClaySegment;

/* Prints segments back to back on one line, each in its own color. */
void clay_segments_println(const ClaySegment *segments, int count);

/* Prints "◆  <message>" - the standard response line prefix. */
void clay_say(const char *fmt, ...);

/* Same as clay_say, but the message itself is drawn in `color`. */
void clay_sayc(const char *color, const char *fmt, ...);

/* Begins, incrementally writes, and ends one streamed assistant reply.
   The writer flushes each chunk so text appears as the provider sends it.
   Ending restores the terminal style but leaves cursor placement to caller. */
void clay_response_begin(void);
void clay_response_write(const char *text);
void clay_response_end(void);
int clay_response_prefix_width(void);

/* Persistent structure around one assistant execution. */
void clay_turn_header(const char *model);

/* Streams and toggles the latest assistant reasoning block in the chat. */
/* `before_new_row` (may be NULL) runs just before the block enters another
   terminal row, so a caller keeping a status row pinned below can push it
   down first. */
void clay_thinking_begin(void (*before_new_row)(void));
void clay_thinking_write(const char *text);

/* Greedy word wrap for text that arrives in chunks. Complete words go out as
   they arrive; the trailing partial word waits so it can move to the next
   row whole. `write` receives the text, `break_row` starts a new row (and is
   where a caller repaints anything pinned below). */
typedef struct {
    int col;    /* column the next character lands on */
    int indent; /* column text resumes at after a break */
    ClayStr word;
    void (*write)(const char *text, void *user_data);
    void (*break_row)(void *user_data);
    void *user_data;
} ClayWrap;

void clay_wrap_init(ClayWrap *wrap, int indent,
                    void (*write)(const char *text, void *user_data),
                    void (*break_row)(void *user_data), void *user_data);
void clay_wrap_write(ClayWrap *wrap, const char *text);
/* Writes whatever word is still buffered. Call before ending the block. */
void clay_wrap_flush(ClayWrap *wrap);
void clay_wrap_free(ClayWrap *wrap);
void clay_thinking_finish(double seconds);
void clay_thinking_restore(const char *text, double seconds);
void clay_thinking_forget(void);
int clay_thinking_can_toggle(void);
void clay_thinking_toggle(void);

/* Prints "◆  <message>" meant to head a list of steps that follow. */
void clay_list_header(const char *fmt, ...);

/* Prints "N. <verb> <target> (<info>)", indented under a clay_list_header.
   `verb` is white, `target` is cyan (optionally a clickable file link),
   `info` is dim gray inside parens. Pass info = NULL to omit it. */
void clay_list_step(int index, const char *verb, const char *target, const char *info, int link);

/* One line of the model's plan: a green check for a finished step, an
   orange arrow for the one running, a dim dot for the rest. */
typedef enum {
    CLAY_STEP_PENDING,
    CLAY_STEP_ACTIVE,
    CLAY_STEP_DONE
} ClayStepState;
void clay_plan_step(ClayStepState state, const char *text);

/* Prints a plain bullet item ("  · text"), for simple unordered lists. */
void clay_list_bullet(const char *fmt, ...);

/* Prints a nested line belonging to a tool result ("    · text"). */
void clay_tool_output_line(const char *fmt, ...);

#endif /* CLAY_LIST_H */
