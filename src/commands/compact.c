#include "context.h"

#include <stdlib.h>
#include <string.h>

/* Manual, LLM-written alternative to the deterministic compaction in
   context.c: /compact asks the model itself to summarize the conversation.
   Unlike an automatic LLM-compaction fallback (which risks failing exactly
   when the context is already overloaded), this is user-triggered, so
   there's always room to make the call. Tool results are stripped to short
   previews before sending - they're usually most of the token count, and
   the summarizer mostly needs to know what happened, not the raw output. */

#define CLAY_COMPACT_TOOL_PREVIEW 300
#define CLAY_COMPACT_SYSTEM_PROMPT                                             \
  "You are compacting a coding-assistant conversation so it can continue "     \
  "with less context. Read the "                                               \
  "transcript below and write a concise summary that preserves: what the "     \
  "user asked for, key decisions "                                             \
  "made, files touched and why, the current state of the task, and anything "  \
  "still pending. Tool call "                                                  \
  "results were stripped to short previews to save space - if an important "   \
  "detail seems to be missing "                                                \
  "because of that, say so explicitly instead of guessing. Plain prose, no "   \
  "meta-commentary about being a "                                             \
  "summarizer."

static char *collapse_for_compact(const char *text) {
  if (!text)
    return strdup("");
  size_t len = strlen(text);
  if (len <= CLAY_COMPACT_TOOL_PREVIEW)
    return strdup(text);
  ClayStr out;
  clay_str_init(&out);
  clay_str_push_n(&out, text, CLAY_COMPACT_TOOL_PREVIEW);
  clay_str_printf(&out, "... (%zu bytes total, truncated)", len);
  return out.data;
}

/* Flattens `conversation` (skipping the system prompt at index 0) into a
   plain-text transcript: user/assistant text verbatim, tool calls as a
   one-line "called name(args)", tool results collapsed. */
char *clay_commands_build_compact_transcript(ClayJson *conversation) {
  ClayStr out;
  clay_str_init(&out);
  for (size_t i = 1; i < clay_json_array_count(conversation); i++) {
    ClayJson *message = clay_json_array_get(conversation, i);
    const char *role =
        clay_json_string_value(clay_json_object_get(message, "role"));
    const char *content =
        clay_json_string_value(clay_json_object_get(message, "content"));

    if (strcmp(role, "user") == 0) {
      clay_str_printf(&out, "User: %s\n\n", content ? content : "");
    } else if (strcmp(role, "assistant") == 0) {
      if (content && *content)
        clay_str_printf(&out, "Assistant: %s\n\n", content);
      ClayJson *tool_calls = clay_json_object_get(message, "tool_calls");
      for (size_t j = 0; j < clay_json_array_count(tool_calls); j++) {
        ClayJson *call = clay_json_array_get(tool_calls, j);
        ClayJson *function = clay_json_object_get(call, "function");
        const char *name =
            clay_json_string_value(clay_json_object_get(function, "name"));
        const char *arguments =
            clay_json_string_value(clay_json_object_get(function, "arguments"));
        clay_str_printf(&out, "Assistant called %s(%s)\n",
                        name && *name ? name : "?", arguments ? arguments : "");
      }
    } else if (strcmp(role, "tool") == 0) {
      char *preview = collapse_for_compact(content);
      clay_str_printf(&out, "Tool result: %s\n\n", preview);
      free(preview);
    }
  }
  return out.data;
}

static void accumulate_token(const char *text, void *userdata) {
  if (text && *text)
    clay_str_push(userdata, text);
}

void clay_cmd_compact(const char *args, void *user_data) {
  (void)args;
  ClayCommands *commands = user_data;

  if (!commands->chat || clay_json_array_count(commands->conversation) <= 1) {
    clay_sayc(CLAY_GRAY, "Nothing to compact yet.");
    return;
  }
  if (!commands->selected_provider || !commands->selected_model) {
    clay_sayc(CLAY_RED,
              "Select a provider and model with /model before compacting.");
    return;
  }
  ClayConnectedProvider *provider =
      clay_commands_find_provider(commands, commands->selected_provider);
  if (!provider) {
    clay_sayc(CLAY_RED, "Selected provider %s is not connected.",
              commands->selected_provider);
    return;
  }

  char *transcript =
      clay_commands_build_compact_transcript(commands->conversation);
  if (!*transcript) {
    free(transcript);
    clay_sayc(CLAY_GRAY, "Nothing to compact yet.");
    return;
  }

  ClayJson *messages = clay_json_array();
  clay_json_array_push(
      messages, clay_openai_message("system", CLAY_COMPACT_SYSTEM_PROMPT));
  clay_json_array_push(messages, clay_openai_message("user", transcript));
  free(transcript);

  ClayStr summary;
  clay_str_init(&summary);
  ClayOpenAICallbacks callbacks = {0};
  callbacks.on_token = accumulate_token;
  callbacks.userdata = &summary;

  size_t before_count = clay_json_array_count(commands->conversation);
  ClayTask *task = clay_app_task_start(commands->app, "Compacting context");
  if (clay_term_is_interactive())
    clay_term_raw_enable();
  int rc = clay_commands_run_completion(commands, messages, NULL, 1,
                                        clay_chat_id(commands->chat),
                                        &callbacks);
  if (clay_term_is_interactive())
    clay_term_raw_disable();
  clay_json_free(messages);

  if (rc != 0 || summary.len == 0) {
    clay_app_task_fail(commands->app, task, "failed");
    clay_str_free(&summary);
    clay_sayc(CLAY_RED, "Could not compact the conversation.");
    return;
  }
  clay_app_task_success(commands->app, task, "%zu messages -> summary",
                        before_count);

  ClayJson *new_conversation = clay_json_array();
  clay_json_array_push(new_conversation, clay_json_clone(clay_json_array_get(
                                             commands->conversation, 0)));
  ClayStr note;
  clay_str_init(&note);
  clay_str_printf(
      &note,
      "Summary of the conversation so far (compacted with /compact):\n%s",
      summary.data);
  clay_json_array_push(new_conversation,
                       clay_openai_message("system", note.data));
  clay_str_free(&note);
  clay_str_free(&summary);

  clay_json_free(commands->conversation);
  commands->conversation = new_conversation;

  clay_sayc(CLAY_GREEN,
            "Compacted. The conversation now starts from a summary.");
}
