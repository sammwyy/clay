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
