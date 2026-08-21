#include "context.h"

void clay_commands_register(ClayCommands *commands) {
    ClayCommandRegistry *registry = clay_app_commands(commands->app);
    clay_command_register(registry, "help", "Show available commands", clay_cmd_help, commands);
    clay_command_register(registry, "exit", "Quit clay", clay_cmd_exit, commands);
    clay_command_register(registry, "confirm", "Demo a yes/no prompt", clay_cmd_confirm, commands);
    clay_command_register(registry, "select", "Demo a multi-option select prompt", clay_cmd_select, commands);
    clay_command_register(registry, "choice", "Demo a navigable choice prompt", clay_cmd_choice, commands);
    clay_command_register(registry, "mm", "Smoke-test the mm module", clay_cmd_mm, commands);
    clay_command_register(registry, "below", "Cycle the below-prompt status modules", clay_cmd_below, commands);
    clay_command_register(registry, "model", "Pick a model from a connected provider", clay_cmd_model, commands);
    clay_command_register(registry, "effort", "Set reasoning effort for supported models", clay_cmd_effort, commands);
    clay_command_register(registry, "connect", "Connect a provider, or /connect <id> directly", clay_cmd_connect, commands);
    clay_command_register(registry, "demo", "Run the render demo sequence", clay_cmd_demo, commands);
}
