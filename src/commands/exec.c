#include "context.h"

#include <stdio.h>
#include <stdlib.h>

#define CLAY_EXEC_OUTPUT_LIMIT (64 * 1024)

void clay_cmd_exec(const char *args, void *user_data) {
    ClayCommands *commands = user_data;
    if (!args || !*args) {
        clay_sayc(CLAY_RED, "Usage: /exec <shell command>");
        return;
    }
    if (!commands->chat) {
        commands->chat = clay_chat_create(commands->system_prompt);
        if (!commands->chat) {
            clay_sayc(CLAY_RED, "Could not create a chat journal.");
            return;
        }
    }

    char *workspace_dir = clay_term_cwd();
    char *scratch_dir = clay_chat_scratch_dir(commands->chat);
    size_t readonly_mount_count = 0;
    char **readonly_mounts = clay_config_sandbox_readonly_mounts(&readonly_mount_count);
    ClaySandboxConfig sandbox = {
        .mode = commands->sandbox_mode,
        .workspace_dir = workspace_dir,
        .scratch_dir = scratch_dir,
        .use_integrated_shell = commands->use_integrated_shell &&
                                commands->sandbox_mode == CLAY_SANDBOX_MODE_SANDBOX,
        .readonly_mounts = (const char *const *)readonly_mounts,
        .readonly_mount_count = readonly_mount_count,
    };
#ifdef _WIN32
    sandbox.use_integrated_shell = commands->use_integrated_shell;
#endif

    ClayTask *task = clay_app_task_start(commands->app, "$ %s", args);
    ClayStr output;
    clay_str_init(&output);
    ClayExecResult exec = {0};
    int rc = clay_sandbox_exec(&sandbox, args, &output, CLAY_EXEC_OUTPUT_LIMIT, NULL, &exec);
    int exit_code = exec.exit_code;
    free(workspace_dir);
    free(scratch_dir);
    for (size_t i = 0; i < readonly_mount_count; i++) free(readonly_mounts[i]);
    free(readonly_mounts);

    if (rc != 0) clay_app_task_fail(commands->app, task, "failed to start");
    else if (exit_code == 0) clay_app_task_success(commands->app, task, "exit 0");
    else clay_app_task_fail(commands->app, task, "exit %d", exit_code);

    if (*output.data) printf("%s\n", output.data);
    else clay_list_bullet("(no output)");
    if (exec.output_truncated) clay_sayc(CLAY_GRAY, "(output truncated)");
    clay_str_free(&output);
}
