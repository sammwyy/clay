#ifndef CLAY_SHELL_H
#define CLAY_SHELL_H

typedef int (*ClayShellAuthorize)(char *const argv[], void *user_data);

/* Experimental, deliberately small command interpreter. It never invokes a
   system shell: parsed programs are executed directly with execvp(). */
int clay_shell_main(int argc, char **argv);

/* Parses every simple command without executing it. Authorization may return
   false to stop before any command has run. */
int clay_shell_authorize(const char *command, ClayShellAuthorize authorize,
                         void *user_data);

/* Executes a parsed command in the current process. Intended for a sandbox
   child after authorization happened in the parent UI process. */
int clay_shell_run_command(const char *command);

#endif /* CLAY_SHELL_H */
