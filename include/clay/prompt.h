#ifndef CLAY_PROMPT_H
#define CLAY_PROMPT_H

#include <stddef.h>

typedef struct ClayCommandRegistry ClayCommandRegistry;

/* Reads one line, printing the "> " prompt. On a tty: up/down recall
   history, below-modules render live. Returns malloc'd string (caller
   frees), NULL on EOF with no input. */
char *clay_prompt_line(ClayCommandRegistry *commands);

/* True when the most recent clay_prompt_line was cancelled by Ctrl-C. */
int clay_prompt_was_interrupted(void);

/* Same, but echoes `*` per character instead of the real text (paste
   still works, just masked) and skips history. Prints `question` first.
   Malloc'd, caller frees; NULL on EOF with no input. */
char *clay_prompt_secret(const char *question);

/* Submitted lines, oldest first, most recent last; consecutive duplicate
   entries are collapsed into one. */
size_t clay_prompt_history_count(void);
const char *clay_prompt_history_get(size_t index); /* NULL if out of range */
void clay_prompt_history_clear(void);

typedef struct {
    const char *title;
    const char *desc; /* optional, shown in gray to the right of the title; NULL to omit */
} ClayChoice;

/* Horizontal picker: left/right + Enter, no custom text. Non-tty
   fallback reads a number. Returns chosen index, or `default_index` on
   EOF/Escape. */
int clay_prompt_select(const char *question, const ClayChoice *options, int count, int default_index);

/* Yes/No confirmation built on clay_prompt_select. Returns 1 for yes,
   0 for no; `default_yes` picks the option pre-selected on entry. */
int clay_prompt_confirm(const char *question, int default_yes);

/* Vertical picker: up/down + Enter. If `allow_custom`, an extra "Type
   your own..." row switches to free text. Non-tty fallback: number or
   free text. Returns chosen index, or -1 with *custom_out set to a
   malloc'd copy for free text (untouched on EOF/Escape). */
int clay_prompt_choice(const char *question, const ClayChoice *choices, int count,
                        int allow_custom, char **custom_out);

#endif /* CLAY_PROMPT_H */
