#include "../src/commands/context.h"

#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(content, file);
    fclose(file);
}

static char *joined_path(const char *dir, const char *name) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/%s", dir, name);
    return path.data;
}

int main(void) {
    char template[] = "/tmp/clay_test_env_block_XXXXXX";
    char *workspace = mkdtemp(template);
    assert(workspace);

    /* Not a git repo: no branch. */
    assert(chdir(workspace) == 0);
    char *branch = clay_commands_find_git_branch();
    assert(branch == NULL);

    /* A normal branch ref. */
    char *git_dir = joined_path(workspace, ".git");
    assert(mkdir(git_dir, 0755) == 0);
    char *head_path = joined_path(git_dir, "HEAD");
    write_file(head_path, "ref: refs/heads/feature/repo-map\n");
    branch = clay_commands_find_git_branch();
    assert(branch);
    assert(strcmp(branch, "feature/repo-map") == 0);
    free(branch);

    /* Detached HEAD: a short SHA. */
    write_file(head_path, "3f9a2c4b8d1e0a5c7b6f4d2e1a0c9b8d7e6f5a4b\n");
    branch = clay_commands_find_git_branch();
    assert(branch);
    assert(strcmp(branch, "3f9a2c4") == 0);
    free(branch);

    /* Works from a subdirectory too (walks up to find .git). */
    char *sub = joined_path(workspace, "sub");
    assert(mkdir(sub, 0755) == 0);
    assert(chdir(sub) == 0);
    write_file(head_path, "ref: refs/heads/main\n");
    branch = clay_commands_find_git_branch();
    assert(branch);
    assert(strcmp(branch, "main") == 0);
    free(branch);

    /* Directory listing: sorted, comma-separated. */
    assert(chdir(workspace) == 0);
    char *a_path = joined_path(workspace, "a.txt");
    char *z_path = joined_path(workspace, "z.txt");
    write_file(a_path, "a");
    write_file(z_path, "z");
    char *listing = clay_commands_list_top_level(workspace);
    assert(listing);
    assert(strstr(listing, "a.txt"));
    assert(strstr(listing, "z.txt"));
    assert(strstr(listing, ".git"));
    /* Sorted: "a.txt" appears before "z.txt". */
    assert(strstr(listing, "a.txt") < strstr(listing, "z.txt"));
    free(listing);

    remove(a_path);
    remove(z_path);
    remove(head_path);
    assert(rmdir(git_dir) == 0);
    assert(rmdir(sub) == 0);
    assert(chdir("/tmp") == 0);
    assert(rmdir(workspace) == 0);

    free(a_path);
    free(z_path);
    free(head_path);
    free(git_dir);
    free(sub);

    printf("environment block tests passed\n");
    return 0;
}
