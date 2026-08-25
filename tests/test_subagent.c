#define _GNU_SOURCE

#include "../src/commands/context.h"

#include "clay/json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

static ClayJson *call(ClayCommands *commands, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    assert(args);
    ClayJson *result = subagent_tool(args, commands);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

static const char *error_of(ClayJson *result) {
    return clay_json_string_value(clay_json_object_get(result, "error"));
}

static int has_tool(ClayToolSet *set, const char *name) {
    for (size_t i = 0; i < set->tools.count; i++) {
        const ClayTool *tool = clay_array_get(&set->tools, i);
        if (strcmp(tool->name, name) == 0) return 1;
    }
    return 0;
}

int main(void) {
    char template[] = "/tmp/clay_test_subagent_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    clay_array_init(&commands.tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&commands.mcp_bindings, sizeof(ClayMcpToolBinding));
    clay_array_init(&commands.mcp_servers, sizeof(ClayMcpServer *));
    commands.mcp_connect_attempted = 1; /* no servers to dial in a test */
    commands.mode = CLAY_MODE_ACT;

    ClayJson *result = call(&commands, "{\"prompt\":\"do the thing\"}");
    assert(!ok(result));
    assert(strstr(error_of(result), "description"));
    clay_json_free(result);

    result = call(&commands, "{\"description\":\"step one\"}");
    assert(!ok(result));
    assert(strstr(error_of(result), "prompt"));
    clay_json_free(result);

    /* Plan mode refuses before anything is spawned. */
    commands.mode = CLAY_MODE_PLAN;
    result = call(&commands, "{\"description\":\"step one\",\"prompt\":\"do it\"}");
    assert(!ok(result));
    assert(strstr(error_of(result), "Plan mode"));
    clay_json_free(result);
    commands.mode = CLAY_MODE_ACT;

    /* Without a chat there is nowhere to record the run. */
    result = call(&commands, "{\"description\":\"step one\",\"prompt\":\"do it\"}");
    assert(!ok(result));
    assert(strstr(error_of(result), "chat"));
    clay_json_free(result);

    /* A subagent gets the working tools but neither the user's ear nor the
       ability to spawn more subagents. */
    ClayToolSet delegated;
    clay_commands_tools_build(&commands, &delegated, 0);
    assert(has_tool(&delegated, "read"));
    assert(has_tool(&delegated, "shell_exec"));
    assert(has_tool(&delegated, "todowrite"));
    assert(!has_tool(&delegated, "subagent"));
    assert(!has_tool(&delegated, "ask_user"));
    size_t delegated_count = delegated.tools.count;
    clay_commands_tools_free(&delegated);

    ClayToolSet top;
    clay_commands_tools_build(&commands, &top, 1);
    assert(has_tool(&top, "subagent"));
    assert(has_tool(&top, "ask_user"));
    assert(top.tools.count == delegated_count + 2);
    clay_commands_tools_free(&top);

    /* Each run gets its own file under the chat. */
    ClayChat *chat = clay_chat_create("system");
    assert(chat);
    char *first = clay_chat_subagent_path(chat, "aaaa");
    char *second = clay_chat_subagent_path(chat, "bbbb");
    assert(first && second && strcmp(first, second) != 0);
    assert(strstr(first, "/subagents/aaaa.json"));
    assert(clay_term_write_file_atomic(first, "{}", 2) == 0);
    free(first);
    free(second);
    clay_chat_destroy(chat);

    ClayJson *schema = subagent_schema();
    ClayJson *properties = clay_json_object_get(schema, "properties");
    assert(clay_json_object_get(properties, "description"));
    assert(clay_json_object_get(properties, "prompt"));
    assert(clay_json_array_count(clay_json_object_get(schema, "required")) == 2);
    clay_json_free(schema);

    printf("subagent tests passed\n");
    return 0;
}
