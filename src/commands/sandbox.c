#include "context.h"

#include <string.h>

/* Two independent axes, cycled together by Shift+Tab: where commands run
   (Sandbox/Unleashed), and whether approvals go through /permissions (Ask)
   or are skipped entirely (Auto). */
static const ClayChoice MODE_CHOICES[] = {
    {"Sandbox (Ask)", "Isolated filesystem, no network; /permissions decides what asks first (default)."},
    {"Sandbox (Auto)", "Isolated filesystem, no network; every tool call runs without asking."},
    {"Unleashed (Ask)", "Commands run in a normal shell; /permissions decides what asks first."},
    {"Unleashed (Auto)", "No sandbox and nothing asks. Only in a workspace you can afford to lose."},
};

static const char *const MODE_IDS[] = {"sandbox", "sandbox-auto", "unleashed",
                                       "unleashed-auto"};

/* Mirrors MODE_IDS. The older bare "auto" still reads as sandboxed with
   approvals off. */
void clay_commands_parse_sandbox_mode(const char *value, ClaySandboxMode *mode,
                                      int *auto_approve) {
    size_t length = strlen(value);
    *mode = strncmp(value, "unleashed", 9) == 0 ? CLAY_SANDBOX_MODE_UNLEASHED
                                                : CLAY_SANDBOX_MODE_SANDBOX;
    *auto_approve = length >= 4 && strcmp(value + length - 4, "auto") == 0;
}

static int mode_index(const ClayCommands *commands) {
    int unleashed = commands->sandbox_mode == CLAY_SANDBOX_MODE_UNLEASHED;
    return unleashed * 2 + (commands->sandbox_auto_approve ? 1 : 0);
}

void clay_commands_update_sandbox_below(ClayCommands *commands) {
    int unleashed = commands->sandbox_mode == CLAY_SANDBOX_MODE_UNLEASHED;
    const char *place_color = unleashed ? CLAY_RED : CLAY_PINK;
    const char *approval_color = commands->sandbox_auto_approve ? CLAY_RED : CLAY_PINK;
    ClayStr text;
    clay_str_init(&text);
    clay_str_printf(&text, "%s%s%s %s(%s)%s", clay_color(place_color),
                    unleashed ? "Unleashed" : "Sandbox", clay_color(CLAY_RESET),
                    clay_color(approval_color),
                    commands->sandbox_auto_approve ? "Auto" : "Ask",
                    clay_color(CLAY_RESET));
    clay_below_set_text("sandbox", text.data);
    clay_str_free(&text);
}

static void set_mode(ClayCommands *commands, int choice) {
    commands->sandbox_mode = choice >= 2 ? CLAY_SANDBOX_MODE_UNLEASHED
                                         : CLAY_SANDBOX_MODE_SANDBOX;
    commands->sandbox_auto_approve = choice % 2 == 1;
    clay_config_set_sandbox_mode(MODE_IDS[choice]);
    clay_commands_update_sandbox_below(commands);
}

void clay_commands_cycle_sandbox(ClayCommands *commands) {
    int next = (mode_index(commands) + 1) % 4;
    /* Without sandbox support there is nothing to switch to, so Shift+Tab
       only flips Ask/Auto. */
    if (!clay_sandbox_supported() && next < 2) next += 2;
    set_mode(commands, next);
}

void clay_cmd_cycle_sandbox(const char *args, void *user_data) {
    (void)args;
    clay_commands_cycle_sandbox(user_data);
}

void clay_cmd_sandbox(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;

    int supported = clay_sandbox_supported();
    if (!supported)
        clay_sayc(CLAY_YELLOW,
                  "Sandbox mode isn't available on this platform yet; commands run Unleashed.");

    int first = supported ? 0 : 2;
    int choice = clay_app_select(commands->app, "Shell execution mode:", MODE_CHOICES + first,
                                 4 - first, mode_index(commands) - first);
    if (choice < 0) return;
    set_mode(commands, first + choice);

    clay_sayc(commands->sandbox_auto_approve ? CLAY_RED : CLAY_GREEN, "Mode: %s.",
              MODE_CHOICES[mode_index(commands)].title);
}
