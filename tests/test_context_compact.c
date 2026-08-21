#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void push_message(ClayJson *conversation, const char *role, const char *content) {
    ClayJson *message = clay_json_object();
    clay_json_object_set(message, "role", clay_json_string(role));
    clay_json_object_set(message, "content", clay_json_string(content));
    clay_json_array_push(conversation, message);
}

int main(void) {
    ClayStr long_output;
    clay_str_init(&long_output);
    for (int i = 0; i < 500; i++) clay_str_push_char(&long_output, 'x');

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    commands.conversation = clay_json_array();
    push_message(commands.conversation, "system", "you are clay");

    /* 8 turns of [user, tool(long), assistant]; keep window is 6 turns. */
    for (int turn = 0; turn < 8; turn++) {
        push_message(commands.conversation, "user", "do something");
        push_message(commands.conversation, "tool", long_output.data);
        push_message(commands.conversation, "assistant", "done");
    }

    /* Below budget: no-op. */
    commands.input_tokens = 0;
    assert(clay_commands_maybe_compact(&commands) == 0);
    ClayJson *first_tool = clay_json_array_get(commands.conversation, 2);
    assert(strcmp(clay_json_string_value(clay_json_object_get(first_tool, "content")), long_output.data) == 0);

    /* Above budget: collapses the two oldest turns' tool results only. */
    commands.input_tokens = 200000;
    int collapsed = clay_commands_maybe_compact(&commands);
    assert(collapsed == 2);

    ClayJson *turn1_tool = clay_json_array_get(commands.conversation, 2);
    ClayJson *turn2_tool = clay_json_array_get(commands.conversation, 5);
    ClayJson *turn3_tool = clay_json_array_get(commands.conversation, 8);
    ClayJson *turn8_tool = clay_json_array_get(commands.conversation, 23);
    const char *turn1_content = clay_json_string_value(clay_json_object_get(turn1_tool, "content"));
    const char *turn2_content = clay_json_string_value(clay_json_object_get(turn2_tool, "content"));
    const char *turn3_content = clay_json_string_value(clay_json_object_get(turn3_tool, "content"));
    const char *turn8_content = clay_json_string_value(clay_json_object_get(turn8_tool, "content"));
    assert(strncmp(turn1_content, "[collapsed", 10) == 0);
    assert(strncmp(turn2_content, "[collapsed", 10) == 0);
    assert(strcmp(turn3_content, long_output.data) == 0);
    assert(strcmp(turn8_content, long_output.data) == 0);

    /* Idempotent: running again doesn't re-collapse already-collapsed content. */
    assert(clay_commands_maybe_compact(&commands) == 0);

    /* User/assistant text is never touched. */
    ClayJson *turn1_user = clay_json_array_get(commands.conversation, 1);
    assert(strcmp(clay_json_string_value(clay_json_object_get(turn1_user, "content")), "do something") == 0);

    clay_json_free(commands.conversation);
    clay_str_free(&long_output);
    printf("context compact tests passed\n");
    return 0;
}
