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

#endif /* CLAY_CONFIG_H */
