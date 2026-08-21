#include "context.h"

void clay_cmd_new(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    clay_commands_new_chat(commands);
    clay_sayc(CLAY_GREEN, "New chat ready.");
}
