#include "clay/encoding.h"

#include "clay/str.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *clay_base64url_encode(const unsigned char *data, size_t len) {
  static const char chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  if (!data && len)
    return NULL;
  ClayStr out;
  clay_str_init(&out);
  for (size_t i = 0; i < len; i += 3) {
    unsigned int value = (unsigned int)data[i] << 16;
    if (i + 1 < len)
      value |= (unsigned int)data[i + 1] << 8;
    if (i + 2 < len)
      value |= data[i + 2];
    clay_str_push_char(&out, chars[(value >> 18) & 63]);
    clay_str_push_char(&out, chars[(value >> 12) & 63]);
    if (i + 1 < len)
      clay_str_push_char(&out, chars[(value >> 6) & 63]);
    if (i + 2 < len)
      clay_str_push_char(&out, chars[value & 63]);
  }
  return out.data;
}

static int base64_value(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '-' || c == '+')
    return 62;
  if (c == '_' || c == '/')
    return 63;
  return -1;
}

char *clay_base64url_decode(const char *text) {
  if (!text || strlen(text) % 4 == 1)
    return NULL;
  ClayStr out;
  clay_str_init(&out);
  unsigned int accumulator = 0;
  int bits = 0;
  for (const char *p = text; *p && *p != '='; p++) {
    int value = base64_value(*p);
    if (value < 0) {
      clay_str_free(&out);
      return NULL;
    }
    accumulator = (accumulator << 6) | (unsigned int)value;
    bits += 6;
    while (bits >= 8) {
      bits -= 8;
      clay_str_push_char(&out, (char)(accumulator >> bits));
    }
  }
  return out.data;
}

char *clay_url_encode(const char *text) {
  if (!text)
    return NULL;
  static const char hex[] = "0123456789ABCDEF";
  ClayStr out;
  clay_str_init(&out);
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
      clay_str_push_char(&out, *p);
    } else {
      clay_str_push_char(&out, '%');
      clay_str_push_char(&out, hex[*p >> 4]);
      clay_str_push_char(&out, hex[*p & 15]);
    }
  }
  return out.data;
}

char *clay_form_url_decode(const char *text, size_t len) {
  if (!text)
    return NULL;
  ClayStr out;
  clay_str_init(&out);
  for (size_t i = 0; i < len; i++) {
    if (text[i] == '+') {
      clay_str_push_char(&out, ' ');
      continue;
    }
    if (text[i] == '%' && i + 2 < len && isxdigit((unsigned char)text[i + 1]) &&
        isxdigit((unsigned char)text[i + 2])) {
      char hex[3] = {text[i + 1], text[i + 2], '\0'};
      clay_str_push_char(&out, (char)strtol(hex, NULL, 16));
      i += 2;
    } else {
      clay_str_push_char(&out, text[i]);
    }
  }
  return out.data;
}
