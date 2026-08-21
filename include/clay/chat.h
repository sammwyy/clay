#ifndef CLAY_CHAT_H
#define CLAY_CHAT_H

#include "clay/json.h"
#include "clay/array.h"

#include <stddef.h>

typedef struct ClayChat ClayChat;

typedef struct {
    char *id;
    long long updated_at;
    size_t message_count;
} ClayChatSummary;

ClayChat *clay_chat_create(void);
ClayChat *clay_chat_load(const char *id);
void clay_chat_destroy(ClayChat *chat);
const char *clay_chat_id(const ClayChat *chat);
size_t clay_chat_message_count(const ClayChat *chat);

/* ~/.clay/chats/<id>/scratch, created if missing. Malloc'd; NULL on
   failure. */
char *clay_chat_scratch_dir(const ClayChat *chat);
int clay_chat_list(ClayArray *summaries); /* ClayChatSummary, caller frees with clay_chat_list_free */
void clay_chat_list_free(ClayArray *summaries);
ClayJson *clay_chat_recent_messages(const ClayChat *chat, size_t count);

/* Rebuilds the provider-neutral journal as OpenAI-compatible messages,
   excluding unfinished turns. Caller frees the returned array. */
ClayJson *clay_chat_openai_messages(const ClayChat *chat);

/* Starts a durable pending turn containing the user's message. */
int clay_chat_begin_turn(ClayChat *chat, const char *content);

/* Replaces the pending turn's messages from an OpenAI-compatible suffix
   and records its final state. start is the suffix's first array index. */
int clay_chat_finish_turn(ClayChat *chat, const ClayJson *messages, size_t start, const char *state);

#endif /* CLAY_CHAT_H */
