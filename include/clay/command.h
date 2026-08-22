#ifndef CLAY_COMMAND_H
#define CLAY_COMMAND_H

typedef void (*ClayCommandHandler)(const char *args, void *user_data);
typedef void (*ClayCommandVisitor)(const char *name, const char *description, void *ctx);

typedef struct ClayCommandRegistry ClayCommandRegistry;

ClayCommandRegistry *clay_command_registry_create(void);
void clay_command_registry_destroy(ClayCommandRegistry *reg);

/* `user_data` is handed back to `handler` untouched on every call. */
void clay_command_register(ClayCommandRegistry *reg, const char *name, const char *description,
                            ClayCommandHandler handler, void *user_data);
/* Registers another name for an existing command without listing it in /help. */
void clay_command_register_alias(ClayCommandRegistry *reg, const char *alias, ClayCommandHandler handler,
                                 void *user_data);
void clay_command_foreach(ClayCommandRegistry *reg, ClayCommandVisitor visitor, void *ctx);

typedef enum {
    CLAY_INPUT_EMPTY,
    CLAY_INPUT_COMMAND,
    CLAY_INPUT_MESSAGE
} ClayInputKind;

typedef struct {
    ClayInputKind kind;
    char *command; /* set when kind == CLAY_INPUT_COMMAND, without leading slash */
    char *args;    /* set when kind == CLAY_INPUT_COMMAND, remainder after the name */
    char *raw;     /* set when kind == CLAY_INPUT_MESSAGE, the full message text */
} ClayInput;

/* Parses a line into a command ("/name args...") or a message. Caller
   frees the result with clay_input_free. */
ClayInput clay_input_parse(const char *line);

/* Looks up input->command and calls its handler. Returns 1 if handled,
   0 if unknown or not a command. */
int clay_command_dispatch(ClayCommandRegistry *reg, const ClayInput *input);

void clay_input_free(ClayInput *input);

#endif /* CLAY_COMMAND_H */
