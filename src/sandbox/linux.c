#define _GNU_SOURCE

#include "clay/sandbox.h"
#include "clay/shell.h"

#include "clay/term.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CLAY_SANDBOX_TIMEOUT_SECONDS 120
#define CLAY_SANDBOX_CPU_SECONDS 60
#define CLAY_SANDBOX_ADDRESS_SPACE_LIMIT (1024ULL * 1024 * 1024)
#define CLAY_SANDBOX_PROCESS_LIMIT 64
#define CLAY_SANDBOX_FILE_SIZE_LIMIT (512ULL * 1024 * 1024)

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

static int optional_mount_allowed(const char *path) {
    return path && path[0] == '/' && strncmp(path, "/workspace", 10) != 0 &&
           strncmp(path, "/scratch", 8) != 0 && strncmp(path, "/proc", 5) != 0 &&
           strncmp(path, "/tmp", 4) != 0 && !strstr(path, "/../");
}

static int mount_optional_readonly(const ClayStr *root, const char *source) {
    if (!optional_mount_allowed(source)) return -1;
    struct stat info;
    if (stat(source, &info) != 0) return -1;
    ClayStr target;
    clay_str_init(&target);
    clay_str_push(&target, root->data);
    clay_str_push(&target, source);
    char *slash = strrchr(target.data, '/');
    if (!slash) { clay_str_free(&target); return -1; }
    *slash = '\0';
    int parents_ok = mkdir_p(target.data) == 0;
    *slash = '/';
    int target_ok;
    if (S_ISDIR(info.st_mode)) target_ok = mkdir(target.data, 0755) == 0 || errno == EEXIST;
    else {
        int fd = open(target.data, O_CREAT | O_RDONLY, 0600);
        target_ok = fd >= 0;
        if (fd >= 0) close(fd);
    }
    int rc = parents_ok && target_ok ? bind_mount(source, target.data, 1) : -1;
    clay_str_free(&target);
    return rc;
}

static int setup_system_dirs(const ClayStr *root) {
    static const char *const SYSTEM_DIRS[] = {
        "/usr", "/bin", "/sbin", "/lib", "/lib64",
    };
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
            ok = mkdir(target.data, 0755) == 0 && bind_mount(host_path, target.data, 1) == 0;
        } else {
            ok = 1;
        }
        clay_str_free(&target);
        if (!ok) return -1;
    }
    return 0;
}

static int setup_devices(const ClayStr *root) {
    static const struct { const char *path; int readonly; } DEVICES[] = {
        {"/dev/null", 0}, {"/dev/zero", 1}, {"/dev/urandom", 1},
    };
    if (make_dir_under(root, "/dev") != 0) return -1;
    for (size_t i = 0; i < sizeof(DEVICES) / sizeof(DEVICES[0]); i++) {
        ClayStr target;
        clay_str_init(&target);
        clay_str_push(&target, root->data);
        clay_str_push(&target, DEVICES[i].path);
        int fd = open(target.data, O_CREAT | O_RDONLY, 0600);
        if (fd >= 0) close(fd);
        int ok = fd >= 0 && bind_mount(DEVICES[i].path, target.data, DEVICES[i].readonly) == 0;
        clay_str_free(&target);
        if (!ok) return -1;
    }
    return 0;
}

static int apply_limits(char *diag, size_t diag_len) {
    struct rlimit limit;

    limit.rlim_cur = CLAY_SANDBOX_CPU_SECONDS;
    limit.rlim_max = CLAY_SANDBOX_CPU_SECONDS + 1;
    if (setrlimit(RLIMIT_CPU, &limit) != 0) goto fail;
    limit.rlim_cur = CLAY_SANDBOX_ADDRESS_SPACE_LIMIT;
    limit.rlim_max = CLAY_SANDBOX_ADDRESS_SPACE_LIMIT;
    if (setrlimit(RLIMIT_AS, &limit) != 0) goto fail;
    limit.rlim_cur = CLAY_SANDBOX_PROCESS_LIMIT;
    limit.rlim_max = CLAY_SANDBOX_PROCESS_LIMIT;
    if (setrlimit(RLIMIT_NPROC, &limit) != 0) goto fail;
    limit.rlim_cur = CLAY_SANDBOX_FILE_SIZE_LIMIT;
    limit.rlim_max = CLAY_SANDBOX_FILE_SIZE_LIMIT;
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) goto fail;
    return 0;

fail:
    snprintf(diag, diag_len, "clay: sandbox resource limit failed: %s\n", strerror(errno));
    return -1;
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
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNET) != 0) {
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
    if (setup_system_dirs(&root) != 0) {
        snprintf(diag, diag_len, "clay: sandbox system mount failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }
    if (setup_devices(&root) != 0) {
        snprintf(diag, diag_len, "clay: sandbox device setup failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }
    for (size_t i = 0; i < config->readonly_mount_count; i++) {
        if (mount_optional_readonly(&root, config->readonly_mounts[i]) != 0) {
            snprintf(diag, diag_len, "clay: sandbox readonly mount failed: %s\n",
                     config->readonly_mounts[i]);
            clay_str_free(&root);
            return -1;
        }
    }
    if (make_dir_under(&root, "/proc") != 0 || make_dir_under(&root, "/workspace") != 0 ||
        make_dir_under(&root, "/scratch") != 0 || make_dir_under(&root, "/tmp") != 0) {
        snprintf(diag, diag_len, "clay: sandbox mountpoint setup failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
    }
    const char *scratch_name = strrchr(config->scratch_dir, '/');
    scratch_name = scratch_name ? scratch_name + 1 : config->scratch_dir;
    ClayStr sandbox_tmp;
    clay_str_init(&sandbox_tmp);
    clay_str_printf(&sandbox_tmp, "/tmp/%s", scratch_name);
    if (make_dir_under(&root, sandbox_tmp.data) != 0 ||
        bind_mount_under(&root, "/workspace", config->workspace_dir, 0) != 0 ||
        bind_mount_under(&root, sandbox_tmp.data, config->scratch_dir, 0) != 0) {
        snprintf(diag, diag_len, "clay: sandbox workspace/scratch mount failed: %s\n", strerror(errno));
        clay_str_free(&sandbox_tmp);
        clay_str_free(&root);
        return -1;
    }

    ClayStr scratch_link;
    clay_str_init(&scratch_link);
    clay_str_push(&scratch_link, root.data);
    clay_str_push(&scratch_link, "/scratch");
    ClayStr scratch_target;
    clay_str_init(&scratch_target);
    clay_str_printf(&scratch_target, "/tmp/%s", scratch_name);
    rmdir(scratch_link.data);
    int tmp_ok = symlink(scratch_target.data, scratch_link.data) == 0;
    clay_str_free(&scratch_target);
    clay_str_free(&scratch_link);
    clay_str_free(&sandbox_tmp);
    if (!tmp_ok) {
        snprintf(diag, diag_len, "clay: sandbox /tmp symlink failed: %s\n", strerror(errno));
        clay_str_free(&root);
        return -1;
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

    if (chdir("/") != 0) {
        snprintf(diag, diag_len, "clay: sandbox chdir / failed: %s\n", strerror(errno));
        return -1;
    }
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
    if (setpgid(0, 0) != 0) {
        fprintf(stderr, "clay: sandbox process group failed: %s\n", strerror(errno));
        _exit(126);
    }
    if (setup_sandbox(config, unshared_fd, mapped_fd, diag, sizeof(diag)) != 0) {
        if (*diag) fputs(diag, stderr);
        _exit(126);
    }
    if (apply_limits(diag, sizeof(diag)) != 0) {
        fputs(diag, stderr);
        _exit(126);
    }

    /* Do not duplicate buffered terminal/UI bytes into the child pipe. */
    fflush(NULL);
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
        const char *scratch_name = strrchr(config->scratch_dir, '/');
        scratch_name = scratch_name ? scratch_name + 1 : config->scratch_dir;
        ClayStr tmpdir;
        clay_str_init(&tmpdir);
        clay_str_printf(&tmpdir, "/tmp/%s", scratch_name);
        int environment_ok = clearenv() == 0 && setenv("HOME", "/scratch", 1) == 0 &&
            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1) == 0 &&
            setenv("TMPDIR", tmpdir.data, 1) == 0;
        clay_str_free(&tmpdir);
        if (!environment_ok) {
            fprintf(stderr, "clay: sandbox environment setup failed: %s\n", strerror(errno));
            _exit(126);
        }
        if (config->use_integrated_shell) _exit(clay_shell_run_command(command));
        execl("/bin/bash", "bash", "-c", command, (char *)NULL);
        fprintf(stderr, "clay: sandbox exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128);
}

static long long monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void append_output(ClayStr *output, size_t output_limit, const char *data, size_t len, int *truncated) {
    if (output->len >= output_limit) {
        *truncated = 1;
        return;
    }
    size_t kept = output_limit - output->len;
    if (kept > len) kept = len;
    clay_str_push_n(output, data, kept);
    if (kept != len) *truncated = 1;
}

static void stop_process_group(pid_t pid) {
    if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
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

    int truncated = 0;
    int status = 0;
    int child_done = 0;
    int pipe_open = 1;
    int timed_out = 0;
    int flags = fcntl(fds[0], F_GETFL);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fds[0]);
        stop_process_group(pid);
        waitpid(pid, NULL, 0);
        cleanup_stage_dir(pid);
        return -1;
    }

    long long deadline = monotonic_milliseconds() + (long long)CLAY_SANDBOX_TIMEOUT_SECONDS * 1000;
    while (pipe_open || !child_done) {
        if (!child_done && waitpid(pid, &status, WNOHANG) == pid) child_done = 1;
        if (!child_done && !timed_out && monotonic_milliseconds() >= deadline) {
            stop_process_group(pid);
            timed_out = 1;
        }

        struct pollfd poll_fd = {fds[0], POLLIN | POLLHUP, 0};
        int polled = pipe_open ? poll(&poll_fd, 1, child_done ? 0 : 100) : poll(NULL, 0, 100);
        if (polled < 0) {
            if (errno == EINTR) continue;
            close(fds[0]);
            stop_process_group(pid);
            waitpid(pid, NULL, 0);
            cleanup_stage_dir(pid);
            return -1;
        }
        if (polled == 0) continue;

        for (;;) {
            char buffer[4096];
            ssize_t read_count = read(fds[0], buffer, sizeof(buffer));
            if (read_count > 0) {
                for (ssize_t i = 0; i < read_count; i++) {
                    if (buffer[i] == '\0') buffer[i] = '?';
                }
                append_output(output, output_limit, buffer, (size_t)read_count, &truncated);
                continue;
            }
            if (read_count == 0) {
                close(fds[0]);
                pipe_open = 0;
            }
            if (read_count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(fds[0]);
                stop_process_group(pid);
                waitpid(pid, NULL, 0);
                cleanup_stage_dir(pid);
                return -1;
            }
            break;
        }
    }

    if (timed_out) {
        static const char timeout_message[] = "\nclay: sandbox command timed out\n";
        append_output(output, output_limit, timeout_message, sizeof(timeout_message) - 1, &truncated);
    }
    if (exit_code) *exit_code = timed_out ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    if (output_truncated) *output_truncated = truncated;
    cleanup_stage_dir(pid);
    return 0;
}
