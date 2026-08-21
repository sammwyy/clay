#include "clay/chat.h"

#include "clay/str.h"
#include "clay/term.h"
#include "clay/time.h"
#include "clay/uuid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_CHAT_FORMAT_VERSION 1

struct ClayChat {
    char *id;
    char *path;
    ClayJson *journal;
    ClayJson *active_turn;
    long next_turn_id;
};

static char *clay_dir(void) {
    char *home = clay_term_home_dir();
    if (!home) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay", home);
    free(home);
    return path.data;
}

static char *chat_path(const char *id) {
    char *dir = clay_dir();
    if (!dir) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/chats/%s/chat.json", dir, id);
    free(dir);
    return path.data;
}

static int ensure_chat_dir(const char *id) {
    char *dir = clay_dir();
    if (!dir || clay_term_mkdir(dir) != 0) {
        free(dir);
        return -1;
    }
    ClayStr chats;
    clay_str_init(&chats);
    clay_str_printf(&chats, "%s/chats", dir);
    free(dir);
    if (clay_term_mkdir(chats.data) != 0) {
        clay_str_free(&chats);
        return -1;
    }
    ClayStr chat;
    clay_str_init(&chat);
    clay_str_printf(&chat, "%s/%s", chats.data, id);
    clay_str_free(&chats);
    int rc = clay_term_mkdir(chat.data);
    clay_str_free(&chat);
    return rc;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    ClayStr text;
    clay_str_init(&text);
    int ch;
    while ((ch = fgetc(file)) != EOF) clay_str_push_char(&text, (char)ch);
    fclose(file);
    return text.data;
}

static int save(ClayChat *chat) {
    if (ensure_chat_dir(chat->id) != 0) return -1;
    clay_json_object_set(chat->journal, "updated_at", clay_json_number(clay_time_now()));
    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(chat->journal, &body);
    ClayStr temporary;
    clay_str_init(&temporary);
    clay_str_printf(&temporary, "%s.tmp", chat->path);
    FILE *file = fopen(temporary.data, "w");
    if (!file) {
        clay_str_free(&body);
        clay_str_free(&temporary);
        return -1;
    }
    size_t written = fwrite(body.data, 1, body.len, file);
    int closed = fclose(file) == 0;
    int ok = written == body.len && closed && rename(temporary.data, chat->path) == 0;
    if (!ok) remove(temporary.data);
    else clay_term_restrict_file(chat->path);
    clay_str_free(&body);
    clay_str_free(&temporary);
    return ok ? 0 : -1;
}

static ClayJson *messages_array(ClayChat *chat) {
    return clay_json_object_get(chat->journal, "turns");
}

static ClayJson *journal_message(const ClayJson *message) {
    ClayJson *record = clay_json_object();
    const char *role = clay_json_string_value(clay_json_object_get(message, "role"));
    clay_json_object_set(record, "kind", clay_json_string("message"));
    clay_json_object_set(record, "role", clay_json_string(role));
    ClayJson *content = clay_json_object_get(message, "content");
    clay_json_object_set(record, "text", content ? clay_json_clone(content) : clay_json_null());

    ClayJson *tool_calls = clay_json_object_get(message, "tool_calls");
    if (clay_json_type(tool_calls) == CLAY_JSON_ARRAY) {
        ClayJson *calls = clay_json_array();
        for (size_t i = 0; i < clay_json_array_count(tool_calls); i++) {
            ClayJson *call = clay_json_array_get(tool_calls, i);
            ClayJson *function = clay_json_object_get(call, "function");
            ClayJson *entry = clay_json_object();
            clay_json_object_set(entry, "id", clay_json_clone(clay_json_object_get(call, "id")));
            clay_json_object_set(entry, "name", clay_json_clone(clay_json_object_get(function, "name")));
            clay_json_object_set(entry, "arguments", clay_json_clone(clay_json_object_get(function, "arguments")));
            clay_json_array_push(calls, entry);
        }
        clay_json_object_set(record, "calls", calls);
    }
    ClayJson *tool_call_id = clay_json_object_get(message, "tool_call_id");
    if (tool_call_id) clay_json_object_set(record, "call_id", clay_json_clone(tool_call_id));
    return record;
}

static ClayJson *openai_message(const ClayJson *record) {
    const char *role = clay_json_string_value(clay_json_object_get(record, "role"));
    ClayJson *message = clay_json_object();
    clay_json_object_set(message, "role", clay_json_string(role));
    ClayJson *text = clay_json_object_get(record, "text");
    clay_json_object_set(message, "content", clay_json_clone(text ? text : clay_json_null()));
    ClayJson *calls = clay_json_object_get(record, "calls");
    if (clay_json_type(calls) == CLAY_JSON_ARRAY) {
        ClayJson *tool_calls = clay_json_array();
        for (size_t i = 0; i < clay_json_array_count(calls); i++) {
            ClayJson *entry = clay_json_array_get(calls, i);
            ClayJson *function = clay_json_object();
            clay_json_object_set(function, "name", clay_json_clone(clay_json_object_get(entry, "name")));
            clay_json_object_set(function, "arguments", clay_json_clone(clay_json_object_get(entry, "arguments")));
            ClayJson *call = clay_json_object();
            clay_json_object_set(call, "id", clay_json_clone(clay_json_object_get(entry, "id")));
            clay_json_object_set(call, "type", clay_json_string("function"));
            clay_json_object_set(call, "function", function);
            clay_json_array_push(tool_calls, call);
        }
        clay_json_object_set(message, "tool_calls", tool_calls);
    }
    ClayJson *call_id = clay_json_object_get(record, "call_id");
    if (call_id) clay_json_object_set(message, "tool_call_id", clay_json_clone(call_id));
    return message;
}

static ClayChat *chat_new(const char *id, ClayJson *journal) {
    ClayChat *chat = calloc(1, sizeof(ClayChat));
    chat->id = strdup(id);
    chat->path = chat_path(id);
    chat->journal = journal;
    ClayJson *turns = messages_array(chat);
    chat->next_turn_id = (long)clay_json_array_count(turns) + 1;
    return chat;
}

ClayChat *clay_chat_create(const char *system_prompt) {
    char *id = clay_uuid_v4();
    if (!id) return NULL;
    ClayJson *journal = clay_json_object();
    clay_json_object_set(journal, "format", clay_json_number(CLAY_CHAT_FORMAT_VERSION));
    clay_json_object_set(journal, "id", clay_json_string(id));
    clay_json_object_set(journal, "created_at", clay_json_number(clay_time_now()));
    clay_json_object_set(journal, "updated_at", clay_json_number(clay_time_now()));
    clay_json_object_set(journal, "system_prompt", clay_json_string(system_prompt ? system_prompt : ""));
    clay_json_object_set(journal, "notes", clay_json_string(""));
    clay_json_object_set(journal, "turns", clay_json_array());
    ClayChat *chat = chat_new(id, journal);
    free(id);
    if (save(chat) == 0) return chat;
    clay_chat_destroy(chat);
    return NULL;
}

ClayChat *clay_chat_load(const char *id) {
    char *path = chat_path(id);
    if (!path) return NULL;
    char *text = read_file(path);
    free(path);
    if (!text) return NULL;
    ClayJson *journal = clay_json_parse(text, NULL);
    free(text);
    if (!journal || clay_json_type(journal) != CLAY_JSON_OBJECT ||
        clay_json_type(clay_json_object_get(journal, "turns")) != CLAY_JSON_ARRAY) {
        clay_json_free(journal);
        return NULL;
    }
    return chat_new(id, journal);
}

void clay_chat_destroy(ClayChat *chat) {
    if (!chat) return;
    free(chat->id);
    free(chat->path);
    clay_json_free(chat->journal);
    free(chat);
}

const char *clay_chat_id(const ClayChat *chat) {
    return chat ? chat->id : "";
}

const char *clay_chat_system_prompt(const ClayChat *chat) {
    return clay_json_string_value(clay_json_object_get(chat->journal, "system_prompt"));
}

const char *clay_chat_notes(const ClayChat *chat) {
    return clay_json_string_value(clay_json_object_get(chat->journal, "notes"));
}

int clay_chat_set_notes(ClayChat *chat, const char *notes) {
    clay_json_object_set(chat->journal, "notes", clay_json_string(notes ? notes : ""));
    return save(chat);
}

size_t clay_chat_message_count(const ClayChat *chat) {
    ClayJson *messages = clay_chat_openai_messages(chat);
    size_t count = clay_json_array_count(messages);
    clay_json_free(messages);
    return count;
}

char *clay_chat_scratch_dir(const ClayChat *chat) {
    if (ensure_chat_dir(chat->id) != 0) return NULL;
    char *dir = clay_dir();
    if (!dir) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/chats/%s/scratch", dir, chat->id);
    free(dir);
    if (clay_term_mkdir(path.data) != 0) {
        clay_str_free(&path);
        return NULL;
    }
    return path.data;
}

char *clay_chat_checkpoints_dir(const ClayChat *chat) {
    if (ensure_chat_dir(chat->id) != 0) return NULL;
    char *dir = clay_dir();
    if (!dir) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/chats/%s/checkpoints", dir, chat->id);
    free(dir);
    return path.data;
}

void clay_chat_list_free(ClayArray *summaries) {
    for (size_t i = 0; i < summaries->count; i++) free(((ClayChatSummary *)clay_array_get(summaries, i))->id);
    clay_array_free(summaries);
}

int clay_chat_list(ClayArray *summaries) {
    clay_array_init(summaries, sizeof(ClayChatSummary));
    char *root = clay_dir();
    if (!root) return -1;
    ClayStr directory;
    clay_str_init(&directory);
    clay_str_printf(&directory, "%s/chats", root);
    free(root);
    ClayArray names;
    int rc = clay_term_list_dir(directory.data, &names);
    clay_str_free(&directory);
    if (rc != 0) return rc;
    for (size_t i = 0; i < names.count; i++) {
        char *id = *(char **)clay_array_get(&names, i);
        ClayChat *chat = clay_chat_load(id);
        if (chat) {
            ClayChatSummary summary = {strdup(id),
                                       (long long)clay_json_number_value(clay_json_object_get(chat->journal, "updated_at")),
                                       clay_chat_message_count(chat)};
            clay_array_push_val(summaries, &summary);
            clay_chat_destroy(chat);
        }
        free(id);
    }
    clay_array_free(&names);
    for (size_t i = 1; i < summaries->count; i++) {
        ClayChatSummary value = *(ClayChatSummary *)clay_array_get(summaries, i);
        size_t j = i;
        while (j > 0 && ((ClayChatSummary *)clay_array_get(summaries, j - 1))->updated_at < value.updated_at) {
            *(ClayChatSummary *)clay_array_get(summaries, j) = *(ClayChatSummary *)clay_array_get(summaries, j - 1);
            j--;
        }
        *(ClayChatSummary *)clay_array_get(summaries, j) = value;
    }
    return 0;
}

ClayJson *clay_chat_openai_messages(const ClayChat *chat) {
    ClayJson *messages = clay_json_array();
    ClayJson *turns = clay_json_object_get(chat->journal, "turns");
    for (size_t i = 0; i < clay_json_array_count(turns); i++) {
        ClayJson *turn = clay_json_array_get(turns, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(turn, "state")), "completed") != 0) continue;
        ClayJson *records = clay_json_object_get(turn, "messages");
        for (size_t j = 0; j < clay_json_array_count(records); j++) {
            ClayJson *record = clay_json_array_get(records, j);
            if (strcmp(clay_json_string_value(clay_json_object_get(record, "kind")), "message") == 0) {
                clay_json_array_push(messages, openai_message(record));
            }
        }
    }
    return messages;
}

ClayJson *clay_chat_recent_messages(const ClayChat *chat, size_t count) {
    ClayJson *all = clay_chat_openai_messages(chat);
    ClayJson *recent = clay_json_array();
    size_t start = clay_json_array_count(all) > count ? clay_json_array_count(all) - count : 0;
    for (size_t i = start; i < clay_json_array_count(all); i++) {
        clay_json_array_push(recent, clay_json_clone(clay_json_array_get(all, i)));
    }
    clay_json_free(all);
    return recent;
}

int clay_chat_begin_turn(ClayChat *chat, const char *content) {
    if (chat->active_turn) return -1;
    ClayJson *record = clay_json_object();
    clay_json_object_set(record, "kind", clay_json_string("message"));
    clay_json_object_set(record, "role", clay_json_string("user"));
    clay_json_object_set(record, "text", clay_json_string(content));
    ClayJson *records = clay_json_array();
    clay_json_array_push(records, record);
    ClayJson *turn = clay_json_object();
    clay_json_object_set(turn, "id", clay_json_number(chat->next_turn_id++));
    clay_json_object_set(turn, "state", clay_json_string("pending"));
    clay_json_object_set(turn, "messages", records);
    clay_json_array_push(messages_array(chat), turn);
    chat->active_turn = turn;
    return save(chat);
}

int clay_chat_finish_turn(ClayChat *chat, const ClayJson *messages, size_t start, const char *state) {
    if (!chat->active_turn) return -1;
    ClayJson *records = clay_json_array();
    for (size_t i = start; i < clay_json_array_count(messages); i++) {
        clay_json_array_push(records, journal_message(clay_json_array_get(messages, i)));
    }
    clay_json_object_set(chat->active_turn, "messages", records);
    clay_json_object_set(chat->active_turn, "state", clay_json_string(state));
    chat->active_turn = NULL;
    return save(chat);
}
