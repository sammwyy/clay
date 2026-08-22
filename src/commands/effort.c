#include "context.h"

void clay_cmd_effort(const char *args, void *user_data) {
  (void)args;
  ClayCommands *commands = user_data;
  size_t count = clay_commands_reasoning_effort_count();
  const ClayReasoningEffort *efforts = clay_commands_reasoning_efforts();
  ClayArray choices;
  clay_array_init(&choices, sizeof(ClayChoice));
  for (size_t i = 0; i < count; i++) {
    ClayChoice choice = {efforts[i].label, efforts[i].description};
    clay_array_push_val(&choices, &choice);
  }
  int index = clay_app_select(commands->app, "Reasoning effort:", choices.data,
                              (int)choices.count, commands->reasoning_effort_index);
  clay_array_free(&choices);
  if (index < 0)
    return;
  if (clay_config_set_reasoning_effort(efforts[index].id) != 0) {
    clay_sayc(CLAY_RED, "Could not save reasoning effort.");
    return;
  }
  commands->reasoning_effort_index = index;
  clay_commands_update_selected_below(commands);
  clay_sayc(CLAY_CYAN, "Reasoning effort: %s.",
            clay_commands_reasoning_effort(commands)->label);
}
