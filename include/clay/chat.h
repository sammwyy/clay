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

/* system_prompt is frozen into the journal at creation and never rebuilt
   on later loads, so a chat's provider-side prefix cache stays valid for
   its whole lifetime. */
ClayChat *clay_chat_create(const char *system_prompt);
ClayChat *clay_chat_load(const char *id);
void clay_chat_destroy(ClayChat *chat);
const char *clay_chat_id(const ClayChat *chat);
size_t clay_chat_message_count(const ClayChat *chat);

/* The system prompt frozen at clay_chat_create time. Never "" for a chat
   created by this build; empty for a pre-memory chat.json on disk. */
const char *clay_chat_system_prompt(const ClayChat *chat);

/* Short-term scratchpad for this chat only - not part of "turns", so it
   survives independent of how much history is kept around it. "" if
   unset. */
const char *clay_chat_notes(const ClayChat *chat);
int clay_chat_set_notes(ClayChat *chat, const char *notes); /* persists; 0 on success */

/* /tmp/clay-<id>, private to this chat and created if missing. Malloc'd; NULL
   on failure. */
char *clay_chat_scratch_dir(const ClayChat *chat);

/* ~/.clay/chats/<id>/checkpoints - a bare git repo (see clay/checkpoint.h)
   holding this chat's workspace snapshots, created if missing. Malloc'd;
   NULL on failure. */
char *clay_chat_checkpoints_dir(const ClayChat *chat);

/* Writes `content` to a fresh file under this chat's scratch dir, named
   "<prefix>-<uuid>.txt". For dumping tool output too large to return
   inline. Malloc'd path, or NULL on failure. */
char *clay_chat_dump_scratch(const ClayChat *chat, const char *prefix, const char *content);
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
