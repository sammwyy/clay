#ifndef CLAY_TASK_H
#define CLAY_TASK_H

typedef struct ClayTask ClayTask;

/* Starts a spinning task line: "◆ clay  <label>… <spinner>", animated in
   yellow on a background thread. Stop it with clay_task_success/fail. */
ClayTask *clay_task_start(const char *fmt, ...);

/* Stops the spinner, replaces it with a green check and elapsed time,
   then prints the given result text on the same line. */
void clay_task_success(ClayTask *task, const char *fmt, ...);

/* Stops the spinner, replaces it with a red cross and elapsed time,
   then prints the given result text on the same line. */
void clay_task_fail(ClayTask *task, const char *fmt, ...);

/* Like clay_task_success/fail, but replaces the running label only after
   the spinner thread has stopped. */
void clay_task_success_with_label(ClayTask *task, const char *label, const char *fmt, ...);
void clay_task_fail_with_label(ClayTask *task, const char *label, const char *fmt, ...);

#endif /* CLAY_TASK_H */
