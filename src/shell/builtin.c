#include "builtin.h"

#ifdef _WIN32
#include <string.h>
int clay_shell_builtin_exists(const char *name) { (void)name; return 0; }
int clay_shell_builtin_run(ClayShell *shell, ClayShellCommand *command) { (void)shell; (void)command; return 127; }
#else
#include <stdio.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int clay_shell_builtin_ls(ClayShellCommand *command);
int clay_shell_builtin_cat(ClayShellCommand *command);
int clay_shell_builtin_rm(ClayShellCommand *command);
int clay_shell_builtin_cp(ClayShellCommand *command);
int clay_shell_builtin_mv(ClayShellCommand *command);
int clay_shell_builtin_mkdir(ClayShellCommand *command);
int clay_shell_builtin_touch(ClayShellCommand *command);

typedef int (*Builtin)(ClayShell *, ClayShellCommand *);
typedef struct { const char *name; Builtin run; } Entry;

static int run_ls(ClayShell *shell, ClayShellCommand *command) { (void)shell; return clay_shell_builtin_ls(command); }
static int run_cat(ClayShell *shell, ClayShellCommand *command) { (void)shell; return clay_shell_builtin_cat(command); }
static int run_rm(ClayShell *shell, ClayShellCommand *command) { (void)shell; return clay_shell_builtin_rm(command); }
static int run_cp(ClayShell *shell, ClayShellCommand *command) { (void)shell; return clay_shell_builtin_cp(command); }
static int run_mv(ClayShell *shell, ClayShellCommand *command) { (void)shell; return clay_shell_builtin_mv(command); }
static int run_mkdir(ClayShell *shell, ClayShellCommand *command) { (void)shell; return clay_shell_builtin_mkdir(command); }
static int run_touch(ClayShell *shell, ClayShellCommand *command) { (void)shell; return clay_shell_builtin_touch(command); }
static int run_cd(ClayShell *shell, ClayShellCommand *command) { (void)shell; const char *path = command->argc > 1 ? command->argv[1] : getenv("HOME"); if (!path || chdir(path)) { perror("cd"); return 1; } return 0; }
static int run_pwd(ClayShell *shell, ClayShellCommand *command) { (void)shell; (void)command; char *cwd = getcwd(NULL, 0); if (!cwd) { perror("pwd"); return 1; } puts(cwd); free(cwd); return 0; }
static int run_echo(ClayShell *shell, ClayShellCommand *command) { (void)shell; size_t i = 1; int newline = 1, first = 1; if (i < command->argc && !strcmp(command->argv[i], "-n")) { newline = 0; i++; } for (; i < command->argc; i++) { printf("%s%s", first ? "" : " ", command->argv[i]); first = 0; } if (newline) putchar('\n'); return 0; }
static int run_true(ClayShell *shell, ClayShellCommand *command) { (void)shell; (void)command; return 0; }
static int run_false(ClayShell *shell, ClayShellCommand *command) { (void)shell; (void)command; return 1; }
static int run_exit(ClayShell *shell, ClayShellCommand *command) { shell->should_exit = 1; shell->exit_status = command->argc > 1 ? atoi(command->argv[1]) : 0; return shell->exit_status; }
static int run_export(ClayShell *shell, ClayShellCommand *command) { (void)shell; for (size_t i = 1; i < command->argc; i++) { char *equal = strchr(command->argv[i], '='); if (!equal) { fprintf(stderr, "export: expected NAME=value\n"); return 1; } *equal = 0; int rc = setenv(command->argv[i], equal + 1, 1); *equal = '='; if (rc) { perror("export"); return 1; } } return 0; }
static int run_unset(ClayShell *shell, ClayShellCommand *command) { (void)shell; for (size_t i = 1; i < command->argc; i++) if (unsetenv(command->argv[i])) { perror("unset"); return 1; } return 0; }
static int run_env(ClayShell *shell, ClayShellCommand *command) { (void)shell; (void)command; extern char **environ; for (char **p = environ; *p; p++) puts(*p); return 0; }
static int run_whoami(ClayShell *shell, ClayShellCommand *command) { (void)shell; (void)command; struct passwd *account = getpwuid(getuid()); if (!account) return 1; puts(account->pw_name); return 0; }
static int run_hostname(ClayShell *shell, ClayShellCommand *command) { (void)shell; (void)command; char host[256] = "host"; if (gethostname(host, sizeof(host) - 1)) return 1; puts(host); return 0; }
static int run_which(ClayShell *shell, ClayShellCommand *command) {
    (void)shell;
    const char *path = getenv("PATH"); int status = 0;
    if (command->argc == 1) { fputs("which: missing command\n", stderr); return 1; }
    for (size_t i = 1; i < command->argc; i++) {
        if (clay_shell_builtin_exists(command->argv[i])) { printf("%s: shell builtin\n", command->argv[i]); continue; }
        int found = 0; const char *part = path;
        while (part && *part) { const char *end = strchr(part, ':'); size_t len = end ? (size_t)(end - part) : strlen(part); size_t size = len + strlen(command->argv[i]) + 2; char *candidate = malloc(size); if (!candidate) return 1; snprintf(candidate, size, "%.*s/%s", (int)len, part, command->argv[i]); if (access(candidate, X_OK) == 0) { puts(candidate); found = 1; free(candidate); break; } free(candidate); part = end ? end + 1 : NULL; }
        if (!found) { fprintf(stderr, "which: %s: not found\n", command->argv[i]); status = 1; }
    }
    return status;
}
static int run_help(ClayShell *shell, ClayShellCommand *command) {
    (void)shell; (void)command;
    puts("shell: clay parser; builtins run in-process and external binaries run directly (never via sh -c)");
    puts("syntax: quotes, escapes, |, &&, ||, ;, &, <, >, >>");
    puts("builtins:");
    puts("  ls [-al] [--all] [--long] [--] [PATH ...]");
    puts("  cat [-n] [--number] [--] [FILE ...]");
    puts("  cp [-fn] [--no-clobber] SOURCE DESTINATION");
    puts("  mv [-fn] [--no-clobber] SOURCE DESTINATION");
    puts("  rm [-rfR] [--recursive] [--force] [--] PATH ...");
    puts("  mkdir [-p] [--parents] [--] PATH ...");
    puts("  touch [--] FILE ...");
    puts("  cd [PATH] | pwd | echo [-n] [TEXT ...] | export NAME=VALUE | unset NAME ...");
    puts("  env | whoami | hostname | which COMMAND ... | type COMMAND ...");
    puts("  true | false | exit [STATUS] | help");
    return 0;
}

static const Entry BUILTINS[] = {
    {"cat", run_cat}, {"cd", run_cd}, {"cp", run_cp}, {"echo", run_echo}, {"env", run_env}, {"exit", run_exit}, {"false", run_false}, {"help", run_help}, {"hostname", run_hostname}, {"ls", run_ls}, {"mkdir", run_mkdir}, {"mv", run_mv}, {"pwd", run_pwd}, {"rm", run_rm}, {"touch", run_touch}, {"true", run_true}, {"type", run_which}, {"unset", run_unset}, {"export", run_export}, {"which", run_which}, {"whoami", run_whoami},
};
int clay_shell_builtin_exists(const char *name) { for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) if (!strcmp(name, BUILTINS[i].name)) return 1; return 0; }
int clay_shell_builtin_run(ClayShell *shell, ClayShellCommand *command) { for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) if (!strcmp(command->argv[0], BUILTINS[i].name)) return BUILTINS[i].run(shell, command); return 127; }
#endif
