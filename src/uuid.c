#include "clay/uuid.h"

#include "clay/str.h"
#include "clay/term.h"

char *clay_uuid_v4(void) {
    unsigned char bytes[16];
    if (clay_term_random_bytes(bytes, sizeof(bytes)) != 0) return NULL;

    bytes[6] = (unsigned char)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (unsigned char)((bytes[8] & 0x3f) | 0x80);

    static const char hex[] = "0123456789abcdef";
    ClayStr uuid;
    clay_str_init(&uuid);
    for (size_t i = 0; i < sizeof(bytes); i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) clay_str_push_char(&uuid, '-');
        clay_str_push_char(&uuid, hex[bytes[i] >> 4]);
        clay_str_push_char(&uuid, hex[bytes[i] & 0x0f]);
    }
    return uuid.data;
}
