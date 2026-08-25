#include "context.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_TASK_BUFFER_LIMIT (256 * 1024)
#define CLAY_TASK_PREVIEW_LIMIT (8 * 1024)
#define CLAY_TASK_DEFAULT_TAIL_LINES 50
#define CLAY_TASK_START_GRACE_MS 400

struct ClayBackgroundTask {
  int id;
  char *command;
  pthread_t thread;
  pthread_mutex_t lock;
  ClayStr output;  /* tail of the command's output, guarded by lock */
  size_t produced; /* total bytes it wrote, guarded by lock */
  int dropped;     /* output's head was discarded, guarded by lock */
  int running;     /* guarded by lock */
  int stop_requested;
  int exit_code;
  int timed_out;
  int stopped; /* ended because task_stop asked, guarded by lock */
  int joined;  /* UI thread only: pthread_join must happen exactly once */
  ClaySandboxMode mode;
  ClaySandboxNamespaces *namespaces; /* the session's, which outlives the task */
  char *workspace_dir;
  char *scratch_dir;
  char **readonly_mounts;
  size_t readonly_mount_count;
};

static ClayBackgroundTask *task_at(ClayCommands *commands, size_t index) {
  return *(ClayBackgroundTask **)clay_array_get(&commands->tasks, index);
}

static ClayBackgroundTask *find_task(ClayCommands *commands, int id) {
  for (size_t i = 0; i < commands->tasks.count; i++)
    if (task_at(commands, i)->id == id)
      return task_at(commands, i);
  return NULL;
}

static int task_is_running(ClayBackgroundTask *task) {
  pthread_mutex_lock(&task->lock);
  int running = task->running;
  pthread_mutex_unlock(&task->lock);
  return running;
}

static void update_tasks_below(ClayCommands *commands) {
  int running = 0;
  for (size_t i = 0; i < commands->tasks.count; i++)
    if (task_is_running(task_at(commands, i)))
      running++;
  if (running == 0) {
    clay_below_set_enabled("tasks", 0);
    return;
  }
  ClayStr text;
  clay_str_init(&text);
  clay_str_printf(&text, "%s%d bg%s", clay_color(CLAY_CYAN), running,
                  clay_color(CLAY_RESET));
  clay_below_set_text("tasks", text.data);
  clay_below_set_enabled("tasks", 1);
  clay_str_free(&text);
}

static int task_should_stop(void *user_data) {
  ClayBackgroundTask *task = user_data;
  pthread_mutex_lock(&task->lock);
  int stop = task->stop_requested;
  pthread_mutex_unlock(&task->lock);
  return stop;
}

static void task_on_output(const char *data, size_t len, void *user_data) {
  ClayBackgroundTask *task = user_data;
  pthread_mutex_lock(&task->lock);
  task->produced += len;
  clay_str_push_n(&task->output, data, len);
  if (task->output.len > CLAY_TASK_BUFFER_LIMIT) {
    clay_str_remove_n(&task->output, 0,
                      task->output.len - CLAY_TASK_BUFFER_LIMIT);
    task->dropped = 1;
  }
  pthread_mutex_unlock(&task->lock);
}

static void *task_thread(void *arg) {
  ClayBackgroundTask *task = arg;
  ClaySandboxConfig sandbox = {
      .mode = task->mode,
      .shared = task->namespaces,
      .workspace_dir = task->workspace_dir,
      .scratch_dir = task->scratch_dir,
      .use_integrated_shell = 0,
      .readonly_mounts = (const char *const *)task->readonly_mounts,
      .readonly_mount_count = task->readonly_mount_count,
  };
  ClayExecOptions options = {0};
  options.timeout_seconds = CLAY_SHELL_MAX_TIMEOUT_SECONDS;
  options.should_stop = task_should_stop;
  options.on_output = task_on_output;
  options.user_data = task;
  /* Output limit 0: nothing accumulates here, task_on_output keeps the
     tail buffer under the task's own lock instead. */
  ClayStr sink;
  clay_str_init(&sink);
  ClayExecResult exec = {0};
  int rc = clay_sandbox_exec(&sandbox, task->command, &sink, 0, &options, &exec);
  clay_str_free(&sink);
  pthread_mutex_lock(&task->lock);
  task->running = 0;
  task->exit_code = rc == 0 ? exec.exit_code : -1;
  task->timed_out = exec.timed_out;
  task->stopped = exec.stopped;
  pthread_mutex_unlock(&task->lock);
  return NULL;
}

static void join_task(ClayBackgroundTask *task) {
  if (task->joined)
    return;
  task->joined = 1;
  pthread_join(task->thread, NULL);
}

static void task_free(ClayBackgroundTask *task) {
  free(task->command);
  free(task->workspace_dir);
  free(task->scratch_dir);
  for (size_t i = 0; i < task->readonly_mount_count; i++)
    free(task->readonly_mounts[i]);
  free(task->readonly_mounts);
  clay_str_free(&task->output);
  pthread_mutex_destroy(&task->lock);
  free(task);
}

/* Last `tail_lines` lines of a task's buffer (all of it when tail_lines is
   0), capped at CLAY_TASK_PREVIEW_LIMIT bytes. Caller frees. */
static char *task_tail(ClayBackgroundTask *task, int tail_lines) {
  pthread_mutex_lock(&task->lock);
  const char *start = task->output.data;
  size_t len = task->output.len;
  if (len > CLAY_TASK_PREVIEW_LIMIT) {
    start += len - CLAY_TASK_PREVIEW_LIMIT;
    len = CLAY_TASK_PREVIEW_LIMIT;
  }
  if (tail_lines > 0) {
    int newlines = 0;
    const char *p = start + len;
    while (p > start && newlines <= tail_lines) {
      if (*(p - 1) == '\n')
        newlines++;
      if (newlines > tail_lines)
        break;
      p--;
    }
    len -= (size_t)(p - start);
    start = p;
  }
  ClayStr text;
  clay_str_init(&text);
  clay_str_push_n(&text, start, len);
  pthread_mutex_unlock(&task->lock);
  return text.data;
}

/* status/exit_code/output on `result` for one task. */
static void describe_task(ClayBackgroundTask *task, ClayJson *result,
                          int tail_lines) {
  pthread_mutex_lock(&task->lock);
  int running = task->running;
  int exit_code = task->exit_code;
  int timed_out = task->timed_out;
  int stopped = task->stopped;
  int dropped = task->dropped;
  size_t produced = task->produced;
  pthread_mutex_unlock(&task->lock);

  clay_json_object_set(result, "task_id", clay_json_number(task->id));
  clay_json_object_set(result, "command", clay_json_string(task->command));
  clay_json_object_set(result, "running", clay_json_bool(running));
  clay_json_object_set(result, "output_bytes", clay_json_number((double)produced));
  if (!running) {
    /* A killed process has no exit code worth reporting. */
    if (stopped)
      clay_json_object_set(result, "stopped", clay_json_bool(1));
    else
      clay_json_object_set(result, "exit_code", clay_json_number(exit_code));
    if (timed_out)
      clay_json_object_set(result, "timed_out", clay_json_bool(1));
  }
  char *tail = task_tail(task, tail_lines);
  if (dropped) {
    ClayStr text;
    clay_str_init(&text);
    clay_str_printf(&text, "... (earlier output dropped, %zu bytes total)\n%s",
                    produced, tail);
    clay_json_object_set(result, "output", clay_json_string(text.data));
    clay_str_free(&text);
  } else {
    clay_json_object_set(result, "output", clay_json_string(tail));
  }
  free(tail);
}

static ClayJson *task_error(const char *message) {
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(0));
  clay_json_object_set(result, "error", clay_json_string(message));
  return result;
}

static ClayBackgroundTask *task_by_argument(ClayCommands *commands,
                                            const ClayJson *arguments,
                                            ClayJson **error_out) {
  const ClayJson *id_value = clay_json_object_get(arguments, "task_id");
  if (!id_value) {
    *error_out = task_error("task_id is required");
    return NULL;
  }
  ClayBackgroundTask *task =
      find_task(commands, (int)clay_json_number_value(id_value));
  if (!task) {
    *error_out = task_error("no background task with that id");
    return NULL;
  }
  return task;
}

ClayJson *task_run_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  const char *command =
      clay_json_string_value(clay_json_object_get(arguments, "command"));
  if (!*command)
    return task_error("command is required");
  if (commands->mode == CLAY_MODE_PLAN)
    return task_error("blocked: clay is in Plan mode - background commands "
                      "are disabled. Describe what you would run instead, or "
                      "ask the user to run /plan to switch to Act mode.");
  if (!clay_permissions_check(commands, CLAY_PERMISSION_EXEC_ALL,
                              "Run in background", command))
    return task_error("denied by the user");

  clay_commands_checkpoint(commands, command);
  ClayBackgroundTask *task = calloc(1, sizeof(*task));
  task->id = ++commands->next_task_id;
  task->command = strdup(command);
  task->running = 1;
  task->exit_code = -1;
  pthread_mutex_init(&task->lock, NULL);
  clay_str_init(&task->output);
  task->mode = commands->sandbox_mode;
  task->namespaces = commands->sandbox_namespaces;
  task->workspace_dir = clay_term_cwd();
  task->scratch_dir = clay_chat_scratch_dir(commands->chat);
  task->readonly_mounts =
      clay_config_sandbox_readonly_mounts(&task->readonly_mount_count);
  if (pthread_create(&task->thread, NULL, task_thread, task) != 0) {
    task_free(task);
    return task_error("failed to start the background task");
  }
  clay_array_push_val(&commands->tasks, &task);

  /* Long enough for a command that dies on startup (port in use, bad flag)
     to report it in this same tool result. */
  clay_term_sleep_ms(CLAY_TASK_START_GRACE_MS);
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(1));
  describe_task(task, result, 0);
  /* The session's sandboxed commands share one network namespace, which
     nothing outside it can route to. */
  if (task->mode == CLAY_SANDBOX_MODE_SANDBOX)
    clay_json_object_set(
        result, "note",
        clay_json_string("Sandboxed: a port this task opens is reachable from "
                         "your other sandboxed commands (curl it with "
                         "shell_exec), but not from the user's browser or "
                         "anything else on their machine. If they need to open "
                         "it themselves, ask them for Unleashed mode "
                         "(Shift+Tab)."));
  update_tasks_below(commands);
  return result;
}

ClayJson *task_output_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  ClayJson *error = NULL;
  ClayBackgroundTask *task = task_by_argument(commands, arguments, &error);
  if (!task)
    return error;
  const ClayJson *lines_value = clay_json_object_get(arguments, "tail_lines");
  int tail_lines = lines_value ? (int)clay_json_number_value(lines_value)
                               : CLAY_TASK_DEFAULT_TAIL_LINES;
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(1));
  describe_task(task, result, tail_lines);
  update_tasks_below(commands);
  return result;
}

ClayJson *task_stop_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  ClayJson *error = NULL;
  ClayBackgroundTask *task = task_by_argument(commands, arguments, &error);
  if (!task)
    return error;
  pthread_mutex_lock(&task->lock);
  task->stop_requested = 1;
  pthread_mutex_unlock(&task->lock);
  join_task(task);
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(1));
  describe_task(task, result, CLAY_TASK_DEFAULT_TAIL_LINES);
  update_tasks_below(commands);
  return result;
}

ClayJson *task_list_tool(const ClayJson *arguments, void *userdata) {
  (void)arguments;
  ClayCommands *commands = userdata;
  ClayJson *tasks = clay_json_array();
  ClayStr summary;
  clay_str_init(&summary);
  for (size_t i = 0; i < commands->tasks.count; i++) {
    ClayBackgroundTask *task = task_at(commands, i);
    pthread_mutex_lock(&task->lock);
    int running = task->running;
    int exit_code = task->exit_code;
    int stopped = task->stopped;
    pthread_mutex_unlock(&task->lock);
    ClayJson *entry = clay_json_object();
    clay_json_object_set(entry, "task_id", clay_json_number(task->id));
    clay_json_object_set(entry, "command", clay_json_string(task->command));
    clay_json_object_set(entry, "running", clay_json_bool(running));
    if (!running && stopped)
      clay_json_object_set(entry, "stopped", clay_json_bool(1));
    else if (!running)
      clay_json_object_set(entry, "exit_code", clay_json_number(exit_code));
    clay_json_array_push(tasks, entry);
    if (running)
      clay_str_printf(&summary, "%s[%d] running: %s", summary.len ? "\n" : "",
                      task->id, task->command);
    else if (stopped)
      clay_str_printf(&summary, "%s[%d] stopped: %s", summary.len ? "\n" : "",
                      task->id, task->command);
    else
      clay_str_printf(&summary, "%s[%d] exit %d: %s", summary.len ? "\n" : "",
                      task->id, exit_code, task->command);
  }
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(1));
  clay_json_object_set(result, "tasks", tasks);
  clay_json_object_set(result, "output",
                       clay_json_string(summary.len ? summary.data
                                                    : "no background tasks"));
  clay_str_free(&summary);
  update_tasks_below(commands);
  return result;
}

ClayJson *task_run_schema(void) {
  ClayJson *command = clay_json_object();
  clay_json_object_set(command, "type", clay_json_string("string"));
  clay_json_object_set(
      command, "description",
      clay_json_string("Shell command to start in the background, with its "
                       "arguments."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "command", command);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("command"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

ClayJson *task_output_schema(void) {
  ClayJson *id = clay_json_object();
  clay_json_object_set(id, "type", clay_json_string("number"));
  clay_json_object_set(id, "description",
                       clay_json_string("Id returned by task_run."));
  ClayJson *lines = clay_json_object();
  clay_json_object_set(lines, "type", clay_json_string("number"));
  clay_json_object_set(
      lines, "description",
      clay_json_string("How many trailing lines to return (default 50, 0 for "
                       "everything buffered)."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "task_id", id);
  clay_json_object_set(properties, "tail_lines", lines);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("task_id"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

ClayJson *task_stop_schema(void) {
  ClayJson *id = clay_json_object();
  clay_json_object_set(id, "type", clay_json_string("number"));
  clay_json_object_set(id, "description",
                       clay_json_string("Id returned by task_run."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "task_id", id);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("task_id"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

ClayJson *task_list_schema(void) {
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", clay_json_object());
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}

void clay_commands_stop_tasks(ClayCommands *commands) {
  for (size_t i = 0; i < commands->tasks.count; i++) {
    ClayBackgroundTask *task = task_at(commands, i);
    pthread_mutex_lock(&task->lock);
    task->stop_requested = 1;
    pthread_mutex_unlock(&task->lock);
  }
  for (size_t i = 0; i < commands->tasks.count; i++) {
    ClayBackgroundTask *task = task_at(commands, i);
    join_task(task);
    task_free(task);
  }
  clay_array_free(&commands->tasks);
  clay_below_set_enabled("tasks", 0);
}
