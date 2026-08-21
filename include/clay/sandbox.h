#ifndef CLAY_SANDBOX_H
#define CLAY_SANDBOX_H

#include "clay/str.h"

#include <stddef.h>

typedef enum {
    CLAY_SANDBOX_MODE_SANDBOX,
    CLAY_SANDBOX_MODE_UNLEASHED,
} ClaySandboxMode;

typedef enum {
    CLAY_SANDBOX_ACCESS_READONLY,
    CLAY_SANDBOX_ACCESS_WRITABLE,
} ClaySandboxAccess;

typedef struct {
    ClaySandboxMode mode;
    ClaySandboxAccess access;
    const char *workspace_dir; /* bound to /workspace */
    const char *scratch_dir;   /* bound to /scratch; /tmp points at it too */
} ClaySandboxConfig;

/* True if this platform can run CLAY_SANDBOX_MODE_SANDBOX. Linux only for
   now; Windows always returns 0 and is forced to Unleashed. */
int clay_sandbox_supported(void);

/* Same contract as clay_term_shell_exec: runs `command` via a shell,
   appending combined stdout/stderr to output up to output_limit. In
   CLAY_SANDBOX_MODE_UNLEASHED this is exactly clay_term_shell_exec; in
   CLAY_SANDBOX_MODE_SANDBOX (Linux only) it namespaces the process and
   remaps the filesystem per `config`. Fails closed: if sandbox setup
   itself fails, the command is never run unsandboxed. */
int clay_sandbox_exec(const ClaySandboxConfig *config, const char *command, ClayStr *output,
                       size_t output_limit, int *exit_code, int *output_truncated);

#endif /* CLAY_SANDBOX_H */
