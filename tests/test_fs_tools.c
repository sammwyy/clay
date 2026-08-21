#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef ClayJson *(*ClayFsTool)(const ClayJson *arguments, void *userdata);

static ClayJson *call(ClayFsTool fn, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    ClayJson *result = fn(args, NULL);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

int main(void) {
    char template[] = "/tmp/clay_test_fs_XXXXXX";
    char *workspace = mkdtemp(template);
    assert(workspace);
    assert(chdir(workspace) == 0);

    ClayJson *result = call(clay_fs_tool_write, "{\"path\":\"a.txt\",\"content\":\"line1\\nline2\\n\"}");
    assert(ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_read, "{\"path\":\"a.txt\"}");
    assert(ok(result));
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "line1"));
    assert(strstr(output, "line2"));
    clay_json_free(result);

    /* Parent directories are created automatically. */
    result = call(clay_fs_tool_write, "{\"path\":\"sub/dir/b.c\",\"content\":\"int main(){return 0;}\\n\"}");
    assert(ok(result));
    clay_json_free(result);

    /* A path escaping the workspace is rejected. */
    result = call(clay_fs_tool_read, "{\"path\":\"../outside\"}");
    assert(!ok(result));
    clay_json_free(result);
    result = call(clay_fs_tool_write, "{\"path\":\"../outside.txt\",\"content\":\"x\"}");
    assert(!ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_glob, "{\"pattern\":\"*.txt\"}");
    assert(ok(result));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "a.txt"));
    assert(!strstr(output, "b.c"));
    clay_json_free(result);

    result = call(clay_fs_tool_glob, "{\"pattern\":\"*.c\"}");
    assert(ok(result));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "sub/dir/b.c"));
    clay_json_free(result);

    /* oldString absent from the file. */
    result = call(clay_fs_tool_edit, "{\"path\":\"a.txt\",\"oldString\":\"nope\",\"newString\":\"x\"}");
    assert(!ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_edit, "{\"path\":\"a.txt\",\"oldString\":\"line1\",\"newString\":\"LINE_ONE\"}");
    assert(ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_read, "{\"path\":\"a.txt\"}");
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "LINE_ONE"));
    assert(!strstr(output, "\tline1\n"));
    clay_json_free(result);

    result = call(clay_fs_tool_write, "{\"path\":\"dup.txt\",\"content\":\"foo foo foo\"}");
    assert(ok(result));
    clay_json_free(result);

    /* Ambiguous oldString is rejected without replaceAll. */
    result = call(clay_fs_tool_edit, "{\"path\":\"dup.txt\",\"oldString\":\"foo\",\"newString\":\"bar\"}");
    assert(!ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_edit,
                  "{\"path\":\"dup.txt\",\"oldString\":\"foo\",\"newString\":\"bar\",\"replaceAll\":true}");
    assert(ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_read, "{\"path\":\"dup.txt\"}");
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "bar bar bar"));
    clay_json_free(result);

    remove("a.txt");
    remove("dup.txt");
    remove("sub/dir/b.c");
    rmdir("sub/dir");
    rmdir("sub");
    assert(chdir("/tmp") == 0);
    rmdir(workspace);

    printf("fs_tools tests passed\n");
    return 0;
}
