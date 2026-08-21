#include "context.h"

void clay_cmd_below(const char *args, void *user_data) {
    (void)args;
    (void)user_data;
    static ClayBelowState states[] = {CLAY_BELOW_NONE, CLAY_BELOW_LOADING, CLAY_BELOW_FINISHED, CLAY_BELOW_IDLE};
    static int index = 0;
    static int tokens_on = 1;
    index = (index + 1) % 4;
    clay_below_set_state("status", states[index]);
    tokens_on = !tokens_on;
    clay_below_set_enabled("tokens", tokens_on);
    clay_sayc(CLAY_CYAN, "status state cycled, tokens module %s", tokens_on ? "enabled" : "disabled");
}
