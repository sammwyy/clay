#include "clay/clay.h"
#include "clay/http.h"

#include <stdio.h>
#include <stdlib.h>

#define CLAY_VERSION "0.0.0"

int main(int argc, char **argv) {
    int cli_status = clay_cli_startup(argc, argv, CLAY_VERSION);
    if (cli_status != 0) return cli_status < 0 ? 1 : 0;

    clay_term_init();
    if (clay_http_init() != 0) {
        fprintf(stderr, "Failed to initialize HTTP.\n");
        return 1;
    }
    clay_banner("clay", CLAY_VERSION, "your AI code agent");

    ClayApp *app = clay_app_create();
    ClayCommands *commands = clay_commands_create(app);
    clay_commands_register(commands);

    int interrupted = 0;
    while (clay_commands_running(commands)) {
        char *line = clay_prompt_line();
        if (!line) {
            interrupted = clay_prompt_was_interrupted() || clay_term_take_interrupt();
            break;
        }

        ClayInput input = clay_input_parse(line);
        free(line);
        int prompt_ready = 0;
        if (input.kind == CLAY_INPUT_COMMAND) {
            if (!clay_command_dispatch(clay_app_commands(app), &input)) {
                clay_sayc(CLAY_RED, "Unknown command: /%s (try /help)", input.command);
            }
        } else if (input.kind == CLAY_INPUT_MESSAGE) {
            prompt_ready = clay_commands_run_message(commands, input.raw);
        }
        clay_input_free(&input);
        if (clay_term_take_interrupt()) {
            interrupted = 1;
            break;
        }
        if (!prompt_ready) fputc('\n', stdout);
    }

    if (interrupted) clay_commands_print_session_summary(commands);
    clay_commands_destroy(commands);
    clay_app_destroy(app);
    clay_http_cleanup();
    if (!interrupted) printf("%sGoodbye.%s\n", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    return 0;
}
