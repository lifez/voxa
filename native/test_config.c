#include "voxa.h"
#include <stdio.h>
#include <stdlib.h>

// Config-only harness never loads credentials; satisfy config_free's erasure hook.
void secure_clear(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

int main(void) {
    Config config;
    if (config_read(&config)) {
        fputs("Invalid configuration\n", stderr);
        return 1;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) { config_free(&config); return 1; }
    CURL *curl = curl_easy_init();
    char *url = curl ? scribe_url(curl, &config) : NULL;
    if (url) puts(url);
    int result = url ? 0 : 1;
    free(url);
    if (curl) curl_easy_cleanup(curl);
    config_free(&config);
    curl_global_cleanup();
    return result;
}
