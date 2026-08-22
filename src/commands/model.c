#include "context.h"

#include <string.h>

void clay_cmd_model(const char *args, void *user_data) {
  ClayCommands *commands = user_data;
  if (commands->providers.count == 0) {
    clay_sayc(CLAY_RED,
              "No provider connected. Connect one with /connect first.");
    return;
  }
  if (args && *args) {
    const char *provider_id = commands->selected_provider;
    if (!provider_id || !clay_commands_find_provider(commands, provider_id)) {
      if (commands->providers.count == 1) {
        ClayConnectedProvider *only = clay_array_get(&commands->providers, 0);
        provider_id = only->type->id;
      } else {
        clay_sayc(
            CLAY_RED,
            "Select a provider with /model before setting a model directly.");
        return;
      }
    }
    int saved = clay_commands_select_model(commands, provider_id, args) == 0;
    clay_sayc(saved ? CLAY_GREEN : CLAY_RED,
              saved ? "Model set to %s via %s."
                    : "Model set, but failed to save config.",
              args, provider_id);
    return;
  }
  if (commands->providers.count == 1) {
    ClayConnectedProvider *only = clay_array_get(&commands->providers, 0);
    if (strcmp(only->type->id, "openai-codex") == 0) {
      clay_sayc(CLAY_GRAY, "If Codex model discovery is unavailable, set a "
                           "known model directly with /model <model-id>.");
    }
  }

  ClayModelProvider providers[commands->providers.count];
  int default_provider = 0;
  for (size_t i = 0; i < commands->providers.count; i++) {
    ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
    providers[i].id = provider->type->id;
    providers[i].label = provider->type->label;
    providers[i].fetch = clay_commands_fetch_models;
    providers[i].ctx = provider;
    if (commands->selected_provider &&
        strcmp(commands->selected_provider, provider->type->id) == 0) {
      default_provider = (int)i;
    }
  }
  ClayModelSelection selection = clay_model_select(
      providers, (int)commands->providers.count, default_provider);
  if (!selection.ok) {
    clay_sayc(CLAY_RED, "Model selection cancelled.");
    return;
  }
  int saved = clay_commands_select_model(commands, selection.provider,
                                         selection.model) == 0;
  clay_sayc(saved ? CLAY_GREEN : CLAY_RED,
            saved ? "Model set to %s via %s."
                  : "Model set, but failed to save config.",
            selection.model, selection.provider);
  clay_model_selection_free(&selection);
}
