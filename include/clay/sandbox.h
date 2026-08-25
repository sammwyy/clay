#ifndef CLAY_SANDBOX_H
#define CLAY_SANDBOX_H

#include "clay/term.h"

#include <stddef.h>

typedef enum {
    CLAY_SANDBOX_MODE_SANDBOX,
    CLAY_SANDBOX_MODE_UNLEASHED,
} ClaySandboxMode;

/* User and network namespaces held open for the whole session. Sandboxed
   commands after the first join them, so a server one command starts is
   reachable from the next; everything else (filesystem, pids, ipc) stays
   private per command. NULL gives every command its own isolated set. */
typedef struct ClaySandboxNamespaces ClaySandboxNamespaces;
ClaySandboxNamespaces *clay_sandbox_namespaces_create(void);
void clay_sandbox_namespaces_destroy(ClaySandboxNamespaces *namespaces);

typedef struct {
    ClaySandboxMode mode;
    const char *workspace_dir; /* bound to /workspace */
    const char *scratch_dir;   /* bound at /tmp/<session>; /scratch aliases it */
    int use_integrated_shell;
    const char *const *readonly_mounts;
    size_t readonly_mount_count;
    ClaySandboxNamespaces *shared; /* optional; see above */
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
                       size_t output_limit, const ClayExecOptions *options, ClayExecResult *result);

#endif /* CLAY_SANDBOX_H */
