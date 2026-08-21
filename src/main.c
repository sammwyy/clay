#include "clay/clay.h"
#include "clay/http.h"
#include "clay/providers/openai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAY_VERSION "0.0.0"
#define CLAY_SYSTEM_PROMPT "You are clay, a helpful AI coding assistant. Be concise, accurate, and practical. " \
                           "Explain code changes clearly and ask for clarification when the request is ambiguous. " \
                           "Use shell_exec when inspecting or changing the current workspace helps answer the user. " \
                           "It runs its command in the current workspace; prefer focused commands and summarize results."

#define CLAY_SHELL_OUTPUT_LIMIT (64 * 1024)
#define CLAY_TOOL_VISIBLE_LINES 8

static int g_running = 1;
static ClayJson *g_conversation = NULL;
static long g_input_tokens = 0;
static long g_output_tokens = 0;
static long g_total_input_tokens = 0;
static long g_total_output_tokens = 0;
static long g_messages_sent = 0;

static void conversation_reset(void);
static void update_tokens_below(void);

static void print_session_summary(void) {
    printf("%sSession summary%s  %s\xe2\x86\x91 %ld%s  %s\xe2\x86\x93 %ld%s  %s%ld messages sent%s\n",
           clay_color(CLAY_GRAY), clay_color(CLAY_RESET), clay_color(CLAY_CYAN), g_total_input_tokens,
           clay_color(CLAY_RESET), clay_color(CLAY_CYAN), g_total_output_tokens, clay_color(CLAY_RESET),
           clay_color(CLAY_GRAY), g_messages_sent, clay_color(CLAY_RESET));
}

static void cmd_exit(const char *args, void *user_data) {
    (void)args;
    ClayApp *app = user_data;
    g_running = 0;
    clay_app_set_state(app, CLAY_APP_EXITING);
}

static void print_help_entry(const char *name, const char *description, void *ctx) {
    (void)ctx;
    printf("  %s/%s%s %s%s%s\n", clay_color(CLAY_ORANGE), name, clay_color(CLAY_RESET),
           clay_color(CLAY_GRAY), description, clay_color(CLAY_RESET));
}

static void cmd_help(const char *args, void *user_data) {
    (void)args;
    ClayApp *app = user_data;
    printf("\nAvailable commands:\n");
    clay_command_foreach(clay_app_commands(app), print_help_entry, NULL);
    fputc('\n', stdout);
}

static void cmd_confirm(const char *args, void *user_data) {
    ClayApp *app = user_data;
    const char *question = (args && *args) ? args : "Proceed?";
    int yes = clay_app_confirm(app, question, 1);
    clay_sayc(yes ? CLAY_GREEN : CLAY_RED, "%s", yes ? "Confirmed." : "Cancelled.");
}

static void cmd_select(const char *args, void *user_data) {
    (void)args;
    ClayApp *app = user_data;
    ClayChoice options[] = {
        {"Staging", "safe, resettable"},
        {"Production", "live traffic"},
        {"Cancel", NULL},
    };
    int index = clay_app_select(app, "Deploy to which environment?", options, 3, 0);
    clay_sayc(CLAY_CYAN, "Selected: %s", options[index].title);
}

static void cmd_choice(const char *args, void *user_data) {
    (void)args;
    ClayApp *app = user_data;
    ClayChoice options[] = {
        {"Commit changes", "git commit the staged edits"},
        {"Show diff", "preview without applying"},
        {"Discard changes", "revert all edits, no undo"},
    };

    char *custom = NULL;
    int index = clay_app_choice(app, "What next?", options, 3, 1, &custom);
    if (index >= 0) {
        clay_sayc(CLAY_CYAN, "You picked: %s", options[index].title);
    } else if (custom) {
        clay_sayc(CLAY_CYAN, "You typed: %s", custom);
        free(custom);
    }
}

static void cmd_below(const char *args, void *user_data) {
    (void)args;
    (void)user_data;
    static ClayBelowState states[] = {CLAY_BELOW_NONE, CLAY_BELOW_LOADING, CLAY_BELOW_FINISHED, CLAY_BELOW_IDLE};
    static int i = 0;
    static int tokens_on = 1;

    i = (i + 1) % 4;
    clay_below_set_state("status", states[i]);

    tokens_on = !tokens_on;
    clay_below_set_enabled("tokens", tokens_on);

    clay_sayc(CLAY_CYAN, "status state cycled, tokens module %s", tokens_on ? "enabled" : "disabled");
}

typedef struct {
    const char *id;
    const char *label;
    const char *default_base_url; /* NULL: ask the user for it */
} ClayProviderType;

static const ClayProviderType PROVIDER_TYPES[] = {
    {"openai", "OpenAI", "https://api.openai.com/v1"},
    {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1"},
    {"custom", "OpenAI Custom", NULL},
};
#define PROVIDER_TYPE_COUNT (sizeof(PROVIDER_TYPES) / sizeof(PROVIDER_TYPES[0]))

typedef struct {
    const ClayProviderType *type;
    ClayProviderConfig *config;
    ClayOpenAI *client;
    ClayArray models; /* char * */
    int models_fetched;
    int models_rc;
} ClayConnectedProvider;

static ClayArray g_connected_providers;
static int g_connected_providers_ready = 0;
static char *g_selected_provider = NULL;
static char *g_selected_model = NULL;

static void provider_models_free(ClayConnectedProvider *provider) {
    for (size_t i = 0; i < provider->models.count; i++) {
        free(*(char **)clay_array_get(&provider->models, i));
    }
    clay_array_free(&provider->models);
}

static void connected_provider_free(ClayConnectedProvider *provider) {
    clay_openai_destroy(provider->client);
    clay_config_free(provider->config);
    provider_models_free(provider);
}

static ClayConnectedProvider *find_connected_provider(const char *id) {
    for (size_t i = 0; i < g_connected_providers.count; i++) {
        ClayConnectedProvider *provider = clay_array_get(&g_connected_providers, i);
        if (strcmp(provider->type->id, id) == 0) return provider;
    }
    return NULL;
}

static void connected_provider_load(const ClayProviderType *type) {
    ClayProviderConfig *config = clay_config_load(type->id);
    if (!config) return;

    ClayConnectedProvider *existing = find_connected_provider(type->id);
    if (existing) {
        clay_openai_destroy(existing->client);
        clay_config_free(existing->config);
        provider_models_free(existing);
        existing->config = config;
        existing->client = clay_openai_create(config->base_url, config->apikey, NULL);
        clay_array_init(&existing->models, sizeof(char *));
        existing->models_fetched = 0;
        existing->models_rc = 0;
        return;
    }

    ClayConnectedProvider provider;
    provider.type = type;
    provider.config = config;
    provider.client = clay_openai_create(config->base_url, config->apikey, NULL);
    clay_array_init(&provider.models, sizeof(char *));
    provider.models_fetched = 0;
    provider.models_rc = 0;
    clay_array_push_val(&g_connected_providers, &provider);
}

static void connected_providers_init(void) {
    if (g_connected_providers_ready) return;
    clay_array_init(&g_connected_providers, sizeof(ClayConnectedProvider));
    g_connected_providers_ready = 1;
    for (size_t i = 0; i < PROVIDER_TYPE_COUNT; i++) connected_provider_load(&PROVIDER_TYPES[i]);
}

static void connected_providers_free(void) {
    if (!g_connected_providers_ready) return;
    for (size_t i = 0; i < g_connected_providers.count; i++) {
        connected_provider_free(clay_array_get(&g_connected_providers, i));
    }
    clay_array_free(&g_connected_providers);
    g_connected_providers_ready = 0;
}

static void update_selected_below(void) {
    ClayStr text;
    clay_str_init(&text);
    if (g_selected_model && g_selected_provider) {
        clay_str_printf(&text, "%s%s%s %s(%s)%s", clay_color(CLAY_CORAL), g_selected_model,
                        clay_color(CLAY_RESET), clay_color(CLAY_GRAY), g_selected_provider, clay_color(CLAY_RESET));
    } else {
        clay_str_push(&text, "None");
    }
    clay_below_set_text("model", text.data);
    clay_str_free(&text);
}

static void set_tokens_below(long input_tokens, long output_tokens) {
    ClayStr text;
    clay_str_init(&text);
    clay_str_printf(&text, "%s\xe2\x86\x91 %ld  \xe2\x86\x93 %ld%s", clay_color(CLAY_CYAN), input_tokens,
                    output_tokens, clay_color(CLAY_RESET));
    clay_below_set_text("tokens", text.data);
    clay_str_free(&text);
}

static void update_tokens_below(void) {
    set_tokens_below(g_input_tokens, g_output_tokens);
}

static int select_model(const char *provider, const char *model) {
    int changed = !g_selected_provider || !g_selected_model || strcmp(g_selected_provider, provider) != 0 ||
                  strcmp(g_selected_model, model) != 0;
    char *provider_copy = strdup(provider);
    char *model_copy = strdup(model);
    free(g_selected_provider);
    free(g_selected_model);
    g_selected_provider = provider_copy;
    g_selected_model = model_copy;
    if (changed) {
        conversation_reset();
        g_input_tokens = 0;
        g_output_tokens = 0;
        update_tokens_below();
    }
    update_selected_below();
    return clay_config_selection_save(g_selected_provider, g_selected_model);
}

static void conversation_reset(void) {
    clay_json_free(g_conversation);
    g_conversation = clay_json_array();
    clay_json_array_push(g_conversation, clay_openai_message("system", CLAY_SYSTEM_PROMPT));
}

static ClayJson *shell_exec_tool(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    const char *command = clay_json_string_value(clay_json_object_get(arguments, "command"));
    const char *args = clay_json_string_value(clay_json_object_get(arguments, "args"));

    ClayJson *result = clay_json_object();
    if (!*command) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("command is required"));
        return result;
    }

    ClayStr invocation;
    clay_str_init(&invocation);
    clay_str_push(&invocation, command);
    if (*args) clay_str_printf(&invocation, " %s", args);

    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;
    int output_truncated = 0;
    int rc = clay_term_shell_exec(invocation.data, &output, CLAY_SHELL_OUTPUT_LIMIT, &exit_code, &output_truncated);

    clay_json_object_set(result, "command", clay_json_string(invocation.data));
    clay_json_object_set(result, "ok", clay_json_bool(rc == 0 && exit_code == 0));
    clay_json_object_set(result, "exit_code", clay_json_number(exit_code));
    clay_json_object_set(result, "output", clay_json_string(output.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(output_truncated));
    if (rc != 0) clay_json_object_set(result, "error", clay_json_string("failed to start command"));

    clay_str_free(&output);
    clay_str_free(&invocation);
    return result;
}

static ClayJson *shell_exec_schema(void) {
    ClayJson *command = clay_json_object();
    clay_json_object_set(command, "type", clay_json_string("string"));
    clay_json_object_set(command, "description", clay_json_string("Program or shell command to run."));

    ClayJson *args = clay_json_object();
    clay_json_object_set(args, "type", clay_json_string("string"));
    clay_json_object_set(args, "description", clay_json_string("Optional command arguments, including shell quoting."));

    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "command", command);
    clay_json_object_set(properties, "args", args);

    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("command"));

    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

static int fetch_connected_models(void *ctx, ClayArray *out) {
    ClayConnectedProvider *provider = ctx;
    if (!provider->models_fetched) {
        ClayTask *task = clay_task_start("Retrieving %s models", provider->type->label);
        provider->models_rc = clay_openai_list_models(provider->client, &provider->models);
        provider->models_fetched = 1;
        if (provider->models_rc == 0) {
            clay_task_success(task, "%zu models available", provider->models.count);
        } else {
            clay_task_fail(task, "Could not retrieve models");
        }
    }

    for (size_t i = 0; i < provider->models.count; i++) {
        ClayModelItem item = {*(char **)clay_array_get(&provider->models, i), NULL};
        clay_array_push_val(out, &item);
    }
    return provider->models_rc;
}

static const ClayProviderType *find_provider_type(const char *id) {
    for (size_t i = 0; i < PROVIDER_TYPE_COUNT; i++) {
        if (strcmp(PROVIDER_TYPES[i].id, id) == 0) return &PROVIDER_TYPES[i];
    }
    return NULL;
}

static void connect_provider_type(ClayApp *app, const ClayProviderType *type) {
    char *base_url;
    if (type->default_base_url) {
        base_url = strdup(type->default_base_url);
        clay_app_say(app, "Base URL: %s", base_url);
    } else {
        clay_app_say(app, "Base URL for %s:", type->label);
        base_url = clay_prompt_line();
    }
    if (!base_url || !*base_url) {
        clay_sayc(CLAY_RED, "Cancelled.");
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

    if (ok) connected_provider_load(type);

    free(config.id);
    free(apikey);
    free(base_url);
}

static void cmd_connect(const char *args, void *user_data) {
    ClayApp *app = user_data;

    if (args && *args) {
        const ClayProviderType *type = find_provider_type(args);
        if (!type) {
            clay_sayc(CLAY_RED, "Unknown provider type: %s", args);
            return;
        }
        connect_provider_type(app, type);
        return;
    }

    ClayChoice choices[PROVIDER_TYPE_COUNT];
    ClayStr titles[PROVIDER_TYPE_COUNT];
    for (size_t i = 0; i < PROVIDER_TYPE_COUNT; i++) {
        clay_str_init(&titles[i]);
        clay_str_push(&titles[i], PROVIDER_TYPES[i].label);
        if (clay_config_exists(PROVIDER_TYPES[i].id)) {
            clay_str_printf(&titles[i], " %s%s%s", clay_color(CLAY_GREEN), CLAY_ICON_CHECK, clay_color(CLAY_RESET));
        }
        choices[i].title = titles[i].data;
        choices[i].desc = NULL;
    }

    int index = clay_app_choice(app, "Connect a provider:", choices, (int)PROVIDER_TYPE_COUNT, 0, NULL);
    for (size_t i = 0; i < PROVIDER_TYPE_COUNT; i++) clay_str_free(&titles[i]);

    if (index < 0) {
        clay_sayc(CLAY_RED, "Cancelled.");
        return;
    }
    connect_provider_type(app, &PROVIDER_TYPES[index]);
}

static void cmd_model(const char *args, void *user_data) {
    (void)user_data;
    connected_providers_init();

    if (g_connected_providers.count == 0) {
        clay_sayc(CLAY_RED, "No provider connected. Connect one with /connect first.");
        return;
    }

    if (args && *args) {
        if (!g_selected_provider || !find_connected_provider(g_selected_provider)) {
            clay_sayc(CLAY_RED, "Select a provider with /model before setting a model directly.");
            return;
        }
        int saved = select_model(g_selected_provider, args) == 0;
        clay_sayc(saved ? CLAY_GREEN : CLAY_RED,
                  saved ? "Model set to %s via %s." : "Model set, but failed to save config.",
                  args, g_selected_provider);
        return;
    }

    ClayModelProvider providers[g_connected_providers.count];
    int default_provider = 0;
    for (size_t i = 0; i < g_connected_providers.count; i++) {
        ClayConnectedProvider *provider = clay_array_get(&g_connected_providers, i);
        providers[i].id = provider->type->id;
        providers[i].label = provider->type->label;
        providers[i].fetch = fetch_connected_models;
        providers[i].ctx = provider;
        if (g_selected_provider && strcmp(g_selected_provider, provider->type->id) == 0) {
            default_provider = (int)i;
        }
    }

    ClayModelSelection sel = clay_model_select(providers, (int)g_connected_providers.count, default_provider);
    if (!sel.ok) {
        clay_sayc(CLAY_RED, "Model selection cancelled.");
        return;
    }

    int saved = select_model(sel.provider, sel.model) == 0;
    clay_sayc(saved ? CLAY_GREEN : CLAY_RED,
              saved ? "Model set to %s via %s." : "Model set, but failed to save config.", sel.model, sel.provider);
    clay_model_selection_free(&sel);
}

typedef struct {
    int status_visible;
    int started;
    int response_active;
    int output_col;
    long error_status;
    long input_tokens;
    long output_tokens;
    int has_usage;
    ClayTask *tool_task;
} ClayConversationStream;

static void hide_conversation_status(ClayConversationStream *stream) {
    if (!stream->status_visible) return;
    clay_below_set_editing(0);
    clay_below_finish();
    stream->status_visible = 0;
}

static void set_conversation_status(double seconds, int success) {
    ClayStr text;
    clay_str_init(&text);
    clay_str_printf(&text, "%s%.1fs%s", clay_color(success ? CLAY_GREEN : CLAY_RED), seconds,
                    clay_color(CLAY_RESET));
    clay_below_set_text("status", text.data);
    clay_below_stop_elapsed("status");
    clay_below_set_state("status", success ? CLAY_BELOW_FINISHED : CLAY_BELOW_NONE);
    clay_below_set_enabled("status", 1);
    clay_str_free(&text);
}

static void show_conversation_thinking(ClayConversationStream *stream) {
    clay_below_set_text("status", "");
    clay_below_set_state("status", CLAY_BELOW_LOADING);
    clay_below_set_enabled("status", 1);
    clay_below_start_elapsed("status");
    if (clay_term_is_interactive()) {
        clay_below_set_editing(1);
        clay_below_render_status();
        stream->status_visible = 1;
    }
}

static void close_response_for_tool(ClayConversationStream *stream) {
    clay_below_set_editing(0);
    if (stream->response_active) {
        clay_response_end();
        if (stream->status_visible) {
            clay_below_status_finish_output();
        } else {
            fputc('\n', stdout);
        }
        stream->response_active = 0;
        stream->status_visible = 0;
    } else if (stream->status_visible) {
        clay_below_finish();
        stream->status_visible = 0;
    }
}

static void append_tool_label_text(ClayStr *out, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == 0x1b) {
            clay_str_push(out, "\\x1b");
        } else if (*p < 0x20) {
            clay_str_push_char(out, '?');
        } else {
            clay_str_push_char(out, (char)*p);
        }
    }
}

static void print_tool_output(const ClayJson *result) {
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    int truncated = clay_json_bool_value(clay_json_object_get(result, "output_truncated"));
    if (!*output) {
        clay_list_bullet("(no output)");
        return;
    }

    ClayStr line;
    clay_str_init(&line);
    int shown = 0;
    int omitted = 0;
    for (const unsigned char *p = (const unsigned char *)output;; p++) {
        if (*p == '\n' || *p == '\0') {
            if (line.len > 0 && shown < CLAY_TOOL_VISIBLE_LINES) {
                clay_list_bullet("%s", line.data);
                shown++;
            } else if (line.len > 0) {
                omitted = 1;
            }
            clay_str_clear(&line);
            if (*p == '\0') break;
            continue;
        }
        if (*p == '\r') continue;
        if (*p == 0x1b) {
            clay_str_push(&line, "\\\\x1b");
        } else if (*p < 0x20) {
            clay_str_push_char(&line, '?');
        } else {
            clay_str_push_char(&line, (char)*p);
        }
    }
    if (omitted || truncated) clay_list_bullet("%s…%s", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    clay_str_free(&line);
}

static void on_conversation_tool_call(const char *name, const char *arguments_json, void *userdata) {
    ClayConversationStream *stream = userdata;
    close_response_for_tool(stream);

    ClayJson *arguments = clay_json_parse(arguments_json, NULL);
    const char *command = clay_json_string_value(clay_json_object_get(arguments, "command"));
    const char *args = clay_json_string_value(clay_json_object_get(arguments, "args"));
    ClayStr label;
    clay_str_init(&label);
    append_tool_label_text(&label, name);
    if (*command) {
        clay_str_push(&label, ": ");
        append_tool_label_text(&label, command);
    }
    if (*args) {
        clay_str_push_char(&label, ' ');
        append_tool_label_text(&label, args);
    }
    stream->tool_task = clay_task_start("Running %s", label.data);
    clay_str_free(&label);
    clay_json_free(arguments);
}

static void on_conversation_tool_result(const char *name, const ClayJson *result, void *userdata) {
    ClayConversationStream *stream = userdata;
    int ok = clay_json_bool_value(clay_json_object_get(result, "ok"));
    long exit_code = (long)clay_json_number_value(clay_json_object_get(result, "exit_code"));

    if (stream->tool_task) {
        if (ok) clay_task_success(stream->tool_task, "exit %ld", exit_code);
        else clay_task_fail(stream->tool_task, "exit %ld", exit_code);
        stream->tool_task = NULL;
    } else {
        clay_sayc(ok ? CLAY_GREEN : CLAY_RED, "%s finished with exit %ld.", name, exit_code);
    }
    print_tool_output(result);
    show_conversation_thinking(stream);
}

static void on_conversation_token(const char *text, void *userdata) {
    if (!text || !*text) return;
    ClayConversationStream *stream = userdata;
    if (!stream->response_active) {
        if (stream->status_visible) {
            clay_below_set_editing(0);
            clay_below_stop_elapsed("status");
            clay_below_set_enabled("status", 0);
            clay_below_render_status();
            clay_below_status_insert_above();
        }
        clay_response_begin();
        stream->output_col = clay_response_prefix_width();
        stream->response_active = 1;
        stream->started = 1;
    }

    ClayStr pending;
    clay_str_init(&pending);
    int width = clay_term_width();
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        if (*p == '\n') {
            clay_response_write(pending.data);
            clay_str_clear(&pending);
            if (stream->status_visible) clay_below_status_push_down();
            clay_response_write("\n");
            stream->output_col = 0;
            p++;
            continue;
        }

        size_t char_len = 1;
        if ((*p & 0xE0) == 0xC0) char_len = 2;
        else if ((*p & 0xF0) == 0xE0) char_len = 3;
        else if ((*p & 0xF8) == 0xF0) char_len = 4;

        if (stream->status_visible && stream->output_col + 1 >= width) {
            clay_response_write(pending.data);
            clay_str_clear(&pending);
            clay_below_status_push_down();
            stream->output_col = 0;
        }
        clay_str_push_n(&pending, (const char *)p, char_len);
        stream->output_col++;
        p += char_len;
    }
    clay_response_write(pending.data);
    clay_str_free(&pending);
}

static void on_conversation_error(long status, const char *body, void *userdata) {
    (void)body;
    ClayConversationStream *stream = userdata;
    stream->error_status = status;
}

static void on_conversation_usage(long input_tokens, long output_tokens, void *userdata) {
    ClayConversationStream *stream = userdata;
    stream->input_tokens += input_tokens;
    stream->output_tokens += output_tokens;
    stream->has_usage = 1;
}

static int run_conversation(ClayApp *app, const char *input) {
    if (!g_selected_provider || !g_selected_model) {
        clay_sayc(CLAY_RED, "Select a provider and model with /model before sending a message.");
        return 0;
    }

    ClayConnectedProvider *provider = find_connected_provider(g_selected_provider);
    if (!provider) {
        clay_sayc(CLAY_RED, "Selected provider %s is not connected.", g_selected_provider);
        return 0;
    }
    if (!g_conversation) conversation_reset();

    ClayJson *messages = clay_json_clone(g_conversation);
    clay_json_array_push(messages, clay_openai_message("user", input));

    ClayOpenAI *client = clay_openai_create(provider->config->base_url, provider->config->apikey, g_selected_model);
    ClayConversationStream stream = {0};
    ClayOpenAICallbacks callbacks = {0};
    callbacks.on_token = on_conversation_token;
    callbacks.on_tool_call = on_conversation_tool_call;
    callbacks.on_tool_result = on_conversation_tool_result;
    callbacks.on_usage = on_conversation_usage;
    callbacks.on_error = on_conversation_error;
    callbacks.userdata = &stream;
    g_messages_sent++;

    struct timespec started_at;
    clock_gettime(CLOCK_MONOTONIC, &started_at);
    show_conversation_thinking(&stream);
    clay_app_set_state(app, CLAY_APP_BUSY);
    ClayJson *tool_schema = shell_exec_schema();
    ClayTool tools[] = {
        {"shell_exec", "Runs a shell command in the current workspace and returns stdout, stderr, and exit status.",
         tool_schema, shell_exec_tool, NULL},
    };
    int rc = clay_openai_run(client, messages, tools, sizeof(tools) / sizeof(tools[0]), 8, &callbacks);
    clay_json_free(tool_schema);
    clay_openai_destroy(client);

    struct timespec finished_at;
    clock_gettime(CLOCK_MONOTONIC, &finished_at);
    double seconds = (double)(finished_at.tv_sec - started_at.tv_sec) +
                     (double)(finished_at.tv_nsec - started_at.tv_nsec) / 1e9;

    if (stream.response_active) clay_response_end();
    if (rc == 0) {
        clay_json_free(g_conversation);
        g_conversation = messages;
        set_conversation_status(seconds, 1);
        if (stream.has_usage) {
            g_input_tokens = stream.input_tokens;
            g_output_tokens = stream.output_tokens;
            g_total_input_tokens += stream.input_tokens;
            g_total_output_tokens += stream.output_tokens;
            update_tokens_below();
        }
        if (stream.started && stream.status_visible) {
            clay_below_status_refresh_below();
            clay_below_status_prepare_prompt();
            stream.status_visible = 0;
            clay_app_set_state(app, CLAY_APP_IDLE);
            return 1;
        }
        hide_conversation_status(&stream);
    } else {
        clay_json_free(messages);
        set_conversation_status(seconds, 0);
        if (stream.started && stream.status_visible) {
            clay_below_status_refresh_below();
            clay_below_status_prepare_prompt();
            stream.status_visible = 0;
            clay_app_set_state(app, CLAY_APP_IDLE);
            return 1;
        }
        hide_conversation_status(&stream);
        if (stream.error_status > 0) {
            clay_sayc(CLAY_RED, "Provider request failed (HTTP %ld).", stream.error_status);
        } else {
            clay_sayc(CLAY_RED, "Provider request failed.");
        }
    }
    clay_app_set_state(app, CLAY_APP_IDLE);
    return 0;
}

static void cmd_mm(const char *args, void *user_data) {
    (void)args;
    (void)user_data;

    ClayStr str;
    clay_str_init(&str);
    clay_str_push(&str, "clay ");
    clay_str_printf(&str, "v%s ", CLAY_VERSION);
    clay_str_push(&str, "mm smoke test");
    clay_sayc(CLAY_CYAN, "ClayStr -> %s", str.data);
    clay_str_free(&str);

    ClayArray numbers;
    clay_array_init(&numbers, sizeof(int));
    for (int i = 0; i < 5; i++) {
        int value = i * i;
        clay_array_push_val(&numbers, &value);
    }
    printf("         %sClayArray ->%s ", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    for (size_t i = 0; i < numbers.count; i++) {
        printf("%d ", *(int *)clay_array_get(&numbers, i));
    }
    fputc('\n', stdout);
    clay_array_free(&numbers);

    ClayMap *map = clay_map_create();
    clay_map_set(map, "orange", "accent");
    clay_map_set(map, "gray", "muted");
    clay_sayc(CLAY_CYAN, "ClayMap -> orange=%s gray=%s",
              (char *)clay_map_get(map, "orange"), (char *)clay_map_get(map, "gray"));
    clay_map_destroy(map);
}

/* Exercises task spinners, a plan list, and a summary line. */
static void run_demo_turn(ClayApp *app) {
    clay_below_set_text("status", "");
    clay_below_set_state("status", CLAY_BELOW_LOADING);
    clay_below_set_enabled("status", 1);
    clay_below_start_elapsed("status");

    ClayTask *scan = clay_app_task_start(app, "Scanning project");
    clay_term_sleep_ms(600);
    clay_app_task_success(app, scan, "42 files indexed");
    set_tokens_below(128, 0);

    clay_app_list_header(app, "Plan:");
    clay_app_list_step(app, 1, "Add", "src/middleware/rateLimit.ts", "new, token-bucket", 1);
    clay_app_list_step(app, 2, "Wire into", "src/routes/auth.ts", "login, signup, refresh", 1);
    clay_app_list_step(app, 3, "Add tests in", "tests/rateLimit.test.ts", NULL, 1);
    clay_app_list_step(app, 4, "Run", "test suite", NULL, 0);
    fputc('\n', stdout);

    ClayTask *write = clay_app_task_start(app, "Writing files");
    clay_term_sleep_ms(500);
    clay_app_task_success(app, write, "3 files changed");

    ClayTask *tests = clay_app_task_start(app, "Running %snpm test%s", clay_color(CLAY_CYAN), clay_color(CLAY_GRAY));
    clay_term_sleep_ms(1200);
    clay_app_task_success(app, tests, "87 passed, 0 failed");
    clay_below_stop_elapsed("status");
    clay_below_set_text("status", "2.3s");
    set_tokens_below(128, 340);
    clay_below_set_state("status", CLAY_BELOW_FINISHED);

    fputc('\n', stdout);
    clay_app_say(app, "Done. Commit with %sclay commit%s or review with %sclay diff%s.",
                 clay_color(CLAY_CYAN), clay_color(CLAY_GRAY), clay_color(CLAY_CYAN), clay_color(CLAY_GRAY));
}

static void cmd_demo(const char *args, void *user_data) {
    (void)args;
    run_demo_turn(user_data);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-color") == 0) {
            clay_term_set_color_enabled(0);
        }
    }

    clay_term_init();
    if (clay_http_init() != 0) {
        fprintf(stderr, "Failed to initialize HTTP.\n");
        return 1;
    }
    clay_banner("clay", CLAY_VERSION, "your AI code agent");

    connected_providers_init();
    clay_config_selection_load(&g_selected_provider, &g_selected_model);
    conversation_reset();

    ClayApp *app = clay_app_create();
    ClayCommandRegistry *commands = clay_app_commands(app);
    clay_command_register(commands, "help", "Show available commands", cmd_help, app);
    clay_command_register(commands, "exit", "Quit clay", cmd_exit, app);
    clay_command_register(commands, "confirm", "Demo a yes/no prompt", cmd_confirm, app);
    clay_command_register(commands, "select", "Demo a multi-option select prompt", cmd_select, app);
    clay_command_register(commands, "choice", "Demo a navigable choice prompt", cmd_choice, app);
    clay_command_register(commands, "mm", "Smoke-test the mm module", cmd_mm, app);
    clay_command_register(commands, "below", "Cycle the below-prompt status modules", cmd_below, app);
    clay_command_register(commands, "model", "Pick a model from a connected provider", cmd_model, app);
    clay_command_register(commands, "connect", "Connect a provider, or /connect <id> directly", cmd_connect, app);
    clay_command_register(commands, "demo", "Run the render demo sequence", cmd_demo, app);

    clay_below_add(0, "status");
    clay_below_set_enabled("status", 0);

    clay_below_add(1, "model");

    clay_below_add(2, "tokens");
    update_tokens_below();

    update_selected_below();

    int interrupted = 0;
    while (g_running) {
        char *line = clay_prompt_line();
        if (!line) {
            interrupted = clay_prompt_was_interrupted() || clay_term_take_interrupt();
            break;
        }

        ClayInput input = clay_input_parse(line);
        free(line);

        int prompt_ready = 0;
        switch (input.kind) {
        case CLAY_INPUT_EMPTY:
            break;
        case CLAY_INPUT_COMMAND:
            if (!clay_command_dispatch(commands, &input)) {
                clay_sayc(CLAY_RED, "Unknown command: /%s (try /help)", input.command);
            }
            break;
        case CLAY_INPUT_MESSAGE:
            prompt_ready = run_conversation(app, input.raw);
            break;
        }

        clay_input_free(&input);
        if (clay_term_take_interrupt()) {
            interrupted = 1;
            break;
        }
        if (!prompt_ready) fputc('\n', stdout);
    }

    if (interrupted) print_session_summary();
    clay_app_destroy(app);
    connected_providers_free();
    clay_json_free(g_conversation);
    free(g_selected_provider);
    free(g_selected_model);
    clay_http_cleanup();
    if (!interrupted) printf("%sGoodbye.%s\n", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    return 0;
}
