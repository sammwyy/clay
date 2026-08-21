#include "clay/checkpoint.h"
#include "clay/str.h"

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

static char *read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    ClayStr text;
    clay_str_init(&text);
    int ch;
    while ((ch = fgetc(file)) != EOF) clay_str_push_char(&text, (char)ch);
    fclose(file);
    return text.data;
}

int main(void) {
    char workspace_template[] = "/tmp/clay_test_ckpt_ws_XXXXXX";
    char checkpoints_template[] = "/tmp/clay_test_ckpt_repo_XXXXXX";
    char *workspace = mkdtemp(workspace_template);
    char *checkpoints = mkdtemp(checkpoints_template);
    assert(workspace && checkpoints);

    ClayStr file_path;
    clay_str_init(&file_path);
    clay_str_printf(&file_path, "%s/a.txt", workspace);

    write_file(file_path.data, "v1\n");
    assert(clay_checkpoint_save(checkpoints, workspace, "write a.txt (v1)") == 0);

    /* A second save with no filesystem changes is a no-op: still one entry. */
    assert(clay_checkpoint_save(checkpoints, workspace, "no-op") == 0);

    ClayArray list;
    assert(clay_checkpoint_list(checkpoints, &list) == 0);
    assert(list.count == 1);
    ClayCheckpoint *first = clay_array_get(&list, 0);
    assert(strcmp(first->label, "write a.txt (v1)") == 0);
    char *first_commit = strdup(first->commit);
    clay_checkpoint_list_free(&list);

    write_file(file_path.data, "v2\n");
    assert(clay_checkpoint_save(checkpoints, workspace, "write a.txt (v2)") == 0);

    assert(clay_checkpoint_list(checkpoints, &list) == 0);
    assert(list.count == 2);
    /* Newest first. */
    ClayCheckpoint *newest = clay_array_get(&list, 0);
    assert(strcmp(newest->label, "write a.txt (v2)") == 0);
    clay_checkpoint_list_free(&list);

    char *content = read_file(file_path.data);
    assert(strcmp(content, "v2\n") == 0);
    free(content);

    assert(clay_checkpoint_restore(checkpoints, workspace, first_commit) == 0);
    content = read_file(file_path.data);
    assert(strcmp(content, "v1\n") == 0);
    free(content);
    free(first_commit);

    clay_str_free(&file_path);
    printf("checkpoint tests passed\n");
    return 0;
}
