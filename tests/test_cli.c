#include "clay/cli.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    char *argv[] = {"clay", "command", "--flag", "--hello", "world", "-foo", "1234"};
    ClayCli *cli = clay_cli_create("clay", "test");
    clay_cli_add_bool(cli, "flag", "test flag");
    clay_cli_add_string(cli, "hello", "test string");
    clay_cli_add_number(cli, "foo", "test number");

    int rc = clay_cli_parse(cli, (int)(sizeof(argv) / sizeof(argv[0])), argv);
    int ok = rc == 0 && clay_cli_bool(cli, "flag") &&
             clay_cli_string(cli, "hello") && strcmp(clay_cli_string(cli, "hello"), "world") == 0 &&
             clay_cli_number(cli, "foo") == 1234.0 &&
             clay_cli_command(cli) && strcmp(clay_cli_command(cli), "command") == 0;
    clay_cli_destroy(cli);

    if (!ok) {
        fputs("cli test failed\n", stderr);
        return 1;
    }
    return 0;
}
