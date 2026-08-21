#include "clay/chat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char template[] = "/tmp/clay-chat-XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    ClayChat *chat = clay_chat_create("you are clay");
    assert(chat);
    char *id = strdup(clay_chat_id(chat));
    assert(strcmp(clay_chat_system_prompt(chat), "you are clay") == 0);
    assert(strcmp(clay_chat_notes(chat), "") == 0);
    assert(clay_chat_set_notes(chat, "remember this") == 0);
    assert(strcmp(clay_chat_notes(chat), "remember this") == 0);
    assert(clay_chat_begin_turn(chat, "hello") == 0);

    ClayJson *messages = clay_json_array();
    clay_json_array_push(messages, clay_json_object());
    ClayJson *user = clay_json_array_get(messages, 0);
    clay_json_object_set(user, "role", clay_json_string("user"));
    clay_json_object_set(user, "content", clay_json_string("hello"));
    clay_json_array_push(messages, clay_json_object());
    ClayJson *assistant = clay_json_array_get(messages, 1);
    clay_json_object_set(assistant, "role", clay_json_string("assistant"));
    clay_json_object_set(assistant, "content", clay_json_string("hi"));
    assert(clay_chat_finish_turn(chat, messages, 0, "completed") == 0);
    clay_json_free(messages);
    clay_chat_destroy(chat);

    chat = clay_chat_load(id);
    assert(chat);
    assert(strcmp(clay_chat_system_prompt(chat), "you are clay") == 0);
    assert(strcmp(clay_chat_notes(chat), "remember this") == 0);
    ClayJson *history = clay_chat_openai_messages(chat);
    assert(clay_json_array_count(history) == 2);
    assert(strcmp(clay_json_string_value(clay_json_object_get(clay_json_array_get(history, 0), "content")), "hello") == 0);
    assert(strcmp(clay_json_string_value(clay_json_object_get(clay_json_array_get(history, 1), "content")), "hi") == 0);
    clay_json_free(history);
    clay_chat_destroy(chat);

    ClayArray summaries;
    assert(clay_chat_list(&summaries) == 0);
    assert(summaries.count == 1);
    assert(strcmp(((ClayChatSummary *)clay_array_get(&summaries, 0))->id, id) == 0);
    clay_chat_list_free(&summaries);

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", home, id);
    assert(remove(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s", home, id);
    assert(rmdir(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats", home);
    assert(rmdir(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay", home);
    assert(rmdir(path.data) == 0);
    assert(rmdir(home) == 0);
    clay_str_free(&path);
    free(id);
    return 0;
}
