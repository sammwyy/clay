#include "context.h"

#include <stdio.h>

void clay_cmd_mm(const char *args, void *user_data) {
    (void)args;
    (void)user_data;
    ClayStr str;
    clay_str_init(&str);
    clay_str_push(&str, "clay v0.0.0 mm smoke test");
    clay_sayc(CLAY_CYAN, "ClayStr -> %s", str.data);
    clay_str_free(&str);

    ClayArray numbers;
    clay_array_init(&numbers, sizeof(int));
    for (int i = 0; i < 5; i++) {
        int value = i * i;
        clay_array_push_val(&numbers, &value);
    }
    printf("         %sClayArray ->%s ", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    for (size_t i = 0; i < numbers.count; i++) printf("%d ", *(int *)clay_array_get(&numbers, i));
    fputc('\n', stdout);
    clay_array_free(&numbers);

    ClayMap *map = clay_map_create();
    clay_map_set(map, "orange", "accent");
    clay_map_set(map, "gray", "muted");
    clay_sayc(CLAY_CYAN, "ClayMap -> orange=%s gray=%s", (char *)clay_map_get(map, "orange"),
              (char *)clay_map_get(map, "gray"));
    clay_map_destroy(map);
}
