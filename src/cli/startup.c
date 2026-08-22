#include "clay/cli.h"

#include "clay/term.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int clay_cli_startup_with_prompt(int argc, char **argv, const char *version,
                                 char **prompt_out) {
    if (prompt_out) *prompt_out = NULL;
    ClayCli *cli = clay_cli_create(argc > 0 ? argv[0] : "clay", "Terminal coding agent.");
    clay_cli_add_bool(cli, "help", "Show this help.");
    clay_cli_add_bool(cli, "version", "Print the version and exit.");
    clay_cli_add_bool(cli, "no-color", "Disable terminal colors.");
    clay_cli_add_string(cli, "cwd", "Run the agent from this directory.");
    clay_cli_add_string(cli, "prompt", "Send one prompt and exit.");
    clay_cli_add_string(cli, "p", "Alias for --prompt.");

    if (clay_cli_parse(cli, argc, argv) != 0) {
        fprintf(stderr, "Error: %s\n\n", clay_cli_error(cli));
        clay_cli_print_help(cli);
        clay_cli_destroy(cli);
        return -1;
    }
    if (clay_cli_bool(cli, "help")) {
        clay_cli_print_help(cli);
        clay_cli_destroy(cli);
        return 1;
    }
    if (clay_cli_bool(cli, "version")) {
        printf("clay %s\n", version);
        clay_cli_destroy(cli);
        return 1;
    }
    if (clay_cli_bool(cli, "no-color")) clay_term_set_color_enabled(0);

    const char *cwd = clay_cli_string(cli, "cwd");
    if (cwd && clay_term_change_dir(cwd) != 0) {
        fprintf(stderr, "Error: could not change directory to %s: %s\n", cwd, strerror(errno));
        clay_cli_destroy(cli);
        return -1;
    }

    const char *prompt = clay_cli_string(cli, "prompt");
    const char *short_prompt = clay_cli_string(cli, "p");
    if (prompt && short_prompt) {
        fprintf(stderr, "Error: use either --prompt or -p, not both.\n");
        clay_cli_destroy(cli);
        return -1;
    }
    if (prompt_out) {
        const char *value = prompt ? prompt : short_prompt;
        if (value) *prompt_out = strdup(value);
    }

    clay_cli_destroy(cli);
    return 0;
}

int clay_cli_startup(int argc, char **argv, const char *version) {
    return clay_cli_startup_with_prompt(argc, argv, version, NULL);
}
