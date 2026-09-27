#include "voxa.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

int cloexec(int fd) { return fcntl(fd, F_SETFD, FD_CLOEXEC); }
int make_pipe(int fd[2], bool asynchronous) {
    if (pipe(fd)) return -1;
    if (cloexec(fd[0]) || cloexec(fd[1]) ||
        (asynchronous && (nonblock(fd[0]) || nonblock(fd[1])))) {
        close(fd[0]); close(fd[1]); return -1;
    }
    return 0;
}
int local_socket(bool asynchronous) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && (cloexec(fd) || (asynchronous && nonblock(fd)))) { close(fd); return -1; }
    return fd;
}
int accept_local(int listener) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return -1;
    bool own = false;
#ifdef __APPLE__
    uid_t uid; gid_t gid;
    own = !getpeereid(fd, &uid, &gid) && uid == getuid();
#else
    struct ucred cred; socklen_t size = sizeof(cred);
    own = !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) && cred.uid == getuid();
#endif
    if (!own || cloexec(fd) || nonblock(fd)) { close(fd); return -1; }
    return fd;
}
void parent_guard(pid_t parent, int sig) {
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, sig);
#else
    (void)sig; // Worker also detects EOF on the parent's control pipe.
#endif
    if (getppid() != parent) _exit(1);
}
void secure_clear(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}
int runtime_path(char *out, size_t size, const char *name) {
    int n;
#ifdef __APPLE__
    const char *home = getenv("HOME");
    if (!home) return -1;
    n = snprintf(out, size, "%s/Library/Caches/voxa/%s", home, name);
#else
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && *dir) n = snprintf(out, size, "%s/%s", dir, name);
    else n = snprintf(out, size, "/run/user/%lu/%s", (unsigned long)getuid(), name);
#endif
    return n < 0 || (size_t)n >= size ? -1 : 0;
}
#ifdef __APPLE__
int app_connect(const char *name) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (runtime_path(addr.sun_path, sizeof(addr.sun_path), name)) return -1;
    int fd = local_socket(false);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) || nonblock(fd)) { close(fd); return -1; }
    return fd;
}
#endif
pid_t microphone_start(const char *device, int *output) {
#ifdef __APPLE__
    if (strcmp(device, "default")) return -1;
    *output = app_connect("mic.sock");
    return *output < 0 ? -1 : 0;
#else
    int pipefd[2];
    if (make_pipe(pipefd, false)) return -1;
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
        child_signals(); parent_guard(parent, SIGKILL);
        dup2(pipefd[1], STDOUT_FILENO);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, STDIN_FILENO); dup2(null, STDERR_FILENO); close(null); }
        close(pipefd[0]); close(pipefd[1]);
        if (!strcmp(device, "default"))
            execlp("pw-record", "pw-record", "--rate", "16000", "--channels", "1", "--format", "s16", "--raw", "--latency", "100ms", "-", (char *)NULL);
        else
            execlp("pw-record", "pw-record", "--rate", "16000", "--channels", "1", "--format", "s16", "--raw", "--latency", "100ms", "--target", device, "-", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); return -1; }
    *output = pipefd[0]; nonblock(*output);
    return pid;
#endif
}
void microphone_stop(pid_t pid, int fd) {
    if (pid > 0) kill(pid, SIGTERM);
    else if (fd >= 0) shutdown(fd, SHUT_WR);
}
void microphone_cleanup(pid_t pid, int fd) {
    if (fd >= 0) close(fd);
    if (pid > 0) { kill(pid, SIGKILL); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} }
}
int insert_text(const char *text) {
    if (transcript_blank(text)) return 0;
#ifdef __APPLE__
    char executable[4096]; uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size)) return -1;
    char resolved[4096];
    if (!realpath(executable, resolved)) return -1;
    char *slash = strrchr(resolved, '/');
    if (!slash || (size_t)(slash-resolved)+sizeof("/voxa-paste") > sizeof(resolved)) return -1;
    strcpy(slash, "/voxa-paste");
    char *args[] = {resolved, NULL};
#else
    char *args[] = {"wtype", "-", NULL};
#endif
    return run_command(args, text, 2000);
}
