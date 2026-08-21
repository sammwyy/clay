#ifndef CLAY_STR_H
#define CLAY_STR_H

#include <stdarg.h>
#include <stddef.h>

/* Growable string buffer. data is always NUL-terminated and safe to use
   as a plain C string. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ClayStr;

void clay_str_init(ClayStr *s);
void clay_str_free(ClayStr *s);
void clay_str_clear(ClayStr *s);
void clay_str_push_char(ClayStr *s, char c);
void clay_str_push(ClayStr *s, const char *text);
void clay_str_push_n(ClayStr *s, const char *text, size_t len);
void clay_str_insert_n(ClayStr *s, size_t at, const char *text, size_t len); /* shifts the tail right */
void clay_str_remove_n(ClayStr *s, size_t at, size_t len);                   /* shifts the tail left */
void clay_str_printf(ClayStr *s, const char *fmt, ...);
void clay_str_vprintf(ClayStr *s, const char *fmt, va_list args);

/* Classic shell-style wildcard match against plain C strings (not
   ClayStr): '*' matches any run of characters (including '/'), '?' matches
   exactly one. No libc glob/fnmatch, which Windows doesn't provide. */
int clay_str_wildcard_match(const char *pattern, const char *text);

#endif /* CLAY_STR_H */
