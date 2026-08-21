#include "../src/commands/context.h"

#include "clay/json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ClayJson *call(ClayCommands *commands, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    ClayJson *result = todowrite_tool(args, commands);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

int main(void) {
    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    clay_array_init(&commands.todos, sizeof(ClayTodoItem));

    ClayJson *result =
        call(&commands, "{\"todos\":[{\"content\":\"Write tests\",\"status\":\"in_progress\"},"
                        "{\"content\":\"Ship it\",\"status\":\"pending\"}]}");
    assert(ok(result));
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "Write tests"));
    assert(strstr(output, "Ship it"));
    clay_json_free(result);
    assert(commands.todos.count == 2);
    ClayTodoItem *first = clay_array_get(&commands.todos, 0);
    assert(strcmp(first->content, "Write tests") == 0);
    assert(strcmp(first->status, "in_progress") == 0);

    /* Replacing with a shorter list drops the old entries, not appends. */
    result = call(&commands, "{\"todos\":[{\"content\":\"Ship it\",\"status\":\"completed\"}]}");
    assert(ok(result));
    clay_json_free(result);
    assert(commands.todos.count == 1);
    ClayTodoItem *only = clay_array_get(&commands.todos, 0);
    assert(strcmp(only->status, "completed") == 0);

    /* An invalid status is rejected and leaves the previous plan intact. */
    result = call(&commands, "{\"todos\":[{\"content\":\"x\",\"status\":\"done\"}]}");
    assert(!ok(result));
    clay_json_free(result);
    assert(commands.todos.count == 1);

    /* Empty content is rejected. */
    result = call(&commands, "{\"todos\":[{\"content\":\"\",\"status\":\"pending\"}]}");
    assert(!ok(result));
    clay_json_free(result);

    /* Not an array at all. */
    result = call(&commands, "{\"todos\":\"nope\"}");
    assert(!ok(result));
    clay_json_free(result);

    clay_commands_clear_todos(&commands);
    clay_array_free(&commands.todos);

    printf("todowrite tests passed\n");
    return 0;
}
