#include "../src/commands/context.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(clay_permissions_is_safe_command("ls -la"));
    assert(clay_permissions_is_safe_command("cat file.txt"));
    assert(clay_permissions_is_safe_command("/bin/grep -r foo ."));
    assert(clay_permissions_is_safe_command("git status"));
    assert(clay_permissions_is_safe_command("git log --oneline"));
    assert(!clay_permissions_is_safe_command("rm -rf /"));
    assert(!clay_permissions_is_safe_command("git commit -m x"));
    assert(!clay_permissions_is_safe_command("npm install"));

    /* shell_exec executes this string with a shell, so no expression with
       shell syntax can inherit a first-word safe-command approval. */
    assert(!clay_permissions_is_safe_command("ls; rm -rf build"));
    assert(!clay_permissions_is_safe_command("ls && rm -rf build"));
    assert(!clay_permissions_is_safe_command("ls | rm -rf build"));
    assert(!clay_permissions_is_safe_command("ls `rm -rf build`"));
    assert(!clay_permissions_is_safe_command("ls $(rm -rf build)"));
    assert(!clay_permissions_is_safe_command("ls\nrm -rf build"));
    assert(!clay_permissions_is_safe_command("ls > generated.txt"));

    assert(clay_permissions_is_mutating_command("rm -rf build"));
    assert(clay_permissions_is_mutating_command("mv a b"));
    assert(clay_permissions_is_mutating_command("cp a b"));
    assert(clay_permissions_is_mutating_command("sudo reboot"));
    assert(clay_permissions_is_mutating_command("git commit -m x"));
    assert(clay_permissions_is_mutating_command("git checkout -b feature"));
    assert(clay_permissions_is_mutating_command("git push"));
    assert(!clay_permissions_is_mutating_command("git status"));
    assert(!clay_permissions_is_mutating_command("git diff"));
    assert(!clay_permissions_is_mutating_command("ls -la"));
    assert(clay_permissions_is_mutating_command("ls; rm -rf build"));
    assert(clay_permissions_is_mutating_command("ls && rm -rf build"));
    assert(clay_permissions_is_mutating_command("ls | rm -rf build"));
    assert(clay_permissions_is_mutating_command("ls `rm -rf build`"));
    assert(clay_permissions_is_mutating_command("ls $(rm -rf build)"));
    assert(clay_permissions_is_mutating_command("ls\nrm -rf build"));
    assert(clay_permissions_is_mutating_command("ls > generated.txt"));
    /* Not on either list: allowed to run in Plan mode, just not auto-safe. */
    assert(!clay_permissions_is_mutating_command("npm test"));
    assert(!clay_permissions_is_safe_command("npm test"));

    assert(strcmp(clay_permissions_category_name(CLAY_PERMISSION_READ), "read") == 0);
    assert(strcmp(clay_permissions_category_name(CLAY_PERMISSION_EDIT), "edit") == 0);
    assert(strcmp(clay_permissions_category_name(CLAY_PERMISSION_EXEC_SAFE), "exec_safe") == 0);
    assert(strcmp(clay_permissions_category_name(CLAY_PERMISSION_EXEC_ALL), "exec_all") == 0);

    printf("permissions tests passed\n");
    return 0;
}
