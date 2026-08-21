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
    ClayChoice choices[chats.count];
    ClayStr titles[chats.count];
    ClayStr descriptions[chats.count];
    long long now = clay_time_now();
    for (size_t i = 0; i < chats.count; i++) {
        ClayChatSummary *chat = clay_array_get(&chats, i);
        char *relative = clay_time_relative(chat->updated_at, now);
        clay_str_init(&titles[i]);
        clay_str_init(&descriptions[i]);
        clay_str_push(&titles[i], chat->id);
        clay_str_printf(&descriptions[i], "%s · %zu messages", relative, chat->message_count);
        free(relative);
        choices[i].title = titles[i].data;
        choices[i].desc = descriptions[i].data;
    }
    int index = clay_app_choice(commands->app, "Resume a chat:", choices, (int)chats.count, 0, NULL);
    for (size_t i = 0; i < chats.count; i++) {
        clay_str_free(&titles[i]);
        clay_str_free(&descriptions[i]);
    }
    if (index >= 0) {
        ClayChatSummary *summary = clay_array_get(&chats, (size_t)index);
        ClayChat *chat = clay_chat_load(summary->id);
        if (!chat) clay_sayc(CLAY_RED, "Could not load that chat.");
        else {
            clay_chat_destroy(commands->chat);
            commands->chat = chat;
            clay_commands_reset_conversation(commands);
            int preview = clay_config_history_preview_count();
            if (preview > 0) {
                clay_sayc(CLAY_CYAN, "Resumed %zu previous messages.", clay_chat_message_count(chat));
                clay_commands_print_history(commands, (size_t)preview);
            }
        }
    }
    clay_chat_list_free(&chats);
}
