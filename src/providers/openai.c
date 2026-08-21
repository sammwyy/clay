#include "clay/providers/openai.h"

#include "clay/array.h"
#include "clay/http.h"
#include "clay/str.h"

#include <stdlib.h>
#include <string.h>

struct ClayOpenAI {
    char *base_url;
    char *api_key;
    char *model;
};

ClayOpenAI *clay_openai_create(const char *base_url, const char *api_key, const char *model) {
    ClayOpenAI *client = malloc(sizeof(ClayOpenAI));
    client->base_url = strdup(base_url);
    client->api_key = strdup(api_key);
    client->model = strdup(model);
    return client;
}

void clay_openai_destroy(ClayOpenAI *client) {
    if (!client) return;
    free(client->base_url);
    free(client->api_key);
    free(client->model);
    free(client);
}

ClayJson *clay_openai_message(const char *role, const char *content) {
    ClayJson *m = clay_json_object();
    clay_json_object_set(m, "role", clay_json_string(role));
    clay_json_object_set(m, "content", clay_json_string(content));
    return m;
}

/* One tool call as it accumulates across streamed deltas: the API sends
   its id/name/argument-fragments as separate events, keyed by `index`. */
typedef struct {
    size_t index;
    ClayStr id;
    ClayStr name;
    ClayStr arguments;
} ClayToolCallAccum;

typedef struct {
    ClayStr line_buf; /* bytes not yet resolved into a complete SSE line */
    ClayStr raw;       /* every byte seen, for error reporting */
    ClayStr content;
    ClayArray tool_calls; /* ClayToolCallAccum */
    const ClayOpenAICallbacks *callbacks;
} ClayStreamState;

static void stream_state_init(ClayStreamState *st, const ClayOpenAICallbacks *callbacks) {
    clay_str_init(&st->line_buf);
    clay_str_init(&st->raw);
    clay_str_init(&st->content);
    clay_array_init(&st->tool_calls, sizeof(ClayToolCallAccum));
    st->callbacks = callbacks;
}

static void stream_state_free(ClayStreamState *st) {
    clay_str_free(&st->line_buf);
    clay_str_free(&st->raw);
    clay_str_free(&st->content);
    for (size_t i = 0; i < st->tool_calls.count; i++) {
        ClayToolCallAccum *tc = clay_array_get(&st->tool_calls, i);
        clay_str_free(&tc->id);
        clay_str_free(&tc->name);
        clay_str_free(&tc->arguments);
    }
    clay_array_free(&st->tool_calls);
}

static ClayToolCallAccum *tool_call_at(ClayArray *calls, size_t index) {
    for (size_t i = 0; i < calls->count; i++) {
        ClayToolCallAccum *tc = clay_array_get(calls, i);
        if (tc->index == index) return tc;
    }

    ClayToolCallAccum tc;
    tc.index = index;
    clay_str_init(&tc.id);
    clay_str_init(&tc.name);
    clay_str_init(&tc.arguments);
    clay_array_push_val(calls, &tc);
    return clay_array_get(calls, calls->count - 1);
}

/* Applies one `data: {...}` payload (already stripped of the prefix) to
   the accumulating stream state. */
static void process_sse_data(const char *json_text, ClayStreamState *st) {
    if (strcmp(json_text, "[DONE]") == 0) return;

    ClayJson *root = clay_json_parse(json_text, NULL);
    if (!root) return;

    ClayJson *choice0 = clay_json_array_get(clay_json_object_get(root, "choices"), 0);
    ClayJson *delta = clay_json_object_get(choice0, "delta");

    ClayJson *content = clay_json_object_get(delta, "content");
    if (clay_json_type(content) == CLAY_JSON_STRING) {
        const char *text = clay_json_string_value(content);
        clay_str_push(&st->content, text);
        if (st->callbacks && st->callbacks->on_token) st->callbacks->on_token(text, st->callbacks->userdata);
    }

    ClayJson *tool_calls = clay_json_object_get(delta, "tool_calls");
    size_t n = clay_json_array_count(tool_calls);
    for (size_t i = 0; i < n; i++) {
        ClayJson *tc = clay_json_array_get(tool_calls, i);
        size_t idx = (size_t)clay_json_number_value(clay_json_object_get(tc, "index"));
        ClayToolCallAccum *acc = tool_call_at(&st->tool_calls, idx);

        ClayJson *id = clay_json_object_get(tc, "id");
        if (clay_json_type(id) == CLAY_JSON_STRING) clay_str_push(&acc->id, clay_json_string_value(id));

        ClayJson *fn = clay_json_object_get(tc, "function");
        ClayJson *name = clay_json_object_get(fn, "name");
        if (clay_json_type(name) == CLAY_JSON_STRING) clay_str_push(&acc->name, clay_json_string_value(name));
        ClayJson *args = clay_json_object_get(fn, "arguments");
        if (clay_json_type(args) == CLAY_JSON_STRING) clay_str_push(&acc->arguments, clay_json_string_value(args));
    }

    clay_json_free(root);
}

/* libcurl write callback: buffers raw bytes into whole lines and hands
   each "data: ..." line to process_sse_data as soon as it's complete -
   a chunk boundary can land anywhere, including mid-line. */
static int on_http_chunk(const char *data, size_t len, void *userdata) {
    ClayStreamState *st = userdata;
    clay_str_push_n(&st->raw, data, len);
    clay_str_push_n(&st->line_buf, data, len);

    for (;;) {
        char *nl = memchr(st->line_buf.data, '\n', st->line_buf.len);
        if (!nl) break;

        size_t line_len = (size_t)(nl - st->line_buf.data);
        size_t trimmed_len = line_len;
        if (trimmed_len > 0 && st->line_buf.data[trimmed_len - 1] == '\r') trimmed_len--;

        if (trimmed_len >= 6 && strncmp(st->line_buf.data, "data: ", 6) == 0) {
            char *saved = st->line_buf.data + trimmed_len;
            char saved_char = *saved;
            *saved = '\0';
            process_sse_data(st->line_buf.data + 6, st);
            *saved = saved_char;
        }

        clay_str_remove_n(&st->line_buf, 0, line_len + 1);
    }

    return 0;
}

static ClayStr build_request_body(ClayOpenAI *client, const ClayJson *messages, const ClayTool *tools,
                                   size_t tool_count) {
    ClayJson *root = clay_json_object();
    clay_json_object_set(root, "model", clay_json_string(client->model));
    clay_json_object_set(root, "messages", clay_json_clone(messages));
    clay_json_object_set(root, "stream", clay_json_bool(1));

    if (tool_count > 0) {
        ClayJson *tools_json = clay_json_array();
        for (size_t i = 0; i < tool_count; i++) {
            ClayJson *fn = clay_json_object();
            clay_json_object_set(fn, "name", clay_json_string(tools[i].name));
            clay_json_object_set(fn, "description", clay_json_string(tools[i].description ? tools[i].description : ""));
            clay_json_object_set(fn, "parameters",
                                  tools[i].parameters ? clay_json_clone(tools[i].parameters) : clay_json_object());

            ClayJson *tool = clay_json_object();
            clay_json_object_set(tool, "type", clay_json_string("function"));
            clay_json_object_set(tool, "function", fn);
            clay_json_array_push(tools_json, tool);
        }
        clay_json_object_set(root, "tools", tools_json);
    }

    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(root, &body);
    clay_json_free(root);
    return body;
}

/* Appends the assistant's tool-call message to `messages`, runs each
   call against `tools`, and appends the resulting "tool" messages. */
static void handle_tool_calls(ClayJson *messages, const ClayTool *tools, size_t tool_count, ClayStreamState *st,
                               const ClayOpenAICallbacks *callbacks) {
    ClayJson *assistant = clay_json_object();
    clay_json_object_set(assistant, "role", clay_json_string("assistant"));
    clay_json_object_set(assistant, "content", st->content.len > 0 ? clay_json_string(st->content.data) : clay_json_null());

    ClayJson *tool_calls_json = clay_json_array();
    for (size_t i = 0; i < st->tool_calls.count; i++) {
        ClayToolCallAccum *tc = clay_array_get(&st->tool_calls, i);

        ClayJson *fn = clay_json_object();
        clay_json_object_set(fn, "name", clay_json_string(tc->name.data));
        clay_json_object_set(fn, "arguments", clay_json_string(tc->arguments.data));

        ClayJson *call = clay_json_object();
        clay_json_object_set(call, "id", clay_json_string(tc->id.data));
        clay_json_object_set(call, "type", clay_json_string("function"));
        clay_json_object_set(call, "function", fn);
        clay_json_array_push(tool_calls_json, call);
    }
    clay_json_object_set(assistant, "tool_calls", tool_calls_json);
    clay_json_array_push(messages, assistant);

    for (size_t i = 0; i < st->tool_calls.count; i++) {
        ClayToolCallAccum *tc = clay_array_get(&st->tool_calls, i);

        if (callbacks && callbacks->on_tool_call) {
            callbacks->on_tool_call(tc->name.data, tc->arguments.data, callbacks->userdata);
        }

        const ClayTool *tool = NULL;
        for (size_t j = 0; j < tool_count; j++) {
            if (strcmp(tools[j].name, tc->name.data) == 0) {
                tool = &tools[j];
                break;
            }
        }

        ClayJson *args = clay_json_parse(tc->arguments.data, NULL);
        ClayJson *result = tool && tool->fn ? tool->fn(args, tool->userdata) : NULL;
        if (!result) result = clay_json_object();
        clay_json_free(args);

        if (callbacks && callbacks->on_tool_result) {
            callbacks->on_tool_result(tc->name.data, result, callbacks->userdata);
        }

        ClayStr result_json;
        clay_str_init(&result_json);
        clay_json_stringify(result, &result_json);
        clay_json_free(result);

        ClayJson *tool_msg = clay_json_object();
        clay_json_object_set(tool_msg, "role", clay_json_string("tool"));
        clay_json_object_set(tool_msg, "tool_call_id", clay_json_string(tc->id.data));
        clay_json_object_set(tool_msg, "content", clay_json_string(result_json.data));
        clay_json_array_push(messages, tool_msg);

        clay_str_free(&result_json);
    }
}

int clay_openai_run(ClayOpenAI *client, ClayJson *messages, const ClayTool *tools, size_t tool_count,
                     int max_rounds, const ClayOpenAICallbacks *callbacks) {
    for (int round = 0; round < max_rounds; round++) {
        ClayStr body = build_request_body(client, messages, tools, tool_count);

        ClayStr url;
        clay_str_init(&url);
        clay_str_printf(&url, "%s/chat/completions", client->base_url);

        ClayStr auth;
        clay_str_init(&auth);
        clay_str_printf(&auth, "Bearer %s", client->api_key);

        ClayHttpHeader headers[] = {
            {"Authorization", auth.data},
            {"Content-Type", "application/json"},
            {"Accept", "text/event-stream"},
        };

        ClayStreamState st;
        stream_state_init(&st, callbacks);

        ClayHttpRequest req = {0};
        req.method = "POST";
        req.url = url.data;
        req.headers = headers;
        req.header_count = sizeof(headers) / sizeof(headers[0]);
        req.body = body.data;
        req.body_len = body.len;
        req.on_chunk = on_http_chunk;
        req.userdata = &st;

        ClayHttpResponse resp;
        int rc = clay_http_request(&req, &resp);

        clay_str_free(&url);
        clay_str_free(&auth);
        clay_str_free(&body);

        if (rc != 0 || resp.status < 200 || resp.status >= 300) {
            if (callbacks && callbacks->on_error) callbacks->on_error(resp.status, st.raw.data, callbacks->userdata);
            clay_http_response_free(&resp);
            stream_state_free(&st);
            return -1;
        }
        clay_http_response_free(&resp);

        if (st.tool_calls.count == 0) {
            clay_json_array_push(messages, clay_openai_message("assistant", st.content.data));
            stream_state_free(&st);
            return 0;
        }

        handle_tool_calls(messages, tools, tool_count, &st, callbacks);
        stream_state_free(&st);
    }

    return -1;
}
