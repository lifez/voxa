#include "voxa.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int config_path(char *out, size_t size, const char *file) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int n;
    if (xdg && *xdg) n = snprintf(out, size, "%s/voxa/%s", xdg, file);
    else if (home) n = snprintf(out, size, "%s/.config/voxa/%s", home, file);
    else return -1;
    return n < 0 || (size_t)n >= size ? -1 : 0;
}
static bool string_ok(json_object *v) {
    return json_object_is_type(v, json_type_string) &&
        strlen(json_object_get_string(v)) == (size_t)json_object_get_string_len(v);
}
static int load_key(Config *c, unsigned index) {
    char input[4096] = "", path[4096];
    const char *env = getenv("ELEVENLABS_API_KEY");
    if (env && *env) {
        if (strlen(env) >= sizeof(input)) return -1;
        strcpy(input, env);
    } else {
        if (config_path(path, sizeof(path), "env")) return -1;
        FILE *f = fopen(path, "r");
        if (!f) return -1;
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            const char *prefix = "ELEVENLABS_API_KEY=";
            if (strncmp(line, prefix, strlen(prefix))) continue;
            char *value = line + strlen(prefix);
            value[strcspn(value, "\r\n")] = 0;
            size_t n = strlen(value);
            if (n >= 2 && (value[0] == '\'' || value[0] == '"') && value[n-1] == value[0]) {
                value[n-1] = 0; value++; n -= 2;
            }
            if (n >= sizeof(input)) { fclose(f); return -1; }
            strcpy(input, value); break;
        }
        fclose(f);
    }
    char *keys[5], *cursor = input, *part;
    unsigned count = 0;
    while ((part = strsep(&cursor, ","))) {
        while (isspace((unsigned char)*part)) part++;
        size_t n = strlen(part);
        while (n && isspace((unsigned char)part[n-1])) part[--n] = 0;
        if (!n || count == 5) return -1;
        for (size_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)part[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || strchr("._~-", ch))) return -1;
        }
        keys[count++] = part;
    }
    if (!count) return -1;
    strcpy(c->key, keys[index % count]);
    return 0;
}
int config_load(Config *c, unsigned key_index) {
    if (config_read(c)) return -1;
    if (load_key(c, key_index)) { config_free(c); return -1; }
    return 0;
}
int config_read(Config *c) {
    memset(c, 0, sizeof(*c));
    char path[4096];
    if (config_path(path, sizeof(path), "config.json")) return -1;
    FILE *f = fopen(path, "rb");
    json_object *user = NULL;
    if (f) {
        char data[65537];
        size_t n = fread(data, 1, sizeof(data)-1, f);
        bool bad = ferror(f) || !feof(f);
        fclose(f);
        if (bad) return -1;
        data[n] = 0;
        struct json_tokener *tok = json_tokener_new();
        if (!tok) return -1;
        json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        user = json_tokener_parse_ex(tok, data, (int)n + 1);
        bool parsed = json_tokener_get_error(tok) == json_tokener_success;
        size_t end = json_tokener_get_parse_end(tok);
        while (end < n && isspace((unsigned char)data[end])) end++;
        json_tokener_free(tok);
        if (!parsed || end < n || !json_object_is_type(user, json_type_object)) {
            if (user) json_object_put(user);
            return -1;
        }
    } else if (errno != ENOENT) return -1;
    c->json = json_tokener_parse("{\"language\":\"th\",\"secondaryLanguages\":[\"en\"],\"audioDevice\":\"default\",\"keyterms\":[],\"debug\":false,\"pasteCommand\":\"wtype\",\"stopPunctuation\":false}");
    if (!c->json) { if (user) json_object_put(user); return -1; }
    if (user) {
        json_object_object_foreach(user, name, value) {
            json_object *old;
            if (!json_object_object_get_ex(c->json, name, &old)) { json_object_put(user); goto invalid; }
            json_object_object_add(c->json, name, json_object_get(value));
        }
        json_object_put(user);
    }
    json_object *v = json_object_object_get(c->json, "language");
    if (v) {
        if (!string_ok(v)) goto invalid;
        const char *s = json_object_get_string(v);
        size_t n = strlen(s);
        if (n < 2 || n > 3) goto invalid;
        for (size_t i = 0; i < n; i++) if (s[i] < 'a' || s[i] > 'z') goto invalid;
    }
    const char *arrays[] = {"secondaryLanguages", "keyterms"};
    for (size_t i = 0; i < 2; i++) {
        v = json_object_object_get(c->json, arrays[i]);
        if (!json_object_is_type(v, json_type_array)) goto invalid;
        for (size_t j = 0; j < json_object_array_length(v); j++)
            if (!string_ok(json_object_array_get_idx(v, j))) goto invalid;
    }
    v = json_object_object_get(c->json, "audioDevice");
    if (!string_ok(v) || !*json_object_get_string(v)) goto invalid;
    c->device = json_object_get_string(v);
    if (!json_object_is_type(json_object_object_get(c->json, "debug"), json_type_boolean) ||
        !json_object_is_type(json_object_object_get(c->json, "stopPunctuation"), json_type_boolean)) goto invalid;
    v = json_object_object_get(c->json, "pasteCommand");
    if (!string_ok(v) || strcmp(json_object_get_string(v), "wtype")) goto invalid;
    c->punctuation = json_object_get_boolean(json_object_object_get(c->json, "stopPunctuation"));
    return 0;
invalid:
    config_free(c);
    return -1;
}
void config_free(Config *c) {
    if (c->json) json_object_put(c->json);
    secure_clear(c->key, sizeof(c->key));
    memset(c, 0, sizeof(*c));
}
static int query_add(CURL *curl, char **url, const char *name, const char *value) {
    char *escaped = curl_easy_escape(curl, value, 0), *next = NULL;
    if (!escaped) return -1;
    int n = asprintf(&next, "%s&%s=%s", *url, name, escaped);
    curl_free(escaped);
    if (n < 0) return -1;
    free(*url); *url = next;
    return 0;
}
char *scribe_url(CURL *curl, Config *c) {
    char *url = strdup("wss://api.elevenlabs.io/v1/speech-to-text/realtime?model_id=scribe_v2_realtime&audio_format=pcm_16000&commit_strategy=manual");
    if (!url) return NULL;
    json_object *language = json_object_object_get(c->json, "language");
    if (language && query_add(curl, &url, "language_code", json_object_get_string(language))) goto fail;
    const char *fields[] = {"secondaryLanguages", "keyterms"}, *params[] = {"secondary_languages", "keyterms"};
    for (size_t i = 0; i < 2; i++) {
        json_object *array = json_object_object_get(c->json, fields[i]);
        for (size_t j = 0; j < json_object_array_length(array); j++)
            if (query_add(curl, &url, params[i], json_object_get_string(json_object_array_get_idx(array, j)))) goto fail;
    }
    return url;
fail:
    free(url); return NULL;
}
// ECMAScript whitespace/line terminators used by String.trim and /\s/.
static size_t whitespace(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c == ' ' || (c >= 9 && c <= 13)) return 1;
    static const char *const unicode[] = {
        "\u00a0", "\u1680", "\u2000", "\u2001", "\u2002", "\u2003", "\u2004", "\u2005", "\u2006", "\u2007", "\u2008", "\u2009", "\u200a",
        "\u2028", "\u2029", "\u202f", "\u205f", "\u3000", "\ufeff"
    };
    for (size_t i = 0; i < sizeof(unicode)/sizeof(unicode[0]); i++) {
        size_t n = strlen(unicode[i]);
        if (!strncmp(s, unicode[i], n)) return n;
    }
    return 0;
}
bool transcript_blank(const char *text) {
    while (*text) {
        size_t n = whitespace(text);
        if (!n) return false;
        text += n;
    }
    return true;
}
void format_transcript(char *text, bool punctuation) {
    if (punctuation) return;
    size_t end = strlen(text), n = 0;
    for (size_t i = 0; i < end;) {
        size_t width = whitespace(text+i);
        if (width) i += width;
        else n = ++i;
    }
    size_t remove = n && text[n-1] == '.' ? 1 : n >= 3 && !memcmp(text+n-3, "。", 3) ? 3 : 0;
    if (remove) memmove(text+n-remove, text+n, end-n+1);
}
