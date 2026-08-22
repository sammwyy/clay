#include "clay/shell.h"
#include "builtin.h"

#ifdef _WIN32

#include <direct.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

static char *win_name_copy(const char *value, size_t length) {
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length);
    copy[length] = '\0';
    return copy;
}
static char *win_join_path(const char *path, const char *name) {
    size_t path_len = strlen(path), name_len = strlen(name);
    if (path_len > SIZE_MAX - 2 || name_len > SIZE_MAX - path_len - 2) return NULL;
    char *joined = malloc(path_len + name_len + 2);
    if (!joined) return NULL;
    memcpy(joined, path, path_len);
    joined[path_len] = '\\';
    memcpy(joined + path_len + 1, name, name_len + 1);
    return joined;
}
static void win_clear_clay_environment(void) {
    extern char **environ;
    for (char **entry = environ; entry && *entry;) {
        char *equal = strchr(*entry, '=');
        if (equal && (size_t)(equal - *entry) >= 5 && !strncmp(*entry, "CLAY_", 5)) {
            size_t len = (size_t)(equal - *entry);
            char *name = win_name_copy(*entry, len);
            if (!name) return;
            _putenv_s(name, "");
            free(name);
            entry = environ;
            continue;
        }
        entry++;
    }
}
static void win_prompt(void) {
    char user[256] = "user", host[256] = "host", cwd[MAX_PATH] = ".";
    DWORD user_len = sizeof(user), host_len = sizeof(host);
    GetUserNameA(user, &user_len); GetComputerNameA(host, &host_len); _getcwd(cwd, sizeof(cwd));
    printf("%s@%s:%s$ ", user, host, cwd);
}

static int win_copy(const char *source, const char *destination) {
    if (CopyFileA(source, destination, FALSE)) return 0;
    fprintf(stderr, "cp: %s\n", source); return 1;
}
static int win_remove(const char *path, int recursive) {
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "rm: %s\n", path); return 1; }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileA(path) ? 0 : 1;
    if (!recursive) { fprintf(stderr, "rm: %s is a directory (use -r)\n", path); return 1; }
    char pattern[MAX_PATH]; snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA data; HANDLE handle = FindFirstFileA(pattern, &data); int status = 0;
    if (handle != INVALID_HANDLE_VALUE) do {
        if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, "..")) continue;
        char *child = win_join_path(path, data.cFileName);
        if (!child) { status = 1; continue; }
        if (win_remove(child, 1)) status = 1;
        free(child);
    } while (FindNextFileA(handle, &data));
    if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
    return (!status && RemoveDirectoryA(path)) ? 0 : 1;
}
static int win_ls(const char *path) {
    char pattern[MAX_PATH]; snprintf(pattern, sizeof(pattern), "%s\\*", path ? path : ".");
    WIN32_FIND_DATAA data; HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) { fprintf(stderr, "ls: %s\n", path ? path : "."); return 1; }
    do { if (strcmp(data.cFileName, ".") && strcmp(data.cFileName, "..")) puts(data.cFileName); } while (FindNextFileA(handle, &data));
    FindClose(handle); return 0;
}
static int win_cat(const char *path) {
    FILE *file = fopen(path, "rb"); if (!file) { perror(path); return 1; }
    char buffer[32768]; size_t count; while ((count = fread(buffer, 1, sizeof(buffer), file))) fwrite(buffer, 1, count, stdout);
    fclose(file); return 0;
}
static int win_tokens(char *line, char **words, int capacity) {
    int count = 0, quote = 0; char *read = line, *write = line;
    while (*read) {
        while (*read == ' ' || *read == '\t' || *read == '\n') read++;
        if (!*read) break;
        if (count == capacity) return -1;
        words[count++] = write;
        while (*read && (quote || (*read != ' ' && *read != '\t' && *read != '\n'))) {
            if (*read == '"') { quote = !quote; read++; continue; }
            *write++ = *read++;
        }
        if (quote) return -1;
        *write++ = 0;
    }
    return count;
}
static int win_run(char *line, int *should_exit) {
    char *words[65]; int count = win_tokens(line, words, 64);
    if (count < 0) { fputs("clay shell: invalid syntax\n", stderr); return 1; }
    if (!count) return 0;
    if (!strcmp(words[0], "exit")) { *should_exit = 1; return count > 1 ? atoi(words[1]) : 0; }
    if (!strcmp(words[0], "help")) { puts("shell: clay native interpreter; builtins: ls cat cp mv rm mkdir touch cd pwd echo true false exit help"); puts("syntax: quoted words; options and -- are accepted by each builtin"); return 0; }
    if (!strcmp(words[0], "echo")) { for (int i = 1; i < count; i++) printf("%s%s", i > 1 ? " " : "", words[i]); putchar('\n'); return 0; }
    if (!strcmp(words[0], "true")) return 0;
    if (!strcmp(words[0], "false")) return 1;
    if (!strcmp(words[0], "pwd")) { char path[MAX_PATH]; return _getcwd(path, sizeof(path)) ? (puts(path), 0) : 1; }
    if (!strcmp(words[0], "cd")) return count == 2 && _chdir(words[1]) == 0 ? 0 : 1;
    if (!strcmp(words[0], "ls")) return win_ls(count > 1 ? words[1] : ".");
    if (!strcmp(words[0], "cat")) { if (count < 2) return 1; int status = 0; for (int i = 1; i < count; i++) if (win_cat(words[i])) status = 1; return status; }
    if (!strcmp(words[0], "cp")) return count == 3 ? win_copy(words[1], words[2]) : 1;
    if (!strcmp(words[0], "mv")) return count == 3 && MoveFileExA(words[1], words[2], MOVEFILE_REPLACE_EXISTING) ? 0 : 1;
    if (!strcmp(words[0], "rm")) { int recursive = count > 1 && (!strcmp(words[1], "-r") || !strcmp(words[1], "-R")); int start = recursive ? 2 : 1; int status = 0; for (int i = start; i < count; i++) if (win_remove(words[i], recursive)) status = 1; return start < count ? status : 1; }
    if (!strcmp(words[0], "mkdir")) return count == 2 && (_mkdir(words[1]) == 0 || errno == EEXIST) ? 0 : 1;
    if (!strcmp(words[0], "touch")) { if (count != 2) return 1; FILE *file = fopen(words[1], "ab"); if (!file) return 1; fclose(file); return 0; }
    words[count] = NULL;
    intptr_t status = _spawnvp(_P_WAIT, words[0], (const char *const *)words);
    if (status == -1) { perror(words[0]); return 127; }
    return (int)status;
}
int clay_shell_authorize(const char *command, ClayShellAuthorize authorize, void *user_data) {
    char *copy = strdup(command); char *words[65];
    if (!copy) return -1;
    int count = win_tokens(copy, words, 64);
    if (count >= 0) words[count] = NULL;
    int allowed = count >= 0 && (!count || !authorize || authorize(words, user_data));
    free(copy);
    return allowed ? 0 : -1;
}
int clay_shell_run_command(const char *command) {
    char *copy = strdup(command); int should_exit = 0;
    if (!copy) return 1;
    int status = win_run(copy, &should_exit); free(copy); return status;
}
int clay_shell_main(int argc, char **argv) {
    int should_exit = 0, status = 0;
    win_clear_clay_environment();
    if (argc) { size_t length = 1; for (int i = 0; i < argc; i++) length += strlen(argv[i]) + 1; char *line = calloc(length, 1); if (!line) return 1; for (int i = 0; i < argc; i++) { if (i) strcat(line, " "); strcat(line, argv[i]); } status = win_run(line, &should_exit); free(line); return status; }
    char line[4096]; while (!should_exit) { win_prompt(); fflush(stdout); if (!fgets(line, sizeof(line), stdin)) break; status = win_run(line, &should_exit); }
    return status;
}

#else

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

typedef enum { TOK_WORD, TOK_PIPE, TOK_AND, TOK_OR, TOK_SEMI, TOK_BG, TOK_IN, TOK_OUT, TOK_APPEND } TokenKind;
typedef struct { TokenKind kind; char *text; } Token;
typedef struct { Token *items; size_t count, cap; } Tokens;
typedef struct { ClayShellCommand base; size_t cap; } Command;
typedef struct { Command *commands; size_t count, cap; int background; } Pipeline;
typedef enum { NEXT_ALWAYS, NEXT_AND, NEXT_OR } Next;
typedef struct { Pipeline pipeline; Next next; } Statement;
typedef struct { Statement *items; size_t count, cap; } Program;

static void *grow(void *data, size_t *cap, size_t size, size_t need) {
    if (*cap >= need) return data;
    size_t next = *cap ? *cap * 2 : 8;
    while (next < need) next *= 2;
    void *result = realloc(data, next * size);
    if (!result) { perror("clay shell: allocation"); exit(1); }
    *cap = next;
    return result;
}
static void token_push(Tokens *tokens, TokenKind kind, char *text) {
    tokens->items = grow(tokens->items, &tokens->cap, sizeof(Token), tokens->count + 1);
    tokens->items[tokens->count++] = (Token){kind, text};
}
static void tokens_free(Tokens *tokens) {
    for (size_t i = 0; i < tokens->count; i++) free(tokens->items[i].text);
    free(tokens->items);
}
static int is_operator(char c) { return c == '|' || c == '&' || c == ';' || c == '<' || c == '>'; }

/* Keep the small AST for commands we understand.  POSIX expansions and
   descriptor redirections are delegated to Bash inside the sandbox; the
   caller marks that path as an opaque, explicitly-approved execution. */
static int needs_native_shell(const char *line) {
    for (const char *p = line; *p; p++) {
        if (*p == '`' || *p == '$' || *p == '*' || *p == '?' || *p == '[')
            return 1;
        if (*p == '~' && (p == line || p[-1] == ' ' || p[-1] == '\t')) return 1;
        if ((*p == '>' || *p == '<') && p > line && p[-1] >= '0' && p[-1] <= '9') return 1;
    }
    return 0;
}

static int run_native_shell(const char *line) {
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) { execl("/bin/bash", "bash", "-c", line, (char *)NULL); _exit(127); }
    int status = 1;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* Quotes and backslash escapes are resolved here. Deliberately unsupported:
   command substitutions, globbing, functions, eval and heredocs. */
static int lex(const char *line, Tokens *out) {
    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') break;
        if (is_operator(*p)) {
            TokenKind kind = *p == '|' ? TOK_PIPE : *p == '&' ? TOK_BG : *p == ';' ? TOK_SEMI : *p == '<' ? TOK_IN : TOK_OUT;
            if ((*p == '|' && p[1] == '|') || (*p == '&' && p[1] == '&') || (*p == '>' && p[1] == '>')) {
                kind = *p == '|' ? TOK_OR : *p == '&' ? TOK_AND : TOK_APPEND;
                p++;
            }
            token_push(out, kind, NULL); p++; continue;
        }
        size_t cap = 32, len = 0; char *word = malloc(cap); int quote = 0;
        if (!word) return -1;
        while (*p && (quote || (!isspace((unsigned char)*p) && !is_operator(*p)))) {
            char c = *p++;
            if ((c == '\'' || c == '"')) {
                if (!quote) { quote = c; continue; }
                if (quote == c) { quote = 0; continue; }
            }
            if (c == '\\' && quote != '\'') {
                if (!*p) { free(word); fprintf(stderr, "clay shell: trailing escape\n"); return -1; }
                c = *p++;
            }
            if (len + 2 > cap) word = grow(word, &cap, 1, len + 2);
            word[len++] = c;
        }
        if (quote) { free(word); fprintf(stderr, "clay shell: unclosed quote\n"); return -1; }
        word[len] = 0;
        token_push(out, TOK_WORD, word);
    }
    return 0;
}
static void command_free(Command *cmd) {
    for (size_t i = 0; i < cmd->base.argc; i++) free(cmd->base.argv[i]);
    free(cmd->base.argv); free(cmd->base.in); free(cmd->base.out);
}
static void program_free(Program *program) {
    for (size_t i = 0; i < program->count; i++) {
        Pipeline *pipe = &program->items[i].pipeline;
        for (size_t j = 0; j < pipe->count; j++) command_free(&pipe->commands[j]);
        free(pipe->commands);
    }
    free(program->items);
}
static Command *pipeline_command(Pipeline *pipe) {
    pipe->commands = grow(pipe->commands, &pipe->cap, sizeof(Command), pipe->count + 1);
    Command *cmd = &pipe->commands[pipe->count++]; memset(cmd, 0, sizeof(*cmd)); return cmd;
}
static void command_arg(Command *cmd, const char *word) {
    cmd->base.argv = grow(cmd->base.argv, &cmd->cap, sizeof(char *), cmd->base.argc + 2);
    cmd->base.argv[cmd->base.argc++] = strdup(word); cmd->base.argv[cmd->base.argc] = NULL;
}
static int parse(const Tokens *tokens, Program *program) {
    size_t i = 0;
    while (i < tokens->count) {
        program->items = grow(program->items, &program->cap, sizeof(Statement), program->count + 1);
        Statement *statement = &program->items[program->count++]; memset(statement, 0, sizeof(*statement));
        Command *cmd = pipeline_command(&statement->pipeline);
        int need_redirection_path = 0;
        while (i < tokens->count) {
            Token token = tokens->items[i++];
            if (token.kind == TOK_WORD) {
                if (need_redirection_path == TOK_IN) { free(cmd->base.in); cmd->base.in = strdup(token.text); }
                else if (need_redirection_path) { free(cmd->base.out); cmd->base.out = strdup(token.text); cmd->base.append = need_redirection_path == TOK_APPEND; }
                else command_arg(cmd, token.text);
                need_redirection_path = 0; continue;
            }
            if (token.kind == TOK_IN || token.kind == TOK_OUT || token.kind == TOK_APPEND) {
                if (need_redirection_path) goto syntax;
                need_redirection_path = token.kind; continue;
            }
            if (need_redirection_path || cmd->base.argc == 0) goto syntax;
            if (token.kind == TOK_PIPE) { cmd = pipeline_command(&statement->pipeline); continue; }
            if (token.kind == TOK_BG) statement->pipeline.background = 1;
            else if (token.kind == TOK_AND) statement->next = NEXT_AND;
            else if (token.kind == TOK_OR) statement->next = NEXT_OR;
            else statement->next = NEXT_ALWAYS;
            if (token.kind == TOK_BG || token.kind == TOK_SEMI || token.kind == TOK_AND || token.kind == TOK_OR) break;
            goto syntax;
        }
        if (need_redirection_path || cmd->base.argc == 0) goto syntax;
    }
    return 0;
syntax:
    fprintf(stderr, "clay shell: invalid command syntax\n"); return -1;
}
static int apply_redirections(const ClayShellCommand *cmd) {
    if (cmd->in) { int fd = open(cmd->in, O_RDONLY); if (fd < 0) { perror(cmd->in); return -1; } if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2"); close(fd); return -1; } close(fd); }
    if (cmd->out) { int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC); int fd = open(cmd->out, flags, 0666); if (fd < 0) { perror(cmd->out); return -1; } if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return -1; } close(fd); }
    return 0;
}
static int execute_pipeline(ClayShell *shell, Pipeline *pipeline) {
    if (pipeline->count == 1 && !pipeline->background && clay_shell_builtin_exists(pipeline->commands[0].base.argv[0])) {
        int saved_in = -1, saved_out = -1;
        if (pipeline->commands[0].base.in) saved_in = dup(STDIN_FILENO);
        if (pipeline->commands[0].base.out) saved_out = dup(STDOUT_FILENO);
        int status = apply_redirections(&pipeline->commands[0].base) ? 1 : clay_shell_builtin_run(shell, &pipeline->commands[0].base);
        fflush(stdout);
        if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        return status;
    }
    pid_t *pids = calloc(pipeline->count, sizeof(pid_t)); int previous = -1;
    if (!pids) return 1;
    for (size_t i = 0; i < pipeline->count; i++) {
        int fds[2] = {-1, -1}; if (i + 1 < pipeline->count && pipe(fds)) { perror("pipe"); free(pids); return 1; }
        pid_t pid = fork();
        if (pid == 0) {
            if (previous >= 0) { dup2(previous, STDIN_FILENO); close(previous); }
            if (fds[1] >= 0) { dup2(fds[1], STDOUT_FILENO); close(fds[0]); close(fds[1]); }
            if (apply_redirections(&pipeline->commands[i].base)) _exit(1);
            if (clay_shell_builtin_exists(pipeline->commands[i].base.argv[0])) {
                int status = clay_shell_builtin_run(shell, &pipeline->commands[i].base);
                fflush(NULL);
                _exit(status);
            }
            execvp(pipeline->commands[i].base.argv[0], pipeline->commands[i].base.argv);
            perror(pipeline->commands[i].base.argv[0]); _exit(127);
        }
        if (pid < 0) { perror("fork"); free(pids); return 1; }
        pids[i] = pid; if (previous >= 0) close(previous); if (fds[1] >= 0) { close(fds[1]); previous = fds[0]; }
    }
    if (previous >= 0) close(previous);
    if (pipeline->background) { printf("[%d]\n", (int)pids[pipeline->count - 1]); free(pids); return 0; }
    int status = 1;
    for (size_t i = 0; i < pipeline->count; i++) { int wait_status; waitpid(pids[i], &wait_status, 0); if (i + 1 == pipeline->count) status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128; }
    free(pids); return status;
}
static int execute(ClayShell *shell, Program *program) {
    int status = 0;
    for (size_t i = 0; i < program->count && !shell->should_exit; i++) {
        if (i && ((program->items[i - 1].next == NEXT_AND && status != 0) || (program->items[i - 1].next == NEXT_OR && status == 0))) continue;
        status = execute_pipeline(shell, &program->items[i].pipeline);
    }
    return shell->should_exit ? shell->exit_status : status;
}
static int run_line(ClayShell *shell, const char *line) {
    Tokens tokens = {0}; Program program = {0}; int status = 1;
    if (needs_native_shell(line)) return run_native_shell(line);
    if (lex(line, &tokens) == 0) {
        if (tokens.count == 0) status = 0;
        else if (parse(&tokens, &program) == 0) status = execute(shell, &program);
    }
    program_free(&program); tokens_free(&tokens); while (waitpid(-1, NULL, WNOHANG) > 0) {}
    return status;
}
int clay_shell_authorize(const char *command, ClayShellAuthorize authorize, void *user_data) {
    if (needs_native_shell(command)) {
        char *const opaque[] = {"bash", "-c", (char *)command, NULL};
        return !authorize || authorize(opaque, user_data) ? 0 : -1;
    }
    Tokens tokens = {0}; Program program = {0}; int allowed = -1;
    if (lex(command, &tokens) == 0 && parse(&tokens, &program) == 0) {
        allowed = 0;
        for (size_t i = 0; i < program.count && allowed == 0; i++)
            for (size_t j = 0; j < program.items[i].pipeline.count; j++)
                if (authorize && !authorize(program.items[i].pipeline.commands[j].base.argv, user_data)) { allowed = -1; break; }
    }
    program_free(&program); tokens_free(&tokens);
    return allowed;
}
int clay_shell_run_command(const char *command) {
    ClayShell shell = {0};
    return run_line(&shell, command);
}
static void clear_clay_environment(void) {
    extern char **environ;
    for (char **entry = environ; *entry;) {
        char *equal = strchr(*entry, '=');
        if (equal && (size_t)(equal - *entry) >= 5 && !strncmp(*entry, "CLAY_", 5)) {
            char *name = strndup(*entry, (size_t)(equal - *entry));
            if (!name) return;
            unsetenv(name); free(name); entry = environ; continue;
        }
        entry++;
    }
}
static void shell_prompt(void) {
    char host[256] = "host";
    char *cwd = getcwd(NULL, 0);
    struct passwd *account = getpwuid(getuid());
    const char *user = account && account->pw_name ? account->pw_name : getenv("USER");
    gethostname(host, sizeof(host) - 1);
    printf("%s@%s:%s$ ", user && *user ? user : "user", host, cwd ? cwd : ".");
    free(cwd);
}
int clay_shell_main(int argc, char **argv) {
    ClayShell shell = {0}; int status = 0;
    clear_clay_environment();
    if (argc) { size_t length = 1; for (int i = 0; i < argc; i++) length += strlen(argv[i]) + 1; char *line = malloc(length); if (!line) return 1; line[0] = 0; for (int i = 0; i < argc; i++) { if (i) strcat(line, " "); strcat(line, argv[i]); } status = run_line(&shell, line); free(line); return status; }
    char *line = NULL; size_t cap = 0;
    while (!shell.should_exit) { shell_prompt(); fflush(stdout); if (getline(&line, &cap, stdin) < 0) break; status = run_line(&shell, line); }
    free(line); return shell.should_exit ? shell.exit_status : status;
}
#endif
