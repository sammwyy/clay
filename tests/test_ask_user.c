#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/term.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ClayJson *call(ClayCommands *commands, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    assert(args);
    ClayJson *result = ask_user_tool(args, commands);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

static const char *error_of(ClayJson *result) {
    return clay_json_string_value(clay_json_object_get(result, "error"));
}

int main(void) {
    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));

    ClayJson *result = call(&commands, "{\"options\":[{\"label\":\"a\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "question"));
    clay_json_free(result);

    result = call(&commands,
                  "{\"question\":\"Which?\",\"options\":[{\"label\":\"1\"},{\"label\":\"2\"},"
                  "{\"label\":\"3\"},{\"label\":\"4\"},{\"label\":\"5\"},{\"label\":\"6\"},"
                  "{\"label\":\"7\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "at most"));
    clay_json_free(result);

    result = call(&commands, "{\"question\":\"Which?\",\"options\":[{\"description\":\"no label\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "label"));
    clay_json_free(result);

    /* Without a tty the question is refused instead of blocking the run. */
    clay_term_set_noninteractive(1);
    result = call(&commands, "{\"question\":\"Which?\",\"options\":[{\"label\":\"a\"},{\"label\":\"b\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "interactive"));
    clay_json_free(result);
    clay_term_set_noninteractive(0);

    ClayJson *schema = ask_user_schema();
    ClayJson *properties = clay_json_object_get(schema, "properties");
    assert(clay_json_object_get(properties, "question"));
    assert(clay_json_object_get(properties, "options"));
    assert(clay_json_object_get(properties, "allow_custom"));
    ClayJson *required = clay_json_object_get(schema, "required");
    assert(clay_json_array_count(required) == 1);
    assert(strcmp(clay_json_string_value(clay_json_array_get(required, 0)), "question") == 0);
    ClayJson *option = clay_json_object_get(clay_json_object_get(properties, "options"), "items");
    assert(clay_json_object_get(clay_json_object_get(option, "properties"), "label"));
    clay_json_free(schema);

    printf("ask_user tests passed\n");
    return 0;
}
