#include "context.h"

#include <stdlib.h>

void clay_cmd_history(const char *args, void *user_data) {
    ClayCommands *commands = user_data;
    if (!commands->chat) {
        clay_sayc(CLAY_GRAY, "No active chat. Send a message or use /resume.");
        return;
    }
    long count = args && *args ? strtol(args, NULL, 10) : clay_config_history_preview_count();
    if (count <= 0) return;
    clay_sayc(CLAY_CYAN, "Last %ld messages:", count);
    clay_commands_print_history(commands, (size_t)count);
}
