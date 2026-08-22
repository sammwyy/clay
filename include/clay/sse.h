#ifndef CLAY_SSE_H
#define CLAY_SSE_H

#include <stddef.h>

typedef struct ClaySseParser ClaySseParser;
typedef void (*ClaySseDataFn)(const char *data, void *userdata);

/* Incremental standard SSE framing. `on_data` receives joined data lines once
   an empty line terminates an event. */
ClaySseParser *clay_sse_create(size_t line_limit, ClaySseDataFn on_data,
                               void *userdata);
int clay_sse_feed(ClaySseParser *parser, const char *data, size_t len);
void clay_sse_destroy(ClaySseParser *parser);

#endif /* CLAY_SSE_H */
