#ifndef CLAY_COMMANDS_CONTEXT_H
#define CLAY_COMMANDS_CONTEXT_H

#include "clay/clay.h"
#include "clay/providers/openai.h"

typedef struct {
    const char *id;
    const char *label;
    const char *default_base_url;
} ClayProviderType;

typedef struct {
    const ClayProviderType *type;
    ClayProviderConfig *config;
    ClayOpenAI *client;
    ClayArray models;
    int models_fetched;
    int models_rc;
} ClayConnectedProvider;

typedef struct {
    const char *id;
    const char *label;
    const char *description;
} ClayReasoningEffort;

struct ClayCommands {
    ClayApp *app;
    int running;
    ClayJson *conversation;
    long input_tokens;
    long output_tokens;
    long total_input_tokens;
    long total_output_tokens;
    long messages_sent;
    ClayChat *chat;
    ClayArray providers;
    char *selected_provider;
    char *selected_model;
    int reasoning_effort_index;
};

ClayConnectedProvider *clay_commands_find_provider(ClayCommands *commands, const char *id);
const ClayProviderType *clay_commands_find_provider_type(const char *id);
const ClayProviderType *clay_commands_provider_types(size_t *count);
int clay_commands_fetch_models(void *ctx, ClayArray *out);
int clay_commands_select_model(ClayCommands *commands, const char *provider, const char *model);
void clay_commands_update_selected_below(ClayCommands *commands);
void clay_commands_set_tokens_below(ClayCommands *commands, long input_tokens, long output_tokens);
void clay_commands_reset_conversation(ClayCommands *commands);
void clay_commands_new_chat(ClayCommands *commands);
const ClayReasoningEffort *clay_commands_reasoning_effort(const ClayCommands *commands);
size_t clay_commands_reasoning_effort_count(void);
const ClayReasoningEffort *clay_commands_reasoning_efforts(void);
void clay_commands_load_provider(ClayCommands *commands, const ClayProviderType *type);
void clay_commands_print_history(ClayCommands *commands, size_t count);

void clay_cmd_exit(const char *args, void *user_data);
void clay_cmd_help(const char *args, void *user_data);
void clay_cmd_confirm(const char *args, void *user_data);
void clay_cmd_select(const char *args, void *user_data);
void clay_cmd_choice(const char *args, void *user_data);
void clay_cmd_below(const char *args, void *user_data);
void clay_cmd_connect(const char *args, void *user_data);
void clay_cmd_model(const char *args, void *user_data);
void clay_cmd_effort(const char *args, void *user_data);
void clay_cmd_resume(const char *args, void *user_data);
void clay_cmd_history(const char *args, void *user_data);
void clay_cmd_new(const char *args, void *user_data);
void clay_cmd_mm(const char *args, void *user_data);
void clay_cmd_demo(const char *args, void *user_data);

#endif /* CLAY_COMMANDS_CONTEXT_H */
