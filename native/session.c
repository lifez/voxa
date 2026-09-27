#include "voxa.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define AUDIO_LIMIT (16000 * 2 * 12)
#define MESSAGE_LIMIT (1024 * 1024)
#define FRAME 6400

typedef struct {
    unsigned char audio[AUDIO_LIMIT];
    size_t audio_len, total;
    char incoming[MESSAGE_LIMIT + 1];
    size_t incoming_len;
    char *outgoing;
    size_t out_len, out_offset;
    bool ready, commit_sent, commit_frame;
    double commit_time;
} Stream;

static char *base64(const unsigned char *data, size_t n) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc(4 * ((n + 2) / 3) + 1);
    if (!out) return NULL;
    size_t p = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned x = (unsigned)data[i] << 16;
        if (i+1 < n) x |= (unsigned)data[i+1] << 8;
        if (i+2 < n) x |= data[i+2];
        out[p++] = chars[(x >> 18) & 63]; out[p++] = chars[(x >> 12) & 63];
        out[p++] = i+1 < n ? chars[(x >> 6) & 63] : '=';
        out[p++] = i+2 < n ? chars[x & 63] : '=';
    }
    out[p] = 0; return out;
}
static int queue_frame(Stream *s, size_t n, bool commit) {
    char *encoded = base64(s->audio, n);
    if (!encoded) return -1;
    int len = asprintf(&s->outgoing, "{\"message_type\":\"input_audio_chunk\",\"audio_base_64\":\"%s\",\"sample_rate\":16000,\"commit\":%s}", encoded, commit ? "true" : "false");
    free(encoded);
    if (len < 0) { s->outgoing = NULL; return -1; }
    s->out_len = (size_t)len; s->out_offset = 0; s->commit_frame = commit;
    memmove(s->audio, s->audio+n, s->audio_len-n); s->audio_len -= n;
    return 0;
}
static int send_pending(CURL *curl, Stream *s) {
    if (!s->outgoing) return 0;
    size_t sent = 0;
    CURLcode code = curl_ws_send(curl, s->outgoing+s->out_offset, s->out_len-s->out_offset, &sent, 0, CURLWS_TEXT);
    if (code != CURLE_OK && code != CURLE_AGAIN) return -1;
    s->out_offset += sent;
    if (s->out_offset == s->out_len) {
        free(s->outgoing); s->outgoing = NULL;
        if (s->commit_frame) { s->commit_sent = true; s->commit_time = now_ms(); }
    }
    return 0;
}
// Return 1 for a final transcript, 0 for progress, -1 for error.
static int receive(CURL *curl, Stream *s, char **text) {
    for (unsigned i = 0; i < 32; i++) {
        char data[8192]; size_t n = 0;
        const struct curl_ws_frame *meta;
        CURLcode code = curl_ws_recv(curl, data, sizeof(data), &n, &meta);
        if (code == CURLE_AGAIN) return 0;
        if (code != CURLE_OK) return -1;
        if (meta->flags & CURLWS_CLOSE) return -1;
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;
        if (!(meta->flags & CURLWS_TEXT) || s->incoming_len+n > MESSAGE_LIMIT) return -1;
        memcpy(s->incoming+s->incoming_len, data, n); s->incoming_len += n;
        if (meta->bytesleft || (meta->flags & CURLWS_CONT)) continue;
        s->incoming[s->incoming_len] = 0;
        struct json_tokener *tok = json_tokener_new();
        if (!tok) return -1;
        json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        json_object *event = json_tokener_parse_ex(tok, s->incoming, (int)s->incoming_len);
        bool ok = json_tokener_get_error(tok) == json_tokener_success;
        json_tokener_free(tok); s->incoming_len = 0;
        if (!ok || !json_object_is_type(event, json_type_object)) { if (event) json_object_put(event); return -1; }
        json_object *type = json_object_object_get(event, "message_type");
        const char *name = json_object_is_type(type, json_type_string) ? json_object_get_string(type) : "";
        int result = 0;
        if (!strcmp(name, "session_started")) s->ready = true;
        else if (!strcmp(name, "committed_transcript")) {
            json_object *value = json_object_object_get(event, "text");
            if (!s->commit_sent || !json_object_is_type(value, json_type_string) ||
                strlen(json_object_get_string(value)) != (size_t)json_object_get_string_len(value)) result = -1;
            else { *text = strdup(json_object_get_string(value)); result = *text ? 1 : -1; }
        } else if (strstr(name, "error")) result = -1;
        json_object_put(event);
        if (result) return result;
    }
    return 0;
}

int session_run(int control, Config *config, const char *test_endpoint, bool print_only) {
    int result = 1, micfd = -1;
    pid_t mic = -1;
    CURL *curl = curl_easy_init();
    CURLM *multi = curl_multi_init();
    Stream *stream = calloc(1, sizeof(*stream));
    struct curl_slist *headers = NULL;
    char *url = NULL, *text = NULL;
    const char *error = "session initialization failed";
    bool added = false, connected = false, stopping = false, drained = false;
    double started = now_ms(), stopped = 0, first_pcm = -1, ready_at = -1, drained_at = -1;
    curl_socket_t network = CURL_SOCKET_BAD;
    if (!curl || !multi || !stream) goto cleanup;
    url = test_endpoint ? strdup(test_endpoint) : scribe_url(curl, config);
    if (!url) goto cleanup;
    char header[4120];
    snprintf(header, sizeof(header), "xi-api-key: %s", config->key);
    headers = curl_slist_append(NULL, header);
    secure_clear(header, sizeof(header));
    if (!headers) goto cleanup;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // No redirects, endpoint override or insecure TLS in the production binary.
    if (curl_multi_add_handle(multi, curl) != CURLM_OK) goto cleanup;
    added = true;
    mic = microphone_start(config->device, &micfd);
    if (mic < 0) { error = "cannot start microphone"; goto cleanup; }
    nonblock(control);
    fprintf(stderr, "[voxa-c] recording started\n");
    while (!quitting) {
        double now = now_ms();
        if (!stopping && now-started >= 60000) { error = "recording exceeded 60 seconds"; goto cleanup; }
        if (!stream->ready && now-started >= 10000) { error = "Scribe session timed out"; goto cleanup; }
        if (stopping && now-stopped >= 18000) { error = "stop timed out"; goto cleanup; }
        if (stream->commit_sent && now-stream->commit_time >= 8000) { error = "Scribe commit timed out"; goto cleanup; }
        char command[16];
        ssize_t count = read(control, command, sizeof(command));
        if (count == 0) { error = "control disconnected"; goto cleanup; }
        if (count < 0 && errno != EAGAIN && errno != EINTR) goto cleanup;
        if (count > 0 && !stopping) {
            stopping = true; stopped = now;
            microphone_stop(mic, micfd);
        }
        if (micfd >= 0) {
            unsigned char data[8192];
            // Bound work so command/timeouts are always serviced.
            for (unsigned i = 0; i < 32; i++) {
                ssize_t n = read(micfd, data, sizeof(data));
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (n < 0) { error = "microphone read failed"; goto cleanup; }
                if (n == 0) {
                    if (!stopping) { error = "microphone exited unexpectedly"; goto cleanup; }
                    close(micfd); micfd = -1; drained = true; drained_at = now_ms(); break;
                }
                if (first_pcm < 0) first_pcm = now_ms()-started;
                if (stream->audio_len+(size_t)n > AUDIO_LIMIT) { error = "audio queue exceeded 12 seconds"; goto cleanup; }
                memcpy(stream->audio+stream->audio_len, data, (size_t)n);
                stream->audio_len += (size_t)n; stream->total += (size_t)n;
            }
        }
        if (stopping && !drained && now-stopped >= 250) {
            if (micfd >= 0) { close(micfd); micfd = -1; }
            if (mic > 0) kill(mic, SIGKILL);
            drained = true; drained_at = now_ms();
        }
        if (!connected) {
            int running = 0, remaining;
            if (curl_multi_perform(multi, &running) != CURLM_OK) goto cleanup;
            CURLMsg *msg;
            while ((msg = curl_multi_info_read(multi, &remaining))) {
                if (msg->msg == CURLMSG_DONE) {
                    if (msg->data.result != CURLE_OK) { error = "WebSocket connection failed"; goto cleanup; }
                    connected = true;
                    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &network);
                }
            }
        }
        if (connected) {
            int received = receive(curl, stream, &text);
            if (received < 0) { error = "Scribe disconnected or invalid/error response"; goto cleanup; }
            if (stream->ready && ready_at < 0) ready_at = now_ms()-started;
            if (received == 1) {
                double transcript_at = now_ms();
                format_transcript(text, config->punctuation);
                if (!transcript_blank(text)) {
                    if (print_only) puts(text);
                    else {
                        if (insert_text(text)) { error = "text insertion failed"; goto cleanup; }
                        fprintf(stderr, "[voxa-c] inserted %zu UTF-8 bytes\n", strlen(text));
                    }
                } else fprintf(stderr, "[voxa-c] empty transcript; nothing pasted\n");
                fprintf(stderr, "[voxa-c] timing first_pcm_ms=%.2f session_ready_ms=%.2f drain_ms=%.2f commit_to_transcript_ms=%.2f stop_to_transcript_ms=%.2f typing_ms=%.2f stop_to_done_ms=%.2f audio_bytes=%zu\n",
                        first_pcm, ready_at, drained_at-stopped, transcript_at-stream->commit_time,
                        transcript_at-stopped, now_ms()-transcript_at, now_ms()-stopped, stream->total);
                result = transcript_blank(text) ? 2 : 0; goto cleanup;
            }
            if (stream->ready && !stream->outgoing && !stream->commit_sent) {
                if (stream->audio_len >= FRAME*2) {
                    if (queue_frame(stream, FRAME, false)) goto cleanup;
                } else if (stopping && drained) {
                    if (!stream->total || !stream->audio_len || stream->audio_len % 2) { error = "no or invalid microphone PCM"; goto cleanup; }
                    if (queue_frame(stream, stream->audio_len, true)) goto cleanup;
                }
            }
            if (send_pending(curl, stream)) { error = "WebSocket send failed"; goto cleanup; }
        }
        if (!connected) {
            struct curl_waitfd extra[2] = {{control, CURL_WAIT_POLLIN, 0}, {micfd, CURL_WAIT_POLLIN, 0}};
            int numfds;
            if (curl_multi_poll(multi, extra, micfd >= 0 ? 2 : 1, 10, &numfds) != CURLM_OK) goto cleanup;
        } else {
            // Don't sleep while queued audio can immediately be framed and sent.
            if (stream->ready && !stream->outgoing && !stream->commit_sent &&
                (stream->audio_len >= FRAME*2 || (stopping && drained))) continue;
            struct pollfd fds[] = {{control, POLLIN, 0}, {micfd, POLLIN, 0},
                {(int)network, (short)(POLLIN | (stream->outgoing ? POLLOUT : 0)), 0}};
            poll(fds, 3, 10);
        }
    }
    error = "session cancelled";
cleanup:
    microphone_cleanup(mic, micfd);
    if (result == 1) fprintf(stderr, "[voxa-c] %s\n", error);
    if (added) curl_multi_remove_handle(multi, curl);
    if (curl) curl_easy_cleanup(curl);
    if (multi) curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    free(url); free(text);
    if (stream) { free(stream->outgoing); free(stream); }
    return result;
}
