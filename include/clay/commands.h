#ifndef CLAY_COMMANDS_H
#define CLAY_COMMANDS_H

typedef struct ClayApp ClayApp;
typedef struct ClayCommands ClayCommands;

ClayCommands *clay_commands_create(ClayApp *app);
void clay_commands_destroy(ClayCommands *commands);
void clay_commands_register(ClayCommands *commands);
int clay_commands_running(const ClayCommands *commands);
int clay_commands_run_message(ClayCommands *commands, const char *input);
void clay_commands_print_session_summary(const ClayCommands *commands);

#endif /* CLAY_COMMANDS_H */
