#include "clay/clay.h"
#include "clay/http.h"
#include "clay/providers/openai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_VERSION "0.0.0"

static int g_running = 1;

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
    clay_below_set_state("provider", states[i]);

    tokens_on = !tokens_on;
    clay_below_set_enabled("tokens", tokens_on);

    clay_sayc(CLAY_CYAN, "provider state cycled, tokens module %s", tokens_on ? "enabled" : "disabled");
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
    clay_str_printf(&text, "Model: %s", g_selected_model ? g_selected_model : "None");
    clay_below_set_text("model", text.data);
    clay_str_clear(&text);
    clay_str_printf(&text, "Provider: %s", g_selected_provider ? g_selected_provider : "None");
    clay_below_set_text("provider", text.data);
    clay_str_free(&text);
}

static int select_model(const char *provider, const char *model) {
    char *provider_copy = strdup(provider);
    char *model_copy = strdup(model);
    free(g_selected_provider);
    free(g_selected_model);
    g_selected_provider = provider_copy;
    g_selected_model = model_copy;
    update_selected_below();
    return clay_config_selection_save(g_selected_provider, g_selected_model);
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
    clay_below_set_state("provider", CLAY_BELOW_LOADING);

    ClayTask *scan = clay_app_task_start(app, "Scanning project");
    clay_term_sleep_ms(600);
    clay_app_task_success(app, scan, "42 files indexed");
    clay_below_set_text("tokens", "Tokens: 128 in / 0 out");

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
    clay_below_set_text("tokens", "Tokens: 128 in / 340 out");
    clay_below_set_state("provider", CLAY_BELOW_FINISHED);

    fputc('\n', stdout);
    clay_app_say(app, "Done. Commit with %sclay commit%s or review with %sclay diff%s.",
                 clay_color(CLAY_CYAN), clay_color(CLAY_GRAY), clay_color(CLAY_CYAN), clay_color(CLAY_GRAY));
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

    clay_below_add(0, "model");

    clay_below_add(1, "tokens");
    clay_below_set_text("tokens", "Tokens: 0 in / 0 out");

    clay_below_add(2, "provider");
    clay_below_set_state("provider", CLAY_BELOW_IDLE);
    update_selected_below();

    while (g_running) {
        char *line = clay_prompt_line();
        if (!line) break;

        ClayInput input = clay_input_parse(line);
        free(line);

        switch (input.kind) {
        case CLAY_INPUT_EMPTY:
            break;
        case CLAY_INPUT_COMMAND:
            if (!clay_command_dispatch(commands, &input)) {
                clay_sayc(CLAY_RED, "Unknown command: /%s (try /help)", input.command);
            }
            break;
        case CLAY_INPUT_MESSAGE:
            run_demo_turn(app);
            break;
        }

        clay_input_free(&input);
        fputc('\n', stdout);
    }

    clay_app_destroy(app);
    connected_providers_free();
    free(g_selected_provider);
    free(g_selected_model);
    clay_http_cleanup();
    printf("%sGoodbye.%s\n", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    return 0;
}
