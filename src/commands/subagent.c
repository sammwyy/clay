#include "context.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAY_SUBAGENT_SUMMARY_LIMIT (16 * 1024)
#define CLAY_SUBAGENT_MAX_PARALLEL 4

/* Added on top of the main system prompt, which already carries the tools,
   the sandbox rules, and the project's conventions. */
#define CLAY_SUBAGENT_PROMPT_SUFFIX                                            \
  "\n\nYou are running as a subagent. Another agent, working for the user on " \
  "a larger job, handed you one branch of it. You start with no history of "   \
  "that conversation: the task below is everything you were told, so read "    \
  "the code you need rather than assuming what the rest of the work did.\n\n"  \
  "Other subagents may be working in parallel on their own branches right "    \
  "now. Stay inside yours: touch the files your task names and leave the "     \
  "rest alone, even if you spot something worth fixing - put that in your "    \
  "summary instead.\n\n"                                                       \
  "Plan your own work: for anything with more than a couple of steps, write "  \
  "a todowrite checklist and work through it. That plan is yours alone.\n\n"   \
  "You cannot ask the user anything. When something is ambiguous, pick the "   \
  "most reasonable reading, say which assumption you made, and carry on.\n\n"  \
  "Do the work - read, edit, run the tests - and end with a plain-text "       \
  "summary for the agent that called you: what you changed and where, what "   \
  "you verified and how, anything you could not do, and whatever the rest of " \
  "the job needs to know. No preamble, no markdown headings, just the facts "  \
  "it would otherwise have to rediscover."

typedef struct ClaySubagentBatch ClaySubagentBatch;

typedef struct {
  ClaySubagentBatch *batch;
  const char *description; /* borrowed from the call's arguments */
  const char *prompt;
  ClayPlan plan;   /* its own checklist, private to this run */
  ClayStr summary; /* whatever it said after its last tool call */
  int tool_calls;
  int finished;
  int rc;
  double seconds;
  long input_tokens;
  long output_tokens;
  char *transcript;
  pthread_t thread;
} ClaySubagentRun;

struct ClaySubagentBatch {
  ClayCommands *commands;
  pthread_mutex_t lock; /* guards what the waiting thread reads mid-flight */
  size_t count;
  ClaySubagentRun runs[CLAY_SUBAGENT_MAX_PARALLEL];
};

static void on_token(const char *text, void *userdata) {
  ClaySubagentRun *run = userdata;
  if (run->summary.len < CLAY_SUBAGENT_SUMMARY_LIMIT)
    clay_str_push(&run->summary, text);
}

static void on_tool_call(const char *name, const char *arguments_json,
                         void *userdata) {
  (void)name;
  (void)arguments_json;
  ClaySubagentRun *run = userdata;
  /* Text before a tool call is the model narrating, not its report: only
     what it says after the last call is the summary. */
  clay_str_clear(&run->summary);
  pthread_mutex_lock(&run->batch->lock);
  run->tool_calls++;
  pthread_mutex_unlock(&run->batch->lock);
}

static void on_usage_details(const ClayTokenUsage *usage, void *userdata) {
  ClaySubagentRun *run = userdata;
  run->input_tokens += usage->input_tokens;
  run->output_tokens += usage->output_tokens;
}

static int should_abort(void *userdata) {
  (void)userdata;
  return clay_term_take_escape() || clay_term_take_interrupt();
}

static ClayJson *subagent_error(const char *message) {
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(0));
  clay_json_object_set(result, "error", clay_json_string(message));
  return result;
}

/* Keeps the whole run under <chat>/subagents/<id>.json: the prompt it was
   given, every message it exchanged, and what it reported back. */
static char *save_transcript(ClayCommands *commands, const ClaySubagentRun *run,
                             const ClayJson *messages) {
  char *id = clay_uuid_v4();
  if (!id)
    return NULL;
  char *path = clay_chat_subagent_path(commands->chat, id);
  free(id);
  if (!path)
    return NULL;
  ClayJson *root = clay_json_object();
  clay_json_object_set(root, "description", clay_json_string(run->description));
  clay_json_object_set(root, "prompt", clay_json_string(run->prompt));
  clay_json_object_set(root, "model",
                       clay_json_string(commands->selected_model
                                            ? commands->selected_model
                                            : ""));
  clay_json_object_set(root, "seconds", clay_json_number(run->seconds));
  clay_json_object_set(root, "tool_calls", clay_json_number(run->tool_calls));
  clay_json_object_set(root, "summary", clay_json_string(run->summary.data));
  clay_json_object_set(root, "messages", clay_json_clone(messages));
  ClayStr text;
  clay_str_init(&text);
  clay_json_stringify(root, &text);
  clay_json_free(root);
  int written = clay_term_write_file_atomic(path, text.data, text.len);
  clay_str_free(&text);
  if (written != 0) {
    free(path);
    return NULL;
  }
  return path;
}

static void *run_subagent(void *argument) {
  ClaySubagentRun *run = argument;
  ClayCommands *commands = run->batch->commands;

  ClayStr system;
  clay_str_init(&system);
  clay_str_push(&system, clay_chat_system_prompt(commands->chat));
  clay_str_push(&system, CLAY_SUBAGENT_PROMPT_SUFFIX);
  ClayJson *messages = clay_json_array();
  clay_json_array_push(messages, clay_openai_message("system", system.data));
  clay_json_array_push(messages, clay_openai_message("user", run->prompt));
  clay_str_free(&system);

  ClayOpenAICallbacks callbacks = {0};
  callbacks.on_token = on_token;
  callbacks.on_tool_call = on_tool_call;
  callbacks.on_usage_details = on_usage_details;
  callbacks.should_abort = should_abort;
  callbacks.userdata = run;

  ClayToolSet tools;
  clay_commands_tools_build(commands, &run->plan, &tools, 0);
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  /* Shares the chat's cache key: the prefix is the same system prompt, so
     the provider can reuse the same cached prefix. */
  int rc = clay_commands_run_completion(commands, messages, &tools,
                                        CLAY_SUBAGENT_MAX_ROUNDS,
                                        clay_chat_id(commands->chat),
                                        &callbacks);
  /* A model that stops right after its last tool call still did the work.
     Ask once, with no tools, instead of losing the whole run. */
  if (rc == 0 && run->summary.len == 0 && run->tool_calls > 0) {
    clay_json_array_push(
        messages,
        clay_openai_message(
            "user", "Summarize what you just did for the agent that called "
                    "you: what you changed and where, what you verified and "
                    "how, and anything left undone."));
    rc = clay_commands_run_completion(commands, messages, NULL, 1,
                                      clay_chat_id(commands->chat), &callbacks);
  }
  clay_commands_tools_free(&tools);
  struct timespec finished;
  clock_gettime(CLOCK_MONOTONIC, &finished);

  run->rc = rc;
  run->seconds = (double)(finished.tv_sec - started.tv_sec) +
                 (double)(finished.tv_nsec - started.tv_nsec) / 1e9;
  run->transcript = save_transcript(commands, run, messages);
  clay_json_free(messages);
  clay_plan_clear(&run->plan);
  clay_array_free(&run->plan.todos);

  pthread_mutex_lock(&run->batch->lock);
  run->finished = 1;
  pthread_mutex_unlock(&run->batch->lock);
  return NULL;
}

/* Keeps the tool's spinner row pointed at how the branches are doing. */
static void report_progress(ClaySubagentBatch *batch) {
  size_t done = 0;
  int tool_calls = 0;
  const char *running = NULL;
  pthread_mutex_lock(&batch->lock);
  for (size_t i = 0; i < batch->count; i++) {
    tool_calls += batch->runs[i].tool_calls;
    if (batch->runs[i].finished)
      done++;
    else if (!running)
      running = batch->runs[i].description;
  }
  pthread_mutex_unlock(&batch->lock);
  if (batch->count == 1) {
    clay_task_relabel(batch->commands->active_tool_task,
                      "Delegating: %s %s(%d tool calls)%s",
                      batch->runs[0].description, clay_color(CLAY_GRAY),
                      tool_calls, clay_color(CLAY_RESET));
    return;
  }
  clay_task_relabel(batch->commands->active_tool_task,
                    "Delegating %zu branches: %s%zu done, %d tool calls%s %s·%s %s",
                    batch->count, clay_color(CLAY_GRAY), done, tool_calls,
                    clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
                    clay_color(CLAY_RESET), running ? running : "collecting");
}

ClayJson *subagent_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  const ClayJson *tasks = clay_json_object_get(arguments, "tasks");
  size_t count = clay_json_array_count(tasks);
  if (count == 0)
    return subagent_error("tasks must be a non-empty array");
  if (count > CLAY_SUBAGENT_MAX_PARALLEL)
    return subagent_error("at most 4 branches at once; send the rest in the "
                          "next call");
  for (size_t i = 0; i < count; i++) {
    const ClayJson *task = clay_json_array_get(tasks, i);
    if (!*clay_json_string_value(clay_json_object_get(task, "description")))
      return subagent_error("every task needs a description");
    if (!*clay_json_string_value(clay_json_object_get(task, "prompt")))
      return subagent_error("every task needs a prompt");
  }
  if (commands->mode == CLAY_MODE_PLAN)
    return subagent_error(
        "blocked: clay is in Plan mode - a subagent would change files. "
        "Describe the plan instead, or ask the user to run /plan to switch "
        "to Act mode.");
  if (!commands->chat)
    return subagent_error("no active chat to record the runs in");

  ClaySubagentBatch *batch = calloc(1, sizeof(*batch));
  batch->commands = commands;
  batch->count = count;
  pthread_mutex_init(&batch->lock, NULL);
  for (size_t i = 0; i < count; i++) {
    const ClayJson *task = clay_json_array_get(tasks, i);
    ClaySubagentRun *run = &batch->runs[i];
    run->batch = batch;
    run->description =
        clay_json_string_value(clay_json_object_get(task, "description"));
    run->prompt = clay_json_string_value(clay_json_object_get(task, "prompt"));
    clay_str_init(&run->summary);
    clay_array_init(&run->plan.todos, sizeof(ClayTodoItem));
  }

  /* All of them start at once: the wall clock is dominated by the model
     thinking, so branches that touch different files overlap almost
     perfectly. Their file writes serialize on the session's tool lock. */
  size_t started = 0;
  for (size_t i = 0; i < count; i++) {
    if (pthread_create(&batch->runs[i].thread, NULL, run_subagent,
                       &batch->runs[i]) != 0)
      break;
    started++;
  }
  for (size_t i = started; i < count; i++) {
    batch->runs[i].finished = 1;
    batch->runs[i].rc = -1;
  }

  for (;;) {
    report_progress(batch);
    size_t done = 0;
    pthread_mutex_lock(&batch->lock);
    for (size_t i = 0; i < count; i++)
      if (batch->runs[i].finished)
        done++;
    pthread_mutex_unlock(&batch->lock);
    if (done == count)
      break;
    clay_term_sleep_ms(100);
  }
  for (size_t i = 0; i < started; i++)
    pthread_join(batch->runs[i].thread, NULL);

  ClayJson *result = clay_json_object();
  ClayJson *results = clay_json_array();
  ClayStr output;
  clay_str_init(&output);
  size_t succeeded = 0;
  long input_tokens = 0;
  long output_tokens = 0;
  for (size_t i = 0; i < count; i++) {
    ClaySubagentRun *run = &batch->runs[i];
    int ok = run->rc == 0 && run->summary.len > 0;
    succeeded += ok ? 1 : 0;
    input_tokens += run->input_tokens;
    output_tokens += run->output_tokens;
    ClayJson *entry = clay_json_object();
    clay_json_object_set(entry, "description",
                         clay_json_string(run->description));
    clay_json_object_set(entry, "ok", clay_json_bool(ok));
    clay_json_object_set(entry, "tool_calls",
                         clay_json_number(run->tool_calls));
    clay_json_object_set(entry, "seconds", clay_json_number(run->seconds));
    if (ok)
      clay_json_object_set(entry, "summary",
                           clay_json_string(run->summary.data));
    else
      clay_json_object_set(
          entry, "error",
          clay_json_string(run->rc == 1
                               ? "the user cancelled the run"
                               : "no summary came back; do this branch "
                                 "yourself or split it further"));
    if (run->transcript)
      clay_json_object_set(entry, "transcript",
                           clay_json_string(run->transcript));
    clay_json_array_push(results, entry);
    clay_str_printf(&output, "%s[%s] %s", output.len ? "\n\n" : "",
                    run->description,
                    ok ? run->summary.data : "(no summary came back)");
    clay_str_free(&run->summary);
    free(run->transcript);
  }
  clay_commands_add_usage(commands, input_tokens, output_tokens);
  clay_json_object_set(result, "ok", clay_json_bool(succeeded == count));
  clay_json_object_set(result, "results", results);
  clay_json_object_set(result, "output", clay_json_string(output.data));
  if (succeeded != count)
    clay_json_object_set(
        result, "error",
        clay_json_string("not every branch reported back; see results"));
  clay_str_free(&output);
  pthread_mutex_destroy(&batch->lock);
  free(batch);
  return result;
}

ClayJson *subagent_schema(void) {
  ClayJson *description = clay_json_object();
  clay_json_object_set(description, "type", clay_json_string("string"));
  clay_json_object_set(
      description, "description",
      clay_json_string("A few words naming this branch, shown to the user "
                       "while it runs."));
  ClayJson *prompt = clay_json_object();
  clay_json_object_set(prompt, "type", clay_json_string("string"));
  clay_json_object_set(
      prompt, "description",
      clay_json_string(
          "Everything the subagent needs, as if briefing someone who has not "
          "seen this conversation: what to build, which files are its own, "
          "any contract it has to honour, and what its summary should "
          "answer. It cannot ask you anything once it starts."));
  ClayJson *task_properties = clay_json_object();
  clay_json_object_set(task_properties, "description", description);
  clay_json_object_set(task_properties, "prompt", prompt);
  ClayJson *task_required = clay_json_array();
  clay_json_array_push(task_required, clay_json_string("description"));
  clay_json_array_push(task_required, clay_json_string("prompt"));
  ClayJson *task = clay_json_object();
  clay_json_object_set(task, "type", clay_json_string("object"));
  clay_json_object_set(task, "properties", task_properties);
  clay_json_object_set(task, "required", task_required);

  ClayJson *tasks = clay_json_object();
  clay_json_object_set(tasks, "type", clay_json_string("array"));
  clay_json_object_set(tasks, "items", task);
  clay_json_object_set(
      tasks, "description",
      clay_json_string("One entry per branch, up to four. They all start at "
                       "once and this returns when the last one is done, so "
                       "give each branch its own files: two of them editing "
                       "the same file will fight."));

  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "tasks", tasks);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("tasks"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}
