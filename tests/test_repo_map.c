#include "../src/commands/context.h"

#include "clay/chat.h"
#include "clay/json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(content, file);
    fclose(file);
}

/* Asserts the map has "helper" ranked above "main" (helper is called three
   times, main isn't called at all) and that both def sites were found. */
static void check_output(const char *output) {
    assert(strstr(output, "helper"));
    assert(strstr(output, "main.c"));
    const char *helper_pos = strstr(output, " helper\t(refs:");
    const char *main_pos = strstr(output, " main\t(refs:");
    assert(helper_pos && main_pos);
    assert(helper_pos < main_pos);
}

int main(void) {
    char home_template[] = "/tmp/clay_test_repo_map_home_XXXXXX";
    char *home = mkdtemp(home_template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    char ws_template[] = "/tmp/clay_test_repo_map_ws_XXXXXX";
    char *workspace = mkdtemp(ws_template);
    assert(workspace);
    assert(chdir(workspace) == 0);

    char c_path[512];
    snprintf(c_path, sizeof(c_path), "%s/main.c", workspace);
    write_file(c_path,
              "int helper(int x) {\n"
              "    return x + 1;\n"
              "}\n"
              "\n"
              "int main(int argc, char **argv) {\n"
              "    return helper(argc) + helper(argc) + helper(argc);\n"
              "}\n");

    char py_path[512];
    snprintf(py_path, sizeof(py_path), "%s/util.py", workspace);
    write_file(py_path, "def unused_helper():\n    pass\n");

    ClayChat *chat = clay_chat_create("test");
    assert(chat);

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    commands.chat = chat;
    commands.sandbox_mode = CLAY_SANDBOX_MODE_UNLEASHED;

    /* Path 1: with ctags on PATH (whatever the host has installed). */
    char *original_path = getenv("PATH");
    char *original_path_copy = original_path ? strdup(original_path) : NULL;

    ClayJson *result = clay_fs_tool_repo_map(NULL, &commands);
    assert(clay_json_bool_value(clay_json_object_get(result, "ok")));
    const char *source = clay_json_string_value(clay_json_object_get(result, "source"));
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(source && (strcmp(source, "ctags") == 0 || strcmp(source, "heuristic") == 0));
    check_output(output);
    clay_json_free(result);

    /* Path 2: force the heuristic fallback by hiding ctags from PATH. */
    assert(setenv("PATH", "/nonexistent-clay-test-path", 1) == 0);
    result = clay_fs_tool_repo_map(NULL, &commands);
    assert(clay_json_bool_value(clay_json_object_get(result, "ok")));
    source = clay_json_string_value(clay_json_object_get(result, "source"));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strcmp(source, "heuristic") == 0);
    check_output(output);
    assert(strstr(output, "util.py"));
    clay_json_free(result);

    if (original_path_copy) {
        setenv("PATH", original_path_copy, 1);
        free(original_path_copy);
    }

    clay_chat_destroy(chat);
    remove(c_path);
    remove(py_path);
    assert(chdir("/tmp") == 0);
    rmdir(workspace);

    printf("repo map tests passed\n");
    return 0;
}
