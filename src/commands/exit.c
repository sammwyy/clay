#include "context.h"

void clay_cmd_exit(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    commands->running = 0;
    clay_app_set_state(commands->app, CLAY_APP_EXITING);
}
