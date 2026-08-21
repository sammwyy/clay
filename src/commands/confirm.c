#include "context.h"

void clay_cmd_confirm(const char *args, void *user_data) {
    ClayCommands *commands = user_data;
    const char *question = (args && *args) ? args : "Proceed?";
    int yes = clay_app_confirm(commands->app, question, 1);
    clay_sayc(yes ? CLAY_GREEN : CLAY_RED, "%s", yes ? "Confirmed." : "Cancelled.");
}
