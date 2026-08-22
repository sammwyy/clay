#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void push_message(ClayJson *conversation, const char *role, const char *content) {
    ClayJson *message = clay_json_object();
    clay_json_object_set(message, "role", clay_json_string(role));
    if (content) clay_json_object_set(message, "content", clay_json_string(content));
    clay_json_array_push(conversation, message);
}

int main(void) {
    ClayJson *conversation = clay_json_array();
    push_message(conversation, "system", "you are clay"); /* skipped by the transcript builder */
    push_message(conversation, "user", "read main.c and summarize it");

    ClayJson *assistant_call = clay_json_object();
    clay_json_object_set(assistant_call, "role", clay_json_string("assistant"));
    ClayJson *tool_calls = clay_json_array();
    ClayJson *call = clay_json_object();
    clay_json_object_set(call, "id", clay_json_string("call_1"));
    ClayJson *function = clay_json_object();
    clay_json_object_set(function, "name", clay_json_string("read"));
    clay_json_object_set(function, "arguments", clay_json_string("{\"path\":\"main.c\"}"));
    clay_json_object_set(call, "function", function);
    clay_json_array_push(tool_calls, call);
    clay_json_object_set(assistant_call, "tool_calls", tool_calls);
    clay_json_array_push(conversation, assistant_call);

    ClayStr long_output;
    clay_str_init(&long_output);
    for (int i = 0; i < 500; i++) clay_str_push_char(&long_output, 'x');
    ClayJson *tool_result = clay_json_object();
    clay_json_object_set(tool_result, "role", clay_json_string("tool"));
    clay_json_object_set(tool_result, "content", clay_json_string(long_output.data));
    clay_json_array_push(conversation, tool_result);

    push_message(conversation, "assistant", "main.c defines the process entry point.");

    char *transcript = clay_commands_build_compact_transcript(conversation);
    assert(transcript);

    /* The system prompt never leaks into the transcript. */
    assert(!strstr(transcript, "you are clay"));

    /* User/assistant text survive verbatim. */
    assert(strstr(transcript, "read main.c and summarize it"));
    assert(strstr(transcript, "main.c defines the process entry point."));

    /* The tool call is recorded compactly. */
    assert(strstr(transcript, "called read("));
    assert(strstr(transcript, "main.c"));

    /* The tool result is collapsed, not included in full. */
    assert(!strstr(transcript, long_output.data));
    assert(strstr(transcript, "truncated"));
    assert(strstr(transcript, "500 bytes total"));

    free(transcript);
    clay_str_free(&long_output);
    clay_json_free(conversation);

    /* A conversation with nothing but the system prompt yields an empty
       transcript - the caller uses this to skip compacting. */
    ClayJson *empty = clay_json_array();
    push_message(empty, "system", "you are clay");
    char *empty_transcript = clay_commands_build_compact_transcript(empty);
    assert(empty_transcript);
    assert(*empty_transcript == '\0');
    free(empty_transcript);
    clay_json_free(empty);

    printf("compact tests passed\n");
    return 0;
}
