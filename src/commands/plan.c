#include "context.h"

void clay_cmd_plan(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    if (commands->mode == CLAY_MODE_PLAN) {
        commands->mode = CLAY_MODE_ACT;
        clay_below_set_enabled("mode", 0);
        clay_sayc(CLAY_GREEN, "Act mode: shell_exec, write, and edit run normally.");
    } else {
        commands->mode = CLAY_MODE_PLAN;
        clay_below_set_text("mode", "Plan");
        clay_below_set_enabled("mode", 1);
        clay_sayc(CLAY_GREEN,
                 "Plan mode: write/edit are refused and shell_exec blocks mutating commands "
                 "(rm/mv/cp, non-read git subcommands, ...). Run /plan again to return to Act mode.");
    }
}
