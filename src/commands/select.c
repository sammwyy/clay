#include "context.h"

void clay_cmd_select(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    ClayChoice options[] = {
        {"Staging", "safe, resettable"},
        {"Production", "live traffic"},
        {"Cancel", NULL},
    };
    int index = clay_app_select(commands->app, "Deploy to which environment?", options, 3, 0);
    clay_sayc(CLAY_CYAN, "Selected: %s", options[index].title);
}
