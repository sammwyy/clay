#include "clay/sandbox.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run(ClaySandboxMode mode, ClaySandboxAccess access, const char *workspace, const char *scratch,
               const char *command, ClayStr *output, int *exit_code) {
    ClaySandboxConfig config = {mode, access, workspace, scratch};
    int truncated = 0;
    clay_str_clear(output);
    return clay_sandbox_exec(&config, command, output, 64 * 1024, exit_code, &truncated);
}

int main(void) {
    if (!clay_sandbox_supported()) {
        printf("sandbox not supported on this platform, skipping\n");
        return 0;
    }

    char workspace_template[] = "/tmp/clay_test_workspace_XXXXXX";
    char scratch_template[] = "/tmp/clay_test_scratch_XXXXXX";
    char *workspace = mkdtemp(workspace_template);
    char *scratch = mkdtemp(scratch_template);
    assert(workspace && scratch);

    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;

    assert(run(CLAY_SANDBOX_MODE_SANDBOX, CLAY_SANDBOX_ACCESS_READONLY, workspace, scratch,
              "echo hi > /workspace/marker && cat /workspace/marker", &output, &exit_code) == 0);
    assert(exit_code == 0);
    assert(strstr(output.data, "hi") != NULL);

    ClayStr marker_path;
    clay_str_init(&marker_path);
    clay_str_printf(&marker_path, "%s/marker", workspace);
    FILE *marker = fopen(marker_path.data, "r");
    assert(marker != NULL);
    fclose(marker);
    clay_str_free(&marker_path);

    assert(run(CLAY_SANDBOX_MODE_SANDBOX, CLAY_SANDBOX_ACCESS_READONLY, workspace, scratch,
              "echo scratch-ok > /scratch/note && cat /tmp/note", &output, &exit_code) == 0);
    assert(exit_code == 0);
    assert(strstr(output.data, "scratch-ok") != NULL);

    assert(run(CLAY_SANDBOX_MODE_SANDBOX, CLAY_SANDBOX_ACCESS_READONLY, workspace, scratch,
              "touch \"$HOME/clay_sandbox_test_should_fail\"", &output, &exit_code) == 0);
    assert(exit_code != 0);

    assert(run(CLAY_SANDBOX_MODE_UNLEASHED, CLAY_SANDBOX_ACCESS_READONLY, workspace, scratch, "echo unleashed-ok",
              &output, &exit_code) == 0);
    assert(exit_code == 0);
    assert(strstr(output.data, "unleashed-ok") != NULL);

    clay_str_free(&output);
    printf("sandbox tests passed\n");
    return 0;
}
