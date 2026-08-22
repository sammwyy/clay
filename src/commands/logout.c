#include "context.h"

#include <stdlib.h>
#include <string.h>

void clay_cmd_logout(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    size_t count = commands->providers.count;
    if (count == 0) {
        clay_sayc(CLAY_YELLOW, "No provider is connected.");
        return;
    }

    ClayChoice *choices = calloc(count, sizeof(*choices));
    if (!choices) {
        clay_sayc(CLAY_RED, "Could not prepare provider selection.");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
        choices[i].title = provider->type->label;
    }

    int index = clay_app_choice(commands->app, "Log out from provider:", choices, (int)count, 0, NULL);
    if (index < 0) {
        free(choices);
        return;
    }

    ClayConnectedProvider *provider = clay_array_get(&commands->providers, (size_t)index);
    char *id = strdup(provider->type->id);
    char *label = strdup(provider->type->label);
    free(choices);

    int rc = clay_commands_logout_provider(commands, id);
    clay_sayc(rc == 0 ? CLAY_GREEN : CLAY_RED, rc == 0 ? "Logged out from %s." : "Failed to log out from %s.", label);
    free(id);
    free(label);
}
