#include "voxa.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int mkdirs(char *path) {
    for (char *p = path+1; *p; p++) if (*p == '/') {
        *p = 0;
        int result = mkdir(path, 0700), saved = errno;
        *p = '/';
        if (result && saved != EEXIST) return -1;
    }
    return 0;
}
static int save(const char *path, const char *data, mode_t mode) {
    char temp[4096];
    int n = snprintf(temp, sizeof(temp), "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(temp) || mkdirs(temp)) return -1;
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    size_t length = strlen(data), offset = 0;
    bool ok = !fchmod(fd, mode);
    while (ok && offset < length) {
        ssize_t count = write(fd, data+offset, length-offset);
        if (count > 0) offset += (size_t)count;
        else if (count < 0 && errno != EINTR) ok = false;
    }
    if (fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && !rename(temp, path)) return 0;
    unlink(temp); return -1;
}
#ifndef __APPLE__
static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *data = malloc(1024*1024+1);
    if (!data) { fclose(file); return NULL; }
    size_t n = fread(data, 1, 1024*1024, file);
    bool bad = ferror(file) || !feof(file) || memchr(data, 0, n);
    fclose(file);
    if (bad) { free(data); return NULL; }
    data[n] = 0; return data;
}
#endif
static bool available(const char *command) {
    const char *path = getenv("PATH");
    if (!path) return false;
    char *copy = strdup(path), *cursor = copy, *dir;
    if (!copy) return false;
    bool found = false;
    while ((dir = strsep(&cursor, ":"))) {
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", command);
        if (n > 0 && (size_t)n < sizeof(full) && !access(full, X_OK)) { found = true; break; }
    }
    free(copy); return found;
}
static int ask(const char *label, const char *current, bool secret, char *out, size_t size) {
    if (!isatty(STDIN_FILENO)) { fputs("Settings require an interactive terminal.\n", stderr); return -1; }
    struct termios old, hidden;
    printf("%s%s%s%s: ", label, *current ? " [" : "", current, *current ? "]" : ""); fflush(stdout);
    if (secret) {
        if (tcgetattr(STDIN_FILENO, &old)) return -1;
        hidden = old; hidden.c_lflag &= ~(ECHO | ECHONL);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden)) return -1;
    }
    // Poll rather than blocking forever: always restore echo after SIGINT/SIGTERM.
    size_t used = 0; bool complete = false, overflow = false;
    while (!quitting) {
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        char ch; ssize_t count = read(STDIN_FILENO, &ch, 1);
        if (count <= 0) break;
        if (ch == '\n') { complete = true; break; }
        if (used+1 < size) out[used++] = ch; else overflow = true;
    }
    if (secret) { tcsetattr(STDIN_FILENO, TCSANOW, &old); puts(""); }
    out[used] = 0;
    return complete && !overflow ? 0 : -1;
}
static int edit_key(void) {
    char input[4096] = "", path[4096], data[4200];
    int result = -1;
    if (config_path(path, sizeof(path), "env") ||
        ask("ElevenLabs keys (1–5 comma-separated; blank keeps current)", "", true, input, sizeof(input))) goto out;
    if (!*input) {
        Config c;
        result = config_load(&c, 0);
        if (!result) config_free(&c);
        goto out;
    }
    char normalized[4096] = "", *cursor = input, *key; unsigned count = 0;
    while ((key = strsep(&cursor, ","))) {
        while (isspace((unsigned char)*key)) key++;
        size_t n = strlen(key);
        while (n && isspace((unsigned char)key[n-1])) key[--n] = 0;
        if (!n || ++count > 5) goto out;
        for (size_t i = 0; i < n; i++) if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._~-", key[i])) goto out;
        if (count > 1) strcat(normalized, ",");
        strcat(normalized, key);
    }
    snprintf(data, sizeof(data), "ELEVENLABS_API_KEY=%s\n", normalized);
    result = save(path, data, 0600);
    secure_clear(normalized, sizeof(normalized));
    if (!result) puts("API key saved; applies to the next recording. Environment overrides take precedence.");
out:
    secure_clear(input, sizeof(input)); secure_clear(data, sizeof(data));
    return result;
}
static int edit_config(int section) {
    Config c;
    if (config_read(&c)) return -1;
    char input[4096], path[4096]; int result = -1;
    if (section == 2) {
        json_object *v = json_object_object_get(c.json, "language");
        if (ask("Primary language (2–3 lowercase letters, auto for detection)", v ? json_object_get_string(v) : "auto", false, input, sizeof(input))) goto out;
        if (*input) {
            if (strcmp(input, "auto")) {
                if (strlen(input) < 2 || strlen(input) > 3) goto out;
                for (char *p = input; *p; p++) if (*p < 'a' || *p > 'z') goto out;
            }
            json_object_object_add(c.json, "language", !strcmp(input, "auto") ? NULL : json_object_new_string(input));
        }
        if (ask("Secondary languages (comma-separated; blank = none)", "", false, input, sizeof(input))) goto out;
        json_object *array = json_object_new_array(); char *cursor = input, *part;
        while ((part = strsep(&cursor, ","))) {
            while (isspace((unsigned char)*part)) part++;
            size_t n = strlen(part); while (n && isspace((unsigned char)part[n-1])) part[--n] = 0;
            if (n) json_object_array_add(array, json_object_new_string(part));
        }
        json_object_object_add(c.json, "secondaryLanguages", array);
    } else if (section == 3) {
        if (ask("Microphone (PipeWire source; macOS: default only)", c.device, false, input, sizeof(input))) goto out;
#ifdef __APPLE__
        if (*input && strcmp(input, "default")) goto out;
#endif
        if (*input) json_object_object_add(c.json, "audioDevice", json_object_new_string(input));
    } else {
        if (ask("Keep final full stop? (yes/no)", c.punctuation ? "yes" : "no", false, input, sizeof(input))) goto out;
        if (*input && strcmp(input, "yes") && strcmp(input, "no")) goto out;
        if (*input) json_object_object_add(c.json, "stopPunctuation", json_object_new_boolean(!strcmp(input, "yes")));
    }
    if (!config_path(path, sizeof(path), "config.json")) result = save(path, json_object_to_json_string_ext(c.json, JSON_C_TO_STRING_PRETTY), 0600);
    if (!result) puts("Config saved; applies to the next recording.");
out:
    config_free(&c); return result;
}
#ifndef __APPLE__
static const char *begin = "-- BEGIN VOXA (managed by voxa settings)", *end = "-- END VOXA";
static int shortcut(char *input, char *out, size_t size) {
    char *cursor = input, *part; unsigned mods = 0; bool key = false; out[0] = 0;
    while ((part = strsep(&cursor, "+"))) {
        while (isspace((unsigned char)*part)) part++;
        size_t n = strlen(part); while (n && isspace((unsigned char)part[n-1])) part[--n] = 0;
        if (!n) return -1;
        for (size_t i = 0; i < n; i++) {
            if (!isalnum((unsigned char)part[i]) && part[i] != '_') return -1;
            if (strncmp(part, "XF86", 4)) part[i] = (char)toupper((unsigned char)part[i]);
        }
        unsigned bit = !strcmp(part, "SUPER") ? 1 : !strcmp(part, "CTRL") ? 2 : !strcmp(part, "ALT") ? 4 : !strcmp(part, "SHIFT") ? 8 : 0;
        if (cursor) { if (!bit || (mods & bit)) return -1; mods |= bit; }
        else { if (bit) return -1; key = true; }
        if (strlen(out)+n+4 >= size) return -1;
        strcat(out, part); if (cursor) strcat(out, " + ");
    }
    return key ? 0 : -1;
}
static int edit_bindings(void) {
    char path[4096], input[256], hold[256], toggle[256], cancel[256], backup[4200];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int n;
    if (xdg && *xdg) n = snprintf(path, sizeof(path), "%s/hypr/bindings.lua", xdg);
    else if (home) n = snprintf(path, sizeof(path), "%s/.config/hypr/bindings.lua", home);
    else return -1;
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    char *old = read_text(path), *outside = NULL, *next = NULL;
    int result = -1;
    if (!old) return -1;
    char *start = strstr(old, begin), *finish = strstr(old, end);
    if (!!start != !!finish || (start && (finish < start || strstr(start+strlen(begin), begin) || strstr(finish+strlen(end), end)))) goto out;
    outside = strdup(old); if (!outside) goto out;
    if (start) memmove(outside+(start-old), finish+strlen(end), strlen(finish+strlen(end))+1);
    if (ask("Hold shortcut (blank = F10)", "F10", false, input, sizeof(input))) goto out;
    if (!*input) strcpy(input, "F10");
    if (shortcut(input, hold, sizeof(hold))) goto out;
    if (ask("Toggle shortcut (blank = F11)", "F11", false, input, sizeof(input))) goto out;
    if (!*input) strcpy(input, "F11");
    if (shortcut(input, toggle, sizeof(toggle)) || !strcmp(hold, toggle)) goto out;
    if (ask("Cancel shortcut (blank = Escape)", "Escape", false, input, sizeof(input))) goto out;
    if (!*input) strcpy(input, "Escape");
    if (shortcut(input, cancel, sizeof(cancel)) || !strcmp(hold, cancel) || !strcmp(toggle, cancel)) goto out;
    // Conservatively reject duplicate/legacy/custom bindings; never rewrite user code.
    char *lines = strdup(outside), *cursor = lines, *line;
    if (!lines) goto out;
    bool conflict = false;
    while ((line = strsep(&cursor, "\n"))) {
        while (isspace((unsigned char)*line)) line++;
        if (!strncmp(line, "--", 2)) continue;
        if (strstr(line, "voxa start") || strstr(line, "voxa stop") || strstr(line, "voxa toggle") || strstr(line, "voxa cancel") ||
            (strstr(line, "o.bind") && (strstr(line, hold) || strstr(line, toggle) || strcasestr(line, cancel)))) conflict = true;
    }
    free(lines);
    if (conflict) { fputs("Conflicting bindings; remove duplicate/custom Voxa lines manually first.\n", stderr); goto out; }
    if (asprintf(&next, "%s\n%s\no.bind(\"%s\", \"Start Voxa (hold)\", \"~/.local/bin/voxa start\")\no.bind(\"%s\", \"Stop Voxa (release)\", \"~/.local/bin/voxa stop\", { release = true })\no.bind(\"%s\", \"Toggle Voxa\", \"~/.local/bin/voxa toggle\")\no.bind(\"%s\", \"Cancel Voxa\", \"~/.local/bin/voxa cancel\"%s)\n%s\n", outside, begin, hold, hold, toggle, cancel, !strcmp(cancel, "ESCAPE") ? ", { non_consuming = true }" : "", end) < 0) { next = NULL; goto out; }
    snprintf(backup, sizeof(backup), "%s.voxa.bak", path);
    struct stat st; if (stat(path, &st)) goto out;
    if (save(backup, old, st.st_mode & 0777) || save(path, next, st.st_mode & 0777)) goto out;
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE") && available("hyprctl")) {
        char *reload[] = {"hyprctl", "reload", NULL};
        // configerrors prints errors with exit status zero; require empty output too.
        char *check[] = {"sh", "-c", "errors=$(hyprctl configerrors) && test -z \"$errors\"", NULL};
        if (run_command(reload, NULL, 10000) || run_command(check, NULL, 10000)) {
            if (save(path, old, st.st_mode & 0777)) fputs("Restore failed; use bindings.lua.voxa.bak\n", stderr);
            run_command(reload, NULL, 10000); goto out;
        }
    }
    puts("Shortcuts saved (previous file: bindings.lua.voxa.bak)."); result = 0;
out:
    free(old); free(outside); free(next); return result;
}
#endif
static int doctor(void) {
    int failures = 0;
#ifdef __APPLE__
    const char *commands[] = {"osascript"};
#else
    const char *commands[] = {"pw-record", "wtype", "hyprctl", "systemctl"};
#endif
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        bool ok = available(commands[i]); printf("%s %s\n", ok ? "OK" : "FAIL", commands[i]); failures += !ok;
    }
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW); bool wss = false;
    for (const char *const *p = v->protocols; *p; p++) if (!strcmp(*p, "wss")) wss = true;
    printf("%s libcurl WSS support\n", wss ? "OK" : "FAIL"); failures += !wss;
    Config c; bool ok = !config_load(&c, 0);
    printf("%s configuration and API key\n", ok ? "OK" : "FAIL"); failures += !ok;
    if (ok) config_free(&c);
    char path[4096]; struct stat st;
    if (!config_path(path, sizeof(path), "env") && !stat(path, &st)) {
        ok = st.st_uid == getuid() && !(st.st_mode & 0077);
        printf("%s key file permissions (expected 600)\n", ok ? "OK" : "FAIL"); failures += !ok;
    }
#ifndef __APPLE__
    char *service[] = {"systemctl", "--user", "is-active", "--quiet", "voxa", NULL};
    ok = !run_command(service, NULL, 3000);
    printf("%s user service\n", ok ? "OK" : "FAIL"); failures += !ok;
#else
    puts("Verify microphone and Accessibility permissions with Voxa.app; test-mic uses the app's audio socket.");
#endif
    return failures ? 1 : 0;
}
static int settings(bool setup) {
    if (!isatty(STDIN_FILENO)) { fputs("Run voxa setup/settings in a terminal.\n", stderr); return 1; }
    if (setup) {
        if (edit_key() || edit_config(2) || edit_config(3) || edit_config(4)) return 1;
#ifndef __APPLE__
        if (edit_bindings()) return 1;
        char *enable[] = {"systemctl", "--user", "enable", "--now", "voxa", NULL};
        if (run_command(enable, NULL, 10000)) return 1;
#endif
        return doctor();
    }
    while (!quitting) {
        puts("\n1) API key  2) Language  3) Microphone  4) Final punctuation  5) Shortcuts  0) Done");
        char input[32];
        if (ask("Choose", "0", false, input, sizeof(input))) return 1;
        if (!*input || !strcmp(input, "0")) return 0;
        int result = -1;
        if (!strcmp(input, "1")) result = edit_key();
        else if (!strcmp(input, "2") || !strcmp(input, "3") || !strcmp(input, "4")) result = edit_config(atoi(input));
        else if (!strcmp(input, "5")) {
#ifdef __APPLE__
            puts("Command+Shift+R: hold; Command+Shift+U: toggle by default. Record shortcuts in the Voxa menu bar."); result = 0;
#else
            result = edit_bindings();
#endif
        }
        if (result) fputs("Invalid input or could not save settings.\n", stderr);
    }
    return 1;
}
static int test_mic(void) {
    Config c; if (config_read(&c)) return 1;
    int fd = -1; pid_t pid = microphone_start(c.device, &fd);
    config_free(&c);
    if (pid < 0) return 1;
    size_t bytes = 0; unsigned peak = 0; int low = -1; double deadline = now_ms()+2000;
    while (!quitting && now_ms() < deadline) {
        struct pollfd event = {fd, POLLIN, 0}; if (poll(&event, 1, 50) <= 0) continue;
        unsigned char data[8192]; ssize_t n = read(fd, data, sizeof(data));
        if (n <= 0) { if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue; break; }
        bytes += (size_t)n;
        for (ssize_t i = 0; i < n; i++) {
            if (low < 0) low = data[i];
            else { int value = (int)(int16_t)((unsigned)low | (unsigned)data[i]<<8); unsigned magnitude = (unsigned)(value < 0 ? -value : value); if (magnitude > peak) peak = magnitude; low = -1; }
        }
    }
    microphone_stop(pid, fd); microphone_cleanup(pid, fd);
    printf("Captured %zu bytes of 16k mono PCM in 2 seconds; peak %u\n", bytes, peak);
    return quitting || !bytes || !peak ? 1 : 0;
}
static int test_scribe(void) {
    Config c; if (config_load(&c, 0)) return 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) { config_free(&c); return 1; }
    int control[2]; if (make_pipe(control, false)) { config_free(&c); curl_global_cleanup(); return 1; }
    pid_t parent = getpid(), timer = fork();
    if (!timer) {
        parent_guard(parent, SIGKILL); close(control[0]);
        sleep(3); (void)write(control[1], "s", 1);
        // Keep pipe open while the final transcript is pending.
        sleep(20); close(control[1]); _exit(0);
    }
    close(control[1]);
    int result = timer < 0 ? 1 : session_run(control[0], &c, NULL, true);
    close(control[0]);
    if (timer > 0) { kill(timer, SIGKILL); while (waitpid(timer, NULL, 0) < 0 && errno == EINTR) {} }
    config_free(&c); curl_global_cleanup(); return result == 2 ? 0 : result;
}
int utilities(int argc, char **argv) {
    const char *cmd = argv[1];
    if (!strcmp(cmd, "settings")) {
        if (argc > 3 || (argc == 3 && strcmp(argv[2], "--terminal"))) return 2;
        return settings(false);
    }
    if (!strcmp(cmd, "test-paste")) {
        if (argc < 3) { fputs("Usage: voxa test-paste TEXT\n", stderr); return 2; }
        size_t size = 1; for (int i = 2; i < argc; i++) size += strlen(argv[i])+1;
        char *text = calloc(size, 1); if (!text) return 1;
        for (int i = 2; i < argc; i++) { if (i > 2) strcat(text, " "); strcat(text, argv[i]); }
        int result = insert_text(text); free(text); return result ? 1 : 0;
    }
    if (strcmp(cmd, "setup") && strcmp(cmd, "doctor") && strcmp(cmd, "test-mic") && strcmp(cmd, "test-scribe")) return -2;
    if (argc != 2) return 2;
    if (!strcmp(cmd, "setup")) return settings(true);
    if (!strcmp(cmd, "doctor")) return doctor();
    if (!strcmp(cmd, "test-mic")) return test_mic();
    return test_scribe();
}
