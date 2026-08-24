#define _GNU_SOURCE

#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/term.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

static ClayJson *call(ClayJson *(*tool)(const ClayJson *, void *),
                      ClayCommands *commands, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    assert(args);
    ClayJson *result = tool(args, commands);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

static const char *text_of(ClayJson *result, const char *key) {
    return clay_json_string_value(clay_json_object_get(result, key));
}

static long long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int stop_immediately(void *user_data) {
    (void)user_data;
    return 1;
}

static void test_exec_timeout(void) {
    ClayStr output;
    clay_str_init(&output);
    ClayExecOptions options = {0};
    options.timeout_seconds = 1;
    ClayExecResult result = {0};
    long long started = now_ms();
    assert(clay_term_shell_exec("echo before; sleep 30", &output, 64 * 1024, &options,
                                &result) == 0);
    long long elapsed = now_ms() - started;
    assert(result.timed_out);
    assert(result.exit_code == 124);
    assert(elapsed < 5000); /* killed at the deadline, not after sleep 30 */
    assert(strstr(output.data, "before"));
    assert(strstr(output.data, "timed out"));
    clay_str_free(&output);
}

static void test_exec_stop_hook(void) {
    ClayStr output;
    clay_str_init(&output);
    ClayExecOptions options = {0};
    options.should_stop = stop_immediately;
    ClayExecResult result = {0};
    long long started = now_ms();
    assert(clay_term_shell_exec("sleep 30", &output, 64 * 1024, &options, &result) == 0);
    assert(result.stopped);
    assert(!result.timed_out);
    assert(now_ms() - started < 5000);
    clay_str_free(&output);
}

static void test_background_tasks(ClayCommands *commands) {
    ClayJson *result =
        call(task_run_tool, commands,
             "{\"command\":\"echo listening; sleep 30\"}");
    assert(ok(result));
    assert(clay_json_bool_value(clay_json_object_get(result, "running")));
    int id = (int)clay_json_number_value(clay_json_object_get(result, "task_id"));
    /* The start grace period is long enough to catch the first line. */
    assert(strstr(text_of(result, "output"), "listening"));
    clay_json_free(result);

    ClayStr args;
    clay_str_init(&args);
    clay_str_printf(&args, "{\"task_id\":%d}", id);

    result = call(task_output_tool, commands, args.data);
    assert(ok(result));
    assert(clay_json_bool_value(clay_json_object_get(result, "running")));
    assert(strstr(text_of(result, "output"), "listening"));
    clay_json_free(result);

    result = call(task_list_tool, commands, "{}");
    assert(ok(result));
    assert(strstr(text_of(result, "output"), "running"));
    clay_json_free(result);

    long long started = now_ms();
    result = call(task_stop_tool, commands, args.data);
    assert(ok(result));
    assert(clay_json_bool_value(clay_json_object_get(result, "stopped")));
    assert(!clay_json_bool_value(clay_json_object_get(result, "running")));
    assert(now_ms() - started < 5000);
    clay_json_free(result);

    /* Stopping an already-stopped task is a no-op, not a second join. */
    result = call(task_stop_tool, commands, args.data);
    assert(ok(result));
    assert(!clay_json_bool_value(clay_json_object_get(result, "running")));
    clay_json_free(result);
    clay_str_free(&args);

    /* A command that exits on its own is already finished when task_run
       returns. */
    result = call(task_run_tool, commands, "{\"command\":\"echo done\"}");
    assert(ok(result));
    assert(!clay_json_bool_value(clay_json_object_get(result, "running")));
    assert(clay_json_number_value(clay_json_object_get(result, "exit_code")) == 0);
    assert(strstr(text_of(result, "output"), "done"));
    clay_json_free(result);

    result = call(task_output_tool, commands, "{\"task_id\":999}");
    assert(!ok(result));
    clay_json_free(result);

    result = call(task_run_tool, commands, "{\"command\":\"\"}");
    assert(!ok(result));
    clay_json_free(result);
}

int main(void) {
    char template[] = "/tmp/clay_test_tasks_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);
    assert(chdir(home) == 0);

    test_exec_timeout();
    test_exec_stop_hook();

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    clay_array_init(&commands.tasks, sizeof(ClayBackgroundTask *));
    commands.sandbox_mode = CLAY_SANDBOX_MODE_UNLEASHED;
    commands.sandbox_auto_approve = 1;
    commands.mode = CLAY_MODE_ACT;
    commands.chat = clay_chat_create("test");
    assert(commands.chat);

    test_background_tasks(&commands);

    /* Plan mode refuses to start anything new. */
    commands.mode = CLAY_MODE_PLAN;
    ClayJson *blocked = call(task_run_tool, &commands, "{\"command\":\"echo nope\"}");
    assert(!ok(blocked));
    assert(strstr(text_of(blocked, "error"), "Plan mode"));
    clay_json_free(blocked);

    clay_commands_stop_tasks(&commands);
    clay_chat_destroy(commands.chat);

    printf("background task tests passed\n");
    return 0;
}
