#define _GNU_SOURCE

#include "../src/commands/context.h"

#include "clay/term.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

static void write_file(const char *path, const char *content) {
    assert(clay_term_write_file_atomic(path, content, strlen(content)) == 0);
}

static void assert_file(const char *path, const char *expected) {
    ClayStr body;
    assert(clay_term_read_file(path, 1024, &body) == 0);
    assert(body.len == strlen(expected));
    assert(memcmp(body.data, expected, body.len) == 0);
    clay_str_free(&body);
}

int main(void) {
    char template[] = "/tmp/clay_test_undo_XXXXXX";
    char *workspace = mkdtemp(template);
    assert(workspace);
    assert(chdir(workspace) == 0);

    ClayCommands commands = {0};
    clay_array_init(&commands.undo_history, sizeof(ClayUndoEntry));

    write_file("existing.txt", "before");
    assert(clay_commands_undo_prepare(&commands, "existing.txt"));
    write_file("existing.txt", "after");
    clay_commands_undo_commit(&commands);
    assert(commands.undo_history.count == 1);
    clay_cmd_undo("", &commands);
    assert_file("existing.txt", "before");
    assert(commands.undo_history.count == 0);

    assert(clay_commands_undo_prepare(&commands, "new.txt"));
    write_file("new.txt", "new");
    clay_commands_undo_commit(&commands);
    clay_cmd_undo("", &commands);
    assert(access("new.txt", F_OK) != 0);

    write_file("changed.txt", "one");
    assert(clay_commands_undo_prepare(&commands, "changed.txt"));
    write_file("changed.txt", "two");
    clay_commands_undo_commit(&commands);
    write_file("changed.txt", "external");
    clay_cmd_undo("", &commands);
    assert_file("changed.txt", "external");
    assert(commands.undo_history.count == 1);

    clay_commands_undo_destroy(&commands);
    remove("existing.txt");
    remove("changed.txt");
    assert(chdir("/tmp") == 0);
    rmdir(workspace);
    printf("undo tests passed\n");
    return 0;
}
