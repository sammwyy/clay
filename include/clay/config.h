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

#endif /* CLAY_CONFIG_H */
