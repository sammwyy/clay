#include "clay/task.h"

#include "clay/color.h"
#include "clay/str.h"
#include "clay/term.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CLAY_SPINNER_FRAME_COUNT 10

static const char *SPINNER_FRAMES[CLAY_SPINNER_FRAME_COUNT] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
    "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"
};

struct ClayTask {
    ClayStr label;
    struct timespec start;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
};

static double elapsed_seconds(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void render_line(const char *icon_color, const char *icon, const char *label, const char *suffix, int active) {
    clay_term_clear_line();
    printf("%s%s clay%s  %s%s%s %s%s%s", clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND, clay_color(CLAY_RESET),
           clay_color(icon_color), icon, clay_color(CLAY_RESET), clay_color(CLAY_GRAY), label, clay_color(CLAY_RESET));
    if (active) fputs("\xe2\x80\xa6", stdout);
    if (suffix) printf(" %s", suffix);
    fflush(stdout);
}

static void *spinner_loop(void *arg) {
    ClayTask *task = arg;
    int frame = 0;

    for (;;) {
        pthread_mutex_lock(&task->lock);
        int running = task->running;
        pthread_mutex_unlock(&task->lock);
        if (!running) break;

        render_line(CLAY_YELLOW, SPINNER_FRAMES[frame], task->label.data, NULL, 1);
        frame = (frame + 1) % CLAY_SPINNER_FRAME_COUNT;
        clay_term_sleep_ms(80);
    }
    return NULL;
}

ClayTask *clay_task_start(const char *fmt, ...) {
    ClayTask *task = malloc(sizeof(ClayTask));
    clay_str_init(&task->label);

    va_list args;
    va_start(args, fmt);
    clay_str_vprintf(&task->label, fmt, args);
    va_end(args);

    clock_gettime(CLOCK_MONOTONIC, &task->start);
    pthread_mutex_init(&task->lock, NULL);
    task->running = 1;

    clay_term_hide_cursor();
    pthread_create(&task->thread, NULL, spinner_loop, task);
    return task;
}

static void finish(ClayTask *task, const char *icon_color, const char *icon, const char *label,
                   const char *fmt, va_list args) {
    pthread_mutex_lock(&task->lock);
    task->running = 0;
    pthread_mutex_unlock(&task->lock);
    pthread_join(task->thread, NULL);

    if (label) {
        clay_str_clear(&task->label);
        clay_str_push(&task->label, label);
    }

    ClayStr result;
    clay_str_init(&result);
    clay_str_vprintf(&result, fmt, args);

    ClayStr suffix;
    clay_str_init(&suffix);
    if (result.len > 0) {
        clay_str_printf(&suffix, "%s%s%s ", clay_color(icon_color), result.data, clay_color(CLAY_RESET));
    }
    clay_str_printf(&suffix, "%s(%.1fs)%s", clay_color(CLAY_GRAY), elapsed_seconds(&task->start),
                    clay_color(CLAY_RESET));

    render_line(icon_color, icon, task->label.data, suffix.data, 0);
    fputc('\n', stdout);
    clay_term_show_cursor();

    clay_str_free(&result);
    clay_str_free(&suffix);
    clay_str_free(&task->label);
    pthread_mutex_destroy(&task->lock);
    free(task);
}

void clay_task_success(ClayTask *task, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    finish(task, CLAY_GREEN, CLAY_ICON_CHECK, NULL, fmt, args);
    va_end(args);
}

void clay_task_fail(ClayTask *task, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    finish(task, CLAY_RED, CLAY_ICON_CROSS, NULL, fmt, args);
    va_end(args);
}

void clay_task_success_with_label(ClayTask *task, const char *label, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    finish(task, CLAY_GREEN, CLAY_ICON_CHECK, label, fmt, args);
    va_end(args);
}

void clay_task_fail_with_label(ClayTask *task, const char *label, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    finish(task, CLAY_RED, CLAY_ICON_CROSS, label, fmt, args);
    va_end(args);
}
