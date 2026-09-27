#ifndef VOXA_H
#define VOXA_H
#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <sys/types.h>
#include <json-c/json.h>
#include <curl/curl.h>

extern volatile sig_atomic_t quitting;
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
void config_free(Config *c);
char *scribe_url(CURL *curl, Config *c);
void format_transcript(char *text, bool punctuation);
bool transcript_blank(const char *text);
// Worker exit codes: 0=inserted, 1=failed, 2=empty (successful, no insertion).
int session_run(int control, Config *config, const char *test_endpoint);
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
