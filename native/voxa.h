#ifndef VOXA_H
#define VOXA_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <sys/types.h>
#include <json-c/json.h>
#include <curl/curl.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
extern volatile sig_atomic_t quitting;
int cloexec(int fd);
int make_pipe(int fd[2], bool asynchronous);
int local_socket(bool asynchronous);
int accept_local(int listener);
void parent_guard(pid_t parent, int sig);
void secure_clear(void *data, size_t size);
int runtime_path(char *out, size_t size, const char *name);
#ifdef __APPLE__
int app_connect(const char *name);
#endif
pid_t microphone_start(const char *device, int *output);
void microphone_stop(pid_t pid, int fd);
void microphone_cleanup(pid_t pid, int fd);
int insert_text(const char *text);
#ifdef __linux__
int keyboard_insert(const char *text);
#endif
int utilities(int argc, char **argv);
int config_path(char *out, size_t size, const char *file);
double now_ms(void);
int nonblock(int fd);
void child_signals(void);
int run_command(char *const argv[], const char *input, unsigned timeout_ms);

typedef struct {
    json_object *json;
    const char *device;
    bool punctuation;
    char key[4096];
} Config;
int config_load(Config *c, unsigned key_index);
int config_read(Config *c);
void config_free(Config *c);
char *scribe_url(CURL *curl, Config *c);
void format_transcript(char *text, bool punctuation);
bool transcript_blank(const char *text);
// Worker exit codes: 0=inserted, 1=failed, 2=empty/cancelled (successful, no insertion).
int session_run(int control, Config *config, const char *test_endpoint, bool print_only);
typedef struct {
    bool enabled;
    pid_t pid;
    double deadline;
    const char *queue[16];
    size_t count;
} Osd;
void osd_tick(Osd *osd);
void osd_show(Osd *osd, const char *state);
void osd_shutdown(Osd *osd);
#endif
