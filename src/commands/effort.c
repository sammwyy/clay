#include "context.h"

void clay_cmd_effort(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    size_t count = clay_commands_reasoning_effort_count();
    const ClayReasoningEffort *efforts = clay_commands_reasoning_efforts();
    ClayChoice choices[count];
    for (size_t i = 0; i < count; i++) {
        choices[i].title = efforts[i].label;
        choices[i].desc = efforts[i].description;
    }
    int index = clay_app_select(commands->app, "Reasoning effort:", choices, (int)count, commands->reasoning_effort_index);
    if (index < 0) return;
    commands->reasoning_effort_index = index;
    clay_commands_update_selected_below(commands);
    clay_sayc(CLAY_CYAN, "Reasoning effort: %s.", clay_commands_reasoning_effort(commands)->label);
}
