#ifndef CLAY_PROVIDERS_OPENAI_H
#define CLAY_PROVIDERS_OPENAI_H

#include "clay/array.h"
#include "clay/json.h"

#include <stddef.h>

typedef struct ClayOpenAI ClayOpenAI;

/* `base_url` has no trailing slash, e.g. "https://api.openai.com/v1" -
   "/chat/completions" is appended to it. */
ClayOpenAI *clay_openai_create(const char *base_url, const char *api_key, const char *model);
void clay_openai_destroy(ClayOpenAI *client);

/* Retrieves every id returned by the provider's OpenAI-compatible
   `GET /models` endpoint. `models` must be a ClayArray of `char *`;
   this appends malloc'd ids which the caller owns. 0 on success. */
int clay_openai_list_models(ClayOpenAI *client, ClayArray *models);

/* Builds a `{"role": role, "content": content}` chat message. */
ClayJson *clay_openai_message(const char *role, const char *content);

/* `parameters` is a JSON Schema object, borrowed (not owned). `fn` gets
   the call's parsed arguments and returns a new ClayJson tool result,
   which clay_openai_run frees. */
typedef ClayJson *(*ClayToolFn)(const ClayJson *arguments, void *userdata);

typedef struct {
    const char *name;
    const char *description;
    ClayJson *parameters;
    ClayToolFn fn;
    void *userdata;
} ClayTool;

/* All optional (NULL to ignore). */
typedef struct {
    void (*on_token)(const char *text, void *userdata);
    void (*on_tool_call)(const char *name, const char *arguments_json, void *userdata);
    void (*on_tool_result)(const char *name, const ClayJson *result, void *userdata);
    void (*on_error)(long status, const char *body, void *userdata);
    void *userdata;
} ClayOpenAICallbacks;

/* Streams the model's reply to `messages` (OpenAI wire format), running
   any requested tool calls and resending until it answers with plain
   content or `max_rounds` is hit. Appends the assistant reply and any
   tool results to `messages` in place. 0 on success. */
int clay_openai_run(ClayOpenAI *client, ClayJson *messages, const ClayTool *tools, size_t tool_count,
                     int max_rounds, const ClayOpenAICallbacks *callbacks);

#endif /* CLAY_PROVIDERS_OPENAI_H */
