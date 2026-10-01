#include "voxa.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t quitting;
static void signal_stop(int sig) { (void)sig; quitting = 1; }
double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec*1000 + (double)ts.tv_nsec/1000000;
}
int nonblock(int fd) { int flags = fcntl(fd, F_GETFL); return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK); }
void child_signals(void) {
    signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
}
int run_command(char *const argv[], const char *input, unsigned timeout_ms) {
    int fds[2];
    if (make_pipe(fds, false)) return -1;
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
        child_signals(); parent_guard(parent, SIGKILL);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]); close(fds[1]);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); close(null); }
        execvp(argv[0], argv); _exit(127);
    }
    close(fds[0]);
    if (pid < 0) { close(fds[1]); return -1; }
    nonblock(fds[1]);
    size_t len = input ? strlen(input) : 0, sent = 0;
    double deadline = now_ms() + timeout_ms;
    int result = -1, status;
    while (!quitting && now_ms() < deadline) {
        if (fds[1] >= 0) {
            if (sent < len) {
                ssize_t n = write(fds[1], input+sent, len-sent);
                if (n > 0) sent += (size_t)n;
                else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
            }
            if (sent == len) { close(fds[1]); fds[1] = -1; }
        }
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) { result = WIFEXITED(status) && WEXITSTATUS(status) == 0 && sent == len ? 0 : -1; pid = -1; break; }
        if (done < 0 && errno != EINTR) break;
        struct pollfd fd = {fds[1], POLLOUT, 0}; poll(&fd, 1, 2);
    }
    if (fds[1] >= 0) close(fds[1]);
    if (pid > 0) { kill(pid, SIGKILL); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} }
    return result;
}
static int socket_address(const char *path, struct sockaddr_un *addr) {
    memset(addr, 0, sizeof(*addr)); addr->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr->sun_path)) return -1;
    strcpy(addr->sun_path, path); return 0;
}
static int client(const char *path, const char *command) {
    struct sockaddr_un addr;
    if (socket_address(path, &addr)) return 1;
    int fd = local_socket(true);
    if (fd < 0) return 1;
    int result = 1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) goto out;
    struct pollfd event = {fd, POLLOUT, 0};
    if (poll(&event, 1, 1500) <= 0) goto out;
    int error = 0; socklen_t size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) goto out;
    char request[32]; int n = snprintf(request, sizeof(request), "%s\n", command);
    if (send(fd, request, (size_t)n, MSG_NOSIGNAL) != n) goto out;
    shutdown(fd, SHUT_WR);
    double deadline = now_ms()+1500;
    char response[128]; size_t used = 0;
    while (now_ms() < deadline && used < sizeof(response)-1) {
        event.events = POLLIN;
        if (poll(&event, 1, (int)(deadline-now_ms())+1) <= 0) break;
        ssize_t count = read(fd, response+used, sizeof(response)-1-used);
        if (count > 0) { used += (size_t)count; if (memchr(response, '\n', used)) break; }
        else if (!count || (errno != EINTR && errno != EAGAIN)) break;
    }
    response[used] = 0;
    if (!strcmp(response, "idle\n") || !strcmp(response, "recording\n") || !strcmp(response, "committing\n")) {
        fputs(response, stdout); result = 0;
    }
out:
    close(fd);
    if (result) fprintf(stderr, "voxa-c: daemon unavailable or invalid response\n");
    return result;
}
#define CLIENTS 32
typedef struct { int fd; char data[64]; size_t used; double deadline; } Client;

static int private_directory(const char *path) {
    char *directory = strdup(path);
    if (!directory) return -1;
    char *slash = strrchr(directory, '/');
    if (!slash || slash == directory) { free(directory); return -1; }
    *slash = 0;
    struct stat st;
    int valid = !lstat(directory, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 0022);
    free(directory);
    return valid ? 0 : -1;
}
static int serve(const char *path, const char *test_endpoint, bool show_osd) {
    Osd osd = {.enabled = show_osd};
    struct sockaddr_un addr;
    if (socket_address(path, &addr) || private_directory(path)) {
        fprintf(stderr, "voxa-c: socket needs a short absolute path in an existing user-owned, non-writable-by-others directory\n"); return 1;
    }
    char lockpath[256];
    snprintf(lockpath, sizeof(lockpath), "%s.lock", path);
    int lock = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lockstat;
    if (lock < 0 || fstat(lock, &lockstat) || !S_ISREG(lockstat.st_mode) || lockstat.st_uid != getuid() || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0) close(lock);
        fprintf(stderr, "voxa-c: cannot lock socket (another daemon?)\n"); return 1;
    }
    int listener = -1, control = -1, result = 1;
    bool bound = false;
    pid_t worker = -1;
    const char *state = "idle";
    unsigned key_index = 0;
    Client clients[CLIENTS];
    for (size_t i = 0; i < CLIENTS; i++) clients[i].fd = -1;
    struct stat st;
    if (!lstat(path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) { fprintf(stderr, "voxa-c: refusing to replace non-owned socket or non-socket\n"); goto cleanup; }
        int probe = local_socket(true);
        if (probe < 0) goto cleanup;
        int connected = connect(probe, (struct sockaddr *)&addr, sizeof(addr)), saved = errno;
        close(probe);
        if (!connected || (saved != ECONNREFUSED && saved != ENOENT)) { fprintf(stderr, "voxa-c: socket is active or cannot safely be probed\n"); goto cleanup; }
        if (unlink(path)) goto cleanup;
    } else if (errno != ENOENT) goto cleanup;
    listener = local_socket(true);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr))) goto cleanup;
    bound = true;
    if (chmod(path, 0600) || listen(listener, 16)) goto cleanup;
    fprintf(stderr, "[voxa-c] ready (native daemon)\n");
    while (!quitting) {
        osd_tick(&osd);
        if (worker > 0) {
            int status;
            pid_t done = waitpid(worker, &status, WNOHANG);
            if (done == worker) {
                // No persistent helpers are needed: insertion does not own the clipboard.
                kill(-worker, SIGKILL);
                worker = -1; close(control); control = -1; state = "idle";
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                osd_show(&osd, code == 0 ? "done" : code == 2 ? "hide" : "error");
                fprintf(stderr, "[voxa-c] session %s\n", code == 0 || code == 2 ? "complete" : "failed");
            }
        }
        struct pollfd fds[CLIENTS+1];
        fds[0] = (struct pollfd){listener, POLLIN, 0};
        for (size_t i = 0; i < CLIENTS; i++) fds[i+1] = (struct pollfd){clients[i].fd, POLLIN, 0};
        if (poll(fds, CLIENTS+1, 20) < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (fds[0].revents & POLLIN) {
            int fd = accept_local(listener);
            if (fd >= 0) {
                bool accepted = false;
                for (size_t i = 0; i < CLIENTS; i++) if (clients[i].fd < 0) {
                    clients[i] = (Client){.fd = fd, .deadline = now_ms()+1000}; accepted = true; break;
                }
                if (!accepted) close(fd);
            }
        }
        for (size_t i = 0; i < CLIENTS; i++) {
            Client *c = &clients[i];
            if (c->fd < 0) continue;
            if (now_ms() > c->deadline) { close(c->fd); c->fd = -1; continue; }
            if (!(fds[i+1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(c->fd, c->data+c->used, sizeof(c->data)-1-c->used);
            if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; close(c->fd); c->fd = -1; continue; }
            c->used += (size_t)n; c->data[c->used] = 0;
            if (n && !memchr(c->data, '\n', c->used) && c->used < sizeof(c->data)-1) continue;
            // No embedded NULs or additional commands in one connection.
            bool valid = !memchr(c->data, 0, c->used);
            while (c->used && (c->data[c->used-1] == '\n' || c->data[c->used-1] == '\r')) c->data[--c->used] = 0;
            const char *reply = NULL;
            bool start = valid && (!strcmp(c->data, "start") || (!strcmp(c->data, "toggle") && worker < 0));
            bool stop = valid && (!strcmp(c->data, "stop") || (!strcmp(c->data, "toggle") && !strcmp(state, "recording")));
            if (start && worker < 0) {
                Config config;
                if (config_load(&config, key_index)) {
                    fprintf(stderr, "[voxa-c] invalid config or missing/invalid API key\n");
                    osd_show(&osd, "error");
                }
                else {
                    int channel[2];
                    if (!make_pipe(channel, true)) {
                        pid_t parent = getpid();
                        worker = fork();
                        if (worker == 0) {
                            setpgid(0, 0); parent_guard(parent, SIGTERM);
                            close(listener); close(lock); close(channel[1]);
                            for (size_t j = 0; j < CLIENTS; j++) if (clients[j].fd >= 0) close(clients[j].fd);
                            int code = session_run(channel[0], &config, test_endpoint, false);
                            close(channel[0]); config_free(&config); curl_global_cleanup(); _exit(code);
                        }
                        close(channel[0]);
                        if (worker > 0) {
                            setpgid(worker, worker); control = channel[1]; state = "recording"; key_index++;
                            osd_show(&osd, "recording");
                        } else { close(channel[1]); osd_show(&osd, "error"); }
                    } else osd_show(&osd, "error");
                    config_free(&config);
                }
            } else if (stop && worker > 0 && !strcmp(state, "recording")) {
                if (write(control, "s", 1) == 1) { state = "committing"; osd_show(&osd, "committing"); }
            } else if (valid && !strcmp(c->data, "cancel")) {
                if (worker > 0 && !strcmp(state, "recording")) write(control, "c", 1);
            } else if (!valid || (strcmp(c->data, "status") && strcmp(c->data, "start") && strcmp(c->data, "stop") && strcmp(c->data, "toggle"))) reply = "unknown command\n";
            char response[32];
            if (!reply) { snprintf(response, sizeof(response), "%s\n", state); reply = response; }
            send(c->fd, reply, strlen(reply), MSG_NOSIGNAL);
            close(c->fd); c->fd = -1;
        }
    }
    result = 0;
cleanup:
    if (worker > 0) {
        kill(-worker, SIGTERM);
        double deadline = now_ms()+2000;
        while (waitpid(worker, NULL, WNOHANG) == 0 && now_ms() < deadline) usleep(10000);
        kill(-worker, SIGKILL);
        while (waitpid(worker, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (control >= 0) close(control);
    for (size_t i = 0; i < CLIENTS; i++) if (clients[i].fd >= 0) close(clients[i].fd);
    if (listener >= 0) close(listener);
    if (bound) { unlink(path); osd_shutdown(&osd); }
    close(lock); // Keep lock file inode in place to avoid lock/unlink races.
    return result;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
    if (argc > 1) {
        int result = utilities(argc, argv);
        if (result != -2) return result;
    }
    const char *path = NULL, *command = NULL, *test_endpoint = NULL;
    bool show_osd = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i+1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--no-osd")) show_osd = false;
#ifdef VOXA_TESTING
        else if (!strcmp(argv[i], "--test-endpoint") && i+1 < argc) {
            test_endpoint = argv[++i];
            if (strncmp(test_endpoint, "ws://127.0.0.1:", 15)) return 2;
        }
#endif
        else if (!command) command = argv[i];
        else goto usage;
    }
    if (!command || (strcmp(command, "daemon") && strcmp(command, "status") && strcmp(command, "start") && strcmp(command, "stop") && strcmp(command, "toggle") && strcmp(command, "cancel") && strcmp(command, "test-osd"))) goto usage;
    if (!strcmp(command, "test-osd")) {
        signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
        Osd osd = {.enabled = show_osd};
        const char *states[] = {"recording", "committing", "done", "error"};
        for (size_t i = 0; i < 4 && !quitting; i++) {
            fprintf(stderr, "[voxa-c] OSD preview: %s (no microphone/API/insertion)\n", states[i]);
            osd_show(&osd, states[i]);
            double end = now_ms() + (i < 2 ? 1000 : 1800);
            while (!quitting && now_ms() < end) { osd_tick(&osd); usleep(10000); }
        }
        osd_shutdown(&osd);
        return 0;
    }
    char default_path[108];
    if (!path) {
        if (runtime_path(default_path, sizeof(default_path), "voxa.sock")) return 1;
        path = default_path;
    }
    signal(SIGPIPE, SIG_IGN);
    if (strcmp(command, "daemon")) return client(path, command);
    umask(0077);
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    bool websocket = false;
    for (const char *const *protocol = version->protocols; *protocol; protocol++) if (!strcmp(*protocol, "wss")) websocket = true;
    if (!websocket) { fprintf(stderr, "voxa-c: libcurl requires WSS support\n"); curl_global_cleanup(); return 1; }
    int result = serve(path, test_endpoint, show_osd);
    curl_global_cleanup(); return result;
usage:
    fprintf(stderr, "Usage: voxa [--socket PATH] [--no-osd] daemon|status|start|stop|toggle|cancel|test-osd\n"
        "       voxa setup|settings [--terminal]|doctor|test-mic|test-scribe|test-paste TEXT\n"); return 2;
}
