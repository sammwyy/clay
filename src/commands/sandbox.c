#include "context.h"

static const ClayChoice MODE_CHOICES[] = {
    {"Sandbox", "Namespaced process, remapped filesystem (default)."},
    {"Unleashed", "No sandbox; commands run directly in a normal shell."},
};

static const ClayChoice ACCESS_CHOICES[] = {
    {"Read only", "Paths outside /workspace and /scratch can only be read."},
    {"Writable", "Paths outside /workspace and /scratch can be read and written."},
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
        int access_index = clay_app_select(commands->app, "Access outside /workspace and /scratch:", ACCESS_CHOICES, 2,
                                            commands->sandbox_access == CLAY_SANDBOX_ACCESS_READONLY ? 0 : 1);
        if (access_index >= 0) {
            commands->sandbox_access = access_index == 0 ? CLAY_SANDBOX_ACCESS_READONLY : CLAY_SANDBOX_ACCESS_WRITABLE;
            clay_config_set_sandbox_access(commands->sandbox_access == CLAY_SANDBOX_ACCESS_READONLY ? "readonly"
                                                                                                      : "writable");
        }
    }

    if (commands->sandbox_mode == CLAY_SANDBOX_MODE_SANDBOX) {
        clay_sayc(CLAY_GREEN, "Sandbox: %s.",
                  commands->sandbox_access == CLAY_SANDBOX_ACCESS_READONLY ? "read-only outside workspace"
                                                                            : "writable outside workspace");
    } else {
        clay_sayc(CLAY_GREEN, "Sandbox: Unleashed.");
    }
}
