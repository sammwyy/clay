#include "clay/term.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#ifdef _WIN32
/* MinGW exposes the CRT's secure rand_s declaration behind this switch. */
#define _CRT_RAND_S
#endif
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#define CLAY_PATH_MAX MAX_PATH
#else
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#define CLAY_PATH_MAX 4096
#endif

static volatile sig_atomic_t g_interrupted = 0;
static int g_pending_escape = 0;

#ifndef _WIN32
static void handle_sigint(int signal_number) {
    (void)signal_number;
    g_interrupted = 1;
}
#endif

void clay_term_init(void) {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        /* The UI strings are UTF-8. cmd.exe commonly starts with an OEM
           code page, which renders each UTF-8 byte as a separate glyph. */
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(in, &mode)) SetConsoleCP(CP_UTF8);
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
#endif
}

void clay_term_hide_cursor(void) { fputs("\x1b[?25l", stdout); }
void clay_term_show_cursor(void) { fputs("\x1b[?25h", stdout); }
void clay_term_clear_line(void) { fputs("\r\x1b[2K", stdout); }

void clay_term_cursor_up(int n) {
    if (n > 0) printf("\x1b[%dA", n);
}

void clay_term_cursor_down(int n) {
    if (n > 0) printf("\x1b[%dB", n);
}

void clay_term_cursor_col(int col) {
    printf("\x1b[%dG", col + 1);
}

void clay_term_insert_line(void) {
    fputs("\x1b[L", stdout);
}

void clay_term_row_enter(int row, int *established) {
    if (row < *established) {
        clay_term_cursor_down(1);
    } else {
        fputc('\n', stdout);
        *established = row + 1;
    }
    fputc('\r', stdout);
}

void clay_term_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((unsigned int)ms * 1000);
#endif
}

int clay_term_shell_exec(const char *command, ClayStr *output, size_t output_limit, int *exit_code,
                         int *output_truncated) {
    ClayStr invocation;
    clay_str_init(&invocation);
    clay_str_printf(&invocation, "%s 2>&1", command);

#ifdef _WIN32
    FILE *pipe = _popen(invocation.data, "r");
#else
    FILE *pipe = popen(invocation.data, "r");
#endif
    clay_str_free(&invocation);
    if (!pipe) return -1;

    int truncated = 0;
    int character;
    while ((character = fgetc(pipe)) != EOF) {
        if (output->len < output_limit) {
            clay_str_push_char(output, character == '\0' ? '?' : (char)character);
        } else {
            truncated = 1;
        }
    }

#ifdef _WIN32
    int status = _pclose(pipe);
    if (exit_code) *exit_code = status < 0 ? -1 : status;
#else
    int status = pclose(pipe);
    if (exit_code) *exit_code = status < 0 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : status);
#endif
    if (output_truncated) *output_truncated = truncated;
    return 0;
}

int clay_term_change_dir(const char *path) {
#ifdef _WIN32
    return _chdir(path);
#else
    return chdir(path);
#endif
}

int clay_term_width(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return info.srWindow.Right - info.srWindow.Left + 1;
    }
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
#endif
}

int clay_term_supports_color(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    if (!isatty(STDOUT_FILENO)) return 0;
    const char *term = getenv("TERM");
    return term != NULL && strcmp(term, "dumb") != 0;
#endif
}

int clay_term_is_interactive(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

static int g_color_enabled = 1;
static int g_color_initialized = 0;

static void ensure_color_initialized(void) {
    if (g_color_initialized) return;
    g_color_initialized = 1;
    if (getenv("NO_COLOR") != NULL) g_color_enabled = 0;
}

void clay_term_set_color_enabled(int enabled) {
    g_color_initialized = 1;
    g_color_enabled = enabled;
}

int clay_term_color_enabled(void) {
    ensure_color_initialized();
    return g_color_enabled;
}

const char *clay_color(const char *code) {
    return clay_term_color_enabled() ? code : "";
}

size_t clay_utf8_width(const char *s) {
    size_t width = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p == 0x1b) {
            p++;
            if (*p == '[') {
                p++;
                while (*p && (*p < 0x40 || *p > 0x7e)) p++;
                if (*p) p++;
            } else if (*p == ']') {
                p++;
                while (*p && *p != 0x07 && !(*p == 0x1b && *(p + 1) == '\\')) p++;
                if (*p == 0x07) p++;
                else if (*p == 0x1b) p += 2;
            } else if (*p) {
                p++;
            }
            continue;
        }
        if ((*p & 0xC0) != 0x80) width++;
        p++;
    }
    return width;
}

void clay_term_hyperlink(const char *url, const char *text) {
    if (clay_term_supports_color()) {
        printf("\x1b]8;;%s\x1b\\%s\x1b]8;;\x1b\\", url, text);
    } else {
        fputs(text, stdout);
    }
}

void clay_term_hyperlink_file(const char *path) {
    if (!clay_term_supports_color()) {
        fputs(path, stdout);
        return;
    }

    char resolved[CLAY_PATH_MAX];
    static char url[CLAY_PATH_MAX + 16];
    const char *link_target = path;

#ifdef _WIN32
    if (_fullpath(resolved, path, sizeof(resolved)) != NULL) {
        snprintf(url, sizeof(url), "file:///%s", resolved);
        for (char *p = url; *p; p++) {
            if (*p == '\\') *p = '/';
        }
        link_target = url;
    }
#else
    if (realpath(path, resolved) != NULL) {
        snprintf(url, sizeof(url), "file://%s", resolved);
        link_target = url;
    }
#endif

    printf("\x1b]8;;%s\x1b\\%s\x1b]8;;\x1b\\", link_target, path);
}

#ifndef _WIN32
static struct termios g_orig_termios;
static int g_raw_active = 0;
#endif

void clay_term_raw_enable(void) {
#ifndef _WIN32
    if (g_raw_active) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_active = 1;
#endif
    /* _getch() on Windows already reads unbuffered, unechoed input. */
}

void clay_term_raw_disable(void) {
#ifndef _WIN32
    if (!g_raw_active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_raw_active = 0;
#endif
}

int clay_term_take_interrupt(void) {
    if (!g_interrupted) return 0;
    g_interrupted = 0;
    return 1;
}

ClayKey clay_term_read_key(char *ch_out) {
    if (g_pending_escape) {
        g_pending_escape = 0;
        return CLAY_KEY_ESCAPE;
    }
#ifdef _WIN32
    int c = _getch();
    if (c == EOF) return CLAY_KEY_EOF;
    if (c == '\r' || c == '\n') return CLAY_KEY_ENTER;
    if (c == 8 || c == 127) return CLAY_KEY_BACKSPACE;
    if (c == 3) return CLAY_KEY_INTERRUPT;
    if (c == 27) return CLAY_KEY_ESCAPE;
    if (c == 0 || c == 0xE0) {
        int ext = _getch();
        switch (ext) {
            case 72: return CLAY_KEY_UP;
            case 80: return CLAY_KEY_DOWN;
            case 75: return CLAY_KEY_LEFT;
            case 77: return CLAY_KEY_RIGHT;
            default: return CLAY_KEY_ESCAPE;
        }
    }
    if (ch_out) *ch_out = (char)c;
    return CLAY_KEY_CHAR;
#else
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n < 0 && errno == EINTR && g_interrupted) return CLAY_KEY_INTERRUPT;
    if (n <= 0) return CLAY_KEY_EOF;
    if (c == '\r' || c == '\n') return CLAY_KEY_ENTER;
    if (c == 127 || c == 8) return CLAY_KEY_BACKSPACE;
    if (c == 3) return CLAY_KEY_INTERRUPT;

    if (c == 0x1b) {
        unsigned char seq[2];
        fd_set fds;
        struct timeval timeout = {0, 20000};
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) <= 0 || read(STDIN_FILENO, &seq[0], 1) <= 0) {
            return CLAY_KEY_ESCAPE;
        }
        if (seq[0] == 0x1b) {
            g_pending_escape = 1;
            return CLAY_KEY_ESCAPE;
        }
        timeout.tv_usec = 20000;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) <= 0 || read(STDIN_FILENO, &seq[1], 1) <= 0) {
            return CLAY_KEY_ESCAPE;
        }
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return CLAY_KEY_UP;
                case 'B': return CLAY_KEY_DOWN;
                case 'C': return CLAY_KEY_RIGHT;
                case 'D': return CLAY_KEY_LEFT;
            }
        }
        return CLAY_KEY_ESCAPE;
    }

    if (ch_out) *ch_out = (char)c;
    return CLAY_KEY_CHAR;
#endif
}

ClayKey clay_term_read_key_timeout(char *ch_out, int timeout_ms) {
    if (g_pending_escape) return clay_term_read_key(ch_out);
#ifdef _WIN32
    int elapsed = 0;
    while (!_kbhit()) {
        if (elapsed >= timeout_ms) return CLAY_KEY_NONE;
        Sleep(10);
        elapsed += 10;
    }
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout);
    if (ready == 0) return CLAY_KEY_NONE;
    if (ready < 0 && errno == EINTR && g_interrupted) return CLAY_KEY_INTERRUPT;
    if (ready < 0) return CLAY_KEY_NONE;
#endif
    return clay_term_read_key(ch_out);
}

int clay_term_take_escape(void) {
    if (!clay_term_input_pending()) return 0;
    ClayKey key = clay_term_read_key(NULL);
    if (key == CLAY_KEY_INTERRUPT) g_interrupted = 1;
    return key == CLAY_KEY_ESCAPE;
}

int clay_term_input_pending(void) {
#ifdef _WIN32
    return _kbhit() != 0;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

char *clay_term_home_dir(void) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    return home ? strdup(home) : NULL;
}

int clay_term_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST ? 0 : -1;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

int clay_term_list_dir(const char *path, ClayArray *names) {
    clay_array_init(names, sizeof(char *));
#ifdef _WIN32
    ClayStr pattern;
    clay_str_init(&pattern);
    clay_str_printf(&pattern, "%s\\*", path);
    WIN32_FIND_DATA data;
    HANDLE handle = FindFirstFile(pattern.data, &data);
    clay_str_free(&pattern);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(data.cFileName, ".") != 0 &&
            strcmp(data.cFileName, "..") != 0) {
            char *name = strdup(data.cFileName);
            clay_array_push_val(names, &name);
        }
    } while (FindNextFile(handle, &data));
    FindClose(handle);
#else
    DIR *directory = opendir(path);
    if (!directory) return -1;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        ClayStr child;
        clay_str_init(&child);
        clay_str_printf(&child, "%s/%s", path, entry->d_name);
        struct stat info;
        int is_dir = stat(child.data, &info) == 0 && S_ISDIR(info.st_mode);
        clay_str_free(&child);
        if (is_dir) {
            char *name = strdup(entry->d_name);
            clay_array_push_val(names, &name);
        }
    }
    closedir(directory);
#endif
    return 0;
}

long long clay_term_file_modified_at(const char *path) {
#ifdef _WIN32
    struct _stat info;
    return _stat(path, &info) == 0 ? (long long)info.st_mtime : 0;
#else
    struct stat info;
    return stat(path, &info) == 0 ? (long long)info.st_mtime : 0;
#endif
}

int clay_term_random_bytes(unsigned char *bytes, size_t count) {
    if (!bytes && count > 0) return -1;
    if (count == 0) return 0;
#ifdef _WIN32
    size_t index = 0;
    while (index < count) {
        unsigned int value;
        if (rand_s(&value) != 0) return -1;
        for (size_t offset = 0; offset < sizeof(value) && index < count; offset++, index++) {
            bytes[index] = (unsigned char)(value >> (offset * 8));
        }
    }
    return 0;
#else
    FILE *file = fopen("/dev/urandom", "rb");
    if (!file) return -1;
    size_t read_count = 0;
    while (read_count < count) {
        size_t read_now = fread(bytes + read_count, 1, count - read_count, file);
        if (read_now == 0) break;
        read_count += read_now;
    }
    int closed = fclose(file) == 0;
    return read_count == count && closed ? 0 : -1;
#endif
}

void clay_term_restrict_file(const char *path) {
#ifndef _WIN32
    chmod(path, 0600);
#else
    (void)path;
#endif
}
