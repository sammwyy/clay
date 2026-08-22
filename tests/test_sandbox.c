#include "clay/sandbox.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run(ClaySandboxMode mode, const char *workspace, const char *scratch, const char *command, ClayStr *output,
               int *exit_code) {
    ClaySandboxConfig config = {.mode = mode, .workspace_dir = workspace, .scratch_dir = scratch};
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

    int rc = run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch, "echo hi > /workspace/marker && cat /workspace/marker",
                 &output, &exit_code);
    if (rc != 0 || exit_code == 126) {
        printf("sandbox unavailable on this host, skipping\n");
        clay_str_free(&output);
        return 0;
    }
    assert(rc == 0);
    assert(exit_code == 0);
    assert(strstr(output.data, "hi") != NULL);

    ClayStr marker_path;
    clay_str_init(&marker_path);
    clay_str_printf(&marker_path, "%s/marker", workspace);
    FILE *marker = fopen(marker_path.data, "r");
    assert(marker != NULL);
    fclose(marker);
    clay_str_free(&marker_path);

    const char *scratch_name = strrchr(scratch, '/');
    scratch_name = scratch_name ? scratch_name + 1 : scratch;
    ClayStr scratch_command;
    clay_str_init(&scratch_command);
    clay_str_printf(&scratch_command,
                    "echo scratch-ok > /scratch/note && test -f /tmp/%s/note && cat /tmp/%s/note && "
                    "test \"$TMPDIR\" = /tmp/%s",
                    scratch_name, scratch_name, scratch_name);
    assert(run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch, scratch_command.data, &output, &exit_code) == 0);
    clay_str_free(&scratch_command);
    assert(exit_code == 0);
    assert(strstr(output.data, "scratch-ok") != NULL);

    assert(run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch,
               "test ! -e /home && test \"$HOME\" = /scratch && test -z \"$OPENAI_API_KEY\"", &output, &exit_code) == 0);
    assert(exit_code == 0);

    assert(run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch,
               "test \"$(ulimit -u)\" -le 64 && test \"$(ulimit -v)\" -le 1048576", &output, &exit_code) == 0);
    assert(exit_code == 0);

    assert(run(CLAY_SANDBOX_MODE_UNLEASHED, workspace, scratch, "echo unleashed-ok", &output, &exit_code) == 0);
    assert(exit_code == 0);
    assert(strstr(output.data, "unleashed-ok") != NULL);

    clay_str_free(&output);
    printf("sandbox tests passed\n");
    return 0;
}
