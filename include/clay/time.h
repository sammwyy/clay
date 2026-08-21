#ifndef CLAY_TIME_H
#define CLAY_TIME_H

long long clay_time_now(void);
char *clay_time_relative(long long timestamp, long long now);

#endif /* CLAY_TIME_H */
