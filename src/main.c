#include "clay/clay.h"

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

static int fetch_anthropic(void *ctx, ClayModelItem *out, int max) {
    (void)ctx;
    static const ClayModelItem items[] = {
        {"claude-opus-5", "most capable"},
        {"claude-sonnet-5", "balanced"},
        {"claude-haiku-4-5", "fastest"},
        {"claude-opus-4-5", "previous flagship"},
        {"claude-sonnet-4-5", "previous balanced"},
        {"claude-haiku-4", "previous fast"},
        {"claude-opus-4-1", "legacy"},
        {"claude-sonnet-3-7", "legacy"},
    };
    int n = (int)(sizeof(items) / sizeof(items[0]));
    if (n > max) n = max;
    memcpy(out, items, (size_t)n * sizeof(ClayModelItem));
    return n;
}

static int fetch_openai(void *ctx, ClayModelItem *out, int max) {
    (void)ctx;
    static const ClayModelItem items[] = {
        {"gpt-5", "flagship"},
        {"gpt-5-mini", "cheaper, faster"},
        {"gpt-5-nano", "smallest"},
        {"gpt-4.1", "previous gen"},
        {"gpt-4.1-mini", "previous gen, cheaper"},
        {"o3", "reasoning"},
        {"o3-mini", "reasoning, cheaper"},
        {"o1", "legacy reasoning"},
    };
    int n = (int)(sizeof(items) / sizeof(items[0]));
    if (n > max) n = max;
    memcpy(out, items, (size_t)n * sizeof(ClayModelItem));
    return n;
}

static int fetch_google(void *ctx, ClayModelItem *out, int max) {
    (void)ctx;
    static const ClayModelItem items[] = {
        {"gemini-2.5-pro", "most capable"},
        {"gemini-2.5-flash", "fast"},
        {"gemini-2.5-flash-lite", "cheapest"},
        {"gemini-2.0-flash", "previous gen"},
        {"gemini-1.5-pro", "legacy"},
    };
    int n = (int)(sizeof(items) / sizeof(items[0]));
    if (n > max) n = max;
    memcpy(out, items, (size_t)n * sizeof(ClayModelItem));
    return n;
}

static void cmd_model(const char *args, void *user_data) {
    (void)user_data;

    if (args && *args) {
        ClayStr buf;
        clay_str_init(&buf);
        clay_str_printf(&buf, "Model: %s", args);
        clay_below_set_text("model", buf.data);
        clay_str_free(&buf);
        clay_sayc(CLAY_GREEN, "Model set to %s", args);
        return;
    }

    ClayModelProvider providers[] = {
        {"anthropic", fetch_anthropic, NULL},
        {"openai", fetch_openai, NULL},
        {"google", fetch_google, NULL},
    };

    ClayModelSelection sel = clay_model_select(providers, 3, 0);
    if (!sel.ok) {
        clay_sayc(CLAY_RED, "Model selection cancelled.");
        return;
    }

    ClayStr buf;
    clay_str_init(&buf);
    clay_str_printf(&buf, "Model: %s (%s)", sel.model, sel.provider);
    clay_below_set_text("model", buf.data);
    clay_sayc(CLAY_GREEN, "Model set to %s via %s", sel.model, sel.provider);
    clay_str_free(&buf);
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
    clay_banner("clay", CLAY_VERSION, "your AI code agent");

    ClayApp *app = clay_app_create();
    ClayCommandRegistry *commands = clay_app_commands(app);
    clay_command_register(commands, "help", "Show available commands", cmd_help, app);
    clay_command_register(commands, "exit", "Quit clay", cmd_exit, app);
    clay_command_register(commands, "confirm", "Demo a yes/no prompt", cmd_confirm, app);
    clay_command_register(commands, "select", "Demo a multi-option select prompt", cmd_select, app);
    clay_command_register(commands, "choice", "Demo a navigable choice prompt", cmd_choice, app);
    clay_command_register(commands, "mm", "Smoke-test the mm module", cmd_mm, app);
    clay_command_register(commands, "below", "Cycle the below-prompt status modules", cmd_below, app);
    clay_command_register(commands, "model", "Pick a model/provider, or /model <id> directly", cmd_model, app);

    clay_below_add(0, "model");
    clay_below_set_text("model", "Model: claude-sonnet-5");

    clay_below_add(1, "tokens");
    clay_below_set_text("tokens", "Tokens: 0 in / 0 out");

    clay_below_add(2, "provider");
    clay_below_set_text("provider", "Provider: anthropic");
    clay_below_set_state("provider", CLAY_BELOW_IDLE);

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
    printf("%sGoodbye.%s\n", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    return 0;
}
