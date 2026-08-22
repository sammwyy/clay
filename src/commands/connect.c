#include "context.h"

#include <stdlib.h>
#include <string.h>

static void connect_type(ClayCommands *commands, const ClayProviderType *type) {
    char *base_url;
    if (type->default_base_url) {
        base_url = strdup(type->default_base_url);
        clay_app_say(commands->app, "Base URL: %s", base_url);
    } else {
        clay_app_say(commands->app, "Base URL for %s:", type->label);
        base_url = clay_prompt_line();
    }
    if (!base_url || !*base_url) {
        clay_sayc(CLAY_RED, "Cancelled.");
        free(base_url);
        return;
    }
    if (!clay_openai_url_is_secure(base_url)) {
        clay_sayc(CLAY_RED, "Provider URLs must use HTTPS.");
        free(base_url);
        return;
    }
    char *apikey = clay_prompt_secret("API key:");
    if (!apikey || !*apikey) {
        clay_sayc(CLAY_RED, "Cancelled.");
        free(base_url);
        free(apikey);
        return;
    }
    ClayProviderConfig config = {strdup(type->id), apikey, base_url};
    int ok = clay_config_save(&config) == 0;
    clay_sayc(ok ? CLAY_GREEN : CLAY_RED, ok ? "Connected %s." : "Failed to save config for %s.", type->label);
    if (ok) clay_commands_load_provider(commands, type);
    free(config.id);
    free(apikey);
    free(base_url);
}

void clay_cmd_connect(const char *args, void *user_data) {
    ClayCommands *commands = user_data;
    if (args && *args) {
        const ClayProviderType *type = clay_commands_find_provider_type(args);
        if (!type) {
            clay_sayc(CLAY_RED, "Unknown provider type: %s", args);
            return;
        }
        connect_type(commands, type);
        return;
    }

    size_t count;
    const ClayProviderType *types = clay_commands_provider_types(&count);
    ClayChoice choices[count];
    ClayStr titles[count];
    for (size_t i = 0; i < count; i++) {
        clay_str_init(&titles[i]);
        clay_str_push(&titles[i], types[i].label);
        if (clay_config_exists(types[i].id)) {
            clay_str_printf(&titles[i], " %s%s%s", clay_color(CLAY_GREEN), CLAY_ICON_CHECK, clay_color(CLAY_RESET));
        }
        choices[i].title = titles[i].data;
        choices[i].desc = NULL;
    }
    int index = clay_app_choice(commands->app, "Connect a provider:", choices, (int)count, 0, NULL);
    for (size_t i = 0; i < count; i++) clay_str_free(&titles[i]);
    if (index < 0) {
        clay_sayc(CLAY_RED, "Cancelled.");
        return;
    }
    connect_type(commands, &types[index]);
}
