#include "clay/app.h"

#include "clay/list.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

struct ClayApp {
    ClayAppState state;
    ClayCommandRegistry *commands;
    ClayAppStateListener listener;
    void *listener_ctx;
    void *user_data;
    int active_tasks;
};

ClayApp *clay_app_create(void) {
    ClayApp *app = malloc(sizeof(ClayApp));
    app->state = CLAY_APP_IDLE;
    app->commands = clay_command_registry_create();
    app->listener = NULL;
    app->listener_ctx = NULL;
    app->user_data = NULL;
    app->active_tasks = 0;
    return app;
}

void clay_app_destroy(ClayApp *app) {
    if (!app) return;
    clay_command_registry_destroy(app->commands);
    free(app);
}

ClayCommandRegistry *clay_app_commands(ClayApp *app) {
    return app->commands;
}

ClayAppState clay_app_state(const ClayApp *app) {
    return app->state;
}

void clay_app_set_state(ClayApp *app, ClayAppState state) {
    ClayAppState old = app->state;
    app->state = state;
    if (app->listener && old != state) {
        app->listener(app, old, state, app->listener_ctx);
    }
}

void clay_app_on_state_change(ClayApp *app, ClayAppStateListener listener, void *ctx) {
    app->listener = listener;
    app->listener_ctx = ctx;
}

void *clay_app_get_data(const ClayApp *app) {
    return app->user_data;
}

void clay_app_set_data(ClayApp *app, void *data) {
    app->user_data = data;
}

void clay_app_say(ClayApp *app, const char *fmt, ...) {
    (void)app;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    clay_say("%s", buf);
}

void clay_app_list_header(ClayApp *app, const char *fmt, ...) {
    (void)app;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    clay_list_header("%s", buf);
}

void clay_app_list_step(ClayApp *app, int index, const char *verb, const char *target,
                         const char *info, int link) {
    (void)app;
    clay_list_step(index, verb, target, info, link);
}

ClayTask *clay_app_task_start(ClayApp *app, const char *fmt, ...) {
    app->active_tasks++;
    clay_app_set_state(app, CLAY_APP_BUSY);

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return clay_task_start("%s", buf);
}

static void app_task_finished(ClayApp *app) {
    if (app->active_tasks > 0) app->active_tasks--;
    if (app->active_tasks == 0) clay_app_set_state(app, CLAY_APP_IDLE);
}

void clay_app_task_success(ClayApp *app, ClayTask *task, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    clay_task_success(task, "%s", buf);
    app_task_finished(app);
}

void clay_app_task_fail(ClayApp *app, ClayTask *task, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    clay_task_fail(task, "%s", buf);
    app_task_finished(app);
}

int clay_app_select(ClayApp *app, const char *question, const ClayChoice *options, int count,
                     int default_index) {
    clay_app_set_state(app, CLAY_APP_PROMPTING);
    int result = clay_prompt_select(question, options, count, default_index);
    clay_app_set_state(app, CLAY_APP_IDLE);
    return result;
}

int clay_app_confirm(ClayApp *app, const char *question, int default_yes) {
    clay_app_set_state(app, CLAY_APP_PROMPTING);
    int result = clay_prompt_confirm(question, default_yes);
    clay_app_set_state(app, CLAY_APP_IDLE);
    return result;
}

int clay_app_choice(ClayApp *app, const char *question, const ClayChoice *choices, int count,
                     int allow_custom, char **custom_out) {
    clay_app_set_state(app, CLAY_APP_PROMPTING);
    int result = clay_prompt_choice(question, choices, count, allow_custom, custom_out);
    clay_app_set_state(app, CLAY_APP_IDLE);
    return result;
}
