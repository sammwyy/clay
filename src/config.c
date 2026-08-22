#include "clay/config.h"

#include "clay/json.h"
#include "clay/str.h"
#include "clay/term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *config_dir(void) {
    char *home = clay_term_home_dir();
    if (!home) return NULL;

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay", home);
    free(home);
    return path.data;
}

static char *providers_dir(void) {
    char *dir = config_dir();
    if (!dir) return NULL;

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/providers", dir);
    free(dir);
    return path.data;
}

static char *selection_path(void) {
    char *dir = config_dir();
    if (!dir) return NULL;

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/config.json", dir);
    free(dir);
    return path.data;
}

static char *provider_path(const char *id) {
    char *dir = providers_dir();
    if (!dir) return NULL;

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/%s.json", dir, id);
    free(dir);
    return path.data;
}

int clay_config_exists(const char *id) {
    char *path = provider_path(id);
    if (!path) return 0;

    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return 0;
    fclose(f);
    return 1;
}

static char *read_whole_file(FILE *f) {
    ClayStr s;
    clay_str_init(&s);
    int c;
    while ((c = fgetc(f)) != EOF) clay_str_push_char(&s, (char)c);
    return s.data;
}

/* Empty object if missing/malformed - every field below is read with a
   fallback default, so a fresh or corrupt file behaves like defaults. */
static ClayJson *load_selection_root(void) {
    char *path = selection_path();
    if (!path) return clay_json_object();
    FILE *file = fopen(path, "r");
    free(path);
    if (!file) return clay_json_object();
    char *text = read_whole_file(file);
    fclose(file);
    ClayJson *root = clay_json_parse(text, NULL);
    free(text);
    if (!root || clay_json_type(root) != CLAY_JSON_OBJECT) {
        clay_json_free(root);
        return clay_json_object();
    }
    return root;
}

static int save_selection_root(ClayJson *root) {
    char *dir = config_dir();
    if (!dir || clay_term_mkdir(dir) != 0) {
        free(dir);
        clay_json_free(root);
        return -1;
    }
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/config.json", dir);
    free(dir);

    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(root, &body);
    clay_json_free(root);

    FILE *f = fopen(path.data, "w");
    if (!f) {
        clay_str_free(&path);
        clay_str_free(&body);
        return -1;
    }
    fwrite(body.data, 1, body.len, f);
    fclose(f);
    clay_term_restrict_file(path.data);

    clay_str_free(&path);
    clay_str_free(&body);
    return 0;
}

ClayProviderConfig *clay_config_load(const char *id) {
    char *path = provider_path(id);
    if (!path) return NULL;

    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return NULL;

    char *text = read_whole_file(f);
    fclose(f);

    ClayJson *root = clay_json_parse(text, NULL);
    free(text);
    if (!root) return NULL;

    ClayProviderConfig *config = malloc(sizeof(ClayProviderConfig));
    config->id = strdup(clay_json_string_value(clay_json_object_get(root, "id")));
    config->apikey = strdup(clay_json_string_value(clay_json_object_get(root, "apikey")));
    config->base_url = strdup(clay_json_string_value(clay_json_object_get(root, "base_url")));
    clay_json_free(root);
    return config;
}

int clay_config_save(const ClayProviderConfig *config) {
    char *dir = config_dir();
    if (!dir || clay_term_mkdir(dir) != 0) {
        free(dir);
        return -1;
    }

    ClayStr providers;
    clay_str_init(&providers);
    clay_str_printf(&providers, "%s/providers", dir);
    free(dir);
    if (clay_term_mkdir(providers.data) != 0) {
        clay_str_free(&providers);
        return -1;
    }

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/%s.json", providers.data, config->id);
    clay_str_free(&providers);

    ClayJson *root = clay_json_object();
    clay_json_object_set(root, "id", clay_json_string(config->id));
    clay_json_object_set(root, "apikey", clay_json_string(config->apikey));
    clay_json_object_set(root, "base_url", clay_json_string(config->base_url ? config->base_url : ""));

    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(root, &body);
    clay_json_free(root);

    FILE *f = fopen(path.data, "w");
    if (!f) {
        clay_str_free(&path);
        clay_str_free(&body);
        return -1;
    }
    fwrite(body.data, 1, body.len, f);
    fclose(f);
    clay_term_restrict_file(path.data);

    clay_str_free(&path);
    clay_str_free(&body);
    return 0;
}

void clay_config_free(ClayProviderConfig *config) {
    if (!config) return;
    free(config->id);
    free(config->apikey);
    free(config->base_url);
    free(config);
}

int clay_config_selection_load(char **provider_out, char **model_out) {
    ClayJson *root = load_selection_root();
    ClayJson *provider = clay_json_object_get(root, "provider");
    ClayJson *model = clay_json_object_get(root, "model");
    *provider_out = clay_json_type(provider) == CLAY_JSON_STRING ? strdup(clay_json_string_value(provider)) : NULL;
    *model_out = clay_json_type(model) == CLAY_JSON_STRING ? strdup(clay_json_string_value(model)) : NULL;
    clay_json_free(root);
    return 0;
}

/* Only touches provider/model - other fields keep whatever
   load_selection_root already found. */
int clay_config_selection_save(const char *provider, const char *model) {
    ClayJson *root = load_selection_root();
    clay_json_object_set(root, "provider", provider ? clay_json_string(provider) : clay_json_null());
    clay_json_object_set(root, "model", model ? clay_json_string(model) : clay_json_null());
    return save_selection_root(root);
}

int clay_config_history_preview_count(void) {
    ClayJson *root = load_selection_root();
    ClayJson *value = clay_json_object_get(root, "history_preview_count");
    int count = clay_json_type(value) == CLAY_JSON_NUMBER ? (int)clay_json_number_value(value) : 4;
    clay_json_free(root);
    return count >= 0 ? count : 4;
}

static char *string_field(ClayJson *root, const char *key, const char *fallback) {
    ClayJson *value = clay_json_object_get(root, key);
    return strdup(clay_json_type(value) == CLAY_JSON_STRING ? clay_json_string_value(value) : fallback);
}

static int set_string_field(const char *key, const char *value) {
    ClayJson *root = load_selection_root();
    clay_json_object_set(root, key, clay_json_string(value));
    return save_selection_root(root);
}

char *clay_config_sandbox_mode(void) {
    ClayJson *root = load_selection_root();
    char *mode = string_field(root, "sandbox_mode", "sandbox");
    clay_json_free(root);
    return mode;
}

int clay_config_set_sandbox_mode(const char *mode) {
    return set_string_field("sandbox_mode", mode);
}

/* Reads and edits are low-risk (and edits are checkpointed, see
   clay/checkpoint.h) so they default to on; running arbitrary commands
   defaults to off, with a curated safe subset defaulting on. */
static int auto_approve_default(const char *category) {
    return strcmp(category, "exec_all") != 0;
}

int clay_config_auto_approve(const char *category) {
    ClayJson *root = load_selection_root();
    ClayStr key;
    clay_str_init(&key);
    clay_str_printf(&key, "auto_approve_%s", category);
    ClayJson *value = clay_json_object_get(root, key.data);
    int result = clay_json_type(value) == CLAY_JSON_BOOL ? clay_json_bool_value(value) : auto_approve_default(category);
    clay_str_free(&key);
    clay_json_free(root);
    return result;
}

int clay_config_set_auto_approve(const char *category, int value) {
    ClayJson *root = load_selection_root();
    ClayStr key;
    clay_str_init(&key);
    clay_str_printf(&key, "auto_approve_%s", category);
    clay_json_object_set(root, key.data, clay_json_bool(value));
    clay_str_free(&key);
    return save_selection_root(root);
}

char *clay_config_auto_test_command(void) {
    ClayJson *root = load_selection_root();
    char *command = string_field(root, "auto_test_command", "");
    clay_json_free(root);
    return command;
}

int clay_config_set_auto_test_command(const char *command) {
    return set_string_field("auto_test_command", command);
}
