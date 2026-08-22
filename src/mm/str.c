#include "clay/str.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_STR_INITIAL_CAP 32

void clay_str_init(ClayStr *s) {
    s->cap = CLAY_STR_INITIAL_CAP;
    s->data = malloc(s->cap);
    if (s->data) s->data[0] = '\0';
    s->len = 0;
    if (!s->data) s->cap = 0;
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

static int str_reserve(ClayStr *s, size_t extra) {
    if (!s) return 0;
    if (s->len == SIZE_MAX || extra > SIZE_MAX - s->len - 1) return 0;
    size_t needed = s->len + extra + 1;
    if (needed <= s->cap && s->data) return 1;
    size_t new_cap = s->cap ? s->cap : CLAY_STR_INITIAL_CAP;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    char *grown = realloc(s->data, new_cap);
    if (!grown) return 0;
    s->data = grown;
    s->cap = new_cap;
    return 1;
}

void clay_str_push_n(ClayStr *s, const char *text, size_t len) {
    if ((!text && len > 0) || !str_reserve(s, len)) return;
    memcpy(s->data + s->len, text, len);
    s->len += len;
    s->data[s->len] = '\0';
}

void clay_str_push(ClayStr *s, const char *text) {
    if (!text) return;
    clay_str_push_n(s, text, strlen(text));
}

void clay_str_push_char(ClayStr *s, char c) {
    if (!str_reserve(s, 1)) return;
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

void clay_str_insert_n(ClayStr *s, size_t at, const char *text, size_t len) {
    if (!s || at > s->len || (!text && len > 0) || !str_reserve(s, len)) return;
    memmove(s->data + at + len, s->data + at, s->len - at + 1); /* +1 carries the NUL */
    memcpy(s->data + at, text, len);
    s->len += len;
}

void clay_str_remove_n(ClayStr *s, size_t at, size_t len) {
    if (at > s->len || len > s->len - at) return;
    memmove(s->data + at, s->data + at + len, s->len - at - len + 1); /* +1 carries the NUL */
    s->len -= len;
}

void clay_str_vprintf(ClayStr *s, const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return;

    if (!str_reserve(s, (size_t)needed)) return;
    vsnprintf(s->data + s->len, (size_t)needed + 1, fmt, args);
    s->len += (size_t)needed;
}

void clay_str_printf(ClayStr *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    clay_str_vprintf(s, fmt, args);
    va_end(args);
}

int clay_str_wildcard_match(const char *pattern, const char *text) {
    const char *p = pattern;
    const char *t = text;
    const char *star_p = NULL;
    const char *star_t = NULL;
    while (*t) {
        if (*p == '?' || (*p && *p == *t)) {
            p++;
            t++;
        } else if (*p == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p) {
            p = star_p + 1;
            t = ++star_t;
        } else return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}
