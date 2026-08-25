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

static pthread_mutex_t g_render_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_render_ready = PTHREAD_COND_INITIALIZER;
static unsigned int g_render_pause_depth = 0;
static int g_line_open = 0; /* a task row is on screen, waiting to be finished */

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
    pthread_mutex_lock(&g_render_lock);
    while (g_render_pause_depth > 0) pthread_cond_wait(&g_render_ready, &g_render_lock);
    clay_term_clear_line();
    /* One row per task: a label that wraps turns the live line into two and
       the spinner then redraws over the wrong one. */
    int room = clay_term_width() - 5 - (int)clay_utf8_width(icon);
    if (suffix) room -= (int)clay_utf8_width(suffix) + 1;
    printf("  %s%s%s %s", clay_color(icon_color), icon, clay_color(CLAY_RESET),
           clay_color(CLAY_GRAY));
    if (room > 1 && (int)clay_utf8_width(label) > room) {
        clay_term_write_clipped(label, room - 1);
        fputs("\xe2\x80\xa6", stdout);
    } else {
        fputs(label, stdout);
    }
    fputs(clay_color(CLAY_RESET), stdout);
    if (active) fputs("\xe2\x80\xa6", stdout);
    if (suffix) printf(" %s", suffix);
    g_line_open = active;
    fflush(stdout);
    pthread_mutex_unlock(&g_render_lock);
}

void clay_task_render_pause(void) {
    pthread_mutex_lock(&g_render_lock);
    g_render_pause_depth++;
    /* Park the live row: whatever interrupts a running task (a permission
       prompt, a question) gets a fresh line instead of overwriting it. */
    if (g_line_open) {
        fputc('\n', stdout);
        fflush(stdout);
        g_line_open = 0;
    }
    pthread_mutex_unlock(&g_render_lock);
}

void clay_task_render_resume(void) {
    pthread_mutex_lock(&g_render_lock);
    if (g_render_pause_depth > 0 && --g_render_pause_depth == 0)
        pthread_cond_broadcast(&g_render_ready);
    pthread_mutex_unlock(&g_render_lock);
}

static void *spinner_loop(void *arg) {
    ClayTask *task = arg;
    int frame = 0;

    ClayStr label;
    clay_str_init(&label);
    for (;;) {
        /* Copy the label out: clay_task_relabel can replace it between
           frames. */
        pthread_mutex_lock(&task->lock);
        int running = task->running;
        clay_str_clear(&label);
        clay_str_push(&label, task->label.data);
        pthread_mutex_unlock(&task->lock);
        if (!running) break;

        render_line(CLAY_YELLOW, SPINNER_FRAMES[frame], label.data, NULL, 1);
        frame = (frame + 1) % CLAY_SPINNER_FRAME_COUNT;
        clay_term_sleep_ms(80);
    }
    clay_str_free(&label);
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

void clay_task_relabel(ClayTask *task, const char *fmt, ...) {
    if (!task) return;
    ClayStr label;
    clay_str_init(&label);
    va_list args;
    va_start(args, fmt);
    clay_str_vprintf(&label, fmt, args);
    va_end(args);
    pthread_mutex_lock(&task->lock);
    clay_str_free(&task->label);
    task->label = label;
    pthread_mutex_unlock(&task->lock);
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
