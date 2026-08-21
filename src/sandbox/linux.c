#define _GNU_SOURCE

#include "clay/sandbox.h"

#include "clay/term.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Creates every directory component of an absolute path, ignoring
   components that already exist. */
static int mkdir_p(const char *path) {
    ClayStr partial;
    clay_str_init(&partial);
    const char *p = path;
    if (*p == '/') {
        clay_str_push_char(&partial, '/');
        p++;
    }
    int ok = 1;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        clay_str_push_n(&partial, p, len);
        if (partial.len > 0 && mkdir(partial.data, 0755) != 0 && errno != EEXIST) {
            ok = 0;
            break;
        }
        if (!slash) break;
        clay_str_push_char(&partial, '/');
        p = slash + 1;
    }
    clay_str_free(&partial);
    return ok ? 0 : -1;
}

/* A readonly remount must repeat any restriction flags the source
   already had (e.g. /run and /dev are nosuid/nodev) - dropping one
   implicitly fails with EPERM inside a user namespace. */
static int bind_mount(const char *source, const char *target, int readonly) {
    if (mount(source, target, NULL, MS_BIND | MS_REC, NULL) != 0) return -1;
    if (!readonly) return 0;
    struct statvfs vfs;
    unsigned long preserve = 0;
    if (statvfs(target, &vfs) == 0) {
        preserve = vfs.f_flag & (MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_NODIRATIME | MS_RELATIME);
    }
    return mount(NULL, target, NULL, MS_BIND | MS_REC | MS_REMOUNT | MS_RDONLY | preserve, NULL) == 0 ? 0 : -1;
}

static int make_dir_under(const ClayStr *root, const char *sub) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_push(&path, root->data);
    clay_str_push(&path, sub);
    int ok = mkdir(path.data, 0755) == 0 || errno == EEXIST;
    clay_str_free(&path);
    return ok ? 0 : -1;
}

static int bind_mount_under(const ClayStr *root, const char *sub, const char *source, int readonly) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_push(&path, root->data);
    clay_str_push(&path, sub);
    int rc = bind_mount(source, path.data, readonly);
    clay_str_free(&path);
    return rc;
}

/* Preserves the shell's system binaries/config: symlinked top-level
   dirs (e.g. /bin -> usr/bin) are recreated as symlinks, real
   directories are bind-mounted. Missing entries are skipped. */
static int setup_system_dirs(const ClayStr *root, ClaySandboxAccess access) {
    static const char *const SYSTEM_DIRS[] = {
        "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/var", "/run", "/opt", "/dev",
    };
    int readonly = access == CLAY_SANDBOX_ACCESS_READONLY;
    for (size_t i = 0; i < sizeof(SYSTEM_DIRS) / sizeof(SYSTEM_DIRS[0]); i++) {
        const char *host_path = SYSTEM_DIRS[i];
        struct stat st;
        if (lstat(host_path, &st) != 0) continue;

        ClayStr target;
        clay_str_init(&target);
        clay_str_push(&target, root->data);
        clay_str_push(&target, host_path);

        int ok;
        if (S_ISLNK(st.st_mode)) {
            char link[PATH_MAX];
            ssize_t n = readlink(host_path, link, sizeof(link) - 1);
            ok = n >= 0;
            if (ok) {
                link[n] = '\0';
                ok = symlink(link, target.data) == 0;
            }
        } else if (S_ISDIR(st.st_mode)) {
            ok = mkdir(target.data, 0755) == 0 && bind_mount(host_path, target.data, readonly) == 0;
        } else {
            ok = 1;
        }
        clay_str_free(&target);
        if (!ok) return -1;
    }
    return 0;
}

/* newuidmap/newgidmap: some distros deny a direct uid_map/gid_map write. */
static int run_id_mapper(const char *tool, pid_t pid, unsigned id) {
    pid_t mapper = fork();
    if (mapper < 0) return -1;
    if (mapper == 0) {
        char pid_str[32];
        char id_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);
        snprintf(id_str, sizeof(id_str), "%u", id);
        execlp(tool, tool, pid_str, id_str, id_str, "1", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(mapper, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Namespaces the process and remaps its filesystem view via pivot_root.
   Any failure returns -1 before exec, so a broken sandbox never falls
   back to running the command unsandboxed. */
static int setup_sandbox(const ClaySandboxConfig *config, int unshared_fd, int mapped_fd, char *diag,
                         size_t diag_len) {
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS) != 0) {
        snprintf(diag, diag_len, "clay: sandbox unshare failed: %s\n", strerror(errno));
        return -1;
    }
    char signal_byte = 1;
    ssize_t unused = write(unshared_fd, &signal_byte, 1);
    (void)unused;
    close(unshared_fd);

    char ready = 0;
    ssize_t n = read(mapped_fd, &ready, 1);
    close(mapped_fd);
    if (n != 1) {
        snprintf(diag, diag_len, "clay: sandbox uid/gid mapping failed (missing newuidmap/newgidmap?)\n");
        return -1;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        snprintf(diag, diag_len, "clay: sandbox mount-private failed: %s\n", strerror(errno));
        return -1;
    }

    char *home = clay_term_home_dir();
    ClayStr root;
    clay_str_init(&root);
    clay_str_printf(&root, "%s/.clay/sandbox/%d", home ? home : "/tmp", (int)getpid());
    free(home);

    if (mkdir_p(root.data) != 0 || mount("tmpfs", root.data, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0) {
        snprintf(diag, diag_len, "clay: sandbox root setup failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }
    if (setup_system_dirs(&root, config->access) != 0) {
        snprintf(diag, diag_len, "clay: sandbox system mount failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }
    if (make_dir_under(&root, "/proc") != 0 || make_dir_under(&root, "/workspace") != 0 ||
        make_dir_under(&root, "/scratch") != 0) {
        snprintf(diag, diag_len, "clay: sandbox mountpoint setup failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }
    if (bind_mount_under(&root, "/workspace", config->workspace_dir, 0) != 0 ||
        bind_mount_under(&root, "/scratch", config->scratch_dir, 0) != 0) {
        snprintf(diag, diag_len, "clay: sandbox workspace/scratch mount failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }

    ClayStr tmp_path;
    clay_str_init(&tmp_path);
    clay_str_push(&tmp_path, root.data);
    clay_str_push(&tmp_path, "/tmp");
    int tmp_ok = symlink("/scratch", tmp_path.data) == 0;
    clay_str_free(&tmp_path);
    if (!tmp_ok) {
        snprintf(diag, diag_len, "clay: sandbox /tmp symlink failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }

    char *sandbox_home = clay_term_home_dir();
    if (sandbox_home) {
        ClayStr home_in_root;
        clay_str_init(&home_in_root);
        clay_str_push(&home_in_root, root.data);
        clay_str_push(&home_in_root, sandbox_home);
        int home_ok = mkdir_p(home_in_root.data) == 0 && bind_mount(sandbox_home, home_in_root.data, 1) == 0;
        clay_str_free(&home_in_root);
        free(sandbox_home);
        if (!home_ok) {
            snprintf(diag, diag_len, "clay: sandbox home mount failed: %s\n", strerror(errno));
            clay_str_free(&root);
            return -1;
        }
    }

    ClayStr oldroot;
    clay_str_init(&oldroot);
    clay_str_push(&oldroot, root.data);
    clay_str_push(&oldroot, "/.oldroot");
    int pivot_ok = mkdir(oldroot.data, 0700) == 0 && syscall(SYS_pivot_root, root.data, oldroot.data) == 0;
    clay_str_free(&oldroot);
    clay_str_free(&root);
    if (!pivot_ok) {
        snprintf(diag, diag_len, "clay: sandbox pivot_root failed: %s\n", strerror(errno));
        return -1;
    }

    chdir("/");
    /* /.oldroot stays mounted - the kernel refuses a fresh /proc mount
       once it's gone. run_child detaches it right after that mount. */
    if (chdir("/workspace") != 0) {
        snprintf(diag, diag_len, "clay: sandbox chdir /workspace failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void run_child(const ClaySandboxConfig *config, const char *command, int unshared_fd, int mapped_fd) {
    char diag[256] = "";
    if (setup_sandbox(config, unshared_fd, mapped_fd, diag, sizeof(diag)) != 0) {
        if (*diag) fputs(diag, stderr);
        _exit(126);
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "clay: sandbox fork failed: %s\n", strerror(errno));
        _exit(126);
    }
    if (pid == 0) {
        if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
            fprintf(stderr, "clay: sandbox mount /proc failed: %s\n", strerror(errno));
            _exit(126);
        }
        umount2("/.oldroot", MNT_DETACH);
        rmdir("/.oldroot");
        execl("/bin/bash", "bash", "-c", command, (char *)NULL);
        fprintf(stderr, "clay: sandbox exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128);
}

static void cleanup_stage_dir(pid_t pid) {
    char *home = clay_term_home_dir();
    if (!home) return;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/sandbox/%d", home, (int)pid);
    free(home);
    rmdir(path.data);
    clay_str_free(&path);
}

int clay_sandbox_supported(void) {
    return 1;
}

int clay_sandbox_exec(const ClaySandboxConfig *config, const char *command, ClayStr *output,
                      size_t output_limit, int *exit_code, int *output_truncated) {
    if (config->mode == CLAY_SANDBOX_MODE_UNLEASHED) {
        return clay_term_shell_exec(command, output, output_limit, exit_code, output_truncated);
    }

    int fds[2];
    if (pipe(fds) != 0) return -1;
    int unshared_fds[2];
    int mapped_fds[2];
    if (pipe(unshared_fds) != 0 || pipe(mapped_fds) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        close(unshared_fds[0]);
        close(unshared_fds[1]);
        close(mapped_fds[0]);
        close(mapped_fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        close(unshared_fds[0]);
        close(mapped_fds[1]);
        run_child(config, command, unshared_fds[1], mapped_fds[0]);
        _exit(126);
    }

    close(fds[1]);
    close(unshared_fds[1]);
    close(mapped_fds[0]);
    char unshared = 0;
    int mapped = read(unshared_fds[0], &unshared, 1) == 1 && run_id_mapper("newuidmap", pid, getuid()) == 0 &&
                 run_id_mapper("newgidmap", pid, getgid()) == 0;
    close(unshared_fds[0]);
    if (mapped) {
        char ready = 1;
        ssize_t unused = write(mapped_fds[1], &ready, 1);
        (void)unused;
    }
    close(mapped_fds[1]);

    FILE *pipe_read = fdopen(fds[0], "r");
    int truncated = 0;
    int character;
    while ((character = fgetc(pipe_read)) != EOF) {
        if (output->len < output_limit) {
            clay_str_push_char(output, character == '\0' ? '?' : (char)character);
        } else {
            truncated = 1;
        }
    }
    fclose(pipe_read);

    int status = 0;
    waitpid(pid, &status, 0);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (output_truncated) *output_truncated = truncated;
    cleanup_stage_dir(pid);
    return 0;
}
