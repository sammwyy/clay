#ifndef CLAY_COMMANDS_CONTEXT_H
#define CLAY_COMMANDS_CONTEXT_H

#include "clay/clay.h"
#include "clay/providers/openai.h"
#include "clay/sandbox.h"

typedef struct {
    const char *id;
    const char *label;
    const char *default_base_url;
} ClayProviderType;

typedef struct {
    const ClayProviderType *type;
    ClayProviderConfig *config;
    ClayOpenAI *client;
    ClayArray models;
    int models_fetched;
    int models_rc;
} ClayConnectedProvider;

typedef struct {
    const char *id;
    const char *label;
    const char *description;
} ClayReasoningEffort;

typedef struct {
    char *content;
    char *status; /* "pending", "in_progress", or "completed" */
} ClayTodoItem;

/* Approval categories, independent of the sandbox (namespace) axis: whether
   a tool call needs the user's OK before it runs at all. */
typedef enum {
    CLAY_PERMISSION_READ,      /* read/glob/grep */
    CLAY_PERMISSION_EDIT,      /* write/edit */
    CLAY_PERMISSION_EXEC_SAFE, /* shell_exec, curated read-only-ish commands */
    CLAY_PERMISSION_EXEC_ALL,  /* shell_exec, everything else */
    CLAY_PERMISSION_CATEGORY_COUNT,
} ClayPermissionCategory;

/* Plan: shell_exec runs but mutating commands are blocked outright, and
   write/edit are refused - for exploring/discussing before committing to
   changes. Act: normal operation. Session-only, not persisted. */
typedef enum {
    CLAY_MODE_ACT,
    CLAY_MODE_PLAN,
} ClayCommandsMode;

/* Whether the user has agreed to let the configured auto-test command run
   after edits, asked once per session (not once per edit). */
typedef enum {
    CLAY_AUTO_TEST_UNASKED,
    CLAY_AUTO_TEST_ALLOWED,
    CLAY_AUTO_TEST_DENIED,
} ClayAutoTestChoice;

struct ClayCommands {
    ClayApp *app;
    int running;
    ClayJson *conversation;
    char *system_prompt; /* text currently at conversation[0]; frozen per-chat once one exists */
    long input_tokens;
    long output_tokens;
    long total_input_tokens;
    long total_output_tokens;
    long messages_sent;
    ClayChat *chat;
    ClayArray providers;
    char *selected_provider;
    char *selected_model;
    int reasoning_effort_index;
    ClaySandboxMode sandbox_mode;
    int auto_approve[CLAY_PERMISSION_CATEGORY_COUNT];
    ClayArray remembered_patterns[CLAY_PERMISSION_CATEGORY_COUNT]; /* char*, approved for this session only */
    ClayCommandsMode mode;
    ClayArray todos; /* ClayTodoItem, the current plan - session-only, not persisted */
    ClayArray mcp_servers;   /* ClayMcpServer*, connected for the life of the session */
    ClayArray mcp_bindings;  /* ClayMcpToolBinding, one per discovered MCP tool */
    int mcp_connect_attempted;
    char *auto_test_command; /* "" if unset */
    ClayAutoTestChoice auto_test_choice;
};

ClayConnectedProvider *clay_commands_find_provider(ClayCommands *commands, const char *id);
const ClayProviderType *clay_commands_find_provider_type(const char *id);
const ClayProviderType *clay_commands_provider_types(size_t *count);
int clay_commands_fetch_models(void *ctx, ClayArray *out);
int clay_commands_select_model(ClayCommands *commands, const char *provider, const char *model);
void clay_commands_update_selected_below(ClayCommands *commands);
void clay_commands_set_tokens_below(ClayCommands *commands, long input_tokens, long output_tokens);
void clay_commands_reset_conversation(ClayCommands *commands);
/* Walks up from the cwd to the repo root, concatenating AGENTS.md/CLAY.md
   at each level (root-first). Malloc'd; NULL if none found. */
char *clay_commands_load_project_instructions(void);
/* Walks up from the cwd looking for a .git directory, returning its
   current branch (or a short SHA in detached HEAD). Malloc'd; NULL if not
   in a git repo. */
char *clay_commands_find_git_branch(void);
/* Comma-separated, sorted, capped names of `dir`'s direct entries. Malloc'd;
   NULL if the directory can't be listed. */
char *clay_commands_list_top_level(const char *dir);
/* Deterministic context compaction (no LLM call): once the last request's
   input tokens crossed ~90% of the context budget, collapses tool-result
   content older than the last few turns to a short preview, in place on
   commands->conversation. Returns how many results it collapsed. */
int clay_commands_maybe_compact(ClayCommands *commands);
/* Frees every item's content/status and empties commands->todos in place
   (keeps the array itself, so it's ready for more clay_array_push_val). */
void clay_commands_clear_todos(ClayCommands *commands);
/* Connects to every configured MCP server (src/commands/mcp.c) the first
   time it's called in this session; later calls are a no-op. Best-effort -
   a server that fails to connect is skipped with a warning, not fatal. */
void clay_commands_connect_mcp_servers(ClayCommands *commands);
void clay_cmd_mcp(const char *args, void *user_data);
void clay_cmd_autotest(const char *args, void *user_data);
void clay_cmd_compact(const char *args, void *user_data);
/* Flattens `conversation` (skipping the system prompt at index 0) into a
   plain-text transcript for /compact's summarization request: user/
   assistant text verbatim, tool calls as "called name(args)", tool
   results collapsed to a short preview. Malloc'd. */
char *clay_commands_build_compact_transcript(ClayJson *conversation);
void clay_commands_new_chat(ClayCommands *commands);
const ClayReasoningEffort *clay_commands_reasoning_effort(const ClayCommands *commands);
size_t clay_commands_reasoning_effort_count(void);
const ClayReasoningEffort *clay_commands_reasoning_efforts(void);
void clay_commands_load_provider(ClayCommands *commands, const ClayProviderType *type);
int clay_commands_logout_provider(ClayCommands *commands, const char *id);
void clay_commands_print_history(ClayCommands *commands, size_t count);

/* Approval gate (src/commands/permissions.c). `action` is a short present-
   tense verb phrase shown in the prompt ("Write", "Run"); `detail` is the
   file path (read/edit) or full command (exec) - used for the prompt and to
   derive a wildcard pattern when the user chooses to remember it for the
   session. True if allowed. */
int clay_permissions_check(ClayCommands *commands, ClayPermissionCategory category, const char *action,
                           const char *detail);
/* True if `command`'s program name is on the curated read-only-ish
   whitelist (ls, cat, grep, git status, ...). */
int clay_permissions_is_safe_command(const char *command);
/* True if `command` would mutate the filesystem or a git repo (rm/mv/cp/...,
   any git subcommand other than a read like status/log/diff) - the
   blacklist Plan mode blocks regardless of approval settings. */
int clay_permissions_is_mutating_command(const char *command);
const char *clay_permissions_category_name(ClayPermissionCategory category);
const char *clay_permissions_category_label(ClayPermissionCategory category);

void clay_cmd_exit(const char *args, void *user_data);
void clay_cmd_help(const char *args, void *user_data);
void clay_cmd_confirm(const char *args, void *user_data);
void clay_cmd_select(const char *args, void *user_data);
void clay_cmd_choice(const char *args, void *user_data);
void clay_cmd_below(const char *args, void *user_data);
void clay_cmd_connect(const char *args, void *user_data);
void clay_cmd_logout(const char *args, void *user_data);
void clay_cmd_model(const char *args, void *user_data);
void clay_cmd_effort(const char *args, void *user_data);
void clay_cmd_resume(const char *args, void *user_data);
void clay_cmd_history(const char *args, void *user_data);
void clay_cmd_memory(const char *args, void *user_data);
void clay_cmd_new(const char *args, void *user_data);
void clay_cmd_mm(const char *args, void *user_data);
void clay_cmd_demo(const char *args, void *user_data);
void clay_cmd_sandbox(const char *args, void *user_data);
void clay_cmd_exec(const char *args, void *user_data);
void clay_cmd_checkpoints(const char *args, void *user_data);
void clay_cmd_permissions(const char *args, void *user_data);
void clay_cmd_plan(const char *args, void *user_data);

/* Dedicated filesystem tools (src/commands/fs_tools.c), each scoped to the
   current workspace directory. userdata is a ClayCommands*. */
ClayJson *clay_fs_tool_read(const ClayJson *arguments, void *userdata);
ClayJson *clay_fs_tool_read_schema(void);
ClayJson *clay_fs_tool_write(const ClayJson *arguments, void *userdata);
ClayJson *clay_fs_tool_write_schema(void);
ClayJson *clay_fs_tool_edit(const ClayJson *arguments, void *userdata);
ClayJson *clay_fs_tool_edit_schema(void);
ClayJson *clay_fs_tool_glob(const ClayJson *arguments, void *userdata);
ClayJson *clay_fs_tool_glob_schema(void);
ClayJson *clay_fs_tool_grep(const ClayJson *arguments, void *userdata);
ClayJson *clay_fs_tool_grep_schema(void);
/* Recursively collects every file under base_dir whose path (joined with
   rel_prefix, "" at the top call) matches `pattern` (clay_str_wildcard_match)
   into `matches` (char*, caller frees each entry and the array). Skips
   .git. Caps at an internal match limit, setting *truncated if it hits it. */
void clay_fs_walk_files(const char *base_dir, const char *rel_prefix, const char *pattern, ClayArray *matches,
                        int *truncated);

/* Heuristic repo map (src/commands/repo_map.c): ranked top-level symbol
   definitions across the workspace, via ctags if installed, else a
   per-language line-heuristic fallback. Not gated by clay_permissions_check
   - it returns only symbol names/kinds/line numbers, not file content. */
ClayJson *clay_fs_tool_repo_map(const ClayJson *arguments, void *userdata);
ClayJson *clay_fs_tool_repo_map_schema(void);

/* Plan/checklist tool (src/commands/message.c). Replaces commands->todos
   wholesale on each call. userdata is a ClayCommands*. */
ClayJson *todowrite_tool(const ClayJson *arguments, void *userdata);
ClayJson *todowrite_schema(void);

#endif /* CLAY_COMMANDS_CONTEXT_H */
