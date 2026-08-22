#ifndef CLAY_CONFIG_H
#define CLAY_CONFIG_H

#include <stddef.h>

/* One provider's saved connection, e.g. ~/.clay/providers/openai.json. */
typedef struct {
  char *id;
  char *apikey;
  char *base_url;
  /* Provider-specific authentication kind, e.g. "api_key" or
     "subscription" for Grok. NULL preserves older provider configs. */
  char *auth_mode;
  /* Present only for the OpenAI Codex subscription provider.  Provider
     files are owner-only, just like the existing API-key field. */
  char *access_token;
  char *refresh_token;
  char *id_token;
  char *account_id;
  long long expires_at;
} ClayProviderConfig;

int clay_config_exists(const char *id);

/* NULL if missing or malformed. Caller frees with clay_config_free. */
ClayProviderConfig *clay_config_load(const char *id);

/* Overwrites the saved config for config->id, creating ~/.clay and its
   providers/ subdirectory as needed. 0 on success. */
int clay_config_save(const ClayProviderConfig *config);

/* Removes a saved provider config. 0 on success. */
int clay_config_remove(const char *id);

void clay_config_free(ClayProviderConfig *config);

/* Loads the selected provider/model from ~/.clay/config.json. Missing or
   malformed values become NULL. The returned strings are malloc'd. */
int clay_config_selection_load(char **provider_out, char **model_out);

/* Saves the selected provider/model to ~/.clay/config.json. NULL values
   are written as JSON null. 0 on success. */
int clay_config_selection_save(const char *provider, const char *model);

/* The selected reasoning effort (`low`, `medium`, etc.). NULL means use the
   provider/model default. Caller frees the returned string. */
char *clay_config_reasoning_effort(void);
int clay_config_set_reasoning_effort(const char *effort);

/* Number of previous messages shown after /resume. Defaults to 4. */
int clay_config_history_preview_count(void);

/* "sandbox" (default), "auto", or "unleashed". Malloc'd. Doesn't know about
   ClaySandboxMode - callers map the string, same as PROVIDER_TYPES does
   for provider ids. */
char *clay_config_sandbox_mode(void);
int clay_config_set_sandbox_mode(const char *mode);

/* Whether shell_exec uses Clay's parsed shell. Defaults to true. */
int clay_config_use_integrated_shell(void);
int clay_config_set_use_integrated_shell(int value);

/* Absolute host paths made visible read-only inside Linux sandbox mode.
   Returns a malloc'd array of malloc'd strings; caller frees every item and
   the array. Invalid config entries are ignored. */
char **clay_config_sandbox_readonly_mounts(size_t *count_out);
int clay_config_set_sandbox_readonly_mounts(const char *const *paths, size_t count);

/* Auto-approve toggles for tool categories. `category` is "read", "edit",
   "exec_safe", or "exec_all". */
int clay_config_auto_approve(const char *category);
int clay_config_set_auto_approve(const char *category, int value);

/* Shell command run after a successful write/edit (e.g. "make test",
   "npm run lint"), with output reported back to the model on failure.
   Malloc'd; "" if unset. */
char *clay_config_auto_test_command(void);
int clay_config_set_auto_test_command(const char *command);

#endif /* CLAY_CONFIG_H */
