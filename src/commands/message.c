#include "context.h"
#include "clay/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAY_SHELL_OUTPUT_LIMIT (64 * 1024) /* shown inline to the model */
#define CLAY_SHELL_CAPTURE_LIMIT                                               \
  (4 * 1024 * 1024) /* captured, for the scratch dump */
#define CLAY_TOOL_VISIBLE_LINES 8
#define CLAY_THINKING_LIMIT (4 * 1024 * 1024)
#define CLAY_ASK_USER_MAX_OPTIONS 6

typedef struct {
  int status_visible;
  int started;
  int response_active;
  long error_status;
  long input_tokens;
  long output_tokens;
  long cached_input_tokens;
  int cached_input_tokens_known;
  int has_usage;
  ClayTask *tool_task;
  ClayStr response;
  ClayWrap wrap; /* word wrap for the streamed answer */
  int wrapping;  /* off when stdout is not a terminal */
  ClayStr thinking;
  struct timespec thinking_started;
  double thinking_seconds;
  int thinking_active;
  int thinking_output_started;
} ClayConversationStream;

void clay_commands_checkpoint(ClayCommands *commands, const char *label) {
  if (!commands->chat)
    return;
  char *checkpoints_dir = clay_chat_checkpoints_dir(commands->chat);
  if (!checkpoints_dir)
    return;
  char *workspace_dir = clay_term_cwd();
  clay_checkpoint_save(checkpoints_dir, workspace_dir, label);
  free(checkpoints_dir);
  free(workspace_dir);
}

/* Runs the user-configured auto-test command after a successful write/edit,
   confirmed once per session (not once per edit). Reuses shell_exec's own
   sandboxed execution - no new mechanism. Adds auto_test_* fields to
   `result` only when the command fails, so a passing edit's tool result
   stays as small as before. */
static void run_auto_test(ClayCommands *commands, ClayJson *result) {
  if (!commands->auto_test_command || !*commands->auto_test_command)
    return;
  if (!clay_json_bool_value(clay_json_object_get(result, "ok")))
    return;

  if (commands->auto_test_choice == CLAY_AUTO_TEST_UNASKED) {
    ClayStr question;
    clay_str_init(&question);
    clay_str_printf(
        &question,
        "Run the configured auto-test command after edits this session? $ %s",
        commands->auto_test_command);
    int allow = clay_app_confirm(commands->app, question.data, 1);
    clay_str_free(&question);
    commands->auto_test_choice =
        allow ? CLAY_AUTO_TEST_ALLOWED : CLAY_AUTO_TEST_DENIED;
  }
  if (commands->auto_test_choice != CLAY_AUTO_TEST_ALLOWED)
    return;

  ClayTask *task =
      clay_app_task_start(commands->app, "$ %s", commands->auto_test_command);
  ClayStr output;
  clay_str_init(&output);
  char *workspace_dir = clay_term_cwd();
  char *scratch_dir = clay_chat_scratch_dir(commands->chat);
  ClaySandboxConfig sandbox = {
      .mode = commands->sandbox_mode,
      .workspace_dir = workspace_dir,
      .scratch_dir = scratch_dir,
  };
  ClayExecResult exec = {0};
  int rc = clay_sandbox_exec(&sandbox, commands->auto_test_command, &output,
                             CLAY_SHELL_OUTPUT_LIMIT, NULL, &exec);
  int exit_code = exec.exit_code;
  free(workspace_dir);
  free(scratch_dir);

  if (rc == 0 && exit_code == 0) {
    clay_app_task_success(commands->app, task, "passed");
  } else {
    clay_app_task_fail(commands->app, task, "exit %d", exit_code);
    clay_json_object_set(result, "auto_test_failed", clay_json_bool(1));
    clay_json_object_set(result, "auto_test_command",
                         clay_json_string(commands->auto_test_command));
    clay_json_object_set(result, "auto_test_output",
                         clay_json_string(output.data));
  }
  clay_str_free(&output);
}

static ClayJson *denied_result(void) {
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(0));
  clay_json_object_set(result, "error", clay_json_string("denied by the user"));
  return result;
}

static ClayJson *plan_blocked_result(const char *why) {
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(0));
  ClayStr error;
  clay_str_init(&error);
  clay_str_printf(&error,
                  "blocked: clay is in Plan mode - %s. Describe the change "
                  "instead, or ask the user to "
                  "run /plan to switch to Act mode.",
                  why);
  clay_json_object_set(result, "error", clay_json_string(error.data));
  clay_str_free(&error);
  return result;
}

static ClayJson *read_tool_gated(const ClayJson *arguments, void *userdata) {
  const char *path =
      clay_json_string_value(clay_json_object_get(arguments, "path"));
  if (path && *path &&
      !clay_permissions_check(userdata, CLAY_PERMISSION_READ, "Read", path))
    return denied_result();
  return clay_fs_tool_read(arguments, userdata);
}

static ClayJson *glob_tool_gated(const ClayJson *arguments, void *userdata) {
  const char *pattern =
      clay_json_string_value(clay_json_object_get(arguments, "pattern"));
  if (pattern && *pattern &&
      !clay_permissions_check(userdata, CLAY_PERMISSION_READ,
                              "List files matching", pattern))
    return denied_result();
  return clay_fs_tool_glob(arguments, userdata);
}

static ClayJson *grep_tool_gated(const ClayJson *arguments, void *userdata) {
  const char *pattern =
      clay_json_string_value(clay_json_object_get(arguments, "pattern"));
  if (pattern && *pattern &&
      !clay_permissions_check(userdata, CLAY_PERMISSION_READ, "Search for",
                              pattern))
    return denied_result();
  return clay_fs_tool_grep(arguments, userdata);
}

static ClayJson *write_tool_checkpointed(const ClayJson *arguments,
                                         void *userdata) {
  if (((ClayCommands *)userdata)->mode == CLAY_MODE_PLAN)
    return plan_blocked_result("writing files is disabled");
  const char *path =
      clay_json_string_value(clay_json_object_get(arguments, "path"));
  if (path && *path &&
      !clay_permissions_check(userdata, CLAY_PERMISSION_EDIT, "Write", path))
    return denied_result();
  ClayStr label;
  clay_str_init(&label);
  clay_str_printf(&label, "write: %s", path && *path ? path : "?");
  clay_commands_checkpoint(userdata, label.data);
  clay_commands_undo_prepare(userdata, path);
  clay_str_free(&label);
  ClayJson *result = clay_fs_tool_write(arguments, userdata);
  if (clay_json_bool_value(clay_json_object_get(result, "ok")))
    clay_commands_undo_commit(userdata);
  else
    clay_commands_undo_discard(userdata);
  run_auto_test(userdata, result);
  return result;
}

static ClayJson *edit_tool_checkpointed(const ClayJson *arguments,
                                        void *userdata) {
  if (((ClayCommands *)userdata)->mode == CLAY_MODE_PLAN)
    return plan_blocked_result("editing files is disabled");
  const char *path =
      clay_json_string_value(clay_json_object_get(arguments, "path"));
  if (path && *path &&
      !clay_permissions_check(userdata, CLAY_PERMISSION_EDIT, "Edit", path))
    return denied_result();
  ClayStr label;
  clay_str_init(&label);
  clay_str_printf(&label, "edit: %s", path && *path ? path : "?");
  clay_commands_checkpoint(userdata, label.data);
  clay_commands_undo_prepare(userdata, path);
  clay_str_free(&label);
  ClayJson *result = clay_fs_tool_edit(arguments, userdata);
  if (clay_json_bool_value(clay_json_object_get(result, "ok")))
    clay_commands_undo_commit(userdata);
  else
    clay_commands_undo_discard(userdata);
  run_auto_test(userdata, result);
  return result;
}

typedef struct {
  ClayCommands *commands;
} ShellAuthorization;

static int blank_command(const char *text) {
  while (*text) {
    if (*text != ' ' && *text != '\t' && *text != '\n' && *text != '\r')
      return 0;
    text++;
  }
  return 1;
}

/* Tool results are model input, not terminal output. Remove CSI/OSC escape
   sequences leaked by terminal UI buffering around a fork. */
static void strip_terminal_escapes(ClayStr *text) {
  ClayStr clean;
  clay_str_init(&clean);
  for (size_t i = 0; i < text->len; i++) {
    unsigned char ch = (unsigned char)text->data[i];
    if (ch == 0x1b && i + 1 < text->len) {
      if (text->data[++i] == '[') {
        while (i + 1 < text->len) {
          unsigned char end = (unsigned char)text->data[++i];
          if (end >= 0x40 && end <= 0x7e) break;
        }
      } else if (text->data[i] == ']') {
        while (i + 1 < text->len) {
          unsigned char end = (unsigned char)text->data[++i];
          if (end == '\a' || (end == '\\' && i > 0 && text->data[i - 1] == 0x1b)) break;
        }
      }
      continue;
    }
    if (ch >= 0x20 || ch == '\n' || ch == '\t' || ch == '\r')
      clay_str_push_char(&clean, (char)ch);
  }
  clay_str_free(text);
  *text = clean;
}

static int authorize_shell_command(char *const argv[], void *user_data) {
  ShellAuthorization *authorization = user_data;
  ClayCommands *commands = authorization->commands;
  ClayStr detail;
  clay_str_init(&detail);
  for (size_t i = 0; argv[i]; i++)
    clay_str_printf(&detail, "%s%s", i ? " " : "", argv[i]);
  /* A shell AST tells us command boundaries, not the side effects of an
     arbitrary executable. Plan mode therefore permits only the curated
     read-only set; anything else needs Act mode and an explicit permission. */
  if (commands->mode == CLAY_MODE_PLAN &&
      !clay_permissions_is_safe_command(detail.data)) {
    clay_str_free(&detail);
    return 0;
  }
  ClayPermissionCategory category = clay_permissions_is_safe_command(detail.data)
                                       ? CLAY_PERMISSION_EXEC_SAFE
                                       : CLAY_PERMISSION_EXEC_ALL;
  int allowed = clay_permissions_check(commands, category, "Run", detail.data);
  clay_str_free(&detail);
  return allowed;
}

static ClayJson *shell_exec_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  const char *command =
      clay_json_string_value(clay_json_object_get(arguments, "command"));
  const char *args =
      clay_json_string_value(clay_json_object_get(arguments, "args"));
  ClayJson *result = clay_json_object();
  if (!*command || blank_command(command)) {
    clay_json_object_set(result, "ok", clay_json_bool(0));
    clay_json_object_set(result, "error",
                         clay_json_string("command is required"));
    return result;
  }
  ClayStr invocation;
  clay_str_init(&invocation);
  clay_str_push(&invocation, command);
  if (*args)
    clay_str_printf(&invocation, " %s", args);
  int integrated_shell = commands->use_integrated_shell &&
                         commands->sandbox_mode == CLAY_SANDBOX_MODE_SANDBOX;
#ifdef _WIN32
  integrated_shell = commands->use_integrated_shell;
#endif
  if (integrated_shell) {
    ShellAuthorization authorization = {commands};
    if (clay_shell_authorize(invocation.data, authorize_shell_command,
                             &authorization) != 0) {
      clay_json_free(result);
      clay_str_free(&invocation);
      return denied_result();
    }
  }
  if (!integrated_shell && commands->mode == CLAY_MODE_PLAN &&
      clay_permissions_is_mutating_command(invocation.data)) {
    clay_json_free(result);
    ClayJson *blocked =
        plan_blocked_result("this command would mutate the workspace");
    clay_str_free(&invocation);
    return blocked;
  }
  if (!integrated_shell) {
    ClayPermissionCategory exec_category =
        clay_permissions_is_safe_command(invocation.data)
            ? CLAY_PERMISSION_EXEC_SAFE
            : CLAY_PERMISSION_EXEC_ALL;
    if (!clay_permissions_check(commands, exec_category, "Run",
                                invocation.data)) {
      clay_json_free(result);
      clay_str_free(&invocation);
      return denied_result();
    }
  }
  clay_commands_checkpoint(commands, invocation.data);
  int timeout_seconds =
      (int)clay_json_number_value(clay_json_object_get(arguments, "timeout_seconds"));
  ClayStr output;
  clay_str_init(&output);
  int exit_code = -1;
  int output_truncated = 0;
  char *workspace_dir = clay_term_cwd();
  char *scratch_dir = clay_chat_scratch_dir(commands->chat);
  size_t readonly_mount_count = 0;
  char **readonly_mounts = clay_config_sandbox_readonly_mounts(&readonly_mount_count);
  ClaySandboxConfig sandbox = {
      .mode = commands->sandbox_mode,
      .workspace_dir = workspace_dir,
      .scratch_dir = scratch_dir,
      .use_integrated_shell = integrated_shell,
      .readonly_mounts = (const char *const *)readonly_mounts,
      .readonly_mount_count = readonly_mount_count,
  };
  ClayExecOptions options = {0};
  options.timeout_seconds = timeout_seconds;
  ClayExecResult exec = {0};
  int rc = clay_sandbox_exec(&sandbox, invocation.data, &output,
                             CLAY_SHELL_CAPTURE_LIMIT, &options, &exec);
  exit_code = exec.exit_code;
  output_truncated = exec.output_truncated;
  free(workspace_dir);
  free(scratch_dir);
  for (size_t i = 0; i < readonly_mount_count; i++) free(readonly_mounts[i]);
  free(readonly_mounts);
  strip_terminal_escapes(&output);
  clay_json_object_set(result, "command", clay_json_string(invocation.data));
  clay_json_object_set(result, "ok", clay_json_bool(rc == 0 && exit_code == 0));
  clay_json_object_set(result, "exit_code", clay_json_number(exit_code));
  if (output.len > CLAY_SHELL_OUTPUT_LIMIT) {
    char *scratch_path =
        clay_chat_dump_scratch(commands->chat, "shell", output.data);
    ClayStr preview;
    clay_str_init(&preview);
    clay_str_push_n(&preview, output.data, CLAY_SHELL_OUTPUT_LIMIT);
    if (scratch_path)
      clay_str_printf(&preview, "\n... (%zu bytes total, full output at %s)",
                      output.len, scratch_path);
    else
      clay_str_push(&preview, "\n... (truncated)");
    clay_json_object_set(result, "output", clay_json_string(preview.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(1));
    if (scratch_path)
      clay_json_object_set(result, "scratch_path",
                           clay_json_string(scratch_path));
    clay_str_free(&preview);
    free(scratch_path);
  } else {
    clay_json_object_set(result, "output", clay_json_string(output.data));
    clay_json_object_set(result, "output_truncated",
                         clay_json_bool(output_truncated));
  }
  if (rc != 0)
    clay_json_object_set(result, "error",
                         clay_json_string("failed to start command"));
  if (exec.timed_out) {
    clay_json_object_set(result, "timed_out", clay_json_bool(1));
    clay_json_object_set(
        result, "error",
        clay_json_string("timed out and was killed; the output above is what "
                         "it produced first. Raise timeout_seconds for a slow "
                         "one-shot command, or start a long-running one with "
                         "task_run instead."));
  }
  clay_str_free(&output);
  clay_str_free(&invocation);
  return result;
}

static ClayJson *memory_save_tool(const ClayJson *arguments, void *userdata) {
  (void)userdata;
  const char *slug =
      clay_json_string_value(clay_json_object_get(arguments, "slug"));
  const char *type =
      clay_json_string_value(clay_json_object_get(arguments, "type"));
  const char *summary =
      clay_json_string_value(clay_json_object_get(arguments, "summary"));
  const char *content =
      clay_json_string_value(clay_json_object_get(arguments, "content"));
  ClayJson *result = clay_json_object();
  if (!clay_memory_valid_slug(slug)) {
    clay_json_object_set(result, "ok", clay_json_bool(0));
    clay_json_object_set(
        result, "error",
        clay_json_string("slug must be lowercase letters, digits, and hyphens, "
                         "up to 64 chars"));
    return result;
  }
  int rc = clay_memory_write(slug, type, summary, content);
  clay_json_object_set(result, "ok", clay_json_bool(rc == 0));
  if (rc != 0)
    clay_json_object_set(result, "error",
                         clay_json_string("failed to write memory entry"));
  return result;
}

static ClayJson *memory_save_schema(void) {
  ClayJson *slug = clay_json_object();
  clay_json_object_set(slug, "type", clay_json_string("string"));
  clay_json_object_set(
      slug, "description",
      clay_json_string("Short id, lowercase letters/digits/hyphens (e.g. "
                       "\"auth-compliance-decision\"). "
                       "Reuse an existing slug to overwrite that entry."));
  ClayJson *type = clay_json_object();
  clay_json_object_set(type, "type", clay_json_string("string"));
  clay_json_object_set(
      type, "description",
      clay_json_string("One word for the kind of memory, e.g. \"decision\", "
                       "\"bug-fix\", \"preference\"."));
  ClayJson *summary = clay_json_object();
  clay_json_object_set(summary, "type", clay_json_string("string"));
  clay_json_object_set(
      summary, "description",
      clay_json_string("One-line summary shown in the memory index - specific "
                       "enough to judge relevance without "
                       "opening the entry."));
  ClayJson *content = clay_json_object();
  clay_json_object_set(content, "type", clay_json_string("string"));
  clay_json_object_set(
      content, "description",
      clay_json_string(
          "The full memory: what happened, why it matters, anything learned."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "slug", slug);
  clay_json_object_set(properties, "type", type);
  clay_json_object_set(properties, "summary", summary);
  clay_json_object_set(properties, "content", content);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("slug"));
  clay_json_array_push(required, clay_json_string("summary"));
  clay_json_array_push(required, clay_json_string("content"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

static ClayJson *memory_read_tool(const ClayJson *arguments, void *userdata) {
  (void)userdata;
  const char *slug =
      clay_json_string_value(clay_json_object_get(arguments, "slug"));
  ClayJson *result = clay_json_object();
  char *content = clay_memory_read(slug);
  if (!content) {
    clay_json_object_set(result, "ok", clay_json_bool(0));
    clay_json_object_set(result, "error",
                         clay_json_string("no memory entry with that slug"));
    return result;
  }
  clay_json_object_set(result, "ok", clay_json_bool(1));
  clay_json_object_set(result, "content", clay_json_string(content));
  free(content);
  return result;
}

static ClayJson *memory_read_schema(void) {
  ClayJson *slug = clay_json_object();
  clay_json_object_set(slug, "type", clay_json_string("string"));
  clay_json_object_set(
      slug, "description",
      clay_json_string("Slug from the memory index in your system prompt."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "slug", slug);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("slug"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

static ClayJson *remember_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  const char *content =
      clay_json_string_value(clay_json_object_get(arguments, "content"));
  int rc = clay_chat_set_notes(commands->chat, content);
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(rc == 0));
  if (rc != 0)
    clay_json_object_set(result, "error",
                         clay_json_string("failed to save notes"));
  return result;
}

static ClayJson *remember_schema(void) {
  ClayJson *content = clay_json_object();
  clay_json_object_set(content, "type", clay_json_string("string"));
  clay_json_object_set(
      content, "description",
      clay_json_string(
          "Full replacement for this chat's scratchpad - rewrite everything "
          "still relevant, not "
          "just what changed. Shown alongside this conversation every turn."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "content", content);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("content"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

static ClayJson *skill_tool(const ClayJson *arguments, void *userdata) {
  (void)userdata;
  const char *name =
      clay_json_string_value(clay_json_object_get(arguments, "name"));
  ClayJson *result = clay_json_object();
  char *content = clay_skill_read(name);
  if (!content) {
    clay_json_object_set(result, "ok", clay_json_bool(0));
    clay_json_object_set(result, "error",
                         clay_json_string("no enabled skill with that name"));
    return result;
  }
  clay_json_object_set(result, "ok", clay_json_bool(1));
  clay_json_object_set(result, "content", clay_json_string(content));
  free(content);
  return result;
}

static ClayJson *skill_schema(void) {
  ClayJson *name = clay_json_object();
  clay_json_object_set(name, "type", clay_json_string("string"));
  clay_json_object_set(
      name, "description",
      clay_json_string("Name from the skill index in your system prompt."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "name", name);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("name"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

static ClayJson *ask_user_error(const char *message) {
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(0));
  clay_json_object_set(result, "error", clay_json_string(message));
  return result;
}

/* Asks the user one question through the same picker widget /permissions
   uses, and hands the answer back to the model in the same turn. */
ClayJson *ask_user_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  const char *question =
      clay_json_string_value(clay_json_object_get(arguments, "question"));
  if (!*question)
    return ask_user_error("question is required");

  const ClayJson *options = clay_json_object_get(arguments, "options");
  size_t count = clay_json_array_count(options);
  if (count > CLAY_ASK_USER_MAX_OPTIONS)
    return ask_user_error(
        "at most 6 options; ask a narrower question instead");

  const ClayJson *allow = clay_json_object_get(arguments, "allow_custom");
  int allow_custom = allow ? clay_json_bool_value(allow) : 1;
  if (count == 0)
    allow_custom = 1;

  ClayChoice choices[CLAY_ASK_USER_MAX_OPTIONS];
  for (size_t i = 0; i < count; i++) {
    const ClayJson *option = clay_json_array_get(options, i);
    const char *label =
        clay_json_string_value(clay_json_object_get(option, "label"));
    const char *desc =
        clay_json_string_value(clay_json_object_get(option, "description"));
    if (!*label)
      return ask_user_error("every option needs a non-empty label");
    choices[i].title = label;
    choices[i].desc = *desc ? desc : NULL;
  }

  if (!clay_term_is_interactive())
    return ask_user_error("this session has no interactive terminal, so the "
                          "user cannot answer: pick the most reasonable "
                          "option, state the assumption you made, and "
                          "continue");

  clay_term_notify("Clay needs an answer", question);
  char *custom = NULL;
  int index = clay_app_choice(commands->app, question, choices, (int)count,
                              allow_custom, &custom);
  const char *answer =
      index >= 0 && index < (int)count ? choices[index].title : custom;
  if (!answer || !*answer) {
    free(custom);
    return ask_user_error("the user dismissed the question without answering");
  }

  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(1));
  clay_json_object_set(result, "answer", clay_json_string(answer));
  clay_json_object_set(result, "output", clay_json_string(answer));
  if (index >= 0)
    clay_json_object_set(result, "selected_index", clay_json_number(index));
  free(custom);
  return result;
}

ClayJson *ask_user_schema(void) {
  ClayJson *question = clay_json_object();
  clay_json_object_set(question, "type", clay_json_string("string"));
  clay_json_object_set(
      question, "description",
      clay_json_string("One focused question, in the user's own terms."));

  ClayJson *label = clay_json_object();
  clay_json_object_set(label, "type", clay_json_string("string"));
  clay_json_object_set(label, "description",
                       clay_json_string("The answer itself, a few words."));
  ClayJson *description = clay_json_object();
  clay_json_object_set(description, "type", clay_json_string("string"));
  clay_json_object_set(
      description, "description",
      clay_json_string("Optional one-line note on what picking this means."));
  ClayJson *option_properties = clay_json_object();
  clay_json_object_set(option_properties, "label", label);
  clay_json_object_set(option_properties, "description", description);
  ClayJson *option_required = clay_json_array();
  clay_json_array_push(option_required, clay_json_string("label"));
  ClayJson *option = clay_json_object();
  clay_json_object_set(option, "type", clay_json_string("object"));
  clay_json_object_set(option, "properties", option_properties);
  clay_json_object_set(option, "required", option_required);

  ClayJson *options = clay_json_object();
  clay_json_object_set(options, "type", clay_json_string("array"));
  clay_json_object_set(options, "items", option);
  clay_json_object_set(
      options, "description",
      clay_json_string("Two to four concrete answers, most likely first. Omit "
                       "only when no option can be guessed."));

  ClayJson *allow_custom = clay_json_object();
  clay_json_object_set(allow_custom, "type", clay_json_string("boolean"));
  clay_json_object_set(
      allow_custom, "description",
      clay_json_string("Offer a \"Type your own...\" row too. Default true; "
                       "set false only when the options are exhaustive."));

  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "question", question);
  clay_json_object_set(properties, "options", options);
  clay_json_object_set(properties, "allow_custom", allow_custom);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("question"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

static int valid_todo_status(const char *status) {
  return strcmp(status, "pending") == 0 || strcmp(status, "in_progress") == 0 ||
         strcmp(status, "completed") == 0;
}

ClayJson *todowrite_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  const ClayJson *todos = clay_json_object_get(arguments, "todos");
  ClayJson *result = clay_json_object();
  if (clay_json_type(todos) != CLAY_JSON_ARRAY) {
    clay_json_object_set(result, "ok", clay_json_bool(0));
    clay_json_object_set(result, "error",
                         clay_json_string("todos must be an array"));
    return result;
  }

  ClayArray items;
  clay_array_init(&items, sizeof(ClayTodoItem));
  for (size_t i = 0; i < clay_json_array_count(todos); i++) {
    ClayJson *entry = clay_json_array_get(todos, i);
    const char *content =
        clay_json_string_value(clay_json_object_get(entry, "content"));
    const char *status =
        clay_json_string_value(clay_json_object_get(entry, "status"));
    if (!content || !*content || !status || !valid_todo_status(status)) {
      for (size_t j = 0; j < items.count; j++) {
        ClayTodoItem *item = clay_array_get(&items, j);
        free(item->content);
        free(item->status);
      }
      clay_array_free(&items);
      clay_json_object_set(result, "ok", clay_json_bool(0));
      clay_json_object_set(
          result, "error",
          clay_json_string("each todo needs non-empty content and status "
                           "pending/in_progress/completed"));
      return result;
    }
    ClayTodoItem item = {strdup(content), strdup(status)};
    clay_array_push_val(&items, &item);
  }

  clay_commands_clear_todos(commands);
  clay_array_free(&commands->todos);
  commands->todos = items;

  ClayStr output;
  clay_str_init(&output);
  for (size_t i = 0; i < commands->todos.count; i++) {
    ClayTodoItem *item = clay_array_get(&commands->todos, i);
    const char *box = strcmp(item->status, "completed") == 0 ? CLAY_ICON_CHECK
                      : strcmp(item->status, "in_progress") == 0
                          ? CLAY_ICON_ARROW
                          : CLAY_ICON_DOT;
    clay_str_printf(&output, "[%s] %s\n", box, item->content);
  }
  clay_json_object_set(result, "ok", clay_json_bool(1));
  clay_json_object_set(result, "output", clay_json_string(output.data));
  clay_json_object_set(result, "output_truncated", clay_json_bool(0));
  clay_str_free(&output);
  return result;
}

ClayJson *todowrite_schema(void) {
  ClayJson *content = clay_json_object();
  clay_json_object_set(content, "type", clay_json_string("string"));
  clay_json_object_set(content, "description",
                       clay_json_string("One task, in imperative form."));
  ClayJson *status = clay_json_object();
  clay_json_object_set(status, "type", clay_json_string("string"));
  ClayJson *status_enum = clay_json_array();
  clay_json_array_push(status_enum, clay_json_string("pending"));
  clay_json_array_push(status_enum, clay_json_string("in_progress"));
  clay_json_array_push(status_enum, clay_json_string("completed"));
  clay_json_object_set(status, "enum", status_enum);
  ClayJson *item_properties = clay_json_object();
  clay_json_object_set(item_properties, "content", content);
  clay_json_object_set(item_properties, "status", status);
  ClayJson *item_required = clay_json_array();
  clay_json_array_push(item_required, clay_json_string("content"));
  clay_json_array_push(item_required, clay_json_string("status"));
  ClayJson *item_schema = clay_json_object();
  clay_json_object_set(item_schema, "type", clay_json_string("object"));
  clay_json_object_set(item_schema, "properties", item_properties);
  clay_json_object_set(item_schema, "required", item_required);
  clay_json_object_set(item_schema, "additionalProperties", clay_json_bool(0));

  ClayJson *todos = clay_json_object();
  clay_json_object_set(todos, "type", clay_json_string("array"));
  clay_json_object_set(todos, "items", item_schema);
  clay_json_object_set(
      todos, "description",
      clay_json_string("The full plan, replacing whatever was there before - "
                       "resend every task, not just the "
                       "one that changed. Exactly one task should be "
                       "in_progress at a time."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "todos", todos);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("todos"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

static ClayJson *shell_exec_schema(void) {
  ClayJson *command = clay_json_object();
  clay_json_object_set(command, "type", clay_json_string("string"));
  clay_json_object_set(command, "description",
                       clay_json_string("Program or shell command to run."));
  ClayJson *args = clay_json_object();
  clay_json_object_set(args, "type", clay_json_string("string"));
  clay_json_object_set(
      args, "description",
      clay_json_string("Optional command arguments, including shell quoting."));
  ClayJson *timeout = clay_json_object();
  clay_json_object_set(timeout, "type", clay_json_string("number"));
  clay_json_object_set(
      timeout, "description",
      clay_json_string("Seconds to wait before the command is killed "
                       "(default 120, max 3600). A command that blocks until "
                       "you stop it belongs in task_run, not here."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "command", command);
  clay_json_object_set(properties, "args", args);
  clay_json_object_set(properties, "timeout_seconds", timeout);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("command"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

static void hide_status(ClayConversationStream *stream) {
  if (!stream->status_visible)
    return;
  clay_below_set_editing(0);
  clay_below_finish();
  stream->status_visible = 0;
}

static void set_status(double seconds, int success) {
  ClayStr text;
  clay_str_init(&text);
  clay_str_printf(&text, "%s%.1fs%s",
                  clay_color(success ? CLAY_GREEN : CLAY_RED), seconds,
                  clay_color(CLAY_RESET));
  clay_below_set_text("status", text.data);
  clay_below_stop_elapsed("status");
  clay_below_set_state("status",
                       success ? CLAY_BELOW_FINISHED : CLAY_BELOW_NONE);
  clay_below_set_enabled("status", 1);
  clay_str_free(&text);
}

static void collapse_thinking(ClayConversationStream *stream) {
  if (!stream->thinking_output_started)
    return;
  clay_thinking_finish(stream->thinking_seconds);
  stream->thinking_output_started = 0;
}

static void show_thinking(ClayConversationStream *stream) {
  clay_below_set_text("status", "");
  clay_below_set_state("status", CLAY_BELOW_LOADING);
  clay_below_set_enabled("status", 1);
  clay_below_start_elapsed("status");
  if (clay_term_is_interactive()) {
    clay_below_set_editing(1);
    clay_below_render_status();
    stream->status_visible = 1;
  }
}

static void finish_thinking(ClayConversationStream *stream) {
  if (!stream->thinking_active) return;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  stream->thinking_seconds =
      (double)(now.tv_sec - stream->thinking_started.tv_sec) +
      (double)(now.tv_nsec - stream->thinking_started.tv_nsec) / 1e9;
  if (stream->thinking_seconds < 0) stream->thinking_seconds = 0;
  stream->thinking_active = 0;
}

static void on_reasoning(const char *text, void *userdata) {
  if (!text || !*text) return;
  ClayConversationStream *stream = userdata;
  if (!stream->thinking_active) {
    clock_gettime(CLOCK_MONOTONIC, &stream->thinking_started);
    stream->thinking_active = 1;
  }
  size_t len = strlen(text);
  if (stream->thinking.len <= CLAY_THINKING_LIMIT &&
      len <= CLAY_THINKING_LIMIT - stream->thinking.len)
    clay_str_push_n(&stream->thinking, text, len);
  if (!stream->response_active && clay_term_is_interactive()) {
    if (!stream->thinking_output_started) {
      /* Reasoning text takes the row the spinner was on: hand it back
         whole, or clay_thinking_finish's row math erases the wrong rows.
         The elapsed clock keeps running for the rest of the turn. */
      clay_below_set_enabled("status", 0);
      hide_status(stream);
      clay_thinking_begin();
      stream->thinking_output_started = 1;
    }
    clay_thinking_write(text);
  }
}

static void persist_thinking(ClayCommands *commands,
                             ClayConversationStream *stream) {
  if (stream->thinking.len > 0)
    clay_chat_set_active_thinking(commands->chat, stream->thinking.data,
                                  stream->thinking_seconds);
}

static void response_write_text(const char *text, void *user_data) {
  (void)user_data;
  clay_response_write(text);
}

/* A new row means the pinned status row has to move down first. */
static void response_break_row(void *user_data) {
  ClayConversationStream *stream = user_data;
  if (stream->status_visible)
    clay_below_status_push_down();
  clay_response_write("\n");
}

static void close_response_for_tool(ClayConversationStream *stream) {
  clay_wrap_flush(&stream->wrap);
  clay_below_set_editing(0);
  if (stream->response_active) {
    clay_response_end();
    if (stream->status_visible)
      clay_below_status_finish_output();
    else
      fputc('\n', stdout);
    stream->response_active = 0;
    stream->status_visible = 0;
  } else if (stream->status_visible) {
    clay_below_finish();
    stream->status_visible = 0;
  }
}

static void append_label_text(ClayStr *out, const char *text) {
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    if (*p == 0x1b)
      clay_str_push(out, "\\x1b");
    else if (*p == '\t')
      clay_str_push_char(out, ' ');
    else if (*p < 0x20)
      clay_str_push_char(out, '?');
    else
      clay_str_push_char(out, (char)*p);
  }
}

static void tool_label(ClayStr *out, const char *name, int completed,
                       int success, const char *detail) {
  const char *verb = NULL;
  if (strcmp(name, "shell_exec") == 0)
    verb = completed ? (success ? "Executed" : "Failed")
                     : "Executing shell command";
  else if (strcmp(name, "memory_save") == 0)
    verb = completed ? (success ? "Saved memory" : "Failed to save memory")
                     : "Saving memory";
  else if (strcmp(name, "memory_read") == 0)
    verb = completed ? (success ? "Read memory" : "Failed to read memory")
                     : "Reading memory";
  else if (strcmp(name, "remember") == 0)
    verb = completed ? (success ? "Updated notes" : "Failed to update notes")
                     : "Updating notes";
  else if (strcmp(name, "read") == 0)
    verb = completed ? (success ? "Read" : "Failed to read") : "Reading";
  else if (strcmp(name, "write") == 0)
    verb = completed ? (success ? "Wrote" : "Failed to write") : "Writing";
  else if (strcmp(name, "edit") == 0)
    verb = completed ? (success ? "Edited" : "Failed to edit") : "Editing";
  else if (strcmp(name, "glob") == 0)
    verb = completed ? (success ? "Found files" : "Glob failed") : "Globbing";
  else if (strcmp(name, "grep") == 0)
    verb = completed ? (success ? "Searched" : "Search failed") : "Searching";
  else if (strcmp(name, "todowrite") == 0)
    verb = completed ? (success ? "Updated plan" : "Failed to update plan")
                     : "Updating plan";
  else if (strcmp(name, "repo_map") == 0)
    verb = completed ? (success ? "Mapped repo" : "Repo map failed")
                     : "Mapping repo";
  else if (strcmp(name, "ask_user") == 0)
    verb = completed ? (success ? "Asked the user" : "Question unanswered")
                     : "Asking the user";
  else if (strcmp(name, "task_run") == 0)
    verb = completed ? (success ? "Started background task" : "Failed to start")
                     : "Starting background task";
  else if (strcmp(name, "task_output") == 0)
    verb = completed ? (success ? "Read task output" : "No such task")
                     : "Reading task output";
  else if (strcmp(name, "task_stop") == 0)
    verb = completed ? (success ? "Stopped task" : "No such task")
                     : "Stopping task";
  else if (strcmp(name, "task_list") == 0)
    verb = completed ? (success ? "Listed tasks" : "Failed to list tasks")
                     : "Listing tasks";
  if (verb) {
    clay_str_push(out, verb);
    if (detail && *detail) {
      clay_str_push(out, ": ");
      append_label_text(out, detail);
    }
    return;
  }
  clay_str_push(out, completed ? (success ? "Executed: " : "Failed: ")
                               : "Executing: ");
  append_label_text(out, name);
}

/* Which argument/result field carries the detail worth showing next to a
   tool's status label, if any. */
static const char *tool_detail_key(const char *name) {
  if (strcmp(name, "read") == 0 || strcmp(name, "write") == 0 ||
      strcmp(name, "edit") == 0)
    return "path";
  if (strcmp(name, "task_run") == 0)
    return "command";
  if (strcmp(name, "glob") == 0 || strcmp(name, "grep") == 0)
    return "pattern";
  return NULL;
}

static int command_fits_inline(const char *command) {
  ClayStr label;
  clay_str_init(&label);
  clay_str_push(&label, "Executed $");
  append_label_text(&label, command);
  size_t width = clay_utf8_width("  ") +
                 clay_utf8_width(label.data) + 8;
  clay_str_free(&label);
  return width < (size_t)clay_term_width();
}

static void print_tool_output(const ClayJson *result, int show_command,
                              int show_error) {
  const char *command =
      clay_json_string_value(clay_json_object_get(result, "command"));
  const char *output =
      clay_json_string_value(clay_json_object_get(result, "output"));
  const char *error =
      clay_json_string_value(clay_json_object_get(result, "error"));
  int truncated =
      clay_json_bool_value(clay_json_object_get(result, "output_truncated"));
  if (show_command && *command) {
    ClayStr display;
    clay_str_init(&display);
    append_label_text(&display, command);
    clay_tool_output_line("$ %s", display.data);
    clay_str_free(&display);
  }
  if (!*output) {
    /* A failure with nothing on stdout still has something to say, unless
       the status line above already said it. */
    if (show_error && *error)
      clay_tool_output_line("%s", error);
    else if (!*error)
      clay_tool_output_line("(no output)");
    return;
  }
  ClayStr line;
  clay_str_init(&line);
  int shown = 0;
  int omitted = 0;
  for (const unsigned char *p = (const unsigned char *)output;; p++) {
    if (*p == '\n' || *p == '\0') {
      if (line.len > 0 && shown < CLAY_TOOL_VISIBLE_LINES) {
        clay_tool_output_line("%s", line.data);
        shown++;
      } else if (line.len > 0)
        omitted = 1;
      clay_str_clear(&line);
      if (*p == '\0')
        break;
    } else if (*p != '\r') {
      if (*p == 0x1b)
        clay_str_push(&line, "\\x1b");
      else if (*p == '\t')
        clay_str_push_char(&line, ' ');
      else if (*p < 0x20)
        clay_str_push_char(&line, '?');
      else
        clay_str_push_char(&line, (char)*p);
    }
  }
  if (omitted || truncated)
    clay_tool_output_line("%s…%s", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
  clay_str_free(&line);
}

static void on_tool_call(const char *name, const char *arguments_json,
                         void *userdata) {
  ClayConversationStream *stream = userdata;
  finish_thinking(stream);
  collapse_thinking(stream);
  close_response_for_tool(stream);
  clay_str_clear(&stream->response);
  if (clay_term_is_interactive())
    clay_term_raw_disable();
  const char *detail_key = tool_detail_key(name);
  ClayJson *args = detail_key && arguments_json
                       ? clay_json_parse(arguments_json, NULL)
                       : NULL;
  const char *detail =
      args ? clay_json_string_value(clay_json_object_get(args, detail_key))
           : NULL;
  ClayStr label;
  clay_str_init(&label);
  tool_label(&label, name, 0, 0, detail);
  clay_json_free(args);
  stream->tool_task = clay_task_start("%s", label.data);
  clay_str_free(&label);
}

static void on_tool_result(const char *name, const ClayJson *result,
                           void *userdata) {
  ClayConversationStream *stream = userdata;
  int ok = clay_json_bool_value(clay_json_object_get(result, "ok"));
  long exit_code =
      (long)clay_json_number_value(clay_json_object_get(result, "exit_code"));
  const char *command =
      clay_json_string_value(clay_json_object_get(result, "command"));
  int inline_command = ok && strcmp(name, "shell_exec") == 0 && *command &&
                       command_fits_inline(command);
  const char *detail_key = tool_detail_key(name);
  const char *detail =
      detail_key
          ? clay_json_string_value(clay_json_object_get(result, detail_key))
          : NULL;
  int error_in_label = 0;
  if (stream->tool_task) {
    ClayStr label;
    clay_str_init(&label);
    if (inline_command) {
      clay_str_push(&label, "Executed $");
      append_label_text(&label, command);
    } else
      tool_label(&label, name, 1, ok, detail);
    if (ok) {
      clay_task_success_with_label(stream->tool_task, label.data, "");
    } else {
      /* An exit code only means something for a command; everything else
         fails with a message worth reading. */
      const char *error =
          clay_json_string_value(clay_json_object_get(result, "error"));
      if (clay_json_object_get(result, "exit_code"))
        clay_task_fail_with_label(stream->tool_task, label.data, "exit %ld",
                                  exit_code);
      else if (*error) {
        clay_task_fail_with_label(stream->tool_task, label.data, "- %s", error);
        error_in_label = 1;
      } else {
        clay_task_fail_with_label(stream->tool_task, label.data, "");
      }
    }
    clay_str_free(&label);
    stream->tool_task = NULL;
  } else
    clay_sayc(ok ? CLAY_GREEN : CLAY_RED, "%s: %s", ok ? "Executed" : "Failed",
              name);
  print_tool_output(result, !inline_command, !error_in_label);
  if (clay_term_is_interactive())
    clay_term_raw_enable();
  show_thinking(stream);
}

static void on_token(const char *text, void *userdata) {
  if (!text || !*text)
    return;
  ClayConversationStream *stream = userdata;
  finish_thinking(stream);
  collapse_thinking(stream);
  clay_str_push(&stream->response, text);
  if (!stream->response_active) {
    if (clay_term_is_interactive()) {
      /* Take the row under the answer back - reasoning output gave it up -
         so the end-of-turn separator and prompt land in the right place. */
      clay_below_set_editing(0);
      /* No animator from here on, so drop the spinner but keep the turn's
         clock: the row repaints on every line the answer streams. */
      clay_below_set_state("status", CLAY_BELOW_NONE);
      clay_below_set_enabled("status", 1);
      clay_below_render_status();
      clay_below_status_insert_above();
      /* clay_response_begin opens with a newline, which would otherwise put
         the first line of the answer on the pinned row. */
      clay_below_status_push_down();
      stream->status_visible = 1;
      stream->wrapping = 1;
    }
    clay_response_begin();
    clay_wrap_init(&stream->wrap, clay_response_prefix_width(),
                   response_write_text, response_break_row, stream);
    stream->response_active = 1;
    stream->started = 1;
  }
  /* Piped output keeps the model's own line breaks: wrapping is for a
     terminal the user is watching. */
  if (stream->wrapping)
    clay_wrap_write(&stream->wrap, text);
  else
    clay_response_write(text);
}


static void on_error(long status, const char *body, void *userdata) {
  (void)body;
  ((ClayConversationStream *)userdata)->error_status = status;
}

static void on_usage_details(const ClayTokenUsage *usage, void *userdata) {
  ClayConversationStream *stream = userdata;
  stream->input_tokens += usage->input_tokens;
  stream->output_tokens += usage->output_tokens;
  if (usage->cached_input_tokens_known) {
    stream->cached_input_tokens += usage->cached_input_tokens;
    stream->cached_input_tokens_known = 1;
  }
  stream->has_usage = 1;
}

static int should_abort(void *userdata) {
  (void)userdata;
  if (clay_term_take_think_toggle()) {
    clay_thinking_toggle();
    return 0;
  }
  return clay_term_take_escape() || clay_term_take_interrupt();
}

int clay_commands_run_message(ClayCommands *commands, const char *input) {
  if (!commands->selected_provider || !commands->selected_model) {
    clay_sayc(
        CLAY_RED,
        "Select a provider and model with /model before sending a message.");
    return 0;
  }
  clay_thinking_forget();
  ClayConnectedProvider *provider =
      clay_commands_find_provider(commands, commands->selected_provider);
  if (!provider) {
    clay_sayc(CLAY_RED, "Selected provider %s is not connected.",
              commands->selected_provider);
    return 0;
  }
  if (!commands->chat) {
    commands->chat = clay_chat_create(commands->system_prompt);
    if (!commands->chat) {
      clay_sayc(CLAY_RED, "Could not create a chat journal.");
      return 0;
    }
    clay_commands_reset_conversation(commands);
  }
  if (clay_chat_begin_turn(commands->chat, input) != 0) {
    clay_sayc(CLAY_RED, "Could not save the chat journal.");
    return 0;
  }
  if (clay_term_is_interactive())
    fputc('\n', stdout); /* breathing room after the user's prompt */
  clay_turn_header(commands->selected_model);
  int collapsed = clay_commands_maybe_compact(commands);
  if (collapsed > 0) {
    clay_sayc(
        CLAY_GRAY,
        "Compacted %d old tool result%s to stay within the context budget.",
        collapsed, collapsed == 1 ? "" : "s");
  }
  size_t history_end = clay_json_array_count(commands->conversation);
  ClayJson *messages = clay_json_clone(commands->conversation);
  const char *notes = clay_chat_notes(commands->chat);
  int has_notes = *notes != '\0';
  if (has_notes) {
    ClayStr block;
    clay_str_init(&block);
    clay_str_printf(&block, "Notes from earlier in this conversation:\n%s",
                    notes);
    clay_json_array_push(messages, clay_openai_message("system", block.data));
    clay_str_free(&block);
  }
  size_t turn_start = clay_json_array_count(messages);
  clay_json_array_push(messages, clay_openai_message("user", input));
  int is_codex = strcmp(provider->type->id, "openai-codex") == 0;
  int is_grok_subscription = strcmp(provider->type->id, "grok") == 0 &&
                             provider->grok_client != NULL;
  ClayOpenAI *client = NULL;
  ClayOpenAICodex *codex = NULL;
  ClayGrok *grok = NULL;
  if (is_codex) {
    ClayCodexCredentials credentials = {
        provider->config->access_token, provider->config->refresh_token,
        provider->config->id_token, provider->config->account_id,
        provider->config->expires_at};
    codex = clay_openai_codex_create(&credentials, commands->selected_model);
    clay_openai_codex_set_reasoning_effort(
        codex, clay_commands_reasoning_effort(commands)->id);
  } else if (is_grok_subscription) {
    ClayGrokCredentials credentials = {provider->config->access_token,
                                       provider->config->refresh_token,
                                       provider->config->id_token,
                                       provider->config->expires_at};
    grok = clay_grok_create(&credentials, commands->selected_model);
    clay_grok_set_reasoning_effort(
        grok, clay_commands_reasoning_effort(commands)->id);
  } else {
    client =
        clay_openai_create(provider->config->base_url, provider->config->apikey,
                           commands->selected_model);
    clay_openai_set_reasoning_effort(
        client, clay_commands_reasoning_effort(commands)->id);
  }
  if (!client && !codex && !grok) {
    clay_json_free(messages);
    clay_sayc(CLAY_RED, "Provider authentication is unavailable. Connect it "
                        "again with /connect.");
    return 0;
  }
  const char *session_id = clay_chat_id(commands->chat);
  if (codex)
    clay_openai_codex_set_prompt_cache_key(codex, session_id);
  else if (grok)
    clay_grok_set_conversation_id(grok, session_id);
  else if (client && strcmp(provider->type->id, "openai") == 0)
    clay_openai_set_prompt_cache_key(client, session_id);
  else if (client && strcmp(provider->type->id, "grok") == 0)
    clay_openai_set_extra_header(client, "x-grok-conv-id", session_id);
  ClayConversationStream stream = {0};
  clay_str_init(&stream.response);
  clay_str_init(&stream.thinking);
  ClayOpenAICallbacks callbacks = {0};
  callbacks.on_token = on_token;
  callbacks.on_tool_call = on_tool_call;
  callbacks.on_tool_result = on_tool_result;
  callbacks.on_usage_details = on_usage_details;
  callbacks.on_reasoning = on_reasoning;
  callbacks.on_error = on_error;
  callbacks.should_abort = should_abort;
  callbacks.userdata = &stream;
  commands->messages_sent++;
  struct timespec started_at;
  clock_gettime(CLOCK_MONOTONIC, &started_at);
  show_thinking(&stream);
  clay_app_set_state(commands->app, CLAY_APP_BUSY);
  ClayJson *shell_schema = shell_exec_schema();
  ClayJson *memory_save_schema_json = memory_save_schema();
  ClayJson *memory_read_schema_json = memory_read_schema();
  ClayJson *remember_schema_json = remember_schema();
  ClayJson *read_schema_json = clay_fs_tool_read_schema();
  ClayJson *write_schema_json = clay_fs_tool_write_schema();
  ClayJson *edit_schema_json = clay_fs_tool_edit_schema();
  ClayJson *glob_schema_json = clay_fs_tool_glob_schema();
  ClayJson *grep_schema_json = clay_fs_tool_grep_schema();
  ClayJson *todowrite_schema_json = todowrite_schema();
  ClayJson *repo_map_schema_json = clay_fs_tool_repo_map_schema();
  ClayJson *skill_schema_json = skill_schema();
  ClayJson *ask_user_schema_json = ask_user_schema();
  ClayJson *task_run_schema_json = task_run_schema();
  ClayJson *task_output_schema_json = task_output_schema();
  ClayJson *task_stop_schema_json = task_stop_schema();
  ClayJson *task_list_schema_json = task_list_schema();
  clay_commands_connect_mcp_servers(commands);
  ClayArray tool_list;
  clay_array_init(&tool_list, sizeof(ClayTool));
  ClayTool builtin_tools[] = {
      {"shell_exec",
       "Runs a shell command in the current workspace and returns stdout, "
       "stderr, and exit status.",
       shell_schema, shell_exec_tool, commands},
      {"memory_save",
       "Saves or updates a long-term memory entry that persists across every "
       "future chat.",
       memory_save_schema_json, memory_save_tool, commands},
      {"memory_read",
       "Reads one long-term memory entry by its slug from the index in your "
       "system prompt.",
       memory_read_schema_json, memory_read_tool, commands},
      {"remember",
       "Replaces this chat's short-term scratchpad, shown alongside the "
       "conversation every turn.",
       remember_schema_json, remember_tool, commands},
      {"read",
       "Reads a file from the workspace, with line numbers. Prefer this over "
       "shell_exec for reading files.",
       read_schema_json, read_tool_gated, commands},
      {"write",
       "Creates or overwrites a file in the workspace with the given content.",
       write_schema_json, write_tool_checkpointed, commands},
      {"edit",
       "Replaces an exact, unique text match in a file. Prefer this over "
       "shell_exec/sed for edits.",
       edit_schema_json, edit_tool_checkpointed, commands},
      {"glob",
       "Lists files in the workspace whose path matches a wildcard pattern.",
       glob_schema_json, glob_tool_gated, commands},
      {"grep",
       "Searches file contents in the workspace for a regular expression.",
       grep_schema_json, grep_tool_gated, commands},
      {"todowrite",
       "Writes the full task plan, shown to the user as a checklist. Use for "
       "any multi-step task.",
       todowrite_schema_json, todowrite_tool, commands},
      {"repo_map",
       "Lists the workspace's top-level definitions (functions, classes, "
       "structs, ...) ranked by how "
       "often each is referenced elsewhere. Good for orienting in an "
       "unfamiliar codebase before "
       "reading specific files.",
       repo_map_schema_json, clay_fs_tool_repo_map, commands},
      {"skill",
       "Loads one skill's full instructions by name from the index in your "
       "system prompt. Call it before starting a task a skill covers.",
       skill_schema_json, skill_tool, commands},
      {"ask_user",
       "Asks the user one question in their terminal, with options to pick "
       "from, and returns their answer. Use it when an unknown would change "
       "what you build.",
       ask_user_schema_json, ask_user_tool, commands},
      {"task_run",
       "Starts a command in the background and returns right away. For "
       "anything that keeps running until you stop it: a dev server, a "
       "watcher, a tail.",
       task_run_schema_json, task_run_tool, commands},
      {"task_output",
       "Returns what a background task has printed so far, plus whether it "
       "is still running.",
       task_output_schema_json, task_output_tool, commands},
      {"task_stop",
       "Stops a background task and returns its exit status and final "
       "output.",
       task_stop_schema_json, task_stop_tool, commands},
      {"task_list", "Lists this session's background tasks and their status.",
       task_list_schema_json, task_list_tool, commands},
  };
  for (size_t i = 0; i < sizeof(builtin_tools) / sizeof(builtin_tools[0]);
       i++) {
    clay_array_push_val(&tool_list, &builtin_tools[i]);
  }
  for (size_t i = 0; i < commands->mcp_bindings.count; i++) {
    ClayMcpToolBinding *binding = clay_array_get(&commands->mcp_bindings, i);
    const ClayMcpTool *mcp_tool =
        clay_mcp_find_tool(binding->server, binding->tool_name);
    ClayTool tool = {binding->exposed_name, mcp_tool->description,
                     mcp_tool->input_schema, clay_mcp_tool_call_fn, binding};
    clay_array_push_val(&tool_list, &tool);
  }

  if (clay_term_is_interactive())
    clay_term_raw_enable();
  int rc = is_codex ? clay_openai_codex_run(codex, messages, tool_list.data,
                                            tool_list.count, 8, &callbacks)
                    : is_grok_subscription
                          ? clay_grok_run(grok, messages, tool_list.data,
                                          tool_list.count, 8, &callbacks)
                          : clay_openai_run(client, messages, tool_list.data,
                                            tool_list.count, 8, &callbacks);
  if (clay_term_is_interactive())
    clay_term_raw_disable();
  clay_array_free(&tool_list);
  clay_json_free(shell_schema);
  clay_json_free(memory_save_schema_json);
  clay_json_free(memory_read_schema_json);
  clay_json_free(remember_schema_json);
  clay_json_free(read_schema_json);
  clay_json_free(write_schema_json);
  clay_json_free(edit_schema_json);
  clay_json_free(glob_schema_json);
  clay_json_free(grep_schema_json);
  clay_json_free(todowrite_schema_json);
  clay_json_free(repo_map_schema_json);
  clay_json_free(skill_schema_json);
  clay_json_free(ask_user_schema_json);
  clay_json_free(task_run_schema_json);
  clay_json_free(task_output_schema_json);
  clay_json_free(task_stop_schema_json);
  clay_json_free(task_list_schema_json);
  clay_openai_destroy(client);
  if (codex) {
    /* Save a refresh or rotated refresh token before discarding this request
     * client. */
    clay_commands_save_codex_credentials(provider, codex);
    clay_openai_codex_destroy(codex);
  }
  if (grok) {
    clay_commands_save_grok_credentials(provider, grok);
    clay_grok_destroy(grok);
  }
  struct timespec finished_at;
  clock_gettime(CLOCK_MONOTONIC, &finished_at);
  double seconds = (double)(finished_at.tv_sec - started_at.tv_sec) +
                   (double)(finished_at.tv_nsec - started_at.tv_nsec) / 1e9;
  finish_thinking(&stream);
  collapse_thinking(&stream);
  int had_response = stream.response_active;
  if (had_response) {
    clay_wrap_flush(&stream.wrap);
    clay_response_end();
  }
  if (rc == 1) {
    if (stream.response.len > 0)
      clay_json_array_push(
          messages, clay_openai_message("assistant", stream.response.data));
    persist_thinking(commands, &stream);
    clay_chat_finish_turn(commands->chat, messages, turn_start, "aborted");
    clay_json_free(messages);
    if (had_response && stream.status_visible) {
      clay_below_status_finish_output();
      stream.status_visible = 0;
    } else {
      hide_status(&stream);
    }
    clay_sayc(CLAY_YELLOW, "Operation aborted by user.");
    clay_str_free(&stream.response);
    clay_wrap_free(&stream.wrap);
    clay_str_free(&stream.thinking);
    clay_app_set_state(commands->app, CLAY_APP_IDLE);
    return 0;
  }
  if (rc == 0) {
    persist_thinking(commands, &stream);
    clay_chat_finish_turn(commands->chat, messages, turn_start, "completed");
    if (has_notes)
      clay_json_array_remove(messages, history_end);
    clay_json_free(commands->conversation);
    commands->conversation = messages;
    set_status(seconds, 1);
    if (stream.has_usage) {
      commands->input_tokens = stream.input_tokens;
      commands->output_tokens = stream.output_tokens;
      commands->cached_input_tokens = stream.cached_input_tokens;
      commands->cached_input_tokens_known = stream.cached_input_tokens_known;
      commands->total_input_tokens += stream.input_tokens;
      commands->total_output_tokens += stream.output_tokens;
      ClayChatUsage usage = {
          stream.input_tokens,
          stream.output_tokens,
          stream.cached_input_tokens,
          stream.cached_input_tokens_known,
          commands->total_input_tokens,
          commands->total_output_tokens};
      clay_chat_set_usage(commands->chat, &usage);
      clay_commands_set_tokens_below_with_cache(
          commands, stream.input_tokens, stream.output_tokens,
          stream.cached_input_tokens, stream.cached_input_tokens_known);
    }
  } else {
    if (stream.response.len > 0)
      clay_json_array_push(
          messages, clay_openai_message("assistant", stream.response.data));
    persist_thinking(commands, &stream);
    clay_chat_finish_turn(commands->chat, messages, turn_start,
                          stream.error_status > 0 ? "provider_error"
                                                  : "network_error");
    clay_json_free(messages);
    set_status(seconds, 0);
  }
  if (stream.started && clay_term_is_interactive()) {
    if (stream.status_visible)
      clay_below_status_refresh_below();
    clay_below_status_prepare_prompt();
    clay_str_free(&stream.response);
    clay_wrap_free(&stream.wrap);
    clay_str_free(&stream.thinking);
    clay_app_set_state(commands->app, CLAY_APP_IDLE);
    return 1;
  }
  hide_status(&stream);
  if (rc != 0) {
    if (stream.error_status > 0)
      clay_sayc(CLAY_RED, "Provider request failed (HTTP %ld).",
                stream.error_status);
    else
      clay_sayc(CLAY_RED, "Provider request failed.");
  }
  clay_str_free(&stream.response);
  clay_wrap_free(&stream.wrap);
  clay_str_free(&stream.thinking);
  clay_app_set_state(commands->app, CLAY_APP_IDLE);
  return 0;
}
