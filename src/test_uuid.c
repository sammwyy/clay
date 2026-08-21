#include "clay/array.h"
#include "clay/uuid.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int uuid_is_valid(const char *uuid) {
    if (strlen(uuid) != 36 || uuid[14] != '4') return 0;
    if (uuid[8] != '-' || uuid[13] != '-' || uuid[18] != '-' || uuid[23] != '-') return 0;
    return uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b';
}

int main(void) {
    ClayArray ids;
    clay_array_init(&ids, sizeof(char *));

    for (size_t i = 0; i < 256; i++) {
        char *uuid = clay_uuid_v4();
        assert(uuid);
        assert(uuid_is_valid(uuid));
        for (size_t j = 0; j < ids.count; j++) {
            assert(strcmp(uuid, *(char **)clay_array_get(&ids, j)) != 0);
        }
        clay_array_push_val(&ids, &uuid);
    }

    for (size_t i = 0; i < ids.count; i++) free(*(char **)clay_array_get(&ids, i));
    clay_array_free(&ids);
    return 0;
}
