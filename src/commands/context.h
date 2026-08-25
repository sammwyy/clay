#ifndef CLAY_COMMANDS_CONTEXT_H
#define CLAY_COMMANDS_CONTEXT_H

#include "clay/clay.h"
#include "clay/providers/openai.h"
#include "clay/providers/openai_codex.h"
#include "clay/providers/grok.h"
#include "clay/sandbox.h"

#include <pthread.h>

typedef struct {
  const char *id;
  const char *label;
  const char *default_base_url;
} ClayProviderType;

typedef struct {
  const ClayProviderType *type;
  ClayProviderConfig *config;
  ClayOpenAI *client;
  ClayOpenAICodex *codex_client;
  ClayGrok *grok_client;
  int environment_override;
  ClayArray models;
  int models_fetched;
  int models_rc;
  long models_status;
} ClayConnectedProvider;

typedef struct {
  const char *id;
  const char *label;
  const char *description;
} ClayReasoningEffort;

typedef struct {
  char *path;
  char *before;
  size_t before_len;
  char *after;
  size_t after_len;
  int before_exists;
  int after_exists;
} ClayUndoEntry;

typedef struct {
  char *content;
  char *status;   /* "pending", "in_progress", or "completed" */
  char *shown;    /* the status already on screen, so a redraw prints only
                     the steps that actually moved. NULL until printed. */
} ClayTodoItem;

/* Where a todowrite call writes. The session has one plan, which is drawn
   for the user; every subagent gets its own, which only it ever sees. */
typedef struct {
  ClayArray todos; /* ClayTodoItem */
  int rendered;    /* draw it in the transcript and the status row */
} ClayPlan;

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
/* One backgrounded shell command (src/commands/tasks.c). Opaque: a
   reader thread owns its output buffer. */
typedef struct ClayBackgroundTask ClayBackgroundTask;

typedef enum {
  CLAY_AUTO_TEST_UNASKED,
  CLAY_AUTO_TEST_ALLOWED,
  CLAY_AUTO_TEST_DENIED,
} ClayAutoTestChoice;

struct ClayCommands {
  ClayApp *app;
  int running;
  ClayJson *conversation;
  char *system_prompt; /* text currently at conversation[0]; frozen per-chat
                          once one exists */
  long input_tokens;
  long output_tokens;
  long cached_input_tokens;
  int cached_input_tokens_known;
  long total_input_tokens;
  long total_output_tokens;
  long messages_sent;
  ClayChat *chat;
  ClayArray providers;
  char *selected_provider;
  char *selected_model;
  int reasoning_effort_index;
  ClaySandboxMode sandbox_mode;
  /* Shared by every sandboxed command this session, so one can reach a
     server another started. */
  ClaySandboxNamespaces *sandbox_namespaces;
  int sandbox_auto_approve;
  int use_integrated_shell;
  int auto_approve[CLAY_PERMISSION_CATEGORY_COUNT];
  ClayArray
      remembered_patterns[CLAY_PERMISSION_CATEGORY_COUNT]; /* char*, approved
                                                              for this session
                                                              only */
  ClayCommandsMode mode;
  ClayPlan plan; /* the session's own checklist, session-only, not persisted */
  ClayArray
      mcp_servers; /* ClayMcpServer*, connected for the life of the session */
  ClayArray mcp_bindings; /* ClayMcpToolBinding, one per discovered MCP tool */
  int mcp_connect_attempted;
  ClayArray undo_history; /* ClayUndoEntry, newest entry last */
  ClayUndoEntry undo_pending;
  int undo_pending_valid;
  char *auto_test_command; /* "" if unset */
  ClayAutoTestChoice auto_test_choice;
  /* Last context block of each kind appended to `conversation`, so an
     unchanged one is never appended twice. */
  char *environment_block;
  char *notes_block;
  ClayArray tasks; /* ClayBackgroundTask*, background commands this session */
  int next_task_id;
  /* The spinner row of the tool call running right now, for a long call that
     wants to report progress on it (see subagent.c). NULL between calls. */
  ClayTask *active_tool_task;
  /* Held by every tool that changes something (files, checkpoints, undo,
     background tasks, approvals) so parallel subagents cannot interleave in
     the middle of one. Recursive: a gated tool takes it around a permission
     check that takes it too. */
  pthread_mutex_t tool_lock;
};

ClayConnectedProvider *clay_commands_find_provider(ClayCommands *commands,
                                                   const char *id);
const ClayProviderType *clay_commands_find_provider_type(const char *id);
const ClayProviderType *clay_commands_provider_types(size_t *count);
int clay_commands_fetch_models(void *ctx, ClayArray *out);
int clay_commands_select_model(ClayCommands *commands, const char *provider,
                               const char *model);
void clay_commands_update_selected_below(ClayCommands *commands);
/* Folds a nested run's usage (a subagent, say) into the session totals so
   the status line and the chat journal stay honest. */
void clay_commands_add_usage(ClayCommands *commands, long input_tokens,
                             long output_tokens);
void clay_commands_set_tokens_below(ClayCommands *commands, long input_tokens,
                                    long output_tokens);
void clay_commands_set_tokens_below_with_cache(
    ClayCommands *commands, long input_tokens, long output_tokens,
    long cached_input_tokens, int cached_input_tokens_known);
void clay_commands_reset_conversation(ClayCommands *commands);
/* Platform, date, working directory, git branch and the workspace's
   top-level entries, as they are at this instant. Malloc'd; sent with each
   message rather than frozen into the system prompt. */
char *clay_commands_environment_block(void);
/* Appends the environment (and the chat's notes, when set) to the
   conversation, but only when they differ from the last ones appended, so
   the message array only ever grows and the provider's prefix cache holds. */
void clay_commands_sync_context_blocks(ClayCommands *commands);
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
/* Frees every item and empties `plan` in place (keeping the array, ready
   for more steps). A rendered plan also clears its row. */
void clay_plan_clear(ClayPlan *plan);
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
const ClayReasoningEffort *
clay_commands_reasoning_effort(const ClayCommands *commands);
size_t clay_commands_reasoning_effort_count(void);
const ClayReasoningEffort *clay_commands_reasoning_efforts(void);
void clay_commands_load_provider(ClayCommands *commands,
                                 const ClayProviderType *type);
int clay_commands_logout_provider(ClayCommands *commands, const char *id);
/* Copies potentially refreshed Codex OAuth credentials into the existing
   owner-only provider config and saves them. */
int clay_commands_save_codex_credentials(ClayConnectedProvider *provider,
                                         const ClayOpenAICodex *client);
int clay_commands_save_grok_credentials(ClayConnectedProvider *provider,
                                        const ClayGrok *client);
void clay_commands_print_history(ClayCommands *commands, size_t count);

/* Approval gate (src/commands/permissions.c). `action` is a short present-
   tense verb phrase shown in the prompt ("Write", "Run"); `detail` is the
   file path (read/edit) or full command (exec) - used for the prompt and to
   derive a wildcard pattern when the user chooses to remember it for the
   session. True if allowed. */
int clay_permissions_check(ClayCommands *commands,
                           ClayPermissionCategory category, const char *action,
                           const char *detail);
void clay_commands_update_sandbox_below(ClayCommands *commands);
/* Splits a persisted sandbox_mode value into its two axes. */
void clay_commands_parse_sandbox_mode(const char *value, ClaySandboxMode *mode,
                                      int *auto_approve);
void clay_commands_cycle_sandbox(ClayCommands *commands);
void clay_cmd_cycle_sandbox(const char *args, void *user_data);
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
void clay_cmd_tasks(const char *args, void *user_data);
void clay_cmd_undo(const char *args, void *user_data);
void clay_cmd_skill(const char *args, void *user_data);

/* Captures and commits one file-level undo entry around a write/edit. The
   snapshot is best-effort; unsupported or oversized files are still edited,
   but simply won't appear in /undo. */
int clay_commands_undo_prepare(ClayCommands *commands, const char *path);
void clay_commands_undo_commit(ClayCommands *commands);
void clay_commands_undo_discard(ClayCommands *commands);
void clay_commands_undo_destroy(ClayCommands *commands);

/* Dedicated filesystem tools (src/commands/fs_tools.c), each scoped to the
   current workspace directory. userdata is a ClayCommands*. */
int clay_fs_resolve_workspace_path(const char *workspace_dir, const char *path,
                                   ClayStr *abs_out);
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
void clay_fs_walk_files(const char *base_dir, const char *rel_prefix,
                        const char *pattern, ClayArray *matches,
                        int *truncated);

/* Heuristic repo map (src/commands/repo_map.c): ranked top-level symbol
   definitions across the workspace, via ctags if installed, else a
   per-language line-heuristic fallback. Not gated by clay_permissions_check
   - it returns only symbol names/kinds/line numbers, not file content. */
ClayJson *clay_fs_tool_repo_map(const ClayJson *arguments, void *userdata);
ClayJson *clay_fs_tool_repo_map_schema(void);

/* Every tool one agent turn can call, plus the schema objects those tools
   borrow. Built by clay_commands_tools_build, released by
   clay_commands_tools_free. */
typedef struct {
  ClayArray tools;   /* ClayTool */
  ClayArray schemas; /* ClayJson*, owned here */
} ClayToolSet;

/* `allow_subagent` adds the two tools that only make sense for the agent
   talking to the user: ask_user and subagent. A subagent gets everything
   else, so it cannot nest or block on a question. */
void clay_commands_tools_build(ClayCommands *commands, ClayPlan *plan,
                               ClayToolSet *set, int allow_subagent);
void clay_commands_tools_free(ClayToolSet *set);

/* Rounds of tool calls one agent gets before the provider loop stops and
   hands the turn back. Real work runs long: a scaffold plus a build plus a
   verification pass is dozens of calls, and the old ceiling of 8 cut turns
   off mid-job. Escape still cancels, and each command has its own timeout. */
#define CLAY_AGENT_MAX_ROUNDS 64
#define CLAY_SUBAGENT_MAX_ROUNDS 32

/* Runs `messages` against the selected provider until the model answers or
   `max_rounds` is spent, appending the reply and tool results in place.
   `cache_key` groups the request for the provider's prefix cache. Returns 0
   on success, 1 when cancelled, -1 on failure. */
int clay_commands_run_completion(ClayCommands *commands, ClayJson *messages,
                                 const ClayToolSet *tools, int max_rounds,
                                 const char *cache_key,
                                 const ClayOpenAICallbacks *callbacks);

/* Delegation tool (src/commands/subagent.c). Runs one step of a plan in a
   fresh agent with no conversation history and returns its summary.
   userdata is a ClayCommands*. */
ClayJson *subagent_tool(const ClayJson *arguments, void *userdata);
ClayJson *subagent_schema(void);

/* Plan/checklist tool (src/commands/message.c). Replaces the plan it was
   built with, wholesale, on each call. userdata is a ClayPlan*. */
ClayJson *todowrite_tool(const ClayJson *arguments, void *userdata);
ClayJson *todowrite_schema(void);

/* Interactive question tool (src/commands/message.c). Blocks on the choice
   widget; fails instead of prompting when stdin/stdout is not a tty.
   userdata is a ClayCommands*. */
ClayJson *ask_user_tool(const ClayJson *arguments, void *userdata);
ClayJson *ask_user_schema(void);

/* Background command tools (src/commands/tasks.c). task_run starts a
   command on its own thread and returns as soon as it has had a moment to
   fail; the others read, stop, and list what is running. userdata is a
   ClayCommands*. */
ClayJson *task_run_tool(const ClayJson *arguments, void *userdata);
ClayJson *task_run_schema(void);
ClayJson *task_output_tool(const ClayJson *arguments, void *userdata);
ClayJson *task_output_schema(void);
ClayJson *task_stop_tool(const ClayJson *arguments, void *userdata);
ClayJson *task_stop_schema(void);
ClayJson *task_list_tool(const ClayJson *arguments, void *userdata);
ClayJson *task_list_schema(void);

/* Stops every background task, waits for its thread, and empties the
   registry. */
void clay_commands_stop_tasks(ClayCommands *commands);

/* Snapshots the workspace into the chat's checkpoint repo before a tool
   call that may change it. Best-effort: a failed snapshot never blocks the
   call. */
void clay_commands_checkpoint(ClayCommands *commands, const char *label);

#endif /* CLAY_COMMANDS_CONTEXT_H */
