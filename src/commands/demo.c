#include "context.h"

#include <stdio.h>

void clay_cmd_demo(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    clay_below_set_text("status", "");
    clay_below_set_state("status", CLAY_BELOW_LOADING);
    clay_below_set_enabled("status", 1);
    clay_below_start_elapsed("status");
    ClayTask *scan = clay_app_task_start(commands->app, "Scanning project");
    clay_term_sleep_ms(600);
    clay_app_task_success(commands->app, scan, "42 files indexed");
    clay_commands_set_tokens_below(commands, 128, 0);
    clay_app_list_header(commands->app, "Plan:");
    clay_app_list_step(commands->app, 1, "Add", "src/middleware/rateLimit.ts", "new, token-bucket", 1);
    clay_app_list_step(commands->app, 2, "Wire into", "src/routes/auth.ts", "login, signup, refresh", 1);
    clay_app_list_step(commands->app, 3, "Add tests in", "tests/rateLimit.test.ts", NULL, 1);
    clay_app_list_step(commands->app, 4, "Run", "test suite", NULL, 0);
    fputc('\n', stdout);
    ClayTask *write = clay_app_task_start(commands->app, "Writing files");
    clay_term_sleep_ms(500);
    clay_app_task_success(commands->app, write, "3 files changed");
    ClayTask *tests = clay_app_task_start(commands->app, "Running %snpm test%s", clay_color(CLAY_CYAN), clay_color(CLAY_GRAY));
    clay_term_sleep_ms(1200);
    clay_app_task_success(commands->app, tests, "87 passed, 0 failed");
    clay_below_stop_elapsed("status");
    clay_below_set_text("status", "2.3s");
    clay_commands_set_tokens_below(commands, 128, 340);
    clay_below_set_state("status", CLAY_BELOW_FINISHED);
    fputc('\n', stdout);
    clay_app_say(commands->app, "Done. Commit with %sclay commit%s or review with %sclay diff%s.",
                 clay_color(CLAY_CYAN), clay_color(CLAY_GRAY), clay_color(CLAY_CYAN), clay_color(CLAY_GRAY));
}
