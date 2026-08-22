#include "clay/term.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#ifdef _WIN32
/* MinGW exposes the CRT's secure rand_s declaration behind this switch. */
#define _CRT_RAND_S
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <aclapi.h>
#include <shellapi.h>
#include <sys/stat.h>
#include <windows.h>
#define CLAY_PATH_MAX MAX_PATH
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define CLAY_PATH_MAX 4096
#endif

#define CLAY_TERM_SHELL_TIMEOUT_SECONDS 120

struct ClayTermHttpServer {
#ifdef _WIN32
  SOCKET listener;
  SOCKET peer;
#else
  int listener;
  int peer;
#endif
  unsigned short port;
};

static volatile sig_atomic_t g_interrupted = 0;
static int g_pending_escape = 0;
static int g_noninteractive = 0;

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
  if (GetConsoleMode(in, &mode))
    SetConsoleCP(CP_UTF8);
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
  if (n > 0)
    printf("\x1b[%dA", n);
}

void clay_term_cursor_down(int n) {
  if (n > 0)
    printf("\x1b[%dB", n);
}

void clay_term_cursor_col(int col) { printf("\x1b[%dG", col + 1); }

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

int clay_term_shell_exec(const char *command, ClayStr *output,
                         size_t output_limit, int *exit_code,
                         int *output_truncated) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE read_handle = NULL, write_handle = NULL;
  if (!CreatePipe(&read_handle, &write_handle, &security, 0) ||
      !SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
    if (read_handle) CloseHandle(read_handle);
    if (write_handle) CloseHandle(write_handle);
    return -1;
  }

  ClayStr invocation;
  clay_str_init(&invocation);
  clay_str_printf(&invocation, "cmd.exe /d /s /c \"%s\"", command);
  STARTUPINFOA startup = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_handle;
  startup.hStdError = write_handle;
  PROCESS_INFORMATION process = {0};
  BOOL started = CreateProcessA(NULL, invocation.data, NULL, NULL, TRUE, 0,
                                NULL, NULL, &startup, &process);
  clay_str_free(&invocation);
  CloseHandle(write_handle);
  if (!started) {
    CloseHandle(read_handle);
    return -1;
  }

  /* Killing only cmd.exe leaves grandchildren (and their inherited pipe
     handle) alive. A kill-on-close job gives this one-shot command the same
     process-group semantics as the POSIX implementation below. */
  HANDLE job = CreateJobObjectA(NULL, NULL);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
      CloseHandle(job);
      job = NULL;
    }
  }

  int truncated = 0;
  int timed_out = 0;
  ULONGLONG deadline = GetTickCount64() +
                       (ULONGLONG)CLAY_TERM_SHELL_TIMEOUT_SECONDS * 1000;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(read_handle, NULL, 0, NULL, &available, NULL)) break;
    while (available > 0) {
      char buffer[4096];
      DWORD requested = available < sizeof(buffer) ? available : sizeof(buffer);
      DWORD count = 0;
      if (!ReadFile(read_handle, buffer, requested, &count, NULL) || count == 0)
        break;
      for (DWORD i = 0; i < count; i++) {
        if (buffer[i] == '\0') buffer[i] = '?';
      }
      size_t kept = output->len < output_limit ? output_limit - output->len : 0;
      if ((size_t)count > kept) truncated = 1;
      if (kept > (size_t)count) kept = count;
      if (kept) clay_str_push_n(output, buffer, kept);
      available -= count;
    }

    DWORD state = WaitForSingleObject(process.hProcess, 50);
    if (state == WAIT_OBJECT_0) break;
    if (state == WAIT_FAILED) {
      if (job) TerminateJobObject(job, 1);
      else TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, INFINITE);
      CloseHandle(read_handle);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      if (job) CloseHandle(job);
      return -1;
    }
    if (GetTickCount64() >= deadline) {
      timed_out = 1;
      if (job) TerminateJobObject(job, 124);
      else TerminateProcess(process.hProcess, 124);
      WaitForSingleObject(process.hProcess, INFINITE);
      break;
    }
  }

  /* Drain output after process termination. The job has closed inherited
     writers, so PeekNamedPipe can no longer leave us blocked indefinitely. */
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(read_handle, NULL, 0, NULL, &available, NULL) || !available)
      break;
    char buffer[4096];
    DWORD requested = available < sizeof(buffer) ? available : sizeof(buffer);
    DWORD count = 0;
    if (!ReadFile(read_handle, buffer, requested, &count, NULL) || count == 0)
      break;
    for (DWORD i = 0; i < count; i++) if (buffer[i] == '\0') buffer[i] = '?';
    size_t kept = output->len < output_limit ? output_limit - output->len : 0;
    if ((size_t)count > kept) truncated = 1;
    if (kept > (size_t)count) kept = count;
    if (kept) clay_str_push_n(output, buffer, kept);
  }
  DWORD status = 1;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(read_handle);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (job) CloseHandle(job);
  if (timed_out) {
    static const char message[] = "\nclay: command timed out\n";
    size_t kept = output->len < output_limit ? output_limit - output->len : 0;
    if (kept > sizeof(message) - 1) kept = sizeof(message) - 1;
    if (kept < sizeof(message) - 1) truncated = 1;
    if (kept) clay_str_push_n(output, message, kept);
  }
  if (exit_code) *exit_code = timed_out ? 124 : (int)status;
  if (output_truncated) *output_truncated = truncated;
  return 0;
#else
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return -1;
  }
  if (pid == 0) {
    setpgid(0, 0);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  close(pipe_fds[1]);
  setpgid(pid, pid);
  int flags = fcntl(pipe_fds[0], F_GETFL);
  if (flags < 0 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    close(pipe_fds[0]);
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }

  int truncated = 0, timed_out = 0, child_done = 0, pipe_open = 1;
  int status = 0;
  struct timespec started;
  clock_gettime(CLOCK_MONOTONIC, &started);
  while (pipe_open || !child_done) {
    if (!child_done && waitpid(pid, &status, WNOHANG) == pid) child_done = 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed = (long long)(now.tv_sec - started.tv_sec) * 1000 +
                        (now.tv_nsec - started.tv_nsec) / 1000000;
    if (pipe_open && elapsed >= CLAY_TERM_SHELL_TIMEOUT_SECONDS * 1000) {
      kill(-pid, SIGKILL);
      timed_out = 1;
      if (!child_done && waitpid(pid, &status, 0) == pid) child_done = 1;
    }
    struct pollfd descriptor = {pipe_fds[0], POLLIN | POLLHUP, 0};
    int polled = pipe_open ? poll(&descriptor, 1, child_done ? 0 : 100) :
                             poll(NULL, 0, 100);
    if (polled < 0) {
      if (errno == EINTR) continue;
      close(pipe_fds[0]);
      if (!child_done) kill(-pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return -1;
    }
    if (pipe_open && (polled > 0 || child_done)) {
      for (;;) {
        char buffer[4096];
        ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
          for (ssize_t i = 0; i < count; i++) if (buffer[i] == '\0') buffer[i] = '?';
          size_t kept = output->len < output_limit ? output_limit - output->len : 0;
          if ((size_t)count > kept) truncated = 1;
          if (kept > (size_t)count) kept = count;
          if (kept) clay_str_push_n(output, buffer, kept);
          continue;
        }
        if (count == 0) {
          close(pipe_fds[0]);
          pipe_open = 0;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          close(pipe_fds[0]);
          pipe_open = 0;
        }
        break;
      }
    }
  }
  if (timed_out) {
    static const char message[] = "\nclay: command timed out\n";
    size_t kept = output->len < output_limit ? output_limit - output->len : 0;
    if (kept > sizeof(message) - 1) kept = sizeof(message) - 1;
    if (kept < sizeof(message) - 1) truncated = 1;
    if (kept) clay_str_push_n(output, message, kept);
  }
  if (exit_code) *exit_code = timed_out ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  if (output_truncated)
    *output_truncated = truncated;
  return 0;
#endif
}

void clay_term_shell_quote(ClayStr *out, const char *value) {
  clay_str_push_char(out, '\'');
  for (const char *p = value; *p; p++) {
    if (*p == '\'')
      clay_str_push(out, "'\\''");
    else
      clay_str_push_char(out, *p);
  }
  clay_str_push_char(out, '\'');
}

void clay_term_setenv(const char *name, const char *value) {
#ifdef _WIN32
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

#ifdef _WIN32
struct ClayProcess {
  HANDLE process_handle;
  HANDLE write_handle;
  HANDLE read_handle;
  ClayStr read_buffer; /* bytes read past the last line boundary returned */
};

ClayProcess *clay_term_process_start(const char *command, char *const argv[]) {
  (void)command;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE child_stdin_read, child_stdin_write;
  HANDLE child_stdout_read, child_stdout_write;
  if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0))
    return NULL;
  if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    return NULL;
  }
  if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    return NULL;
  }
  if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdin_write);
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdout_write);
    return NULL;
  }

  ClayStr cmdline;
  clay_str_init(&cmdline);
  for (int i = 0; argv[i]; i++) {
    if (i > 0)
      clay_str_push_char(&cmdline, ' ');
    clay_str_push_char(&cmdline, '"');
    for (const char *p = argv[i]; *p; p++) {
      if (*p == '"')
        clay_str_push(&cmdline, "\\\"");
      else
        clay_str_push_char(&cmdline, *p);
    }
    clay_str_push_char(&cmdline, '"');
  }

  STARTUPINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdInput = child_stdin_read;
  si.hStdOutput = child_stdout_write;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  BOOL started = CreateProcess(NULL, cmdline.data, NULL, NULL, TRUE, 0, NULL,
                               NULL, &si, &pi);
  clay_str_free(&cmdline);
  CloseHandle(child_stdin_read);
  CloseHandle(child_stdout_write);
  if (!started) {
    CloseHandle(child_stdin_write);
    CloseHandle(child_stdout_read);
    return NULL;
  }
  CloseHandle(pi.hThread);

  ClayProcess *process = malloc(sizeof(ClayProcess));
  process->process_handle = pi.hProcess;
  process->write_handle = child_stdin_write;
  process->read_handle = child_stdout_read;
  clay_str_init(&process->read_buffer);
  return process;
}

int clay_term_process_write(ClayProcess *process, const char *data,
                            size_t len) {
  size_t written = 0;
  while (written < len) {
    DWORD n = 0;
    if (!WriteFile(process->write_handle, data + written,
                   (DWORD)(len - written), &n, NULL))
      return -1;
    written += n;
  }
  return 0;
}

char *clay_term_process_read_line(ClayProcess *process) {
  for (;;) {
    for (size_t i = 0; i < process->read_buffer.len; i++) {
      if (process->read_buffer.data[i] != '\n')
        continue;
      size_t line_len = i;
      if (line_len > 0 && process->read_buffer.data[line_len - 1] == '\r')
        line_len--;
      char *line = malloc(line_len + 1);
      memcpy(line, process->read_buffer.data, line_len);
      line[line_len] = '\0';
      clay_str_remove_n(&process->read_buffer, 0, i + 1);
      return line;
    }
    char chunk[4096];
    DWORD n = 0;
    BOOL ok = ReadFile(process->read_handle, chunk, sizeof(chunk), &n, NULL);
    if (!ok || n == 0) {
      if (process->read_buffer.len == 0)
        return NULL;
      char *line = strdup(process->read_buffer.data);
      clay_str_clear(&process->read_buffer);
      return line;
    }
    clay_str_push_n(&process->read_buffer, chunk, n);
  }
}

void clay_term_process_stop(ClayProcess *process) {
  if (!process)
    return;
  CloseHandle(process->write_handle);
  TerminateProcess(process->process_handle, 0);
  WaitForSingleObject(process->process_handle, 2000);
  CloseHandle(process->read_handle);
  CloseHandle(process->process_handle);
  clay_str_free(&process->read_buffer);
  free(process);
}
#else
struct ClayProcess {
  pid_t pid;
  int write_fd;
  FILE *read_file;
};

ClayProcess *clay_term_process_start(const char *command, char *const argv[]) {
  int stdin_pipe[2];
  int stdout_pipe[2];
  if (pipe(stdin_pipe) != 0)
    return NULL;
  if (pipe(stdout_pipe) != 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    return NULL;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return NULL;
  }
  if (pid == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    execvp(command, argv);
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  ClayProcess *process = malloc(sizeof(ClayProcess));
  process->pid = pid;
  process->write_fd = stdin_pipe[1];
  process->read_file = fdopen(stdout_pipe[0], "r");
  if (!process->read_file) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    free(process);
    return NULL;
  }
  return process;
}

int clay_term_process_write(ClayProcess *process, const char *data,
                            size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = write(process->write_fd, data + written, len - written);
    if (n <= 0)
      return -1;
    written += (size_t)n;
  }
  return 0;
}

char *clay_term_process_read_line(ClayProcess *process) {
  char *line = NULL;
  size_t cap = 0;
  ssize_t n = getline(&line, &cap, process->read_file);
  if (n < 0) {
    free(line);
    return NULL;
  }
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
    line[--n] = '\0';
  return line;
}

void clay_term_process_stop(ClayProcess *process) {
  if (!process)
    return;
  close(process->write_fd);
  fclose(process->read_file);
  kill(process->pid, SIGTERM);
  int status = 0;
  waitpid(process->pid, &status, 0);
  free(process);
}
#endif

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
  if (!isatty(STDOUT_FILENO))
    return 0;
  const char *term = getenv("TERM");
  return term != NULL && strcmp(term, "dumb") != 0;
#endif
}

int clay_term_is_interactive(void) {
  if (g_noninteractive)
    return 0;
#ifdef _WIN32
  return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
  return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

void clay_term_set_noninteractive(int noninteractive) {
  g_noninteractive = noninteractive != 0;
}

static int g_color_enabled = 1;
static int g_color_initialized = 0;

static void ensure_color_initialized(void) {
  if (g_color_initialized)
    return;
  g_color_initialized = 1;
  if (getenv("NO_COLOR") != NULL)
    g_color_enabled = 0;
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
        while (*p && (*p < 0x40 || *p > 0x7e))
          p++;
        if (*p)
          p++;
      } else if (*p == ']') {
        p++;
        while (*p && *p != 0x07 && !(*p == 0x1b && *(p + 1) == '\\'))
          p++;
        if (*p == 0x07)
          p++;
        else if (*p == 0x1b)
          p += 2;
      } else if (*p) {
        p++;
      }
      continue;
    }
    if ((*p & 0xC0) != 0x80)
      width++;
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
      if (*p == '\\')
        *p = '/';
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
  if (g_raw_active)
    return;
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
  if (!g_raw_active)
    return;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
  g_raw_active = 0;
#endif
}

int clay_term_take_interrupt(void) {
  if (!g_interrupted)
    return 0;
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
  if (c == EOF)
    return CLAY_KEY_EOF;
  if (c == '\r' || c == '\n')
    return CLAY_KEY_ENTER;
  if (c == 8 || c == 127)
    return CLAY_KEY_BACKSPACE;
  if (c == 3)
    return CLAY_KEY_INTERRUPT;
  if (c == 0x0c)
    return CLAY_KEY_CLEAR_SCREEN;
  if (c == 0x12)
    return CLAY_KEY_HISTORY_SEARCH;
  if (c == 27)
    return CLAY_KEY_ESCAPE;
  if (c == 0 || c == 0xE0) {
    int ext = _getch();
    switch (ext) {
    case 72:
      return CLAY_KEY_UP;
    case 80:
      return CLAY_KEY_DOWN;
    case 75:
      return CLAY_KEY_LEFT;
    case 77:
      return CLAY_KEY_RIGHT;
    case 15:
      return CLAY_KEY_CYCLE_SANDBOX;
    default:
      return CLAY_KEY_ESCAPE;
    }
  }
  if (ch_out)
    *ch_out = (char)c;
  return CLAY_KEY_CHAR;
#else
  unsigned char c;
  ssize_t n = read(STDIN_FILENO, &c, 1);
  if (n < 0 && errno == EINTR && g_interrupted)
    return CLAY_KEY_INTERRUPT;
  if (n <= 0)
    return CLAY_KEY_EOF;
  if (c == '\r' || c == '\n')
    return CLAY_KEY_ENTER;
  if (c == 127 || c == 8)
    return CLAY_KEY_BACKSPACE;
  if (c == 3)
    return CLAY_KEY_INTERRUPT;
  if (c == 0x0c)
    return CLAY_KEY_CLEAR_SCREEN;
  if (c == 0x12)
    return CLAY_KEY_HISTORY_SEARCH;

  if (c == 0x1b) {
    unsigned char seq[16];
    size_t seq_len = 0;
    fd_set fds;
    struct timeval timeout = {0, 20000};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) <= 0 ||
        read(STDIN_FILENO, &seq[seq_len], 1) <= 0) {
      return CLAY_KEY_ESCAPE;
    }
    seq_len++;
    if (seq[0] == 0x1b) {
      g_pending_escape = 1;
      return CLAY_KEY_ESCAPE;
    }
    while (seq_len < sizeof(seq) - 1) {
      timeout.tv_usec = 20000;
      FD_ZERO(&fds);
      FD_SET(STDIN_FILENO, &fds);
      if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout) <= 0 ||
          read(STDIN_FILENO, &seq[seq_len], 1) <= 0)
        break;
      if ((seq[seq_len] >= 'A' && seq[seq_len] <= 'Z') ||
          (seq[seq_len] >= 'a' && seq[seq_len] <= 'z') || seq[seq_len] == '~') {
        seq_len++;
        break;
      }
      seq_len++;
    }
    seq[seq_len] = '\0';
    if (strcmp((char *)seq, "[A") == 0) return CLAY_KEY_UP;
    if (strcmp((char *)seq, "[B") == 0) return CLAY_KEY_DOWN;
    if (strcmp((char *)seq, "[C") == 0) return CLAY_KEY_RIGHT;
    if (strcmp((char *)seq, "[D") == 0) return CLAY_KEY_LEFT;
    /* Shift+Tab is standardized as CSI Z by xterm-compatible terminals. */
    if (strcmp((char *)seq, "[Z") == 0)
      return CLAY_KEY_CYCLE_SANDBOX;
    return CLAY_KEY_ESCAPE;
  }

  if (ch_out)
    *ch_out = (char)c;
  return CLAY_KEY_CHAR;
#endif
}

ClayKey clay_term_read_key_timeout(char *ch_out, int timeout_ms) {
  if (g_pending_escape)
    return clay_term_read_key(ch_out);
#ifdef _WIN32
  int elapsed = 0;
  while (!_kbhit()) {
    if (elapsed >= timeout_ms)
      return CLAY_KEY_NONE;
    Sleep(10);
    elapsed += 10;
  }
#else
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout);
  if (ready == 0)
    return CLAY_KEY_NONE;
  if (ready < 0 && errno == EINTR && g_interrupted)
    return CLAY_KEY_INTERRUPT;
  if (ready < 0)
    return CLAY_KEY_NONE;
#endif
  return clay_term_read_key(ch_out);
}

int clay_term_take_escape(void) {
  if (!clay_term_input_pending())
    return 0;
  ClayKey key = clay_term_read_key(NULL);
  if (key == CLAY_KEY_INTERRUPT)
    g_interrupted = 1;
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

char *clay_term_platform_name(void) {
#ifdef _WIN32
  return strdup("Windows");
#elif defined(__APPLE__)
  return strdup("macOS");
#else
  /* PRETTY_NAME on a distro that ships /etc/os-release, else a bare "Linux". */
  FILE *file = fopen("/etc/os-release", "r");
  if (!file)
    return strdup("Linux");
  ClayStr text;
  clay_str_init(&text);
  int ch;
  while ((ch = fgetc(file)) != EOF)
    clay_str_push_char(&text, (char)ch);
  fclose(file);
  const char *marker = "PRETTY_NAME=";
  const char *line = strstr(text.data, marker);
  if (!line || (line != text.data && line[-1] != '\n')) {
    clay_str_free(&text);
    return strdup("Linux");
  }
  const char *value = line + strlen(marker);
  const char *end = strchr(value, '\n');
  size_t len = end ? (size_t)(end - value) : strlen(value);
  if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
    value++;
    len -= 2;
  }
  char *name = malloc(len + 1);
  memcpy(name, value, len);
  name[len] = '\0';
  clay_str_free(&text);
  return name;
#endif
}

char *clay_term_cwd(void) {
#ifdef _WIN32
  return _getcwd(NULL, 0);
#else
  return getcwd(NULL, 0);
#endif
}

int clay_term_mkdir(const char *path) {
#ifdef _WIN32
  if (_mkdir(path) != 0 && errno != EEXIST) return -1;
  clay_term_restrict_file(path);
  return 0;
#else
  return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

int clay_term_write_file_atomic(const char *path, const void *data, size_t len) {
  if (!path || (!data && len > 0)) return -1;
#ifdef _WIN32
  char directory[MAX_PATH];
  strncpy(directory, path, sizeof(directory) - 1);
  directory[sizeof(directory) - 1] = '\0';
  char *slash = strrchr(directory, '/');
  char *backslash = strrchr(directory, '\\');
  if (backslash && (!slash || backslash > slash)) slash = backslash;
  if (slash) {
    *slash = '\0';
  } else {
    strcpy(directory, ".");
  }
  char temporary[MAX_PATH];
  if (GetTempFileNameA(directory, "clay", 0, temporary) == 0) return -1;
  FILE *file = fopen(temporary, "wb");
  if (!file) {
    DeleteFileA(temporary);
    return -1;
  }
  int ok = fwrite(data, 1, len, file) == len;
  if (ok && fflush(file) != 0) ok = 0;
  if (ok && _commit(_fileno(file)) != 0) ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok) {
    DeleteFileA(temporary);
    return -1;
  }
  if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(temporary);
    return -1;
  }
  clay_term_restrict_file(path);
  return 0;
#else
  ClayStr temporary;
  clay_str_init(&temporary);
  clay_str_printf(&temporary, "%s.tmp.XXXXXX", path);
  int descriptor = mkstemp(temporary.data);
  if (descriptor < 0) {
    clay_str_free(&temporary);
    return -1;
  }
  FILE *file = fdopen(descriptor, "wb");
  if (!file) {
    close(descriptor);
    remove(temporary.data);
    clay_str_free(&temporary);
    return -1;
  }
  int ok = fwrite(data, 1, len, file) == len;
  if (ok && fflush(file) != 0) ok = 0;
  if (ok && fsync(fileno(file)) != 0) ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok) {
    remove(temporary.data);
    clay_str_free(&temporary);
    return -1;
  }
  if (rename(temporary.data, path) != 0) {
    remove(temporary.data);
    clay_str_free(&temporary);
    return -1;
  }
  clay_term_restrict_file(path);
  clay_str_free(&temporary);
  return 0;
#endif
}

int clay_term_read_file(const char *path, size_t max_bytes, ClayStr *out) {
  if (!path || !out) return -1;
  FILE *file = fopen(path, "rb");
  if (!file) return -1;
  clay_str_init(out);
  char buffer[4096];
  for (;;) {
    size_t count = fread(buffer, 1, sizeof(buffer), file);
    if (count > 0) {
      if (max_bytes > 0 && (out->len > max_bytes || count > max_bytes - out->len)) {
        fclose(file);
        clay_str_free(out);
        errno = EFBIG;
        return -1;
      }
      clay_str_push_n(out, buffer, count);
    }
    if (count < sizeof(buffer)) {
      if (ferror(file)) {
        fclose(file);
        clay_str_free(out);
        errno = EIO;
        return -1;
      }
      break;
    }
  }
  int ok = fclose(file) == 0;
  if (!ok) {
    clay_str_free(out);
    return -1;
  }
  return 0;
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
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0) {
      char *name = strdup(data.cFileName);
      clay_array_push_val(names, &name);
    }
  } while (FindNextFile(handle, &data));
  FindClose(handle);
#else
  DIR *directory = opendir(path);
  if (!directory)
    return -1;
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
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

int clay_term_list_entries(const char *path, ClayArray *names) {
  clay_array_init(names, sizeof(char *));
#ifdef _WIN32
  ClayStr pattern;
  clay_str_init(&pattern);
  clay_str_printf(&pattern, "%s\\*", path);
  WIN32_FIND_DATA data;
  HANDLE handle = FindFirstFile(pattern.data, &data);
  clay_str_free(&pattern);
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  do {
    if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0)
      continue;
    char *name = strdup(data.cFileName);
    clay_array_push_val(names, &name);
  } while (FindNextFile(handle, &data));
  FindClose(handle);
#else
  DIR *directory = opendir(path);
  if (!directory)
    return -1;
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char *name = strdup(entry->d_name);
    clay_array_push_val(names, &name);
  }
  closedir(directory);
#endif
  return 0;
}

int clay_term_is_dir(const char *path) {
#ifdef _WIN32
  struct _stat info;
  return _stat(path, &info) == 0 && (info.st_mode & _S_IFDIR) != 0;
#else
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
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
  if (!bytes && count > 0)
    return -1;
  if (count == 0)
    return 0;
#ifdef _WIN32
  size_t index = 0;
  while (index < count) {
    unsigned int value;
    if (rand_s(&value) != 0)
      return -1;
    for (size_t offset = 0; offset < sizeof(value) && index < count;
         offset++, index++) {
      bytes[index] = (unsigned char)(value >> (offset * 8));
    }
  }
  return 0;
#else
  FILE *file = fopen("/dev/urandom", "rb");
  if (!file)
    return -1;
  size_t read_count = 0;
  while (read_count < count) {
    size_t read_now = fread(bytes + read_count, 1, count - read_count, file);
    if (read_now == 0)
      break;
    read_count += read_now;
  }
  int closed = fclose(file) == 0;
  return read_count == count && closed ? 0 : -1;
#endif
}

int clay_term_open_browser(const char *url) {
  if (!url || !*url)
    return -1;
#ifdef _WIN32
  return (intptr_t)ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL) >
                 32
             ? 0
             : -1;
#else
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execlp("xdg-open", "xdg-open", url, (char *)NULL);
    _exit(127);
  }
  return 0;
#endif
}

ClayTermHttpServer *clay_term_http_server_create(unsigned short port) {
#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return NULL;
#endif
  ClayTermHttpServer *server = calloc(1, sizeof(*server));
  if (!server)
    goto failed;
#ifdef _WIN32
  server->listener = INVALID_SOCKET;
  server->peer = INVALID_SOCKET;
#else
  server->listener = -1;
  server->peer = -1;
#endif
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  if (server->listener == INVALID_SOCKET)
    goto failed;
#else
  if (server->listener < 0)
    goto failed;
#endif
  int yes = 1;
  setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
             sizeof(yes));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (bind(server->listener, (struct sockaddr *)&address, sizeof(address)) !=
          0 ||
      listen(server->listener, 1) != 0)
    goto failed;
#ifdef _WIN32
  int address_len = sizeof(address);
#else
  socklen_t address_len = sizeof(address);
#endif
  if (getsockname(server->listener, (struct sockaddr *)&address,
                  &address_len) != 0)
    goto failed;
  server->port = ntohs(address.sin_port);
  return server;
failed:
  clay_term_http_server_destroy(server);
  return NULL;
}

unsigned short clay_term_http_server_port(const ClayTermHttpServer *server) {
  return server ? server->port : 0;
}

int clay_term_http_server_receive(ClayTermHttpServer *server, ClayStr *request,
                                  int timeout_ms) {
  if (!server || !request)
    return -1;
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(server->listener, &ready);
  struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int rc = select((int)server->listener + 1, &ready, NULL, NULL, &timeout);
  if (rc == 0)
    return 0;
  if (rc < 0)
    return -1;
  /* A preflight can arrive before the real callback. */
#ifdef _WIN32
  if (server->peer != INVALID_SOCKET) {
    closesocket(server->peer);
    server->peer = INVALID_SOCKET;
  }
#else
  if (server->peer >= 0) {
    close(server->peer);
    server->peer = -1;
  }
#endif
  server->peer = accept(server->listener, NULL, NULL);
#ifdef _WIN32
  if (server->peer == INVALID_SOCKET)
    return -1;
#else
  if (server->peer < 0)
    return -1;
#endif
  clay_str_clear(request);
  char chunk[1024];
  for (;;) {
    int received = recv(server->peer, chunk, sizeof(chunk), 0);
    if (received <= 0)
      return -1;
    clay_str_push_n(request, chunk, (size_t)received);
    if (strstr(request->data, "\r\n\r\n") ||
        strstr(request->data, "\n\n"))
      return 1;
    if (request->len > 16384)
      return -1;
  }
}

int clay_term_http_server_reply(ClayTermHttpServer *server, int status,
                                const char *body) {
  return clay_term_http_server_reply_with_headers(server, status, body, NULL);
}

int clay_term_http_server_reply_with_headers(ClayTermHttpServer *server,
                                             int status, const char *body,
                                             const char *extra_headers) {
  if (!server || !body)
    return -1;
#ifdef _WIN32
  if (server->peer == INVALID_SOCKET)
    return -1;
#else
  if (server->peer < 0)
    return -1;
#endif
  ClayStr response;
  clay_str_init(&response);
  clay_str_printf(
      &response,
      "HTTP/1.1 %d %s\r\nContent-Type: text/html; "
      "charset=utf-8\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n%s",
      status, status == 200 ? "OK" : (status == 204 ? "No Content" : "Bad Request"),
      strlen(body), extra_headers ? extra_headers : "", body);
  size_t sent = 0;
  while (sent < response.len) {
    int count =
        send(server->peer, response.data + sent, (int)(response.len - sent), 0);
    if (count <= 0) {
      clay_str_free(&response);
      return -1;
    }
    sent += (size_t)count;
  }
  clay_str_free(&response);
  return 0;
}

void clay_term_http_server_destroy(ClayTermHttpServer *server) {
  if (!server)
    return;
#ifdef _WIN32
  if (server->peer != INVALID_SOCKET)
    closesocket(server->peer);
  if (server->listener != INVALID_SOCKET)
    closesocket(server->listener);
  WSACleanup();
#else
  if (server->peer >= 0)
    close(server->peer);
  if (server->listener >= 0)
    close(server->listener);
#endif
  free(server);
}

void clay_term_restrict_file(const char *path) {
#ifndef _WIN32
  chmod(path, 0600);
#else
  HANDLE token = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
  DWORD token_size = 0;
  GetTokenInformation(token, TokenUser, NULL, 0, &token_size);
  if (!token_size) {
    CloseHandle(token);
    return;
  }
  PTOKEN_USER user = (PTOKEN_USER)malloc(token_size);
  PACL acl = NULL;
  if (!user || !GetTokenInformation(token, TokenUser, user, token_size, &token_size)) {
    free(user);
    CloseHandle(token);
    return;
  }
  EXPLICIT_ACCESSA access = {0};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  BuildTrusteeWithSidA(&access.Trustee, user->User.Sid);
  DWORD result = SetEntriesInAclA(1, &access, NULL, &acl);
  if (result == ERROR_SUCCESS) {
    SetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                          NULL, NULL, acl, NULL);
  }
  if (acl) LocalFree(acl);
  free(user);
  CloseHandle(token);
#endif
}
