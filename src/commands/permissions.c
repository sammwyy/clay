#include "context.h"

#include <stdlib.h>
#include <string.h>

static const char *const SAFE_COMMANDS[] = {
    "ls",  "cat",  "pwd",   "echo",  "grep",   "find",     "wc",     "head", "tail",
    "file", "which", "env", "date",  "whoami", "uname",    "du",     "df",   "ps",
    "diff", "sort",  "uniq", "tree", "stat",   "basename", "dirname", "printf",
};

static const char *const SAFE_GIT_SUBCOMMANDS[] = {
    "status", "log", "diff", "show", "branch", "remote", "rev-parse", "describe", "blame",
};

static int token_matches(const char *token, size_t token_len, const char *const *list, size_t list_count) {
    for (size_t i = 0; i < list_count; i++) {
        if (token_len == strlen(list[i]) && strncmp(token, list[i], token_len) == 0) return 1;
    }
    return 0;
}

int clay_permissions_is_safe_command(const char *command) {
    while (*command == ' ') command++;
    const char *end = command;
    while (*end && *end != ' ') end++;
    const char *name = command;
    for (const char *p = command; p < end; p++) {
        if (*p == '/') name = p + 1;
    }
    size_t name_len = (size_t)(end - name);

    if (name_len == 3 && strncmp(name, "git", 3) == 0) {
        const char *sub = end;
        while (*sub == ' ') sub++;
        const char *sub_end = sub;
        while (*sub_end && *sub_end != ' ') sub_end++;
        return token_matches(sub, (size_t)(sub_end - sub), SAFE_GIT_SUBCOMMANDS,
                             sizeof(SAFE_GIT_SUBCOMMANDS) / sizeof(SAFE_GIT_SUBCOMMANDS[0]));
    }
    return token_matches(name, name_len, SAFE_COMMANDS, sizeof(SAFE_COMMANDS) / sizeof(SAFE_COMMANDS[0]));
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
    if (commands->auto_approve[category]) return 1;

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
