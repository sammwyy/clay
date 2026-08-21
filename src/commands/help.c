#include "context.h"

#include <stdio.h>

static void print_entry(const char *name, const char *description, void *ctx) {
    (void)ctx;
    printf("  %s/%s%s %s%s%s\n", clay_color(CLAY_ORANGE), name, clay_color(CLAY_RESET),
           clay_color(CLAY_GRAY), description, clay_color(CLAY_RESET));
}

void clay_cmd_help(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    printf("\nAvailable commands:\n");
    clay_command_foreach(clay_app_commands(commands->app), print_entry, NULL);
    fputc('\n', stdout);
}
