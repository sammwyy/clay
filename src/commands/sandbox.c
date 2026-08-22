#include "context.h"

static const ClayChoice MODE_CHOICES[] = {
    {"Sandbox", "Isolated filesystem, no network, resource limits (default)."},
    {"Unleashed", "No sandbox; commands run directly in a normal shell."},
};

void clay_cmd_sandbox(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;

    if (!clay_sandbox_supported()) {
        clay_sayc(CLAY_YELLOW, "Sandbox mode isn't available on this platform yet; shell commands run Unleashed.");
        return;
    }

    int mode_index = clay_app_select(commands->app, "Shell execution mode:", MODE_CHOICES, 2,
                                      commands->sandbox_mode == CLAY_SANDBOX_MODE_SANDBOX ? 0 : 1);
    if (mode_index < 0) return;
    commands->sandbox_mode = mode_index == 0 ? CLAY_SANDBOX_MODE_SANDBOX : CLAY_SANDBOX_MODE_UNLEASHED;
    clay_config_set_sandbox_mode(commands->sandbox_mode == CLAY_SANDBOX_MODE_SANDBOX ? "sandbox" : "unleashed");

    if (commands->sandbox_mode == CLAY_SANDBOX_MODE_SANDBOX) {
        clay_sayc(CLAY_GREEN, "Sandbox: filesystem isolated, network disabled.");
    } else {
        clay_sayc(CLAY_GREEN, "Sandbox: Unleashed.");
    }
}
