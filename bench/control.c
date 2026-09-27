// Experimental POSIX control client, not installed by default.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static void timeout_handler(int sig) {
    (void)sig;
    const char msg[] = "voxa: daemon timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "status") && strcmp(argv[1], "start") &&
        strcmp(argv[1], "stop") && strcmp(argv[1], "toggle"))) {
        fprintf(stderr, "Usage: voxa-control status|start|stop|toggle\n"); return 2;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    int n;
#ifdef __APPLE__
    const char *home = getenv("HOME");
    if (!home) return 1;
    n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/Library/Caches/voxa/voxa.sock", home);
#else
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir) n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/voxa.sock", dir);
    else n = snprintf(addr.sun_path, sizeof(addr.sun_path), "/run/user/%lu/voxa.sock", (unsigned long)getuid());
#endif
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) { fprintf(stderr, "voxa: socket path too long\n"); return 1; }
    signal(SIGALRM, timeout_handler);
    signal(SIGPIPE, SIG_IGN);
    alarm(2);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("voxa: connect"); return 1; }
    char request[16];
    n = snprintf(request, sizeof(request), "%s\n", argv[1]);
    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t count = write(fd, request + sent, (size_t)n - sent);
        if (count <= 0) { perror("voxa: write"); close(fd); return 1; }
        sent += (size_t)count;
    }
    shutdown(fd, SHUT_WR);
    char response[256];
    ssize_t count;
    int received = 0;
    while ((count = read(fd, response, sizeof(response))) > 0) {
        received = 1;
        if (fwrite(response, 1, (size_t)count, stdout) != (size_t)count) { close(fd); return 1; }
    }
    close(fd);
    if (count < 0) { perror("voxa: read"); return 1; }
    return received ? 0 : 1;
}
