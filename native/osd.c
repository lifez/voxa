#include "voxa.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Only the control-server process owns this queue. Never wait in audio/typing paths.
void osd_tick(Osd *osd) {
    if (!osd->enabled) return;
    if (osd->pid > 0) {
        pid_t done = waitpid(osd->pid, NULL, WNOHANG);
        if (done == osd->pid || (done < 0 && errno == ECHILD)) osd->pid = 0;
        else if (now_ms() >= osd->deadline) {
            kill(-osd->pid, SIGKILL);
            // Reap on the next tick rather than blocking the server.
            return;
        } else return;
    }
    if (!osd->count) return;
    const char *state = osd->queue[0];
    memmove(osd->queue, osd->queue+1, (--osd->count)*sizeof(osd->queue[0]));
    pid_t parent = getpid(), pid = fork();
    if (!pid) {
        child_signals(); setpgid(0, 0); parent_guard(parent, SIGKILL);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO); dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO) close(null);
        }
#ifdef __APPLE__
        int fd = app_connect("osd.sock");
        if (fd >= 0) {
            char message[64]; int n = snprintf(message, sizeof(message), "%s\n", state);
            write(fd, message, (size_t)n); close(fd);
        }
        _exit(0);
#else
        if (!strcmp(state, "hide"))
            execlp("omarchy-shell", "omarchy-shell", "-q", "shell", "hide", "voxa.osd", (char *)NULL);
        else {
            char json[64]; snprintf(json, sizeof(json), "{\"state\":\"%s\"}", state);
            execlp("omarchy-shell", "omarchy-shell", "-q", "shell", "summon", "voxa.osd", json, (char *)NULL);
        }
        _exit(127); // Optional display backend: failures never fail a dictation.
#endif
    }
    if (pid > 0) {
        setpgid(pid, pid); osd->pid = pid; osd->deadline = now_ms()+1500;
    }
}
void osd_show(Osd *osd, const char *state) {
    if (!osd->enabled) return;
    // Commands use static state strings. Under repeated failures prefer current
    // state over replaying an unbounded backlog of stale recording indicators.
    if (osd->count == sizeof(osd->queue)/sizeof(osd->queue[0])) osd->count = 0;
    osd->queue[osd->count++] = state;
    osd_tick(osd);
}
void osd_shutdown(Osd *osd) {
    if (!osd->enabled) return;
    if (osd->pid > 0) {
        kill(-osd->pid, SIGKILL);
        while (waitpid(osd->pid, NULL, 0) < 0 && errno == EINTR) {}
        osd->pid = 0;
    }
    osd->count = 0;
    osd_show(osd, "hide");
    // Bounded shutdown only; no recording or IPC is waiting for this.
    while (osd->pid > 0 || osd->count) { osd_tick(osd); usleep(10000); }
}
