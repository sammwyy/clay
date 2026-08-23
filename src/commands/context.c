#include "context.h"

#include "clay/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CLAY_SYSTEM_PROMPT_BASE                                                \
  "You are clay, a helpful AI coding assistant. Be concise, accurate, and "    \
  "practical. "                                                                \
  "Explain code changes clearly and ask for clarification when the request "   \
  "is ambiguous. "                                                             \
  "Use read/write/edit/glob/grep for inspecting or changing files in the "     \
  "workspace - they are scoped to "                                            \
  "it directly and are preferred over shell_exec's cat/sed/find/grep for "     \
  "that purpose. Reach for shell_exec "                                        \
  "for everything else (running builds/tests, git, other programs). It runs "  \
  "in /workspace, the sandbox alias of the project working directory; do not " \
  "try to discover its host path or inspect mounts. /scratch is an alias for " \
  "this conversation's private directory under /tmp; use it for temporary "    \
  "files. Sandboxed commands cannot "                                        \
  "access host paths outside configured read-only mounts or the network. "      \
  "Normal POSIX syntax (variables, command substitution, globs, and stderr "    \
  "redirections) works, but may require an execution confirmation. Prefer focused commands "   \
  "and "                                                                       \
  "summarize results. The user can put you in Plan mode (/plan) to discuss "   \
  "an approach before any files "                                              \
  "change - write/edit are refused and mutating shell commands are blocked "   \
  "there; a blocked tool result "                                              \
  "means explain the plan in words instead of retrying the call.\n\n"          \
  "You have two kinds of memory. Long-term memory (memory_save/memory_read) "  \
  "persists across every future "                                              \
  "chat: call memory_save after completing significant work - a decision, a "  \
  "bug fix, a preference the user "                                            \
  "stated - so later sessions have it; the index of existing entries is "      \
  "below, and memory_read loads one by "                                       \
  "its slug. Short-term memory (remember) is a scratchpad for this chat "      \
  "only, replayed every turn even if "                                         \
  "earlier messages are later dropped - use it to pin details you'll still "   \
  "need many turns from now.\n\n"                                              \
  "For any task with multiple steps, call todowrite with the full plan "       \
  "before starting, and again whenever a "                                     \
  "step's status changes - it's shown to the user as a checklist. Keep "       \
  "exactly one task in_progress at a time; "                                   \
  "mark it completed before moving to the next. Skip it for a single quick "   \
  "action.\n\n"                                                                \
  "Tools named mcp__<server>__<tool> come from MCP servers the user "          \
  "configured with /mcp - use them like any "                                  \
  "other tool.\n\n"                                                            \
  "If the user set up /autotest, a write/edit result carrying "                \
  "auto_test_failed means the configured command "                             \
  "failed after that change - read auto_test_output and fix it before moving " \
  "on."

/* Sliding window: reused as-is (and its clock reset) while a chat-less
   session starts within this long of the cache's last use, since the
   provider's own prefix cache is likely still warm; rebuilt with fresh
   memory/environment data once it's likely gone cold anyway. Never
   applies to an existing chat - see clay_chat_system_prompt. */
#define CLAY_SYSTEM_PROMPT_TTL_SECONDS 600

static const ClayProviderType PROVIDER_TYPES[] = {
    {"openai", "OpenAI", "https://api.openai.com/v1"},
    {"openai-codex", "OpenAI Codex", NULL},
    {"grok", "Grok", CLAY_GROK_API_URL},
    {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1"},
    {"groq", "Groq", "https://api.groq.com/openai/v1"},
    {"deepseek", "DeepSeek", "https://api.deepseek.com"},
    {"mistral", "Mistral", "https://api.mistral.ai/v1"},
    {"together", "Together AI", "https://api.together.xyz/v1"},
    {"custom", "OpenAI Custom", NULL},
};

static const ClayReasoningEffort REASONING_EFFORTS[] = {
    {NULL, "Default",
     "Let the selected model use its default reasoning level."},
    {"minimal", "Minimal", "Use the fewest reasoning tokens when supported."},
    {"low", "Low", "Favor lower latency and token use when supported."},
    {"medium", "Medium", "Balance reasoning depth, latency, and token use."},
    {"high", "High", "Favor deeper reasoning when supported."},
    {"xhigh", "XHigh", "Use the highest reasoning level when supported."},
};

static const char *env_value(const char *first, const char *second) {
  const char *value = first ? getenv(first) : NULL;
  if (value && *value) return value;
  value = second ? getenv(second) : NULL;
  return value && *value ? value : NULL;
}

static void replace_string(char **field, const char *value) {
  if (!value) return;
  free(*field);
  *field = strdup(value);
}

static void uppercase_provider_id(const char *id, char *out, size_t out_size) {
  size_t i = 0;
  for (; id[i] && i + 1 < out_size; i++) {
    char ch = id[i];
    out[i] = (char)(ch == '-' ? '_' : toupper((unsigned char)ch));
  }
  out[i] = '\0';
}

/* Environment credentials are deliberately an in-memory override: they make
   CI and ephemeral shells usable without copying secrets into ~/.clay. */
static int apply_environment_credentials(const ClayProviderType *type,
                                         ClayProviderConfig **config_ptr) {
  const char *api_key = NULL;
  const char *base_url = NULL;
  const char *access_token = NULL;
  const char *refresh_token = NULL;
  const char *id_token = NULL;
  const char *account_id = NULL;
  const char *expires_at = NULL;
  char provider_name[64];
  char api_name[80];
  char base_name[80];

  uppercase_provider_id(type->id, provider_name, sizeof(provider_name));
  snprintf(api_name, sizeof(api_name), "CLAY_%s_API_KEY", provider_name);
  snprintf(base_name, sizeof(base_name), "CLAY_%s_BASE_URL", provider_name);

  if (strcmp(type->id, "openai") == 0) {
    api_key = env_value("OPENAI_API_KEY", api_name);
    base_url = env_value("OPENAI_BASE_URL", base_name);
  } else if (strcmp(type->id, "openrouter") == 0) {
    api_key = env_value("OPENROUTER_API_KEY", api_name);
    base_url = env_value("OPENROUTER_BASE_URL", base_name);
  } else if (strcmp(type->id, "groq") == 0) {
    api_key = env_value("GROQ_API_KEY", api_name);
    base_url = env_value("GROQ_BASE_URL", base_name);
  } else if (strcmp(type->id, "deepseek") == 0) {
    api_key = env_value("DEEPSEEK_API_KEY", api_name);
    base_url = env_value("DEEPSEEK_BASE_URL", base_name);
  } else if (strcmp(type->id, "mistral") == 0) {
    api_key = env_value("MISTRAL_API_KEY", api_name);
    base_url = env_value("MISTRAL_BASE_URL", base_name);
  } else if (strcmp(type->id, "together") == 0) {
    api_key = env_value("TOGETHER_API_KEY", api_name);
    base_url = env_value("TOGETHER_BASE_URL", base_name);
  } else if (strcmp(type->id, "grok") == 0) {
    api_key = env_value("XAI_API_KEY", "GROK_API_KEY");
    if (!api_key) api_key = getenv(api_name);
    base_url = env_value("GROK_BASE_URL", base_name);
    access_token = env_value("GROK_ACCESS_TOKEN", "CLAY_GROK_ACCESS_TOKEN");
    refresh_token = env_value("GROK_REFRESH_TOKEN", "CLAY_GROK_REFRESH_TOKEN");
    id_token = env_value("GROK_ID_TOKEN", "CLAY_GROK_ID_TOKEN");
    account_id = env_value("GROK_ACCOUNT_ID", "CLAY_GROK_ACCOUNT_ID");
    expires_at = env_value("GROK_EXPIRES_AT", "CLAY_GROK_EXPIRES_AT");
  } else if (strcmp(type->id, "openai-codex") == 0) {
    access_token = env_value("OPENAI_CODEX_ACCESS_TOKEN",
                             "CLAY_CODEX_ACCESS_TOKEN");
    refresh_token = env_value("OPENAI_CODEX_REFRESH_TOKEN",
                              "CLAY_CODEX_REFRESH_TOKEN");
    id_token = env_value("OPENAI_CODEX_ID_TOKEN", "CLAY_CODEX_ID_TOKEN");
    account_id = env_value("OPENAI_CODEX_ACCOUNT_ID",
                           "CLAY_CODEX_ACCOUNT_ID");
    expires_at = env_value("OPENAI_CODEX_EXPIRES_AT",
                           "CLAY_CODEX_EXPIRES_AT");
  } else if (strcmp(type->id, "custom") == 0) {
    api_key = env_value("CLAY_API_KEY", "CUSTOM_API_KEY");
    base_url = env_value("CLAY_BASE_URL", "CUSTOM_BASE_URL");
  } else {
    api_key = getenv(api_name);
    base_url = getenv(base_name);
  }

  int has_override = api_key || base_url || access_token || refresh_token ||
                     id_token || account_id || expires_at;
  if (!has_override) return 0;

  ClayProviderConfig *config = *config_ptr;
  if (!config) {
    config = calloc(1, sizeof(*config));
    if (!config) return -1;
    config->id = strdup(type->id);
    if (type->default_base_url) config->base_url = strdup(type->default_base_url);
    *config_ptr = config;
  }

  if (api_key) replace_string(&config->apikey, api_key);
  if (base_url) replace_string(&config->base_url, base_url);
  if (strcmp(type->id, "grok") == 0 && api_key) {
    replace_string(&config->base_url, base_url ? base_url : CLAY_GROK_API_URL);
    replace_string(&config->auth_mode, "api_key");
  }
  if (access_token) replace_string(&config->access_token, access_token);
  if (refresh_token) replace_string(&config->refresh_token, refresh_token);
  if (id_token) replace_string(&config->id_token, id_token);
  if (account_id) replace_string(&config->account_id, account_id);
  if (access_token || refresh_token) {
    if (strcmp(type->id, "grok") == 0) {
      replace_string(&config->auth_mode, "subscription");
      replace_string(&config->base_url, CLAY_GROK_SUBSCRIPTION_URL);
    }
  }
  if (expires_at) {
    char *end = NULL;
    long long value = strtoll(expires_at, &end, 10);
    if (end && *end == '\0' && value > 0) config->expires_at = value;
  }
  if (strcmp(type->id, "openai-codex") == 0 && !config->base_url)
    config->base_url = strdup("");
  return 1;
}

static const char *environment_provider(void) {
  const char *provider = env_value("CLAY_PROVIDER", NULL);
  if (provider) return provider;
  if (env_value("OPENAI_CODEX_ACCESS_TOKEN", "CLAY_CODEX_ACCESS_TOKEN"))
    return "openai-codex";
  if (env_value("OPENROUTER_API_KEY", "CLAY_OPENROUTER_API_KEY"))
    return "openrouter";
  if (env_value("GROQ_API_KEY", "CLAY_GROQ_API_KEY")) return "groq";
  if (env_value("DEEPSEEK_API_KEY", "CLAY_DEEPSEEK_API_KEY"))
    return "deepseek";
  if (env_value("MISTRAL_API_KEY", "CLAY_MISTRAL_API_KEY"))
    return "mistral";
  if (env_value("TOGETHER_API_KEY", "CLAY_TOGETHER_API_KEY"))
    return "together";
  if (env_value("XAI_API_KEY", "GROK_API_KEY")) return "grok";
  if (env_value("OPENAI_API_KEY", "CLAY_OPENAI_API_KEY")) return "openai";
  if (env_value("CLAY_API_KEY", "CUSTOM_API_KEY")) return "custom";
  return NULL;
}

static const char *environment_model(const char *provider) {
  const char *model = env_value("CLAY_MODEL", NULL);
  if (model) return model;
  if (strcmp(provider, "openai") == 0) return getenv("OPENAI_MODEL");
  if (strcmp(provider, "openrouter") == 0) return getenv("OPENROUTER_MODEL");
  if (strcmp(provider, "groq") == 0) return getenv("GROQ_MODEL");
  if (strcmp(provider, "deepseek") == 0) return getenv("DEEPSEEK_MODEL");
  if (strcmp(provider, "mistral") == 0) return getenv("MISTRAL_MODEL");
  if (strcmp(provider, "together") == 0) return getenv("TOGETHER_MODEL");
  if (strcmp(provider, "grok") == 0) return env_value("GROK_MODEL", "XAI_MODEL");
  if (strcmp(provider, "openai-codex") == 0)
    return env_value("OPENAI_CODEX_MODEL", "CODEX_MODEL");
  if (strcmp(provider, "custom") == 0) return getenv("CUSTOM_MODEL");
  return NULL;
}

static void provider_models_free(ClayConnectedProvider *provider) {
  for (size_t i = 0; i < provider->models.count; i++)
    free(*(char **)clay_array_get(&provider->models, i));
  clay_array_free(&provider->models);
}

static void provider_free(ClayConnectedProvider *provider) {
  clay_openai_destroy(provider->client);
  clay_openai_codex_destroy(provider->codex_client);
  clay_grok_destroy(provider->grok_client);
  clay_config_free(provider->config);
  provider_models_free(provider);
}

ClayConnectedProvider *clay_commands_find_provider(ClayCommands *commands,
                                                   const char *id) {
  for (size_t i = 0; i < commands->providers.count; i++) {
    ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
    if (strcmp(provider->type->id, id) == 0)
      return provider;
  }
  return NULL;
}

const ClayProviderType *clay_commands_find_provider_type(const char *id) {
  for (size_t i = 0; i < sizeof(PROVIDER_TYPES) / sizeof(PROVIDER_TYPES[0]);
       i++) {
    if (strcmp(PROVIDER_TYPES[i].id, id) == 0)
      return &PROVIDER_TYPES[i];
  }
  return NULL;
}

const ClayProviderType *clay_commands_provider_types(size_t *count) {
  *count = sizeof(PROVIDER_TYPES) / sizeof(PROVIDER_TYPES[0]);
  return PROVIDER_TYPES;
}

void clay_commands_load_provider(ClayCommands *commands,
                                 const ClayProviderType *type) {
  ClayProviderConfig *config = clay_config_load(type->id);
  int environment_override = apply_environment_credentials(type, &config);
  if (environment_override < 0) {
    clay_config_free(config);
    return;
  }
  if (!config)
    return;
  int is_codex = strcmp(type->id, "openai-codex") == 0;
  int is_grok_subscription = strcmp(type->id, "grok") == 0 &&
                             config->auth_mode &&
                             strcmp(config->auth_mode, "subscription") == 0;
  int is_grok_api_key = strcmp(type->id, "grok") == 0 &&
                        !is_grok_subscription;
  if ((!is_codex && !is_grok_subscription &&
       (!clay_openai_url_is_secure(config->base_url) || !config->apikey ||
        !*config->apikey)) ||
      (is_grok_api_key && strcmp(config->base_url, CLAY_GROK_API_URL) != 0) ||
      (is_codex && (!config->access_token || !config->refresh_token ||
                    !config->account_id)) ||
      (is_grok_subscription &&
       (!config->access_token || !config->refresh_token))) {
    clay_config_free(config);
    return;
  }

  ClayConnectedProvider *existing =
      clay_commands_find_provider(commands, type->id);
  if (existing) {
    clay_openai_destroy(existing->client);
    clay_openai_codex_destroy(existing->codex_client);
    clay_grok_destroy(existing->grok_client);
    clay_config_free(existing->config);
    provider_models_free(existing);
    existing->config = config;
    existing->environment_override = environment_override;
    existing->client = (is_codex || is_grok_subscription)
                           ? NULL
                           : clay_openai_create(config->base_url, config->apikey,
                                                NULL);
    ClayCodexCredentials credentials = {config->access_token,
                                        config->refresh_token, config->id_token,
                                        config->account_id, config->expires_at};
    existing->codex_client =
        is_codex ? clay_openai_codex_create(&credentials, NULL) : NULL;
    ClayGrokCredentials grok_credentials = {config->access_token,
                                             config->refresh_token,
                                             config->id_token,
                                             config->expires_at};
    existing->grok_client = is_grok_subscription
                                ? clay_grok_create(&grok_credentials, NULL)
                                : NULL;
    clay_array_init(&existing->models, sizeof(char *));
    existing->models_fetched = 0;
    existing->models_rc = 0;
    return;
  }

  ClayConnectedProvider provider = {0};
  provider.type = type;
  provider.config = config;
  provider.environment_override = environment_override;
  provider.client = (is_codex || is_grok_subscription)
                        ? NULL
                        : clay_openai_create(config->base_url, config->apikey,
                                             NULL);
  ClayCodexCredentials credentials = {config->access_token,
                                      config->refresh_token, config->id_token,
                                      config->account_id, config->expires_at};
  provider.codex_client =
      is_codex ? clay_openai_codex_create(&credentials, NULL) : NULL;
  ClayGrokCredentials grok_credentials = {config->access_token,
                                           config->refresh_token,
                                           config->id_token,
                                           config->expires_at};
  provider.grok_client = is_grok_subscription
                             ? clay_grok_create(&grok_credentials, NULL)
                             : NULL;
  clay_array_init(&provider.models, sizeof(char *));
  clay_array_push_val(&commands->providers, &provider);
}

int clay_commands_save_grok_credentials(ClayConnectedProvider *provider,
                                        const ClayGrok *client) {
  if (!provider || !provider->config || !client)
    return -1;
  if (clay_grok_authentication_invalid(client)) {
    int rc = provider->environment_override
                 ? 0
                 : clay_config_remove(provider->config->id);
    free(provider->config->access_token);
    free(provider->config->refresh_token);
    free(provider->config->id_token);
    provider->config->access_token = NULL;
    provider->config->refresh_token = NULL;
    provider->config->id_token = NULL;
    provider->config->expires_at = 0;
    if (provider->grok_client) {
      clay_grok_destroy(provider->grok_client);
      provider->grok_client = NULL;
    }
    return rc;
  }
  ClayGrokCredentials credentials = {0};
  clay_grok_copy_credentials(client, &credentials);
  free(provider->config->access_token);
  free(provider->config->refresh_token);
  free(provider->config->id_token);
  provider->config->access_token = credentials.access_token;
  provider->config->refresh_token = credentials.refresh_token;
  provider->config->id_token = credentials.id_token;
  provider->config->expires_at = credentials.expires_at;
  int rc = provider->environment_override
               ? 0
               : clay_config_save(provider->config);
  /* Message requests use a short-lived Grok client. Keep the session's
     model-picker client synchronized when a refresh token rotated. */
  if (rc == 0 && provider->grok_client != client) {
    ClayGrokCredentials current = {provider->config->access_token,
                                    provider->config->refresh_token,
                                    provider->config->id_token,
                                    provider->config->expires_at};
    clay_grok_destroy(provider->grok_client);
    provider->grok_client = clay_grok_create(&current, NULL);
  }
  return rc;
}

int clay_commands_save_codex_credentials(ClayConnectedProvider *provider,
                                         const ClayOpenAICodex *client) {
  if (!provider || !provider->config || !client)
    return -1;
  if (clay_openai_codex_authentication_invalid(client)) {
    if (!provider->environment_override)
      return clay_config_remove(provider->config->id);
    return 0;
  }
  ClayCodexCredentials credentials = {0};
  clay_openai_codex_copy_credentials(client, &credentials);
  free(provider->config->access_token);
  free(provider->config->refresh_token);
  free(provider->config->id_token);
  free(provider->config->account_id);
  provider->config->access_token = credentials.access_token;
  provider->config->refresh_token = credentials.refresh_token;
  provider->config->id_token = credentials.id_token;
  provider->config->account_id = credentials.account_id;
  provider->config->expires_at = credentials.expires_at;
  if (provider->environment_override) return 0;
  return clay_config_save(provider->config);
}

int clay_commands_logout_provider(ClayCommands *commands, const char *id) {
  for (size_t i = 0; i < commands->providers.count; i++) {
    ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
    if (strcmp(provider->type->id, id) != 0)
      continue;

    int selected = commands->selected_provider &&
                   strcmp(commands->selected_provider, id) == 0;
    if (selected && clay_config_selection_save(NULL, NULL) != 0)
      return -1;
    if (clay_config_remove(provider->config->id) != 0)
      return -1;

    provider_free(provider);
    clay_array_remove(&commands->providers, i);
    if (selected) {
      free(commands->selected_provider);
      free(commands->selected_model);
      commands->selected_provider = NULL;
      commands->selected_model = NULL;
      clay_commands_update_selected_below(commands);
    }
    return 0;
  }
  return -1;
}

static void append_display_cwd(ClayStr *text) {
  char *cwd = clay_term_cwd();
  if (!cwd) {
    clay_str_push(text, ".");
    return;
  }
  const char *home = getenv("HOME");
#ifdef _WIN32
  if (!home || !*home) home = getenv("USERPROFILE");
#endif
  if (home && *home) {
    size_t length = strlen(home);
    size_t cwd_length = strlen(cwd);
    if (cwd_length >= length && strncmp(cwd, home, length) == 0 &&
        (cwd[length] == '\0' || cwd[length] == '/' || cwd[length] == '\\')) {
      clay_str_push(text, "~");
      clay_str_push(text, cwd + length);
      free(cwd);
      return;
    }
  }
  clay_str_push(text, cwd);
  free(cwd);
}

void clay_commands_update_selected_below(ClayCommands *commands) {
  ClayStr text;
  clay_str_init(&text);
  append_display_cwd(&text);
  if (commands->selected_model && commands->selected_provider) {
    const ClayReasoningEffort *effort =
        clay_commands_reasoning_effort(commands);
    clay_str_printf(&text, " %s%s%s %s·%s %s(%s)%s %s[%s%s%s]%s",
                    clay_color(CLAY_CORAL), commands->selected_model,
                    clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
                    clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
                    commands->selected_provider, clay_color(CLAY_RESET),
                    clay_color(CLAY_GRAY), clay_color(CLAY_CYAN), effort->label,
                    clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
  } else {
    clay_str_push(&text, " · None");
  }
  clay_below_set_text("model", text.data);
  clay_str_free(&text);
}

void clay_commands_set_tokens_below(ClayCommands *commands, long input_tokens,
                                    long output_tokens) {
  clay_commands_set_tokens_below_with_cache(commands, input_tokens,
                                            output_tokens, 0, 0);
}

void clay_commands_set_tokens_below_with_cache(
    ClayCommands *commands, long input_tokens, long output_tokens,
    long cached_input_tokens, int cached_input_tokens_known) {
  (void)commands;
  ClayStr text;
  clay_str_init(&text);
  if (cached_input_tokens_known && input_tokens > 0) {
    if (cached_input_tokens < 0)
      cached_input_tokens = 0;
    if (cached_input_tokens > input_tokens)
      cached_input_tokens = input_tokens;
    long percent = (long)((double)cached_input_tokens * 100.0 /
                          (double)input_tokens);
    clay_str_printf(&text, "%s\xe2\x86\x91 %ld  \xe2\x86\x93 %ld  "
                    "%s(cache: %ld%%)%s",
                    clay_color(CLAY_CYAN), input_tokens, output_tokens,
                    clay_color(CLAY_GRAY), percent, clay_color(CLAY_RESET));
  } else {
    clay_str_printf(&text, "%s\xe2\x86\x91 %ld  \xe2\x86\x93 %ld%s",
                    clay_color(CLAY_CYAN), input_tokens, output_tokens,
                    clay_color(CLAY_RESET));
  }
  clay_below_set_text("tokens", text.data);
  clay_str_free(&text);
}

static char *system_prompt_cache_path(void) {
  return clay_storage_path("system_prompt.json");
}

/* 0 with text_out/last_used_out/cwd_out set on a hit, -1 on a miss. */
static int load_system_prompt_cache(char **text_out, long long *last_used_out,
                                    char **cwd_out) {
  char *path = system_prompt_cache_path();
  if (!path)
    return -1;
  ClayStr body;
  int read_rc = clay_storage_read_limited(path, 4 * 1024 * 1024, &body);
  free(path);
  if (read_rc != 0)
    return -1;
  ClayJson *root = clay_json_parse(body.data, NULL);
  clay_str_free(&body);
  const char *text =
      clay_json_type(root) == CLAY_JSON_OBJECT
          ? clay_json_string_value(clay_json_object_get(root, "text"))
          : NULL;
  if (!text || !*text) {
    clay_json_free(root);
    return -1;
  }
  *text_out = strdup(text);
  *last_used_out = (long long)clay_json_number_value(
      clay_json_object_get(root, "last_used_at"));
  const char *cwd = clay_json_string_value(clay_json_object_get(root, "cwd"));
  *cwd_out = strdup(cwd ? cwd : "");
  clay_json_free(root);
  return 0;
}

static void save_system_prompt_cache(const char *text, long long last_used_at,
                                     const char *cwd) {
  if (clay_storage_ensure_dir("") != 0)
    return;
  ClayJson *root = clay_json_object();
  clay_json_object_set(root, "text", clay_json_string(text));
  clay_json_object_set(root, "last_used_at",
                       clay_json_number((double)last_used_at));
  clay_json_object_set(root, "cwd", clay_json_string(cwd));
  ClayStr body;
  clay_str_init(&body);
  clay_json_stringify(root, &body);
  clay_json_free(root);
  char *path = clay_storage_path("system_prompt.json");
  if (!path) {
    clay_str_free(&body);
    return;
  }
  clay_storage_write_atomic_private(path, body.data, body.len);
  free(path);
  clay_str_free(&body);
}

#define CLAY_PROJECT_INSTRUCTIONS_MAX_DEPTH 32

static char *read_file_if_exists(const char *path) {
  ClayStr text;
  if (clay_storage_read_limited(path, 4 * 1024 * 1024, &text) != 0) return NULL;
  return text.data;
}

/* AGENTS.md takes priority over CLAY.md at the same level - only one is
   read per directory. */
static char *read_level_instructions(const char *dir) {
  ClayStr path;
  clay_str_init(&path);
  clay_str_printf(&path, "%s/AGENTS.md", dir);
  char *content = read_file_if_exists(path.data);
  if (!content) {
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/CLAY.md", dir);
    content = read_file_if_exists(path.data);
  }
  clay_str_free(&path);
  return content;
}

/* Walks up from the cwd to the repo root (a directory holding .git),
   reading AGENTS.md/CLAY.md at each level - same mechanism as Codex CLI.
   Concatenated root-first, so more specific instructions near the cwd
   follow more general ones. Malloc'd; NULL if none found. */
char *clay_commands_load_project_instructions(void) {
  char *dir = clay_term_cwd();
  if (!dir)
    return NULL;

  ClayArray levels;
  clay_array_init(&levels, sizeof(char *));
  for (int depth = 0; depth < CLAY_PROJECT_INSTRUCTIONS_MAX_DEPTH; depth++) {
    char *content = read_level_instructions(dir);
    if (content)
      clay_array_push_val(&levels, &content);

    ClayStr git_path;
    clay_str_init(&git_path);
    clay_str_printf(&git_path, "%s/.git", dir);
    int at_repo_root = clay_term_is_dir(git_path.data);
    clay_str_free(&git_path);
    if (at_repo_root || strcmp(dir, "/") == 0)
      break;

    char *slash = strrchr(dir, '/');
    if (!slash)
      break;
    if (slash == dir)
      slash[1] = '\0'; /* "/foo" -> "/" */
    else
      *slash = '\0';
  }
  free(dir);

  if (levels.count == 0) {
    clay_array_free(&levels);
    return NULL;
  }
  ClayStr combined;
  clay_str_init(&combined);
  for (size_t i = levels.count; i-- > 0;) {
    char *content = *(char **)clay_array_get(&levels, i);
    if (combined.len > 0)
      clay_str_push(&combined, "\n\n");
    clay_str_push(&combined, content);
    free(content);
  }
  clay_array_free(&levels);
  return combined.data;
}

#define CLAY_ENV_REPO_ROOT_MAX_DEPTH 32
#define CLAY_ENV_DIR_LISTING_MAX 50

/* Walks up from the cwd looking for a .git directory, returning its
   current branch (or a short SHA in detached HEAD). Malloc'd; NULL if not
   in a git repo. */
char *clay_commands_find_git_branch(void) {
  char *dir = clay_term_cwd();
  if (!dir)
    return NULL;
  char *branch = NULL;
  for (int depth = 0; depth < CLAY_ENV_REPO_ROOT_MAX_DEPTH; depth++) {
    ClayStr head_path;
    clay_str_init(&head_path);
    clay_str_printf(&head_path, "%s/.git/HEAD", dir);
    FILE *file = fopen(head_path.data, "r");
    clay_str_free(&head_path);
    if (file) {
      ClayStr content;
      clay_str_init(&content);
      int ch;
      while ((ch = fgetc(file)) != EOF && ch != '\n')
        clay_str_push_char(&content, (char)ch);
      fclose(file);
      const char *prefix = "ref: refs/heads/";
      if (strncmp(content.data, prefix, strlen(prefix)) == 0)
        branch = strdup(content.data + strlen(prefix));
      else if (content.len >= 7) {
        ClayStr short_sha;
        clay_str_init(&short_sha);
        clay_str_push_n(&short_sha, content.data, 7);
        branch = short_sha.data;
      }
      clay_str_free(&content);
      break;
    }
    if (strcmp(dir, "/") == 0)
      break;
    char *slash = strrchr(dir, '/');
    if (!slash)
      break;
    if (slash == dir)
      slash[1] = '\0';
    else
      *slash = '\0';
  }
  free(dir);
  return branch;
}

static int compare_strings(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Comma-separated, sorted, capped names of `dir`'s direct entries. Malloc'd;
   NULL if the directory can't be listed. */
char *clay_commands_list_top_level(const char *dir) {
  ClayArray entries;
  if (clay_term_list_entries(dir, &entries) != 0)
    return NULL;
  if (entries.count > 0)
    qsort(entries.data, entries.count, sizeof(char *), compare_strings);

  ClayStr out;
  clay_str_init(&out);
  size_t shown = entries.count < CLAY_ENV_DIR_LISTING_MAX
                     ? entries.count
                     : CLAY_ENV_DIR_LISTING_MAX;
  for (size_t i = 0; i < shown; i++) {
    if (i > 0)
      clay_str_push(&out, ", ");
    clay_str_push(&out, *(char **)clay_array_get(&entries, i));
  }
  if (entries.count > shown)
    clay_str_printf(&out, ", ... (%zu more)", entries.count - shown);
  for (size_t i = 0; i < entries.count; i++)
    free(*(char **)clay_array_get(&entries, i));
  clay_array_free(&entries);
  return out.data;
}

static char *build_fresh_system_prompt(void) {
  ClayStr text;
  clay_str_init(&text);
  clay_str_push(&text, CLAY_SYSTEM_PROMPT_BASE);

  char *platform = clay_term_platform_name();
  char *date = clay_time_format_date(clay_time_now());
  clay_str_printf(&text, "\n\nEnvironment: %s. Today's date (UTC): %s.",
                  platform, date);
  free(platform);
  free(date);

  char *cwd = clay_term_cwd();
  if (cwd) {
    clay_str_printf(&text, " Working directory: %s.", cwd);
    char *branch = clay_commands_find_git_branch();
    if (branch) {
      clay_str_printf(&text, " Git branch: %s.", branch);
      free(branch);
    }
    char *listing = clay_commands_list_top_level(cwd);
    if (listing && *listing)
      clay_str_printf(&text, "\nTop-level directory listing: %s", listing);
    free(listing);
    free(cwd);
  }

  char *index = clay_memory_index();
  if (*index)
    clay_str_printf(&text, "\n\nLong-term memory index:\n%s", index);
  free(index);

  char *project_instructions = clay_commands_load_project_instructions();
  if (project_instructions) {
    clay_str_printf(&text, "\n\nProject instructions (AGENTS.md/CLAY.md):\n%s",
                    project_instructions);
    free(project_instructions);
  }

  return text.data;
}

/* Reuses the cached prompt (and slides its TTL forward) if it's recent
   enough that the provider's own prefix cache is plausibly still warm;
   otherwise rebuilds with current memory/environment data. Only ever
   called for a chat-less session - clay_commands_reset_conversation
   never calls this once commands->chat exists. */
static char *clay_commands_build_system_prompt(void) {
  char *cached_text = NULL;
  char *cached_cwd = NULL;
  long long last_used = 0;
  long long now = clay_time_now();
  char *cwd = clay_term_cwd();
  int hit =
      load_system_prompt_cache(&cached_text, &last_used, &cached_cwd) == 0 &&
      now - last_used < CLAY_SYSTEM_PROMPT_TTL_SECONDS && cwd &&
      strcmp(cached_cwd, cwd) == 0;
  free(cached_cwd);
  if (hit) {
    save_system_prompt_cache(cached_text, now, cwd);
    free(cwd);
    return cached_text;
  }
  free(cached_text);
  char *fresh = build_fresh_system_prompt();
  save_system_prompt_cache(fresh, now, cwd ? cwd : "");
  free(cwd);
  return fresh;
}

void clay_commands_reset_conversation(ClayCommands *commands) {
  clay_json_free(commands->conversation);
  commands->conversation = clay_json_array();
  free(commands->system_prompt);
  const char *persisted =
      commands->chat ? clay_chat_system_prompt(commands->chat) : "";
  commands->system_prompt =
      *persisted ? strdup(persisted) : clay_commands_build_system_prompt();
  clay_json_array_push(commands->conversation,
                       clay_openai_message("system", commands->system_prompt));
  if (!commands->chat)
    return;
  ClayJson *history = clay_chat_openai_messages(commands->chat);
  for (size_t i = 0; i < clay_json_array_count(history); i++) {
    clay_json_array_push(commands->conversation,
                         clay_json_clone(clay_json_array_get(history, i)));
  }
  clay_json_free(history);
}

#define CLAY_CONTEXT_COMPACT_RATIO 0.9
#define CLAY_CONTEXT_KEEP_TURNS 6
#define CLAY_CONTEXT_TOOL_PREVIEW 200
#define CLAY_CONTEXT_COLLAPSED_MARKER "[collapsed - "

static int is_user_message(const ClayJson *message) {
  return strcmp(clay_json_string_value(clay_json_object_get(message, "role")),
                "user") == 0;
}

static int collapse_tool_content(ClayJson *message) {
  const char *text =
      clay_json_string_value(clay_json_object_get(message, "content"));
  if (!text)
    return 0;
  size_t len = strlen(text);
  if (len <= CLAY_CONTEXT_TOOL_PREVIEW ||
      strncmp(text, CLAY_CONTEXT_COLLAPSED_MARKER,
              strlen(CLAY_CONTEXT_COLLAPSED_MARKER)) == 0) {
    return 0;
  }
  ClayStr summary;
  clay_str_init(&summary);
  clay_str_printf(&summary, "%s%zu bytes, showing the first %d]\n%.*s...",
                  CLAY_CONTEXT_COLLAPSED_MARKER, len, CLAY_CONTEXT_TOOL_PREVIEW,
                  CLAY_CONTEXT_TOOL_PREVIEW, text);
  clay_json_object_set(message, "content", clay_json_string(summary.data));
  clay_str_free(&summary);
  return 1;
}

int clay_commands_maybe_compact(ClayCommands *commands) {
  long token_budget = clay_config_context_token_budget();
  if (commands->input_tokens <
      (long)(token_budget * CLAY_CONTEXT_COMPACT_RATIO))
    return 0;
  size_t count = clay_json_array_count(commands->conversation);
  if (count < 2)
    return 0;

  /* Everything at index < cutoff is older than the last CLAY_CONTEXT_KEEP_TURNS
     user turns and is eligible for collapsing; index 0 (system prompt) never
     is. */
  size_t cutoff = 1;
  int turns_seen = 0;
  for (size_t i = count; i-- > 1;) {
    if (is_user_message(clay_json_array_get(commands->conversation, i))) {
      turns_seen++;
      if (turns_seen == CLAY_CONTEXT_KEEP_TURNS) {
        cutoff = i;
        break;
      }
    }
  }

  int collapsed = 0;
  for (size_t i = 1; i < cutoff; i++) {
    ClayJson *message = clay_json_array_get(commands->conversation, i);
    if (strcmp(clay_json_string_value(clay_json_object_get(message, "role")),
               "tool") == 0) {
      collapsed += collapse_tool_content(message);
    }
  }
  return collapsed;
}

void clay_commands_clear_todos(ClayCommands *commands) {
  for (size_t i = 0; i < commands->todos.count; i++) {
    ClayTodoItem *item = clay_array_get(&commands->todos, i);
    free(item->content);
    free(item->status);
  }
  clay_array_clear(&commands->todos);
}

void clay_commands_new_chat(ClayCommands *commands) {
  clay_chat_destroy(commands->chat);
  commands->chat = NULL;
  clay_commands_reset_conversation(commands);
  commands->input_tokens = 0;
  commands->output_tokens = 0;
  commands->cached_input_tokens = 0;
  commands->cached_input_tokens_known = 0;
  clay_below_stop_elapsed("status");
  clay_below_set_enabled("status", 0);
  clay_commands_set_tokens_below(commands, 0, 0);
  clay_commands_clear_todos(commands);
}

int clay_commands_select_model(ClayCommands *commands, const char *provider,
                               const char *model) {
  int changed = !commands->selected_provider || !commands->selected_model ||
                strcmp(commands->selected_provider, provider) != 0 ||
                strcmp(commands->selected_model, model) != 0;
  char *provider_copy = strdup(provider);
  char *model_copy = strdup(model);
  free(commands->selected_provider);
  free(commands->selected_model);
  commands->selected_provider = provider_copy;
  commands->selected_model = model_copy;
  if (changed) {
    clay_chat_destroy(commands->chat);
    commands->chat = NULL;
    clay_commands_reset_conversation(commands);
    commands->input_tokens = 0;
    commands->output_tokens = 0;
    commands->cached_input_tokens = 0;
    commands->cached_input_tokens_known = 0;
    clay_commands_set_tokens_below(commands, 0, 0);
  }
  clay_commands_update_selected_below(commands);
  return clay_config_selection_save(commands->selected_provider,
                                    commands->selected_model);
}

int clay_commands_fetch_models(void *ctx, ClayArray *out) {
  ClayConnectedProvider *provider = ctx;
  if (!provider->models_fetched) {
    ClayTask *task =
        clay_task_start("Retrieving %s models", provider->type->label);
    if (strcmp(provider->type->id, "openai-codex") == 0) {
      provider->models_rc = clay_openai_codex_list_models(
          provider->codex_client, &provider->models);
      provider->models_status =
          clay_openai_codex_last_status(provider->codex_client);
      if (provider->models_rc == 0)
        clay_commands_save_codex_credentials(provider, provider->codex_client);
    } else if (strcmp(provider->type->id, "grok") == 0 &&
               provider->grok_client) {
      provider->models_rc =
          clay_grok_list_models(provider->grok_client, &provider->models);
      provider->models_status = clay_grok_last_status(provider->grok_client);
      if (provider->models_rc == 0)
        clay_commands_save_grok_credentials(provider, provider->grok_client);
    } else {
      provider->models_rc =
          clay_openai_list_models(provider->client, &provider->models);
    }
    provider->models_fetched = 1;
    if (provider->models_rc == 0)
      clay_task_success(task, "%zu models available", provider->models.count);
    else if (provider->models_status < 200 || provider->models_status >= 300)
      clay_task_fail(task, "HTTP %ld", provider->models_status);
    else
      clay_task_fail(task, "Invalid or empty model catalog");
  }
  for (size_t i = 0; i < provider->models.count; i++) {
    ClayModelItem item = {*(char **)clay_array_get(&provider->models, i), NULL};
    clay_array_push_val(out, &item);
  }
  return provider->models_rc;
}

const ClayReasoningEffort *
clay_commands_reasoning_effort(const ClayCommands *commands) {
  return &REASONING_EFFORTS[commands->reasoning_effort_index];
}

size_t clay_commands_reasoning_effort_count(void) {
  return sizeof(REASONING_EFFORTS) / sizeof(REASONING_EFFORTS[0]);
}

const ClayReasoningEffort *clay_commands_reasoning_efforts(void) {
  return REASONING_EFFORTS;
}

void clay_commands_print_history(ClayCommands *commands, size_t count) {
  ClayJson *messages = clay_chat_recent_messages(commands->chat, count);
  for (size_t i = 0; i < clay_json_array_count(messages); i++) {
    ClayJson *message = clay_json_array_get(messages, i);
    const char *role =
        clay_json_string_value(clay_json_object_get(message, "role"));
    const char *content =
        clay_json_string_value(clay_json_object_get(message, "content"));
    clay_sayc(strcmp(role, "user") == 0 ? CLAY_CYAN : CLAY_GRAY, "%s: %s",
              strcmp(role, "user") == 0 ? "You" : "Clay", content);
  }
  clay_json_free(messages);
}

ClayCommands *clay_commands_create(ClayApp *app) {
  ClayCommands *commands = calloc(1, sizeof(ClayCommands));
  commands->app = app;
  commands->running = 1;
  clay_array_init(&commands->providers, sizeof(ClayConnectedProvider));
  size_t count;
  const ClayProviderType *types = clay_commands_provider_types(&count);
  for (size_t i = 0; i < count; i++)
    clay_commands_load_provider(commands, &types[i]);
  clay_config_selection_load(&commands->selected_provider,
                             &commands->selected_model);
  int environment_selection = 0;
  const char *forced_provider = environment_provider();
  if (forced_provider && clay_commands_find_provider(commands, forced_provider)) {
    if (!commands->selected_provider ||
        strcmp(commands->selected_provider, forced_provider) != 0) {
      free(commands->selected_provider);
      commands->selected_provider = strdup(forced_provider);
      free(commands->selected_model);
      commands->selected_model = NULL;
      environment_selection = 1;
    }
  } else if (!commands->selected_provider && commands->providers.count == 1) {
    ClayConnectedProvider *only =
        clay_array_get(&commands->providers, 0);
    commands->selected_provider = strdup(only->type->id);
  }
  if (commands->selected_provider) {
    const char *model = environment_model(commands->selected_provider);
    if (model && *model) {
      free(commands->selected_model);
      commands->selected_model = strdup(model);
      environment_selection = 1;
    }
  }
  char *saved_effort = clay_config_reasoning_effort();
  if (saved_effort) {
    for (size_t i = 1; i < clay_commands_reasoning_effort_count(); i++)
      if (strcmp(REASONING_EFFORTS[i].id, saved_effort) == 0) {
        commands->reasoning_effort_index = (int)i;
        break;
      }
  }
  free(saved_effort);
  char *sandbox_mode = clay_config_sandbox_mode();
  commands->sandbox_auto_approve = strcmp(sandbox_mode, "auto") == 0;
  commands->sandbox_mode = strcmp(sandbox_mode, "unleashed") == 0
                               ? CLAY_SANDBOX_MODE_UNLEASHED
                               : CLAY_SANDBOX_MODE_SANDBOX;
  free(sandbox_mode);
  commands->use_integrated_shell = clay_config_use_integrated_shell();
  if (!clay_sandbox_supported()) {
    commands->sandbox_mode = CLAY_SANDBOX_MODE_UNLEASHED;
    commands->sandbox_auto_approve = 0;
  }
  for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) {
    commands->auto_approve[i] = clay_config_auto_approve(
        clay_permissions_category_name((ClayPermissionCategory)i));
    clay_array_init(&commands->remembered_patterns[i], sizeof(char *));
  }
  clay_array_init(&commands->todos, sizeof(ClayTodoItem));
  clay_array_init(&commands->mcp_servers, sizeof(ClayMcpServer *));
  clay_array_init(&commands->mcp_bindings, sizeof(ClayMcpToolBinding));
  clay_array_init(&commands->undo_history, sizeof(ClayUndoEntry));
  commands->auto_test_command = clay_config_auto_test_command();
  commands->auto_test_choice = CLAY_AUTO_TEST_UNASKED;
  clay_commands_reset_conversation(commands);
  if (!environment_selection)
    clay_config_selection_save(commands->selected_provider,
                               commands->selected_model);
  clay_below_add(6, "status");
  clay_below_set_enabled("status", 0);
  clay_below_add(1, "model");
  clay_below_set_alignment("model", CLAY_BELOW_ALIGN_LEFT);
  clay_below_add(2, "tokens");
  clay_below_set_alignment("tokens", CLAY_BELOW_ALIGN_RIGHT);
  clay_below_add(3, "hint");
  clay_below_set_enabled("hint", 0);
  clay_below_add(4, "mode");
  clay_below_set_enabled("mode", 0);
  clay_below_add(5, "sandbox");
  clay_below_set_alignment("sandbox", CLAY_BELOW_ALIGN_RIGHT);
  clay_below_set_alignment("status", CLAY_BELOW_ALIGN_RIGHT);
  clay_commands_set_tokens_below(commands, 0, 0);
  clay_commands_update_selected_below(commands);
  clay_commands_update_sandbox_below(commands);
  return commands;
}

void clay_commands_destroy(ClayCommands *commands) {
  if (!commands)
    return;
  for (size_t i = 0; i < commands->providers.count; i++)
    provider_free(clay_array_get(&commands->providers, i));
  clay_array_free(&commands->providers);
  clay_json_free(commands->conversation);
  free(commands->system_prompt);
  clay_chat_destroy(commands->chat);
  free(commands->selected_provider);
  free(commands->selected_model);
  for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) {
    ClayArray *remembered = &commands->remembered_patterns[i];
    for (size_t j = 0; j < remembered->count; j++)
      free(*(char **)clay_array_get(remembered, j));
    clay_array_free(remembered);
  }
  clay_commands_clear_todos(commands);
  clay_array_free(&commands->todos);
  for (size_t i = 0; i < commands->mcp_bindings.count; i++) {
    ClayMcpToolBinding *binding = clay_array_get(&commands->mcp_bindings, i);
    free(binding->tool_name);
    free(binding->exposed_name);
  }
  clay_array_free(&commands->mcp_bindings);
  clay_commands_undo_destroy(commands);
  for (size_t i = 0; i < commands->mcp_servers.count; i++) {
    clay_mcp_disconnect(
        *(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i));
  }
  clay_array_free(&commands->mcp_servers);
  free(commands->auto_test_command);
  free(commands);
}

int clay_commands_running(const ClayCommands *commands) {
  return commands->running;
}

int clay_commands_has_provider(const ClayCommands *commands) {
  return commands->providers.count > 0;
}

void clay_commands_print_session_summary(const ClayCommands *commands) {
  printf("%sSession summary%s  %s\xe2\x86\x91 %ld%s  %s\xe2\x86\x93 %ld%s  "
         "%s%ld messages sent%s\n",
         clay_color(CLAY_GRAY), clay_color(CLAY_RESET), clay_color(CLAY_CYAN),
         commands->total_input_tokens, clay_color(CLAY_RESET),
         clay_color(CLAY_CYAN), commands->total_output_tokens,
         clay_color(CLAY_RESET), clay_color(CLAY_GRAY), commands->messages_sent,
         clay_color(CLAY_RESET));
}
