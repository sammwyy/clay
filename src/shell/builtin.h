#ifndef CLAY_SHELL_BUILTIN_H
#define CLAY_SHELL_BUILTIN_H

#include <stddef.h>

typedef struct {
    char **argv;
    size_t argc;
    char *in;
    char *out;
    int append;
} ClayShellCommand;

typedef struct {
    int should_exit;
    int exit_status;
} ClayShell;

typedef struct {
    unsigned long long flags;
    size_t first;
} ClayShellOptions;

/* Parses POSIX-style short clusters (-alR), a terminating --, and the listed
   long aliases. `short_flags` contains valid short letters. */
int clay_shell_options(const ClayShellCommand *command, const char *short_flags,
                       const char *const *long_flags, ClayShellOptions *options);
int clay_shell_option_seen(const ClayShellOptions *options, char flag);

int clay_shell_builtin_exists(const char *name);
int clay_shell_builtin_run(ClayShell *shell, ClayShellCommand *command);

int clay_shell_copy_file(const char *source, const char *destination, int no_clobber);
int clay_shell_remove_path(const char *path, int recursive, int force);

#endif
