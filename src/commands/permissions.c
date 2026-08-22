#include "context.h"

#include "clay/task.h"

#include <stdlib.h>
#include <string.h>

static const char *const SAFE_COMMANDS[] = {
    "ls",  "cat",  "pwd",   "echo",  "grep",   "find",     "wc",     "head", "tail",
    "file", "which", "env", "date",  "whoami", "uname",    "du",     "df",   "ps",
    "diff", "sort",  "uniq", "tree", "stat",   "basename", "dirname", "printf", "true",
    "false", "hostname", "realpath", "readlink", "test",
};

static const char *const SAFE_GIT_SUBCOMMANDS[] = {
    "status", "log", "diff", "show", "branch", "remote", "rev-parse", "describe", "blame",
};

/* Always mutates the filesystem regardless of arguments. */
static const char *const MUTATING_COMMANDS[] = {
    "rm", "mv", "cp", "dd", "shred", "mkfs", "chmod", "chown", "truncate", "sudo",
};

static int token_matches(const char *token, size_t token_len, const char *const *list, size_t list_count) {
    for (size_t i = 0; i < list_count; i++) {
        if (token_len == strlen(list[i]) && strncmp(token, list[i], token_len) == 0) return 1;
    }
    return 0;
}

/* shell_exec ultimately passes the complete invocation to a shell.  A
   first-word allowlist is therefore only meaningful for a simple command:
   `ls; rm -rf .` is not an ls invocation.  Until shell_exec has a real AST
   policy, reject any syntax which can compose another command, redirect I/O,
   or run a command substitution.  This is deliberately conservative: the
   permission prompt is the safe fallback for legitimate shell expressions. */
static int has_shell_syntax(const char *command) {
    for (const unsigned char *p = (const unsigned char *)command; *p; p++) {
        switch (*p) {
            case ';': case '|': case '&': case '`': case '$':
            case '(': case ')': case '<': case '>': case '\n': case '\r':
                return 1;
        }
    }
    return 0;
}

/* Splits `command` into its program name (basename of the first token) and
   the token after it (a subcommand, for programs like git). *name_len is 0
   if `command` is empty. */
static void split_program(const char *command, const char **name, size_t *name_len, const char **sub,
                          size_t *sub_len) {
    while (*command == ' ') command++;
    const char *end = command;
    while (*end && *end != ' ') end++;
    *name = command;
    for (const char *p = command; p < end; p++) {
        if (*p == '/') *name = p + 1;
    }
    *name_len = (size_t)(end - *name);

    const char *rest = end;
    while (*rest == ' ') rest++;
    const char *rest_end = rest;
    while (*rest_end && *rest_end != ' ') rest_end++;
    *sub = rest;
    *sub_len = (size_t)(rest_end - rest);
}

int clay_permissions_is_safe_command(const char *command) {
    if (has_shell_syntax(command)) return 0;
    const char *name;
    const char *sub;
    size_t name_len;
    size_t sub_len;
    split_program(command, &name, &name_len, &sub, &sub_len);

    if (name_len == 3 && strncmp(name, "git", 3) == 0) {
        return token_matches(sub, sub_len, SAFE_GIT_SUBCOMMANDS, sizeof(SAFE_GIT_SUBCOMMANDS) / sizeof(SAFE_GIT_SUBCOMMANDS[0]));
    }
    return token_matches(name, name_len, SAFE_COMMANDS, sizeof(SAFE_COMMANDS) / sizeof(SAFE_COMMANDS[0]));
}

int clay_permissions_is_mutating_command(const char *command) {
    /* Plan mode must not run an expression whose effects we cannot inspect. */
    if (has_shell_syntax(command)) return 1;
    const char *name;
    const char *sub;
    size_t name_len;
    size_t sub_len;
    split_program(command, &name, &name_len, &sub, &sub_len);

    if (name_len == 3 && strncmp(name, "git", 3) == 0) {
        return !token_matches(sub, sub_len, SAFE_GIT_SUBCOMMANDS, sizeof(SAFE_GIT_SUBCOMMANDS) / sizeof(SAFE_GIT_SUBCOMMANDS[0]));
    }
    return token_matches(name, name_len, MUTATING_COMMANDS, sizeof(MUTATING_COMMANDS) / sizeof(MUTATING_COMMANDS[0]));
}

const char *clay_permissions_category_name(ClayPermissionCategory category) {
    switch (category) {
        case CLAY_PERMISSION_READ: return "read";
        case CLAY_PERMISSION_EDIT: return "edit";
        case CLAY_PERMISSION_EXEC_SAFE: return "exec_safe";
        case CLAY_PERMISSION_EXEC_ALL: return "exec_all";
        default: return "";
    }
}

const char *clay_permissions_category_label(ClayPermissionCategory category) {
    switch (category) {
        case CLAY_PERMISSION_READ: return "Read files (read/glob/grep)";
        case CLAY_PERMISSION_EDIT: return "Edit files (write/edit)";
        case CLAY_PERMISSION_EXEC_SAFE: return "Run safe commands (ls, git status, ...)";
        case CLAY_PERMISSION_EXEC_ALL: return "Run any command";
        default: return "";
    }
}

/* read/edit: directory the path is under, so approving one file covers its
   siblings. exec: the program name (first token), so approving one
   invocation covers the same program with different arguments. */
static char *derive_pattern(ClayPermissionCategory category, const char *detail) {
    ClayStr pattern;
    clay_str_init(&pattern);
    if (category == CLAY_PERMISSION_READ || category == CLAY_PERMISSION_EDIT) {
        const char *slash = strrchr(detail, '/');
        if (slash) clay_str_push_n(&pattern, detail, (size_t)(slash - detail));
        clay_str_push(&pattern, "/*");
    } else {
        const char *end = detail;
        while (*end && *end != ' ') end++;
        clay_str_push_n(&pattern, detail, (size_t)(end - detail));
        clay_str_push(&pattern, " *");
    }
    return pattern.data;
}

int clay_permissions_check(ClayCommands *commands, ClayPermissionCategory category, const char *action,
                           const char *detail) {
    if (commands->sandbox_auto_approve || commands->auto_approve[category]) return 1;

    ClayArray *remembered = &commands->remembered_patterns[category];
    for (size_t i = 0; i < remembered->count; i++) {
        const char *pattern = *(char **)clay_array_get(remembered, i);
        if (clay_str_wildcard_match(pattern, detail)) return 1;
    }

    ClayStr question;
    clay_str_init(&question);
    clay_str_printf(&question, "%s: %s", action, detail);
    static const ClayChoice OPTIONS[] = {
        {"Allow once", "Run this one call; ask again next time."},
        {"Always allow this session", "Don't ask again for similar calls until clay exits."},
        {"Deny", "Skip this call and tell the model it was denied."},
    };
    int index = clay_app_choice(commands->app, question.data, OPTIONS, 3, 0, NULL);
    const char *result = index == 0 ? "Allowed once" :
                         index == 1 ? "Allowed for this session" : "Denied";
    clay_task_render_pause();
    clay_prompt_choice_compact_result(result, 3, 0);
    clay_task_render_resume();
    clay_str_free(&question);

    if (index == 1) {
        char *pattern = derive_pattern(category, detail);
        clay_array_push_val(remembered, &pattern);
        return 1;
    }
    return index == 0;
}

static const ClayChoice AUTO_APPROVE_CHOICES[] = {
    {"On", "Auto-approve without asking."},
    {"Off", "Ask each time (or approve once and remember it for the session)."},
};

void clay_cmd_permissions(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;

    ClayChoice choices[CLAY_PERMISSION_CATEGORY_COUNT];
    ClayStr descriptions[CLAY_PERMISSION_CATEGORY_COUNT];
    for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) {
        clay_str_init(&descriptions[i]);
        clay_str_push(&descriptions[i], commands->auto_approve[i] ? "On" : "Off");
        choices[i].title = clay_permissions_category_label((ClayPermissionCategory)i);
        choices[i].desc = descriptions[i].data;
    }
    int index = clay_app_choice(commands->app, "Auto-approve categories (pick one to toggle):", choices,
                                CLAY_PERMISSION_CATEGORY_COUNT, 0, NULL);
    for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) clay_str_free(&descriptions[i]);
    if (index < 0 || index >= CLAY_PERMISSION_CATEGORY_COUNT) return;

    ClayPermissionCategory category = (ClayPermissionCategory)index;
    int choice = clay_app_select(commands->app, "Auto-approve?", AUTO_APPROVE_CHOICES, 2,
                                 commands->auto_approve[category] ? 0 : 1);
    if (choice < 0) return;
    commands->auto_approve[category] = choice == 0;
    clay_config_set_auto_approve(clay_permissions_category_name(category), commands->auto_approve[category]);
    clay_sayc(CLAY_GREEN, "%s: %s.", clay_permissions_category_label(category),
             commands->auto_approve[category] ? "on" : "off");
}
