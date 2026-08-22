#ifndef CLAY_ENCODING_H
#define CLAY_ENCODING_H

#include <stddef.h>

/* Malloc'd Base64URL text with no padding, or NULL on invalid input/allocation
   failure. The decoder accepts padded and unpadded Base64URL input. */
char *clay_base64url_encode(const unsigned char *data, size_t len);
char *clay_base64url_decode(const char *text);

/* RFC 3986 percent encoding and form/query decoding. Both return malloc'd
   strings; form decoding converts '+' to a space. */
char *clay_url_encode(const char *text);
char *clay_form_url_decode(const char *text, size_t len);

#endif /* CLAY_ENCODING_H */
