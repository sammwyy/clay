#include "context.h"

static const ClayChoice MODE_CHOICES[] = {
    {"Sandbox", "Isolated filesystem, no network, resource limits (default)."},
    {"Auto", "Sandboxed; automatically approves every tool call for this session."},
    {"Unleashed", "No sandbox; commands run directly in a normal shell."},
};

void clay_commands_update_sandbox_below(ClayCommands *commands) {
    ClayStr text;
    clay_str_init(&text);
    if (commands->sandbox_mode == CLAY_SANDBOX_MODE_UNLEASHED) {
        clay_str_printf(&text, "%sUnleashed%s", clay_color(CLAY_RED),
                        clay_color(CLAY_RESET));
    } else if (commands->sandbox_auto_approve) {
        clay_str_printf(&text, "%sAuto%s", clay_color(CLAY_YELLOW),
                        clay_color(CLAY_RESET));
    } else {
        clay_str_printf(&text, "%sSandbox%s", clay_color(CLAY_PINK),
                        clay_color(CLAY_RESET));
    }
    clay_below_set_text("sandbox", text.data);
    clay_str_free(&text);
}

static void set_mode(ClayCommands *commands, int choice) {
    commands->sandbox_auto_approve = choice == 1;
    commands->sandbox_mode = choice == 2 ? CLAY_SANDBOX_MODE_UNLEASHED
                                          : CLAY_SANDBOX_MODE_SANDBOX;
    clay_config_set_sandbox_mode(choice == 0 ? "sandbox" :
                                 choice == 1 ? "auto" : "unleashed");
    clay_commands_update_sandbox_below(commands);
}

void clay_commands_cycle_sandbox(ClayCommands *commands) {
    if (!clay_sandbox_supported()) return;
    int choice = commands->sandbox_mode == CLAY_SANDBOX_MODE_UNLEASHED
                     ? 0
                     : commands->sandbox_auto_approve ? 2 : 1;
    set_mode(commands, choice);
}

void clay_cmd_cycle_sandbox(const char *args, void *user_data) {
    (void)args;
    clay_commands_cycle_sandbox(user_data);
}

void clay_cmd_sandbox(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;

    if (!clay_sandbox_supported()) {
        clay_sayc(CLAY_YELLOW, "Sandbox mode isn't available on this platform yet; shell commands run Unleashed.");
        return;
    }

    int default_index = commands->sandbox_mode == CLAY_SANDBOX_MODE_UNLEASHED
                            ? 2 : commands->sandbox_auto_approve ? 1 : 0;
    int mode_index = clay_app_select(commands->app, "Shell execution mode:", MODE_CHOICES, 3,
                                      default_index);
    if (mode_index < 0) return;
    set_mode(commands, mode_index);

    if (commands->sandbox_mode == CLAY_SANDBOX_MODE_UNLEASHED) {
        clay_sayc(CLAY_RED, "Sandbox: Unleashed.");
    } else if (commands->sandbox_auto_approve) {
        clay_sayc(CLAY_YELLOW, "Sandbox: Auto (all tool calls are approved).");
    } else {
        clay_sayc(CLAY_GREEN, "Sandbox: filesystem isolated, network disabled.");
    }
}
