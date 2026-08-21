#include "context.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLAY_SHELL_OUTPUT_LIMIT (64 * 1024)
#define CLAY_TOOL_VISIBLE_LINES 8

typedef struct {
    int status_visible;
    int started;
    int response_active;
    int output_col;
    long error_status;
    long input_tokens;
    long output_tokens;
    int has_usage;
    ClayTask *tool_task;
} ClayConversationStream;

static ClayJson *shell_exec_tool(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    const char *command = clay_json_string_value(clay_json_object_get(arguments, "command"));
    const char *args = clay_json_string_value(clay_json_object_get(arguments, "args"));
    ClayJson *result = clay_json_object();
    if (!*command) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("command is required"));
        return result;
    }
    ClayStr invocation;
    clay_str_init(&invocation);
    clay_str_push(&invocation, command);
    if (*args) clay_str_printf(&invocation, " %s", args);
    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;
    int output_truncated = 0;
    int rc = clay_term_shell_exec(invocation.data, &output, CLAY_SHELL_OUTPUT_LIMIT, &exit_code, &output_truncated);
    clay_json_object_set(result, "command", clay_json_string(invocation.data));
    clay_json_object_set(result, "ok", clay_json_bool(rc == 0 && exit_code == 0));
    clay_json_object_set(result, "exit_code", clay_json_number(exit_code));
    clay_json_object_set(result, "output", clay_json_string(output.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(output_truncated));
    if (rc != 0) clay_json_object_set(result, "error", clay_json_string("failed to start command"));
    clay_str_free(&output);
    clay_str_free(&invocation);
    return result;
}

static ClayJson *shell_exec_schema(void) {
    ClayJson *command = clay_json_object();
    clay_json_object_set(command, "type", clay_json_string("string"));
    clay_json_object_set(command, "description", clay_json_string("Program or shell command to run."));
    ClayJson *args = clay_json_object();
    clay_json_object_set(args, "type", clay_json_string("string"));
    clay_json_object_set(args, "description", clay_json_string("Optional command arguments, including shell quoting."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "command", command);
    clay_json_object_set(properties, "args", args);
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
    if (!stream->status_visible) return;
    clay_below_set_editing(0);
    clay_below_finish();
    stream->status_visible = 0;
}

static void set_status(double seconds, int success) {
    ClayStr text;
    clay_str_init(&text);
    clay_str_printf(&text, "%s%.1fs%s", clay_color(success ? CLAY_GREEN : CLAY_RED), seconds, clay_color(CLAY_RESET));
    clay_below_set_text("status", text.data);
    clay_below_stop_elapsed("status");
    clay_below_set_state("status", success ? CLAY_BELOW_FINISHED : CLAY_BELOW_NONE);
    clay_below_set_enabled("status", 1);
    clay_str_free(&text);
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

static void close_response_for_tool(ClayConversationStream *stream) {
    clay_below_set_editing(0);
    if (stream->response_active) {
        clay_response_end();
        if (stream->status_visible) clay_below_status_finish_output();
        else fputc('\n', stdout);
        stream->response_active = 0;
        stream->status_visible = 0;
    } else if (stream->status_visible) {
        clay_below_finish();
        stream->status_visible = 0;
    }
}

static void append_label_text(ClayStr *out, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == 0x1b) clay_str_push(out, "\\x1b");
        else if (*p < 0x20) clay_str_push_char(out, '?');
        else clay_str_push_char(out, (char)*p);
    }
}

static void tool_label(ClayStr *out, const char *name, int completed, int success) {
    if (strcmp(name, "shell_exec") == 0) {
        clay_str_push(out, completed ? (success ? "Executed" : "Failed") : "Executing shell command");
        return;
    }
    clay_str_push(out, completed ? (success ? "Executed: " : "Failed: ") : "Executing: ");
    append_label_text(out, name);
}

static int command_fits_inline(const char *command) {
    ClayStr label;
    clay_str_init(&label);
    clay_str_push(&label, "Executed $");
    append_label_text(&label, command);
    size_t width = clay_utf8_width("\xe2\x97\x86 clay  \xe2\x9c\x93 ") + clay_utf8_width(label.data) + 8;
    clay_str_free(&label);
    return width < (size_t)clay_term_width();
}

static void print_tool_output(const ClayJson *result, int show_command) {
    const char *command = clay_json_string_value(clay_json_object_get(result, "command"));
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    int truncated = clay_json_bool_value(clay_json_object_get(result, "output_truncated"));
    if (show_command && *command) {
        ClayStr display;
        clay_str_init(&display);
        append_label_text(&display, command);
        clay_list_bullet("$ %s", display.data);
        clay_str_free(&display);
    }
    if (!*output) {
        clay_list_bullet("(no output)");
        return;
    }
    ClayStr line;
    clay_str_init(&line);
    int shown = 0;
    int omitted = 0;
    for (const unsigned char *p = (const unsigned char *)output;; p++) {
        if (*p == '\n' || *p == '\0') {
            if (line.len > 0 && shown < CLAY_TOOL_VISIBLE_LINES) {
                clay_list_bullet("%s", line.data);
                shown++;
            } else if (line.len > 0) omitted = 1;
            clay_str_clear(&line);
            if (*p == '\0') break;
        } else if (*p != '\r') {
            if (*p == 0x1b) clay_str_push(&line, "\\x1b");
            else if (*p < 0x20) clay_str_push_char(&line, '?');
            else clay_str_push_char(&line, (char)*p);
        }
    }
    if (omitted || truncated) clay_list_bullet("%s…%s", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    clay_str_free(&line);
}

static void on_tool_call(const char *name, const char *arguments_json, void *userdata) {
    (void)arguments_json;
    ClayConversationStream *stream = userdata;
    close_response_for_tool(stream);
    if (clay_term_is_interactive()) clay_term_raw_disable();
    ClayStr label;
    clay_str_init(&label);
    tool_label(&label, name, 0, 0);
    stream->tool_task = clay_task_start("%s", label.data);
    clay_str_free(&label);
}

static void on_tool_result(const char *name, const ClayJson *result, void *userdata) {
    ClayConversationStream *stream = userdata;
    int ok = clay_json_bool_value(clay_json_object_get(result, "ok"));
    long exit_code = (long)clay_json_number_value(clay_json_object_get(result, "exit_code"));
    const char *command = clay_json_string_value(clay_json_object_get(result, "command"));
    int inline_command = ok && strcmp(name, "shell_exec") == 0 && *command && command_fits_inline(command);
    if (stream->tool_task) {
        ClayStr label;
        clay_str_init(&label);
        if (inline_command) {
            clay_str_push(&label, "Executed $");
            append_label_text(&label, command);
        } else tool_label(&label, name, 1, ok);
        if (ok) clay_task_success_with_label(stream->tool_task, label.data, "");
        else clay_task_fail_with_label(stream->tool_task, label.data, "exit %ld", exit_code);
        clay_str_free(&label);
        stream->tool_task = NULL;
    } else clay_sayc(ok ? CLAY_GREEN : CLAY_RED, "%s: %s", ok ? "Executed" : "Failed", name);
    print_tool_output(result, !inline_command);
    if (clay_term_is_interactive()) clay_term_raw_enable();
    show_thinking(stream);
}

static void on_token(const char *text, void *userdata) {
    if (!text || !*text) return;
    ClayConversationStream *stream = userdata;
    if (!stream->response_active) {
        if (stream->status_visible) {
            clay_below_set_editing(0);
            clay_below_stop_elapsed("status");
            clay_below_set_enabled("status", 0);
            clay_below_render_status();
            clay_below_status_insert_above();
        }
        clay_response_begin();
        stream->output_col = clay_response_prefix_width();
        stream->response_active = 1;
        stream->started = 1;
    }
    ClayStr pending;
    clay_str_init(&pending);
    int width = clay_term_width();
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        if (*p == '\n') {
            clay_response_write(pending.data);
            clay_str_clear(&pending);
            if (stream->status_visible) clay_below_status_push_down();
            clay_response_write("\n");
            stream->output_col = 0;
            p++;
            continue;
        }
        size_t char_len = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 1;
        if (stream->status_visible && stream->output_col + 1 >= width) {
            clay_response_write(pending.data);
            clay_str_clear(&pending);
            clay_below_status_push_down();
            stream->output_col = 0;
        }
        clay_str_push_n(&pending, (const char *)p, char_len);
        stream->output_col++;
        p += char_len;
    }
    clay_response_write(pending.data);
    clay_str_free(&pending);
}

static void on_error(long status, const char *body, void *userdata) {
    (void)body;
    ((ClayConversationStream *)userdata)->error_status = status;
}

static void on_usage(long input_tokens, long output_tokens, void *userdata) {
    ClayConversationStream *stream = userdata;
    stream->input_tokens += input_tokens;
    stream->output_tokens += output_tokens;
    stream->has_usage = 1;
}

static int should_abort(void *userdata) {
    (void)userdata;
    return clay_term_take_escape();
}

int clay_commands_run_message(ClayCommands *commands, const char *input) {
    if (!commands->selected_provider || !commands->selected_model) {
        clay_sayc(CLAY_RED, "Select a provider and model with /model before sending a message.");
        return 0;
    }
    ClayConnectedProvider *provider = clay_commands_find_provider(commands, commands->selected_provider);
    if (!provider) {
        clay_sayc(CLAY_RED, "Selected provider %s is not connected.", commands->selected_provider);
        return 0;
    }
    ClayJson *messages = clay_json_clone(commands->conversation);
    clay_json_array_push(messages, clay_openai_message("user", input));
    ClayOpenAI *client = clay_openai_create(provider->config->base_url, provider->config->apikey, commands->selected_model);
    clay_openai_set_reasoning_effort(client, clay_commands_reasoning_effort(commands)->id);
    ClayConversationStream stream = {0};
    ClayOpenAICallbacks callbacks = {0};
    callbacks.on_token = on_token;
    callbacks.on_tool_call = on_tool_call;
    callbacks.on_tool_result = on_tool_result;
    callbacks.on_usage = on_usage;
    callbacks.on_error = on_error;
    callbacks.should_abort = should_abort;
    callbacks.userdata = &stream;
    commands->messages_sent++;
    struct timespec started_at;
    clock_gettime(CLOCK_MONOTONIC, &started_at);
    show_thinking(&stream);
    clay_app_set_state(commands->app, CLAY_APP_BUSY);
    ClayJson *schema = shell_exec_schema();
    ClayTool tools[] = {{"shell_exec", "Runs a shell command in the current workspace and returns stdout, stderr, and exit status.", schema, shell_exec_tool, NULL}};
    if (clay_term_is_interactive()) clay_term_raw_enable();
    int rc = clay_openai_run(client, messages, tools, 1, 8, &callbacks);
    if (clay_term_is_interactive()) clay_term_raw_disable();
    clay_json_free(schema);
    clay_openai_destroy(client);
    struct timespec finished_at;
    clock_gettime(CLOCK_MONOTONIC, &finished_at);
    double seconds = (double)(finished_at.tv_sec - started_at.tv_sec) + (double)(finished_at.tv_nsec - started_at.tv_nsec) / 1e9;
    int had_response = stream.response_active;
    if (had_response) clay_response_end();
    if (rc == 1) {
        clay_json_free(messages);
        if (had_response && stream.status_visible) {
            clay_below_status_finish_output();
            stream.status_visible = 0;
        } else {
            hide_status(&stream);
        }
        clay_sayc(CLAY_YELLOW, "Operation aborted by user.");
        clay_app_set_state(commands->app, CLAY_APP_IDLE);
        return 0;
    }
    if (rc == 0) {
        clay_json_free(commands->conversation);
        commands->conversation = messages;
        set_status(seconds, 1);
        if (stream.has_usage) {
            commands->input_tokens = stream.input_tokens;
            commands->output_tokens = stream.output_tokens;
            commands->total_input_tokens += stream.input_tokens;
            commands->total_output_tokens += stream.output_tokens;
            clay_commands_set_tokens_below(commands, stream.input_tokens, stream.output_tokens);
        }
    } else {
        clay_json_free(messages);
        set_status(seconds, 0);
    }
    if (stream.started && stream.status_visible) {
        clay_below_status_refresh_below();
        clay_below_status_prepare_prompt();
        clay_app_set_state(commands->app, CLAY_APP_IDLE);
        return 1;
    }
    hide_status(&stream);
    if (rc != 0) {
        if (stream.error_status > 0) clay_sayc(CLAY_RED, "Provider request failed (HTTP %ld).", stream.error_status);
        else clay_sayc(CLAY_RED, "Provider request failed.");
    }
    clay_app_set_state(commands->app, CLAY_APP_IDLE);
    return 0;
}
