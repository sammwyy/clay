#include "clay/sandbox.h"

#include "clay/term.h"

int clay_sandbox_supported(void) {
    return 0;
}

int clay_sandbox_exec(const ClaySandboxConfig *config, const char *command, ClayStr *output,
                      size_t output_limit, int *exit_code, int *output_truncated) {
    (void)config;
    return clay_term_shell_exec(command, output, output_limit, exit_code, output_truncated);
}
