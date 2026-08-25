#include "context.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAY_SUBAGENT_SUMMARY_LIMIT (16 * 1024)

/* Added on top of the main system prompt, which already carries the tools,
   the sandbox rules, and the project's conventions. */
#define CLAY_SUBAGENT_PROMPT_SUFFIX                                            \
  "\n\nYou are running as a subagent. Another agent, working for the user on " \
  "a larger job, handed you one step of its plan. You start with no history " \
  "of that conversation: the task below is everything you were told, so "      \
  "read the code you need rather than assuming what the rest of the plan "     \
  "did.\n\n"                                                                   \
  "You cannot ask the user anything. When something is ambiguous, pick the "   \
  "most reasonable reading, say which assumption you made, and carry on.\n\n"  \
  "Do the work - read, edit, run the tests - and end with a plain-text "       \
  "summary for the agent that called you: what you changed and where, what "   \
  "you verified and how, anything you could not do, and whatever the next "    \
  "step needs to know. No preamble, no markdown headings, just the facts it "  \
  "would otherwise have to rediscover."

typedef struct {
  ClayCommands *commands;
  const char *description;
  ClayStr summary;
  int tool_calls;
  long input_tokens;
  long output_tokens;
} ClaySubagentRun;

static void on_token(const char *text, void *userdata) {
  ClaySubagentRun *run = userdata;
  if (run->summary.len < CLAY_SUBAGENT_SUMMARY_LIMIT)
    clay_str_push(&run->summary, text);
}

static void on_tool_call(const char *name, const char *arguments_json,
                         void *userdata) {
  (void)arguments_json;
  ClaySubagentRun *run = userdata;
  run->tool_calls++;
  /* Text before a tool call is the model narrating, not its report: only
     what it says after the last call is the summary. */
  clay_str_clear(&run->summary);
  /* The tool's own spinner row is the only window the user has into a
     subagent, so keep it pointed at what is running right now. */
  clay_task_relabel(run->commands->active_tool_task, "Delegating: %s %s·%s %s",
                    run->description, clay_color(CLAY_GRAY),
                    clay_color(CLAY_RESET), name);
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
static char *save_transcript(ClayCommands *commands, const char *description,
                             const char *prompt, const ClayJson *messages,
                             const char *summary, double seconds,
                             int tool_calls) {
  char *id = clay_uuid_v4();
  if (!id)
    return NULL;
  char *path = clay_chat_subagent_path(commands->chat, id);
  free(id);
  if (!path)
    return NULL;
  ClayJson *root = clay_json_object();
  clay_json_object_set(root, "description", clay_json_string(description));
  clay_json_object_set(root, "prompt", clay_json_string(prompt));
  clay_json_object_set(root, "model",
                       clay_json_string(commands->selected_model
                                            ? commands->selected_model
                                            : ""));
  clay_json_object_set(root, "seconds", clay_json_number(seconds));
  clay_json_object_set(root, "tool_calls", clay_json_number(tool_calls));
  clay_json_object_set(root, "summary", clay_json_string(summary));
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

ClayJson *subagent_tool(const ClayJson *arguments, void *userdata) {
  ClayCommands *commands = userdata;
  const char *description =
      clay_json_string_value(clay_json_object_get(arguments, "description"));
  const char *prompt =
      clay_json_string_value(clay_json_object_get(arguments, "prompt"));
  if (!*description)
    return subagent_error("description is required");
  if (!*prompt)
    return subagent_error("prompt is required");
  if (commands->mode == CLAY_MODE_PLAN)
    return subagent_error(
        "blocked: clay is in Plan mode - a subagent would change files. "
        "Describe the plan instead, or ask the user to run /plan to switch "
        "to Act mode.");
  if (!commands->chat)
    return subagent_error("no active chat to record the run in");

  ClayStr system;
  clay_str_init(&system);
  clay_str_push(&system, clay_chat_system_prompt(commands->chat));
  clay_str_push(&system, CLAY_SUBAGENT_PROMPT_SUFFIX);
  ClayJson *messages = clay_json_array();
  clay_json_array_push(messages, clay_openai_message("system", system.data));
  clay_json_array_push(messages, clay_openai_message("user", prompt));
  clay_str_free(&system);

  ClaySubagentRun run = {0};
  run.commands = commands;
  run.description = description;
  clay_str_init(&run.summary);

  ClayOpenAICallbacks callbacks = {0};
  callbacks.on_token = on_token;
  callbacks.on_tool_call = on_tool_call;
  callbacks.on_usage_details = on_usage_details;
  callbacks.should_abort = should_abort;
  callbacks.userdata = &run;

  ClayToolSet tools;
  clay_commands_tools_build(commands, &tools, 0);
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  /* A subagent shares the chat's cache key: its prefix is the same system
     prompt, so the provider can reuse the same cached prefix. */
  int rc = clay_commands_run_completion(commands, messages, &tools,
                                        CLAY_SUBAGENT_MAX_ROUNDS,
                                        clay_chat_id(commands->chat),
                                        &callbacks);
  /* A model that stops right after its last tool call still did the work.
     Ask once, with no tools, instead of losing the whole run. */
  if (rc == 0 && run.summary.len == 0 && run.tool_calls > 0) {
    clay_task_relabel(commands->active_tool_task,
                      "Delegating: %s %s·%s summary", description,
                      clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
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
  double seconds = (double)(finished.tv_sec - started.tv_sec) +
                   (double)(finished.tv_nsec - started.tv_nsec) / 1e9;

  clay_commands_add_usage(commands, run.input_tokens, run.output_tokens);
  char *path = save_transcript(commands, description, prompt, messages,
                               run.summary.data, seconds, run.tool_calls);
  clay_json_free(messages);

  ClayJson *result = clay_json_object();
  if (rc != 0 || run.summary.len == 0) {
    clay_json_object_set(result, "ok", clay_json_bool(0));
    clay_json_object_set(
        result, "error",
        clay_json_string(rc == 1 ? "the user cancelled the run"
                                 : "the subagent produced no summary; do this "
                                   "step yourself or split it further"));
  } else {
    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "summary",
                         clay_json_string(run.summary.data));
    clay_json_object_set(result, "output", clay_json_string(run.summary.data));
  }
  clay_json_object_set(result, "description", clay_json_string(description));
  clay_json_object_set(result, "tool_calls", clay_json_number(run.tool_calls));
  clay_json_object_set(result, "seconds", clay_json_number(seconds));
  if (path) {
    clay_json_object_set(result, "transcript", clay_json_string(path));
    free(path);
  }
  clay_str_free(&run.summary);
  return result;
}

ClayJson *subagent_schema(void) {
  ClayJson *description = clay_json_object();
  clay_json_object_set(description, "type", clay_json_string("string"));
  clay_json_object_set(
      description, "description",
      clay_json_string("A few words naming this step, shown to the user while "
                       "it runs."));
  ClayJson *prompt = clay_json_object();
  clay_json_object_set(prompt, "type", clay_json_string("string"));
  clay_json_object_set(
      prompt, "description",
      clay_json_string(
          "Everything the subagent needs, as if briefing someone who has not "
          "seen this conversation: what to do, which files or commands "
          "matter, what the previous step produced, and what its summary "
          "should answer. It cannot ask you anything once it starts."));
  ClayJson *properties = clay_json_object();
  clay_json_object_set(properties, "description", description);
  clay_json_object_set(properties, "prompt", prompt);
  ClayJson *required = clay_json_array();
  clay_json_array_push(required, clay_json_string("description"));
  clay_json_array_push(required, clay_json_string("prompt"));
  ClayJson *schema = clay_json_object();
  clay_json_object_set(schema, "type", clay_json_string("object"));
  clay_json_object_set(schema, "properties", properties);
  clay_json_object_set(schema, "required", required);
  clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
  return schema;
}
