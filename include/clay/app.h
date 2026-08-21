#ifndef CLAY_APP_H
#define CLAY_APP_H

#include "clay/command.h"
#include "clay/prompt.h"
#include "clay/task.h"

/* Command handlers call clay_app_* instead of the render functions
   directly, so the state below is always kept in sync with what's drawn. */
typedef enum {
    CLAY_APP_IDLE,      /* waiting for the next command or message */
    CLAY_APP_BUSY,      /* one or more tasks are running */
    CLAY_APP_PROMPTING, /* waiting on a confirm/choice answer from the user */
    CLAY_APP_EXITING    /* shutdown requested */
} ClayAppState;

typedef struct ClayApp ClayApp;

typedef void (*ClayAppStateListener)(ClayApp *app, ClayAppState old_state, ClayAppState new_state, void *ctx);

ClayApp *clay_app_create(void);
void clay_app_destroy(ClayApp *app);

ClayCommandRegistry *clay_app_commands(ClayApp *app);

ClayAppState clay_app_state(const ClayApp *app);
void clay_app_set_state(ClayApp *app, ClayAppState state);
void clay_app_on_state_change(ClayApp *app, ClayAppStateListener listener, void *ctx);

/* Opaque slot for the driver (main loop today, an agent daemon later)
   to stash context. */
void *clay_app_get_data(const ClayApp *app);
void clay_app_set_data(ClayApp *app, void *data);

/* Actions: mutate state, then render. Command handlers call these. */
void clay_app_say(ClayApp *app, const char *fmt, ...);
void clay_app_list_header(ClayApp *app, const char *fmt, ...);
void clay_app_list_step(ClayApp *app, int index, const char *verb, const char *target,
                         const char *info, int link);

ClayTask *clay_app_task_start(ClayApp *app, const char *fmt, ...);
void clay_app_task_success(ClayApp *app, ClayTask *task, const char *fmt, ...);
void clay_app_task_fail(ClayApp *app, ClayTask *task, const char *fmt, ...);

int clay_app_select(ClayApp *app, const char *question, const ClayChoice *options, int count,
                     int default_index);
int clay_app_confirm(ClayApp *app, const char *question, int default_yes);
int clay_app_choice(ClayApp *app, const char *question, const ClayChoice *choices, int count,
                     int allow_custom, char **custom_out);

#endif /* CLAY_APP_H */
