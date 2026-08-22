#define _GNU_SOURCE

#include "clay/array.h"
#include "clay/storage.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

int main(void) {
    char template[] = "/tmp/clay_test_storage_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    char *root = clay_storage_root();
    assert(root);
    assert(strstr(root, "/.clay") != NULL);
    free(root);

    assert(clay_storage_ensure_dir("providers/nested") == 0);
    char *path = clay_storage_path("providers/nested/config.json");
    assert(path);
    assert(clay_storage_write_atomic_private(path, "hello", 5) == 0);

    ClayStr body;
    assert(clay_storage_read_limited(path, 5, &body) == 0);
    assert(body.len == 5 && memcmp(body.data, "hello", 5) == 0);
    clay_str_free(&body);
    assert(clay_storage_read_limited(path, 4, &body) != 0);
    assert(errno == EFBIG);
    remove(path);
    free(path);

    ClayStr overflow = {0};
    overflow.data = malloc(1);
    overflow.cap = 1;
    overflow.len = SIZE_MAX;
    clay_str_push_char(&overflow, 'x');
    assert(overflow.len == SIZE_MAX);
    clay_str_free(&overflow);

    ClayArray array = {0};
    array.elem_size = 2;
    array.count = SIZE_MAX;
    array.cap = SIZE_MAX;
    assert(clay_array_push(&array) == NULL);
    free(array.data);

    assert(chdir("/tmp") == 0);
    char nested[512], providers[512], clay[512];
    snprintf(nested, sizeof(nested), "%s/.clay/providers/nested", home);
    snprintf(providers, sizeof(providers), "%s/.clay/providers", home);
    snprintf(clay, sizeof(clay), "%s/.clay", home);
    assert(rmdir(nested) == 0);
    assert(rmdir(providers) == 0);
    assert(rmdir(clay) == 0);
    assert(rmdir(home) == 0);
    printf("storage tests passed\n");
    return 0;
}
