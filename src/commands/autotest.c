#include "context.h"

#include <stdlib.h>
#include <string.h>

void clay_cmd_autotest(const char *args, void *user_data) {
    ClayCommands *commands = user_data;

    if (args && strcmp(args, "clear") == 0) {
        free(commands->auto_test_command);
        commands->auto_test_command = strdup("");
        commands->auto_test_choice = CLAY_AUTO_TEST_UNASKED;
        clay_config_set_auto_test_command("");
        clay_sayc(CLAY_GREEN, "Auto-test command cleared.");
        return;
    }

    if (args && *args) {
        free(commands->auto_test_command);
        commands->auto_test_command = strdup(args);
        commands->auto_test_choice = CLAY_AUTO_TEST_UNASKED;
        clay_config_set_auto_test_command(args);
        clay_sayc(CLAY_GREEN, "Auto-test command set to: %s", args);
        clay_sayc(CLAY_GRAY, "You'll be asked to confirm the first time it runs after an edit this session.");
        return;
    }

    if (*commands->auto_test_command) {
        clay_sayc(CLAY_CYAN, "Auto-test command: %s", commands->auto_test_command);
    } else {
        clay_sayc(CLAY_GRAY, "No auto-test command configured. Usage: /autotest <command>, /autotest clear.");
    }
}
