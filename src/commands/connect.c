#include "context.h"

#include <stdlib.h>
#include <string.h>

static void connect_type(ClayCommands *commands, const ClayProviderType *type) {
  if (strcmp(type->id, "openai-codex") == 0) {
    clay_app_say(commands->app, "Starting OpenAI Codex authentication...");
    ClayCodexCredentials credentials = {0};
    ClayStr error;
    clay_str_init(&error);
    int rc = clay_openai_codex_authenticate(&credentials, &error);
    if (rc != 0) {
      clay_sayc(CLAY_RED, "OpenAI Codex authentication failed: %s", error.data);
      clay_str_free(&error);
      return;
    }
    ClayProviderConfig config = {0};
    config.id = strdup(type->id);
    config.base_url = strdup("");
    config.access_token = credentials.access_token;
    config.refresh_token = credentials.refresh_token;
    config.id_token = credentials.id_token;
    config.account_id = credentials.account_id;
    config.expires_at = credentials.expires_at;
    int ok = clay_config_save(&config) == 0;
    clay_sayc(ok ? CLAY_GREEN : CLAY_RED,
              ok ? "OpenAI Codex authentication successful."
                 : "Failed to save OpenAI Codex authentication.");
    if (ok)
      clay_commands_load_provider(commands, type);
    /* config owns the credential strings while it exists. */
    free(config.id);
    free(config.base_url);
    free(config.access_token);
    free(config.refresh_token);
    free(config.id_token);
    free(config.account_id);
    clay_str_free(&error);
    return;
  }
  if (strcmp(type->id, "grok") == 0) {
    ClayChoice choices[] = {
        {"Sign in with Grok", "Use an eligible Grok account or subscription."},
        {"Use xAI API key", "Connect to the public xAI API."},
    };
    int mode = clay_app_choice(commands->app, "Authentication:", choices, 2,
                               0, NULL);
    if (mode < 0) {
      clay_sayc(CLAY_RED, "Cancelled.");
      return;
    }
    if (mode == 0) {
      clay_app_say(commands->app, "Starting Grok authentication...");
      clay_app_say(commands->app, "Opening browser...");
      clay_app_say(commands->app, "Waiting for authorization...");
      ClayGrokCredentials credentials = {0};
      ClayStr error;
      clay_str_init(&error);
      int rc = clay_grok_authenticate(&credentials, &error);
      if (rc != 0) {
        clay_sayc(CLAY_RED, "Grok authentication failed: %s", error.data);
        clay_str_free(&error);
        return;
      }
      ClayProviderConfig config = {0};
      config.id = strdup(type->id);
      config.auth_mode = strdup("subscription");
      config.base_url = strdup(CLAY_GROK_SUBSCRIPTION_URL);
      config.access_token = credentials.access_token;
      config.refresh_token = credentials.refresh_token;
      config.id_token = credentials.id_token;
      config.expires_at = credentials.expires_at;
      int ok = clay_config_save(&config) == 0;
      clay_sayc(ok ? CLAY_GREEN : CLAY_RED,
                ok ? "Grok authentication successful."
                   : "Failed to save Grok authentication.");
      if (ok)
        clay_commands_load_provider(commands, type);
      free(config.id);
      free(config.auth_mode);
      free(config.base_url);
      free(config.access_token);
      free(config.refresh_token);
      free(config.id_token);
      clay_str_free(&error);
      return;
    }
    char *apikey = clay_prompt_secret("xAI API key:");
    if (!apikey || !*apikey) {
      clay_sayc(CLAY_RED, "Cancelled.");
      free(apikey);
      return;
    }
    ClayProviderConfig config = {.id = strdup(type->id),
                                 .apikey = apikey,
                                 .base_url = strdup(CLAY_GROK_API_URL),
                                 .auth_mode = strdup("api_key")};
    int ok = clay_config_save(&config) == 0;
    clay_sayc(ok ? CLAY_GREEN : CLAY_RED,
              ok ? "Connected Grok." : "Failed to save config for Grok.");
    if (ok)
      clay_commands_load_provider(commands, type);
    free(config.id);
    free(config.apikey);
    free(config.base_url);
    free(config.auth_mode);
    return;
  }
  char *base_url;
  if (type->default_base_url) {
    base_url = strdup(type->default_base_url);
    clay_app_say(commands->app, "Base URL: %s", base_url);
  } else {
    clay_app_say(commands->app, "Base URL for %s:", type->label);
    base_url = clay_prompt_line(NULL);
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
  ClayProviderConfig config = {
      .id = strdup(type->id), .apikey = apikey, .base_url = base_url};
  int ok = clay_config_save(&config) == 0;
  clay_sayc(ok ? CLAY_GREEN : CLAY_RED,
            ok ? "Connected %s." : "Failed to save config for %s.",
            type->label);
  if (ok)
    clay_commands_load_provider(commands, type);
  free(config.id);
  free(apikey);
  free(base_url);
}

int clay_commands_connect(ClayCommands *commands) {
  size_t provider_count = commands->providers.count;
  size_t count;
  const ClayProviderType *types = clay_commands_provider_types(&count);
  ClayArray choices, titles;
  clay_array_init(&choices, sizeof(ClayChoice));
  clay_array_init(&titles, sizeof(ClayStr));
  for (size_t i = 0; i < count; i++) {
    ClayStr title;
    clay_str_init(&title);
    clay_str_push(&title, types[i].label);
    if (clay_config_exists(types[i].id)) {
      clay_str_printf(&title, " %s%s%s", clay_color(CLAY_GREEN),
                      CLAY_ICON_CHECK, clay_color(CLAY_RESET));
    }
    clay_array_push_val(&titles, &title);
    ClayChoice choice = {title.data, NULL};
    clay_array_push_val(&choices, &choice);
  }
  int index = clay_app_choice(commands->app, "Connect a provider:", choices.data,
                              (int)choices.count, 0, NULL);
  for (size_t i = 0; i < titles.count; i++)
    clay_str_free(clay_array_get(&titles, i));
  clay_array_free(&choices);
  clay_array_free(&titles);
  if (index < 0) {
    clay_sayc(CLAY_RED, "Cancelled.");
    return 0;
  }
  connect_type(commands, &types[index]);
  return commands->providers.count > provider_count;
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

  clay_commands_connect(commands);
}
