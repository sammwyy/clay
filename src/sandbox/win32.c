#include "clay/sandbox.h"
#include "clay/shell.h"

#include "clay/term.h"

#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

static int integrated_exec(const char *command, ClayStr *output, size_t output_limit,
                           int *exit_code, int *output_truncated) {
    FILE *capture = tmpfile();
    if (!capture) return -1;
    char *cwd = _getcwd(NULL, 0);
    int saved_out = _dup(_fileno(stdout)), saved_err = _dup(_fileno(stderr));
    if (saved_out < 0 || saved_err < 0 || _dup2(_fileno(capture), _fileno(stdout)) < 0 ||
        _dup2(_fileno(capture), _fileno(stderr)) < 0) { fclose(capture); free(cwd); return -1; }
    *exit_code = clay_shell_run_command(command);
    fflush(stdout); fflush(stderr);
    _dup2(saved_out, _fileno(stdout)); _dup2(saved_err, _fileno(stderr));
    _close(saved_out); _close(saved_err);
    if (cwd) { _chdir(cwd); free(cwd); }
    rewind(capture);
    char buffer[4096]; size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), capture)) != 0) {
        size_t remaining = output->len < output_limit ? output_limit - output->len : 0;
        size_t kept = count < remaining ? count : remaining;
        if (kept) clay_str_push_n(output, buffer, kept);
        if (kept != count) *output_truncated = 1;
    }
    fclose(capture);
    return 0;
}

int clay_sandbox_supported(void) {
    return 0;
}

int clay_sandbox_exec(const ClaySandboxConfig *config, const char *command, ClayStr *output,
                      size_t output_limit, int *exit_code, int *output_truncated) {
    if (config->use_integrated_shell)
        return integrated_exec(command, output, output_limit, exit_code, output_truncated);
    return clay_term_shell_exec(command, output, output_limit, exit_code, output_truncated);
}
