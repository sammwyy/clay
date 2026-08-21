#include "context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_SYSTEM_PROMPT_BASE                                                                                      \
    "You are clay, a helpful AI coding assistant. Be concise, accurate, and practical. "                             \
    "Explain code changes clearly and ask for clarification when the request is ambiguous. "                         \
    "Use read/write/edit/glob/grep for inspecting or changing files in the workspace - they are scoped to "         \
    "it directly and are preferred over shell_exec's cat/sed/find/grep for that purpose. Reach for shell_exec "      \
    "for everything else (running builds/tests, git, other programs). It runs in /workspace (the project "           \
    "root); /scratch and /tmp are a temporary scratch area for this conversation. Depending on the user's "          \
    "/sandbox settings, paths outside those may be read-only or unavailable. Prefer focused commands and "           \
    "summarize results. The user can put you in Plan mode (/plan) to discuss an approach before any files "          \
    "change - write/edit are refused and mutating shell commands are blocked there; a blocked tool result "         \
    "means explain the plan in words instead of retrying the call.\n\n"                                              \
    "You have two kinds of memory. Long-term memory (memory_save/memory_read) persists across every future "         \
    "chat: call memory_save after completing significant work - a decision, a bug fix, a preference the user "       \
    "stated - so later sessions have it; the index of existing entries is below, and memory_read loads one by "      \
    "its slug. Short-term memory (remember) is a scratchpad for this chat only, replayed every turn even if "        \
    "earlier messages are later dropped - use it to pin details you'll still need many turns from now.\n\n"          \
    "For any task with multiple steps, call todowrite with the full plan before starting, and again whenever a "     \
    "step's status changes - it's shown to the user as a checklist. Keep exactly one task in_progress at a time; "   \
    "mark it completed before moving to the next. Skip it for a single quick action.\n\n"                            \
    "Tools named mcp__<server>__<tool> come from MCP servers the user configured with /mcp - use them like any "     \
    "other tool.\n\n"                                                                                                \
    "If the user set up /autotest, a write/edit result carrying auto_test_failed means the configured command "     \
    "failed after that change - read auto_test_output and fix it before moving on."

/* Sliding window: reused as-is (and its clock reset) while a chat-less
   session starts within this long of the cache's last use, since the
   provider's own prefix cache is likely still warm; rebuilt with fresh
   memory/environment data once it's likely gone cold anyway. Never
   applies to an existing chat - see clay_chat_system_prompt. */
#define CLAY_SYSTEM_PROMPT_TTL_SECONDS 600

static const ClayProviderType PROVIDER_TYPES[] = {
    {"openai", "OpenAI", "https://api.openai.com/v1"},
    {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1"},
    {"custom", "OpenAI Custom", NULL},
};

static const ClayReasoningEffort REASONING_EFFORTS[] = {
    {NULL, "Default", "Let the selected model use its default reasoning level."},
    {"minimal", "Minimal", "Use the fewest reasoning tokens when supported."},
    {"low", "Low", "Favor lower latency and token use when supported."},
    {"medium", "Medium", "Balance reasoning depth, latency, and token use."},
    {"high", "High", "Favor deeper reasoning when supported."},
    {"xhigh", "XHigh", "Use the highest reasoning level when supported."},
};

static void provider_models_free(ClayConnectedProvider *provider) {
    for (size_t i = 0; i < provider->models.count; i++) free(*(char **)clay_array_get(&provider->models, i));
    clay_array_free(&provider->models);
}

static void provider_free(ClayConnectedProvider *provider) {
    clay_openai_destroy(provider->client);
    clay_config_free(provider->config);
    provider_models_free(provider);
}

ClayConnectedProvider *clay_commands_find_provider(ClayCommands *commands, const char *id) {
    for (size_t i = 0; i < commands->providers.count; i++) {
        ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
        if (strcmp(provider->type->id, id) == 0) return provider;
    }
    return NULL;
}

const ClayProviderType *clay_commands_find_provider_type(const char *id) {
    for (size_t i = 0; i < sizeof(PROVIDER_TYPES) / sizeof(PROVIDER_TYPES[0]); i++) {
        if (strcmp(PROVIDER_TYPES[i].id, id) == 0) return &PROVIDER_TYPES[i];
    }
    return NULL;
}

const ClayProviderType *clay_commands_provider_types(size_t *count) {
    *count = sizeof(PROVIDER_TYPES) / sizeof(PROVIDER_TYPES[0]);
    return PROVIDER_TYPES;
}

void clay_commands_load_provider(ClayCommands *commands, const ClayProviderType *type) {
    ClayProviderConfig *config = clay_config_load(type->id);
    if (!config) return;

    ClayConnectedProvider *existing = clay_commands_find_provider(commands, type->id);
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

    ClayConnectedProvider provider = {0};
    provider.type = type;
    provider.config = config;
    provider.client = clay_openai_create(config->base_url, config->apikey, NULL);
    clay_array_init(&provider.models, sizeof(char *));
    clay_array_push_val(&commands->providers, &provider);
}

void clay_commands_update_selected_below(ClayCommands *commands) {
    ClayStr text;
    clay_str_init(&text);
    if (commands->selected_model && commands->selected_provider) {
        const ClayReasoningEffort *effort = clay_commands_reasoning_effort(commands);
        clay_str_printf(&text, "%s%s%s %s(%s)%s %s[%s%s%s]%s", clay_color(CLAY_CORAL), commands->selected_model,
                        clay_color(CLAY_RESET), clay_color(CLAY_GRAY), commands->selected_provider, clay_color(CLAY_RESET),
                        clay_color(CLAY_GRAY), clay_color(CLAY_CYAN), effort->label, clay_color(CLAY_GRAY),
                        clay_color(CLAY_RESET));
    } else {
        clay_str_push(&text, "None");
    }
    clay_below_set_text("model", text.data);
    clay_str_free(&text);
}

void clay_commands_set_tokens_below(ClayCommands *commands, long input_tokens, long output_tokens) {
    (void)commands;
    ClayStr text;
    clay_str_init(&text);
    clay_str_printf(&text, "%s\xe2\x86\x91 %ld  \xe2\x86\x93 %ld%s", clay_color(CLAY_CYAN), input_tokens,
                    output_tokens, clay_color(CLAY_RESET));
    clay_below_set_text("tokens", text.data);
    clay_str_free(&text);
}

static char *system_prompt_cache_path(void) {
    char *home = clay_term_home_dir();
    if (!home) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/system_prompt.json", home);
    free(home);
    return path.data;
}

/* 0 with text_out/last_used_out/cwd_out set on a hit, -1 on a miss. */
static int load_system_prompt_cache(char **text_out, long long *last_used_out, char **cwd_out) {
    char *path = system_prompt_cache_path();
    if (!path) return -1;
    FILE *file = fopen(path, "r");
    free(path);
    if (!file) return -1;
    ClayStr body;
    clay_str_init(&body);
    int ch;
    while ((ch = fgetc(file)) != EOF) clay_str_push_char(&body, (char)ch);
    fclose(file);
    ClayJson *root = clay_json_parse(body.data, NULL);
    clay_str_free(&body);
    const char *text = clay_json_type(root) == CLAY_JSON_OBJECT ? clay_json_string_value(clay_json_object_get(root, "text")) : NULL;
    if (!text || !*text) {
        clay_json_free(root);
        return -1;
    }
    *text_out = strdup(text);
    *last_used_out = (long long)clay_json_number_value(clay_json_object_get(root, "last_used_at"));
    const char *cwd = clay_json_string_value(clay_json_object_get(root, "cwd"));
    *cwd_out = strdup(cwd ? cwd : "");
    clay_json_free(root);
    return 0;
}

static void save_system_prompt_cache(const char *text, long long last_used_at, const char *cwd) {
    char *home = clay_term_home_dir();
    if (!home) return;
    ClayStr dir;
    clay_str_init(&dir);
    clay_str_printf(&dir, "%s/.clay", home);
    free(home);
    clay_term_mkdir(dir.data);
    ClayJson *root = clay_json_object();
    clay_json_object_set(root, "text", clay_json_string(text));
    clay_json_object_set(root, "last_used_at", clay_json_number((double)last_used_at));
    clay_json_object_set(root, "cwd", clay_json_string(cwd));
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/system_prompt.json", dir.data);
    clay_str_free(&dir);
    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(root, &body);
    clay_json_free(root);
    FILE *file = fopen(path.data, "w");
    if (file) {
        fwrite(body.data, 1, body.len, file);
        fclose(file);
    }
    clay_str_free(&path);
    clay_str_free(&body);
}

#define CLAY_PROJECT_INSTRUCTIONS_MAX_DEPTH 32

static char *read_file_if_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    ClayStr text;
    clay_str_init(&text);
    int ch;
    while ((ch = fgetc(file)) != EOF) clay_str_push_char(&text, (char)ch);
    fclose(file);
    return text.data;
}

/* AGENTS.md takes priority over CLAY.md at the same level - only one is
   read per directory. */
static char *read_level_instructions(const char *dir) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/AGENTS.md", dir);
    char *content = read_file_if_exists(path.data);
    if (!content) {
        clay_str_clear(&path);
        clay_str_printf(&path, "%s/CLAY.md", dir);
        content = read_file_if_exists(path.data);
    }
    clay_str_free(&path);
    return content;
}

/* Walks up from the cwd to the repo root (a directory holding .git),
   reading AGENTS.md/CLAY.md at each level - same mechanism as Codex CLI.
   Concatenated root-first, so more specific instructions near the cwd
   follow more general ones. Malloc'd; NULL if none found. */
char *clay_commands_load_project_instructions(void) {
    char *dir = clay_term_cwd();
    if (!dir) return NULL;

    ClayArray levels;
    clay_array_init(&levels, sizeof(char *));
    for (int depth = 0; depth < CLAY_PROJECT_INSTRUCTIONS_MAX_DEPTH; depth++) {
        char *content = read_level_instructions(dir);
        if (content) clay_array_push_val(&levels, &content);

        ClayStr git_path;
        clay_str_init(&git_path);
        clay_str_printf(&git_path, "%s/.git", dir);
        int at_repo_root = clay_term_is_dir(git_path.data);
        clay_str_free(&git_path);
        if (at_repo_root || strcmp(dir, "/") == 0) break;

        char *slash = strrchr(dir, '/');
        if (!slash) break;
        if (slash == dir) slash[1] = '\0'; /* "/foo" -> "/" */
        else *slash = '\0';
    }
    free(dir);

    if (levels.count == 0) {
        clay_array_free(&levels);
        return NULL;
    }
    ClayStr combined;
    clay_str_init(&combined);
    for (size_t i = levels.count; i-- > 0;) {
        char *content = *(char **)clay_array_get(&levels, i);
        if (combined.len > 0) clay_str_push(&combined, "\n\n");
        clay_str_push(&combined, content);
        free(content);
    }
    clay_array_free(&levels);
    return combined.data;
}

static char *build_fresh_system_prompt(void) {
    ClayStr text;
    clay_str_init(&text);
    clay_str_push(&text, CLAY_SYSTEM_PROMPT_BASE);

    char *platform = clay_term_platform_name();
    char *date = clay_time_format_date(clay_time_now());
    clay_str_printf(&text, "\n\nEnvironment: %s. Today's date (UTC): %s.", platform, date);
    free(platform);
    free(date);

    char *index = clay_memory_index();
    if (*index) clay_str_printf(&text, "\n\nLong-term memory index:\n%s", index);
    free(index);

    char *project_instructions = clay_commands_load_project_instructions();
    if (project_instructions) {
        clay_str_printf(&text, "\n\nProject instructions (AGENTS.md/CLAY.md):\n%s", project_instructions);
        free(project_instructions);
    }

    return text.data;
}

/* Reuses the cached prompt (and slides its TTL forward) if it's recent
   enough that the provider's own prefix cache is plausibly still warm;
   otherwise rebuilds with current memory/environment data. Only ever
   called for a chat-less session - clay_commands_reset_conversation
   never calls this once commands->chat exists. */
static char *clay_commands_build_system_prompt(void) {
    char *cached_text = NULL;
    char *cached_cwd = NULL;
    long long last_used = 0;
    long long now = clay_time_now();
    char *cwd = clay_term_cwd();
    int hit = load_system_prompt_cache(&cached_text, &last_used, &cached_cwd) == 0 &&
              now - last_used < CLAY_SYSTEM_PROMPT_TTL_SECONDS && cwd && strcmp(cached_cwd, cwd) == 0;
    free(cached_cwd);
    if (hit) {
        save_system_prompt_cache(cached_text, now, cwd);
        free(cwd);
        return cached_text;
    }
    free(cached_text);
    char *fresh = build_fresh_system_prompt();
    save_system_prompt_cache(fresh, now, cwd ? cwd : "");
    free(cwd);
    return fresh;
}

void clay_commands_reset_conversation(ClayCommands *commands) {
    clay_json_free(commands->conversation);
    commands->conversation = clay_json_array();
    free(commands->system_prompt);
    const char *persisted = commands->chat ? clay_chat_system_prompt(commands->chat) : "";
    commands->system_prompt = *persisted ? strdup(persisted) : clay_commands_build_system_prompt();
    clay_json_array_push(commands->conversation, clay_openai_message("system", commands->system_prompt));
    if (!commands->chat) return;
    ClayJson *history = clay_chat_openai_messages(commands->chat);
    for (size_t i = 0; i < clay_json_array_count(history); i++) {
        clay_json_array_push(commands->conversation, clay_json_clone(clay_json_array_get(history, i)));
    }
    clay_json_free(history);
}

/* No per-model context window lookup exists yet, so this is a conservative
   fixed budget rather than something read off the selected model. */
#define CLAY_CONTEXT_TOKEN_BUDGET 128000
#define CLAY_CONTEXT_COMPACT_RATIO 0.9
#define CLAY_CONTEXT_KEEP_TURNS 6
#define CLAY_CONTEXT_TOOL_PREVIEW 200
#define CLAY_CONTEXT_COLLAPSED_MARKER "[collapsed - "

static int is_user_message(const ClayJson *message) {
    return strcmp(clay_json_string_value(clay_json_object_get(message, "role")), "user") == 0;
}

static int collapse_tool_content(ClayJson *message) {
    const char *text = clay_json_string_value(clay_json_object_get(message, "content"));
    if (!text) return 0;
    size_t len = strlen(text);
    if (len <= CLAY_CONTEXT_TOOL_PREVIEW || strncmp(text, CLAY_CONTEXT_COLLAPSED_MARKER,
                                                    strlen(CLAY_CONTEXT_COLLAPSED_MARKER)) == 0) {
        return 0;
    }
    ClayStr summary;
    clay_str_init(&summary);
    clay_str_printf(&summary, "%s%zu bytes, showing the first %d]\n%.*s...", CLAY_CONTEXT_COLLAPSED_MARKER, len,
                    CLAY_CONTEXT_TOOL_PREVIEW, CLAY_CONTEXT_TOOL_PREVIEW, text);
    clay_json_object_set(message, "content", clay_json_string(summary.data));
    clay_str_free(&summary);
    return 1;
}

int clay_commands_maybe_compact(ClayCommands *commands) {
    if (commands->input_tokens < (long)(CLAY_CONTEXT_TOKEN_BUDGET * CLAY_CONTEXT_COMPACT_RATIO)) return 0;
    size_t count = clay_json_array_count(commands->conversation);
    if (count < 2) return 0;

    /* Everything at index < cutoff is older than the last CLAY_CONTEXT_KEEP_TURNS
       user turns and is eligible for collapsing; index 0 (system prompt) never is. */
    size_t cutoff = 1;
    int turns_seen = 0;
    for (size_t i = count; i-- > 1;) {
        if (is_user_message(clay_json_array_get(commands->conversation, i))) {
            turns_seen++;
            if (turns_seen == CLAY_CONTEXT_KEEP_TURNS) {
                cutoff = i;
                break;
            }
        }
    }

    int collapsed = 0;
    for (size_t i = 1; i < cutoff; i++) {
        ClayJson *message = clay_json_array_get(commands->conversation, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(message, "role")), "tool") == 0) {
            collapsed += collapse_tool_content(message);
        }
    }
    return collapsed;
}

void clay_commands_clear_todos(ClayCommands *commands) {
    for (size_t i = 0; i < commands->todos.count; i++) {
        ClayTodoItem *item = clay_array_get(&commands->todos, i);
        free(item->content);
        free(item->status);
    }
    clay_array_clear(&commands->todos);
}

void clay_commands_new_chat(ClayCommands *commands) {
    clay_chat_destroy(commands->chat);
    commands->chat = NULL;
    clay_commands_reset_conversation(commands);
    commands->input_tokens = 0;
    commands->output_tokens = 0;
    clay_below_stop_elapsed("status");
    clay_below_set_enabled("status", 0);
    clay_commands_set_tokens_below(commands, 0, 0);
    clay_commands_clear_todos(commands);
}

int clay_commands_select_model(ClayCommands *commands, const char *provider, const char *model) {
    int changed = !commands->selected_provider || !commands->selected_model ||
                  strcmp(commands->selected_provider, provider) != 0 || strcmp(commands->selected_model, model) != 0;
    char *provider_copy = strdup(provider);
    char *model_copy = strdup(model);
    free(commands->selected_provider);
    free(commands->selected_model);
    commands->selected_provider = provider_copy;
    commands->selected_model = model_copy;
    if (changed) {
        clay_chat_destroy(commands->chat);
        commands->chat = NULL;
        clay_commands_reset_conversation(commands);
        commands->input_tokens = 0;
        commands->output_tokens = 0;
        clay_commands_set_tokens_below(commands, 0, 0);
    }
    clay_commands_update_selected_below(commands);
    return clay_config_selection_save(commands->selected_provider, commands->selected_model);
}

int clay_commands_fetch_models(void *ctx, ClayArray *out) {
    ClayConnectedProvider *provider = ctx;
    if (!provider->models_fetched) {
        ClayTask *task = clay_task_start("Retrieving %s models", provider->type->label);
        provider->models_rc = clay_openai_list_models(provider->client, &provider->models);
        provider->models_fetched = 1;
        if (provider->models_rc == 0) clay_task_success(task, "%zu models available", provider->models.count);
        else clay_task_fail(task, "Could not retrieve models");
    }
    for (size_t i = 0; i < provider->models.count; i++) {
        ClayModelItem item = {*(char **)clay_array_get(&provider->models, i), NULL};
        clay_array_push_val(out, &item);
    }
    return provider->models_rc;
}

const ClayReasoningEffort *clay_commands_reasoning_effort(const ClayCommands *commands) {
    return &REASONING_EFFORTS[commands->reasoning_effort_index];
}

size_t clay_commands_reasoning_effort_count(void) {
    return sizeof(REASONING_EFFORTS) / sizeof(REASONING_EFFORTS[0]);
}

const ClayReasoningEffort *clay_commands_reasoning_efforts(void) {
    return REASONING_EFFORTS;
}

void clay_commands_print_history(ClayCommands *commands, size_t count) {
    ClayJson *messages = clay_chat_recent_messages(commands->chat, count);
    for (size_t i = 0; i < clay_json_array_count(messages); i++) {
        ClayJson *message = clay_json_array_get(messages, i);
        const char *role = clay_json_string_value(clay_json_object_get(message, "role"));
        const char *content = clay_json_string_value(clay_json_object_get(message, "content"));
        clay_sayc(strcmp(role, "user") == 0 ? CLAY_CYAN : CLAY_GRAY,
                  "%s: %s", strcmp(role, "user") == 0 ? "You" : "Clay", content);
    }
    clay_json_free(messages);
}

ClayCommands *clay_commands_create(ClayApp *app) {
    ClayCommands *commands = calloc(1, sizeof(ClayCommands));
    commands->app = app;
    commands->running = 1;
    clay_array_init(&commands->providers, sizeof(ClayConnectedProvider));
    size_t count;
    const ClayProviderType *types = clay_commands_provider_types(&count);
    for (size_t i = 0; i < count; i++) clay_commands_load_provider(commands, &types[i]);
    clay_config_selection_load(&commands->selected_provider, &commands->selected_model);
    char *sandbox_mode = clay_config_sandbox_mode();
    commands->sandbox_mode = strcmp(sandbox_mode, "unleashed") == 0 ? CLAY_SANDBOX_MODE_UNLEASHED : CLAY_SANDBOX_MODE_SANDBOX;
    free(sandbox_mode);
    if (!clay_sandbox_supported()) commands->sandbox_mode = CLAY_SANDBOX_MODE_UNLEASHED;
    char *sandbox_access = clay_config_sandbox_access();
    commands->sandbox_access =
        strcmp(sandbox_access, "writable") == 0 ? CLAY_SANDBOX_ACCESS_WRITABLE : CLAY_SANDBOX_ACCESS_READONLY;
    free(sandbox_access);
    for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) {
        commands->auto_approve[i] = clay_config_auto_approve(clay_permissions_category_name((ClayPermissionCategory)i));
        clay_array_init(&commands->remembered_patterns[i], sizeof(char *));
    }
    clay_array_init(&commands->todos, sizeof(ClayTodoItem));
    clay_array_init(&commands->mcp_servers, sizeof(ClayMcpServer *));
    clay_array_init(&commands->mcp_bindings, sizeof(ClayMcpToolBinding));
    commands->auto_test_command = clay_config_auto_test_command();
    commands->auto_test_choice = CLAY_AUTO_TEST_UNASKED;
    clay_commands_reset_conversation(commands);
    clay_config_selection_save(commands->selected_provider, commands->selected_model);
    clay_below_add(0, "status");
    clay_below_set_enabled("status", 0);
    clay_below_add(1, "model");
    clay_below_add(2, "tokens");
    clay_below_add(3, "hint");
    clay_below_set_enabled("hint", 0);
    clay_below_add(4, "mode");
    clay_below_set_enabled("mode", 0);
    clay_commands_set_tokens_below(commands, 0, 0);
    clay_commands_update_selected_below(commands);
    return commands;
}

void clay_commands_destroy(ClayCommands *commands) {
    if (!commands) return;
    for (size_t i = 0; i < commands->providers.count; i++) provider_free(clay_array_get(&commands->providers, i));
    clay_array_free(&commands->providers);
    clay_json_free(commands->conversation);
    free(commands->system_prompt);
    clay_chat_destroy(commands->chat);
    free(commands->selected_provider);
    free(commands->selected_model);
    for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) {
        ClayArray *remembered = &commands->remembered_patterns[i];
        for (size_t j = 0; j < remembered->count; j++) free(*(char **)clay_array_get(remembered, j));
        clay_array_free(remembered);
    }
    clay_commands_clear_todos(commands);
    clay_array_free(&commands->todos);
    for (size_t i = 0; i < commands->mcp_bindings.count; i++) {
        ClayMcpToolBinding *binding = clay_array_get(&commands->mcp_bindings, i);
        free(binding->tool_name);
        free(binding->exposed_name);
    }
    clay_array_free(&commands->mcp_bindings);
    for (size_t i = 0; i < commands->mcp_servers.count; i++) {
        clay_mcp_disconnect(*(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i));
    }
    clay_array_free(&commands->mcp_servers);
    free(commands->auto_test_command);
    free(commands);
}

int clay_commands_running(const ClayCommands *commands) {
    return commands->running;
}

void clay_commands_print_session_summary(const ClayCommands *commands) {
    printf("%sSession summary%s  %s\xe2\x86\x91 %ld%s  %s\xe2\x86\x93 %ld%s  %s%ld messages sent%s\n",
           clay_color(CLAY_GRAY), clay_color(CLAY_RESET), clay_color(CLAY_CYAN), commands->total_input_tokens,
           clay_color(CLAY_RESET), clay_color(CLAY_CYAN), commands->total_output_tokens, clay_color(CLAY_RESET),
           clay_color(CLAY_GRAY), commands->messages_sent, clay_color(CLAY_RESET));
}
