#ifndef CLAY_CONFIG_H
#define CLAY_CONFIG_H

/* One provider's saved connection, e.g. ~/.clay/providers/openai.json. */
typedef struct {
    char *id;
    char *apikey;
    char *base_url;
} ClayProviderConfig;

int clay_config_exists(const char *id);

/* NULL if missing or malformed. Caller frees with clay_config_free. */
ClayProviderConfig *clay_config_load(const char *id);

/* Overwrites the saved config for config->id, creating ~/.clay and its
   providers/ subdirectory as needed. 0 on success. */
int clay_config_save(const ClayProviderConfig *config);

void clay_config_free(ClayProviderConfig *config);

/* Loads the selected provider/model from ~/.clay/config.json. Missing or
   malformed values become NULL. The returned strings are malloc'd. */
int clay_config_selection_load(char **provider_out, char **model_out);

/* Saves the selected provider/model to ~/.clay/config.json. NULL values
   are written as JSON null. 0 on success. */
int clay_config_selection_save(const char *provider, const char *model);

/* Number of previous messages shown after /resume. Defaults to 4. */
int clay_config_history_preview_count(void);

/* "sandbox" (default) or "unleashed". Malloc'd. Doesn't know about
   ClaySandboxMode - callers map the string, same as PROVIDER_TYPES does
   for provider ids. */
char *clay_config_sandbox_mode(void);
int clay_config_set_sandbox_mode(const char *mode);

/* "readonly" (default) or "writable". Malloc'd. */
char *clay_config_sandbox_access(void);
int clay_config_set_sandbox_access(const char *access);

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
