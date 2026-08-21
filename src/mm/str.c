#include "clay/str.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_STR_INITIAL_CAP 32

void clay_str_init(ClayStr *s) {
    s->cap = CLAY_STR_INITIAL_CAP;
    s->data = malloc(s->cap);
    s->data[0] = '\0';
    s->len = 0;
}

void clay_str_free(ClayStr *s) {
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void clay_str_clear(ClayStr *s) {
    s->len = 0;
    if (s->data) s->data[0] = '\0';
}

static void str_reserve(ClayStr *s, size_t extra) {
    size_t needed = s->len + extra + 1;
    if (needed <= s->cap) return;
    size_t new_cap = s->cap ? s->cap : CLAY_STR_INITIAL_CAP;
    while (new_cap < needed) new_cap *= 2;
    s->data = realloc(s->data, new_cap);
    s->cap = new_cap;
}

void clay_str_push_n(ClayStr *s, const char *text, size_t len) {
    str_reserve(s, len);
    memcpy(s->data + s->len, text, len);
    s->len += len;
    s->data[s->len] = '\0';
}

void clay_str_push(ClayStr *s, const char *text) {
    clay_str_push_n(s, text, strlen(text));
}

void clay_str_push_char(ClayStr *s, char c) {
    str_reserve(s, 1);
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

void clay_str_vprintf(ClayStr *s, const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return;

    str_reserve(s, (size_t)needed);
    vsnprintf(s->data + s->len, (size_t)needed + 1, fmt, args);
    s->len += (size_t)needed;
}

void clay_str_printf(ClayStr *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    clay_str_vprintf(s, fmt, args);
    va_end(args);
}
