#include "context.h"

#include <stdlib.h>

void clay_cmd_resume(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    ClayArray chats;
    if (clay_chat_list(&chats) != 0 || chats.count == 0) {
        clay_sayc(CLAY_GRAY, "No saved chats.");
        clay_chat_list_free(&chats);
        return;
    }
    ClayArray choices, titles, descriptions;
    clay_array_init(&choices, sizeof(ClayChoice));
    clay_array_init(&titles, sizeof(ClayStr));
    clay_array_init(&descriptions, sizeof(ClayStr));
    long long now = clay_time_now();
    for (size_t i = 0; i < chats.count; i++) {
        ClayChatSummary *chat = clay_array_get(&chats, i);
        char *relative = clay_time_relative(chat->updated_at, now);
        ClayStr title, description;
        clay_str_init(&title);
        clay_str_init(&description);
        clay_str_push(&title, chat->id);
        clay_str_printf(&description, "%s · %zu messages", relative, chat->message_count);
        free(relative);
        clay_array_push_val(&titles, &title);
        clay_array_push_val(&descriptions, &description);
        ClayChoice choice = {title.data, description.data};
        clay_array_push_val(&choices, &choice);
    }
    int index = clay_app_choice(commands->app, "Resume a chat:", choices.data, (int)choices.count, 0, NULL);
    for (size_t i = 0; i < titles.count; i++) {
        clay_str_free(clay_array_get(&titles, i));
        clay_str_free(clay_array_get(&descriptions, i));
    }
    clay_array_free(&choices);
    clay_array_free(&titles);
    clay_array_free(&descriptions);
    if (index >= 0) {
        ClayChatSummary *summary = clay_array_get(&chats, (size_t)index);
        ClayChat *chat = clay_chat_load(summary->id);
        if (!chat) clay_sayc(CLAY_RED, "Could not load that chat.");
        else {
            clay_chat_destroy(commands->chat);
            commands->chat = chat;
            clay_commands_reset_conversation(commands);
            ClayChatUsage usage;
            clay_chat_get_usage(chat, &usage);
            commands->input_tokens = usage.input_tokens;
            commands->output_tokens = usage.output_tokens;
            commands->cached_input_tokens = usage.cached_input_tokens;
            commands->cached_input_tokens_known =
                usage.cached_input_tokens_known;
            commands->total_input_tokens = usage.total_input_tokens;
            commands->total_output_tokens = usage.total_output_tokens;
            clay_commands_set_tokens_below_with_cache(
                commands, usage.input_tokens, usage.output_tokens,
                usage.cached_input_tokens, usage.cached_input_tokens_known);
            int preview = clay_config_history_preview_count();
            if (preview > 0) {
                clay_sayc(CLAY_CYAN, "Resumed %zu previous messages.", clay_chat_message_count(chat));
                clay_commands_print_history(commands, (size_t)preview);
            }
        }
    }
    clay_chat_list_free(&chats);
}
