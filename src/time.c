#include "clay/time.h"

#include "clay/str.h"

#include <stdlib.h>
#include <time.h>

long long clay_time_now(void) {
    return (long long)time(NULL);
}

char *clay_time_format_date(long long timestamp) {
    time_t seconds = (time_t)timestamp;
    struct tm tm_utc;
    gmtime_r(&seconds, &tm_utc);
    ClayStr text;
    clay_str_init(&text);
    clay_str_printf(&text, "%04d-%02d-%02d", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
    return text.data;
}

char *clay_time_relative(long long timestamp, long long now) {
    long long seconds = now > timestamp ? now - timestamp : 0;
    ClayStr text;
    clay_str_init(&text);
    if (seconds < 10) clay_str_push(&text, "just now");
    else if (seconds < 60) clay_str_printf(&text, "%llds ago", seconds);
    else if (seconds < 3600) clay_str_printf(&text, "%lldm ago", seconds / 60);
    else if (seconds < 86400) clay_str_printf(&text, "%lldh ago", seconds / 3600);
    else clay_str_printf(&text, "%lldd ago", seconds / 86400);
    return text.data;
}
