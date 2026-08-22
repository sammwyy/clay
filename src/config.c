#include "clay/config.h"

#include "clay/json.h"
#include "clay/str.h"
#include "clay/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *selection_path(void) {
  return clay_storage_path("config.json");
}

static char *provider_path(const char *id) {
  ClayStr path;
  clay_str_init(&path);
  clay_str_printf(&path, "providers/%s.json", id);
  char *full_path = clay_storage_path(path.data);
  clay_str_free(&path);
  return full_path;
}

int clay_config_exists(const char *id) {
  char *path = provider_path(id);
  if (!path)
    return 0;

  FILE *f = fopen(path, "r");
  free(path);
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

#define CLAY_CONFIG_FILE_LIMIT (4 * 1024 * 1024)

static char *json_optional_string(ClayJson *root, const char *key) {
  ClayJson *value = clay_json_object_get(root, key);
  return clay_json_type(value) == CLAY_JSON_STRING
             ? strdup(clay_json_string_value(value))
             : NULL;
}

/* Empty object if missing/malformed - every field below is read with a
   fallback default, so a fresh or corrupt file behaves like defaults. */
static ClayJson *load_selection_root(void) {
  char *path = selection_path();
  if (!path)
    return clay_json_object();
  ClayStr text;
  int read_rc = clay_storage_read_limited(path, CLAY_CONFIG_FILE_LIMIT, &text);
  free(path);
  if (read_rc != 0)
    return clay_json_object();
  ClayJson *root = clay_json_parse(text.data, NULL);
  clay_str_free(&text);
  if (!root || clay_json_type(root) != CLAY_JSON_OBJECT) {
    clay_json_free(root);
    return clay_json_object();
  }
  return root;
}

static int save_selection_root(ClayJson *root) {
  if (clay_storage_ensure_dir("") != 0) {
    clay_json_free(root);
    return -1;
  }
  char *path = selection_path();
  if (!path) {
    clay_json_free(root);
    return -1;
  }

  ClayStr body;
  clay_str_init(&body);
  clay_json_stringify(root, &body);
  clay_json_free(root);

  int rc = clay_storage_write_atomic_private(path, body.data, body.len);

  free(path);
  clay_str_free(&body);
  return rc;
}

ClayProviderConfig *clay_config_load(const char *id) {
  char *path = provider_path(id);
  if (!path)
    return NULL;

  ClayStr text;
  int read_rc = clay_storage_read_limited(path, CLAY_CONFIG_FILE_LIMIT, &text);
  free(path);
  if (read_rc != 0)
    return NULL;
  ClayJson *root = clay_json_parse(text.data, NULL);
  clay_str_free(&text);
  if (!root)
    return NULL;

  ClayProviderConfig *config = malloc(sizeof(ClayProviderConfig));
  config->id = json_optional_string(root, "id");
  config->apikey = json_optional_string(root, "apikey");
  config->base_url = json_optional_string(root, "base_url");
  config->auth_mode = json_optional_string(root, "auth_mode");
  config->access_token = json_optional_string(root, "access_token");
  config->refresh_token = json_optional_string(root, "refresh_token");
  config->id_token = json_optional_string(root, "id_token");
  config->account_id = json_optional_string(root, "account_id");
  config->expires_at = (long long)clay_json_number_value(
      clay_json_object_get(root, "expires_at"));
  if (!config->id) {
    clay_config_free(config);
    config = NULL;
  }
  clay_json_free(root);
  return config;
}

int clay_config_save(const ClayProviderConfig *config) {
  if (!config || !config->id || clay_storage_ensure_dir("providers") != 0)
    return -1;
  char *path = provider_path(config->id);
  if (!path) return -1;

  ClayJson *root = clay_json_object();
  clay_json_object_set(root, "id", clay_json_string(config->id));
  clay_json_object_set(root, "apikey",
                       config->apikey ? clay_json_string(config->apikey)
                                      : clay_json_null());
  clay_json_object_set(
      root, "base_url",
      clay_json_string(config->base_url ? config->base_url : ""));
  if (config->auth_mode)
    clay_json_object_set(root, "auth_mode", clay_json_string(config->auth_mode));
  if (config->access_token)
    clay_json_object_set(root, "access_token",
                         clay_json_string(config->access_token));
  if (config->refresh_token)
    clay_json_object_set(root, "refresh_token",
                         clay_json_string(config->refresh_token));
  if (config->id_token)
    clay_json_object_set(root, "id_token", clay_json_string(config->id_token));
  if (config->account_id)
    clay_json_object_set(root, "account_id",
                         clay_json_string(config->account_id));
  if (config->expires_at > 0)
    clay_json_object_set(root, "expires_at",
                         clay_json_number(config->expires_at));

  ClayStr body;
  clay_str_init(&body);
  clay_json_stringify(root, &body);
  clay_json_free(root);

  int rc = clay_storage_write_atomic_private(path, body.data, body.len);

  free(path);
  clay_str_free(&body);
  return rc;
}

int clay_config_remove(const char *id) {
  char *path = provider_path(id);
  if (!path)
    return -1;
  int rc = remove(path);
  free(path);
  return rc == 0 ? 0 : -1;
}

void clay_config_free(ClayProviderConfig *config) {
  if (!config)
    return;
  free(config->id);
  free(config->apikey);
  free(config->base_url);
  free(config->auth_mode);
  free(config->access_token);
  free(config->refresh_token);
  free(config->id_token);
  free(config->account_id);
  free(config);
}

int clay_config_selection_load(char **provider_out, char **model_out) {
  ClayJson *root = load_selection_root();
  ClayJson *provider = clay_json_object_get(root, "provider");
  ClayJson *model = clay_json_object_get(root, "model");
  *provider_out = clay_json_type(provider) == CLAY_JSON_STRING
                      ? strdup(clay_json_string_value(provider))
                      : NULL;
  *model_out = clay_json_type(model) == CLAY_JSON_STRING
                   ? strdup(clay_json_string_value(model))
                   : NULL;
  clay_json_free(root);
  return 0;
}

/* Only touches provider/model - other fields keep whatever
   load_selection_root already found. */
int clay_config_selection_save(const char *provider, const char *model) {
  ClayJson *root = load_selection_root();
  clay_json_object_set(root, "provider",
                       provider ? clay_json_string(provider)
                                : clay_json_null());
  clay_json_object_set(root, "model",
                       model ? clay_json_string(model) : clay_json_null());
  return save_selection_root(root);
}

char *clay_config_reasoning_effort(void) {
  ClayJson *root = load_selection_root();
  ClayJson *value = clay_json_object_get(root, "reasoning_effort");
  char *effort = clay_json_type(value) == CLAY_JSON_STRING
                     ? strdup(clay_json_string_value(value))
                     : NULL;
  clay_json_free(root);
  return effort;
}

int clay_config_set_reasoning_effort(const char *effort) {
  ClayJson *root = load_selection_root();
  clay_json_object_set(root, "reasoning_effort",
                       effort ? clay_json_string(effort) : clay_json_null());
  return save_selection_root(root);
}

int clay_config_history_preview_count(void) {
  ClayJson *root = load_selection_root();
  ClayJson *value = clay_json_object_get(root, "history_preview_count");
  int count = clay_json_type(value) == CLAY_JSON_NUMBER
                  ? (int)clay_json_number_value(value)
                  : 4;
  clay_json_free(root);
  return count >= 0 ? count : 4;
}

long clay_config_context_token_budget(void) {
  ClayJson *root = load_selection_root();
  ClayJson *value = clay_json_object_get(root, "context_token_budget");
  long budget = clay_json_type(value) == CLAY_JSON_NUMBER
                    ? (long)clay_json_number_value(value)
                    : 128000;
  clay_json_free(root);
  return budget >= 1024 && budget <= 1073741824L ? budget : 128000;
}

int clay_config_set_context_token_budget(long budget) {
  if (budget < 1024 || budget > 1073741824L) return -1;
  ClayJson *root = load_selection_root();
  clay_json_object_set(root, "context_token_budget", clay_json_number((double)budget));
  return save_selection_root(root);
}

static char *string_field(ClayJson *root, const char *key,
                          const char *fallback) {
  ClayJson *value = clay_json_object_get(root, key);
  return strdup(clay_json_type(value) == CLAY_JSON_STRING
                    ? clay_json_string_value(value)
                    : fallback);
}

static int set_string_field(const char *key, const char *value) {
  ClayJson *root = load_selection_root();
  clay_json_object_set(root, key, clay_json_string(value));
  return save_selection_root(root);
}

static int set_bool_field(const char *key, int value) {
  ClayJson *root = load_selection_root();
  clay_json_object_set(root, key, clay_json_bool(value));
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

int clay_config_use_integrated_shell(void) {
  ClayJson *root = load_selection_root();
  ClayJson *value = clay_json_object_get(root, "use_integrated_shell");
  int enabled = clay_json_type(value) == CLAY_JSON_BOOL
                    ? clay_json_bool_value(value)
                    : 1;
  clay_json_free(root);
  return enabled;
}

int clay_config_set_use_integrated_shell(int value) {
  return set_bool_field("use_integrated_shell", value);
}

static int valid_mount_path(const char *path) {
  return path && path[0] == '/' && !strstr(path, "/../") &&
         strcmp(path, "/..") != 0;
}

char **clay_config_sandbox_readonly_mounts(size_t *count_out) {
  if (count_out) *count_out = 0;
  ClayJson *root = load_selection_root();
  ClayJson *array = clay_json_object_get(root, "sandbox_readonly_mounts");
  size_t count = clay_json_array_count(array), kept = 0;
  char **paths = count ? calloc(count, sizeof(char *)) : NULL;
  for (size_t i = 0; i < count; i++) {
    ClayJson *item = clay_json_array_get(array, i);
    const char *path = clay_json_type(item) == CLAY_JSON_STRING
                           ? clay_json_string_value(item)
                           : NULL;
    if (valid_mount_path(path)) paths[kept++] = strdup(path);
  }
  clay_json_free(root);
  if (count_out) *count_out = kept;
  return paths;
}

int clay_config_set_sandbox_readonly_mounts(const char *const *paths, size_t count) {
  ClayJson *root = load_selection_root();
  ClayJson *array = clay_json_array();
  for (size_t i = 0; i < count; i++)
    if (valid_mount_path(paths[i])) clay_json_array_push(array, clay_json_string(paths[i]));
  clay_json_object_set(root, "sandbox_readonly_mounts", array);
  return save_selection_root(root);
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
  int result = clay_json_type(value) == CLAY_JSON_BOOL
                   ? clay_json_bool_value(value)
                   : auto_approve_default(category);
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
