#include "clay/sse.h"

#include "clay/str.h"

#include <stdlib.h>
#include <string.h>

struct ClaySseParser {
  ClayStr line;
  ClayStr event_data;
  size_t line_limit;
  ClaySseDataFn on_data;
  void *userdata;
};

ClaySseParser *clay_sse_create(size_t line_limit, ClaySseDataFn on_data,
                               void *userdata) {
  ClaySseParser *parser = calloc(1, sizeof(*parser));
  if (!parser)
    return NULL;
  clay_str_init(&parser->line);
  clay_str_init(&parser->event_data);
  parser->line_limit = line_limit;
  parser->on_data = on_data;
  parser->userdata = userdata;
  return parser;
}

static void finish_line(ClaySseParser *parser) {
  size_t len = parser->line.len;
  if (len && parser->line.data[len - 1] == '\r')
    len--;
  if (!len) {
    if (parser->event_data.len && parser->on_data)
      parser->on_data(parser->event_data.data, parser->userdata);
    clay_str_clear(&parser->event_data);
  } else if (parser->line.data[0] != ':' && len >= 5 &&
             strncmp(parser->line.data, "data:", 5) == 0) {
    size_t start = 5;
    if (start < len && parser->line.data[start] == ' ')
      start++;
    if (parser->event_data.len)
      clay_str_push_char(&parser->event_data, '\n');
    clay_str_push_n(&parser->event_data, parser->line.data + start,
                    len - start);
  }
}

int clay_sse_feed(ClaySseParser *parser, const char *data, size_t len) {
  if (!parser || (!data && len))
    return -1;
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '\n') {
      finish_line(parser);
      clay_str_clear(&parser->line);
    } else {
      if (parser->line.len >= parser->line_limit)
        return -1;
      clay_str_push_char(&parser->line, data[i]);
    }
  }
  return 0;
}

void clay_sse_destroy(ClaySseParser *parser) {
  if (!parser)
    return;
  clay_str_free(&parser->line);
  clay_str_free(&parser->event_data);
  free(parser);
}
