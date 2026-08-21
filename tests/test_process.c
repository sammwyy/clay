#include "clay/term.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* `cat` echoes each stdin line back on stdout - a simple, universally
       available way to exercise bidirectional line I/O. */
    char *argv[] = {"cat", NULL};
    ClayProcess *process = clay_term_process_start("cat", argv);
    assert(process);

    assert(clay_term_process_write(process, "hello\n", 6) == 0);
    char *line = clay_term_process_read_line(process);
    assert(line);
    assert(strcmp(line, "hello") == 0);
    free(line);

    assert(clay_term_process_write(process, "world\n", 6) == 0);
    line = clay_term_process_read_line(process);
    assert(line);
    assert(strcmp(line, "world") == 0);
    free(line);

    clay_term_process_stop(process);

    /* A nonexistent program fails to start. */
    char *bad_argv[] = {"clay-does-not-exist-anywhere", NULL};
    ClayProcess *bad = clay_term_process_start("clay-does-not-exist-anywhere", bad_argv);
    if (bad) {
        /* fork+exec platforms report the exec failure asynchronously: the
           process starts, then the pipe closes once exec fails. */
        char *first_line = clay_term_process_read_line(bad);
        assert(first_line == NULL);
        free(first_line);
        clay_term_process_stop(bad);
    }

    printf("process tests passed\n");
    return 0;
}
