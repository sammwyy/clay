#define _GNU_SOURCE

#include "../src/commands/context.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

static void assert_mode(ClayCommands *commands, ClaySandboxMode mode, int auto_approve,
                        const char *persisted) {
    assert(commands->sandbox_mode == mode);
    assert(commands->sandbox_auto_approve == auto_approve);
    char *saved = clay_config_sandbox_mode();
    assert(strcmp(saved, persisted) == 0);
    free(saved);
}

int main(void) {
    char template[] = "/tmp/clay_test_modes_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);
    assert(clay_sandbox_supported()); /* the 4-way cycle needs a real sandbox */

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    commands.sandbox_mode = CLAY_SANDBOX_MODE_SANDBOX;
    commands.sandbox_auto_approve = 0;

    /* Shift+Tab order: Sandbox (Ask) - Sandbox (Auto) - Unleashed (Ask) -
       Unleashed (Auto) - back to the start. */
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_SANDBOX, 1, "sandbox-auto");
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_UNLEASHED, 0, "unleashed");
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_UNLEASHED, 1, "unleashed-auto");
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_SANDBOX, 0, "sandbox");

    ClaySandboxMode mode;
    int auto_approve;
    clay_commands_parse_sandbox_mode("sandbox", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_SANDBOX && !auto_approve);
    clay_commands_parse_sandbox_mode("sandbox-auto", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_SANDBOX && auto_approve);
    clay_commands_parse_sandbox_mode("unleashed", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_UNLEASHED && !auto_approve);
    clay_commands_parse_sandbox_mode("unleashed-auto", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_UNLEASHED && auto_approve);
    /* Written by older versions. */
    clay_commands_parse_sandbox_mode("auto", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_SANDBOX && auto_approve);

    printf("mode tests passed\n");
    return 0;
}
