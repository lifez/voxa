#include "voxa.h"
#ifdef __linux__
#include "virtual-keyboard-client.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include <xkbcommon/xkbcommon.h>

// Only physical writing keys: Ghostty/GTK can treat other codes as Ctrl,
// function keys, navigation, etc. regardless of their Unicode keysym.
static const unsigned text_codes[] = {
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
    43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 57
};
#define TEXT_KEYS (sizeof(text_codes)/sizeof(text_codes[0]))
// ponytail: 48 writing keys × 2 levels × 4 XKB groups = 384 distinct symbols.
// Reject larger alphabets before typing; add more levels if real dictation needs it.
#define SYMBOL_LIMIT (TEXT_KEYS * 2 * 4)
typedef struct { xkb_keysym_t symbol; unsigned code, level, group; } Key;
typedef struct { Key *keys; size_t count, length; unsigned *sequence; } Keymap;

static void keymap_free(Keymap *map) { free(map->keys); free(map->sequence); }
static int keymap_build(const char *text, Keymap *map) {
    size_t bytes = strlen(text);
    if (bytes > 1024 * 1024 || !setlocale(LC_CTYPE, "C.UTF-8")) return -1;
    map->keys = calloc(SYMBOL_LIMIT, sizeof(*map->keys));
    map->sequence = calloc(bytes + 1, sizeof(*map->sequence));
    if (!map->keys || !map->sequence) return -1;
    mbstate_t state = {0};
    while (*text) {
        wchar_t ch;
        size_t width = mbrtowc(&ch, text, bytes, &state);
        if (width == (size_t)-1 || width == (size_t)-2 || !width) return -1;
        text += width;
        bytes -= width;
        xkb_keysym_t symbol = ch == '\n' || ch == '\r' ? XKB_KEY_Return : ch == '\t' ? XKB_KEY_Tab : xkb_utf32_to_keysym((uint32_t)ch);
        if (!symbol || (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 127) return -1;
        size_t i = 0;
        // ponytail: linear lookup of distinct characters; hash only if large
        // multilingual transcripts make this measurable.
        while (i < map->count && map->keys[i].symbol != symbol) i++;
        if (i == map->count) {
            if (map->count == SYMBOL_LIMIT) return -1;
            Key key = {symbol, text_codes[i % TEXT_KEYS], (unsigned)(i / TEXT_KEYS % 2), (unsigned)(i / (TEXT_KEYS * 2))};
            if (symbol == XKB_KEY_Return || symbol == XKB_KEY_Tab)
                key = (Key){symbol, symbol == XKB_KEY_Return ? 28 : 15, 0, 0};
            map->keys[map->count++] = key;
        }
        map->sequence[map->length++] = (unsigned)i;
    }
    return 0;
}

static FILE *keymap_file(const Keymap *map) {
    int fd = memfd_create("voxa-keymap", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, "w+");
    if (!file) { close(fd); return NULL; }
    fputs("xkb_keymap { xkb_keycodes { minimum = 8; maximum = 65;\n", file);
    for (size_t i = 0; i < TEXT_KEYS; i++) fprintf(file, "<K%zu> = %u;\n", i, text_codes[i] + 8);
    fputs("<RET> = 36; <TAB> = 23; }; xkb_types { type \"ONE_LEVEL\" { modifiers = None; map[None] = Level1; }; type \"TEXT\" { modifiers = Shift; map[None] = Level1; map[Shift] = Level2; }; }; xkb_compatibility {}; xkb_symbols {\n"
          "key <RET> { type = \"ONE_LEVEL\", [Return] }; key <TAB> { type = \"ONE_LEVEL\", [Tab] };\n", file);
    for (size_t i = 0; i < TEXT_KEYS; i++) {
        fprintf(file, "key <K%zu> {", i);
        for (size_t group = 0; group < 4; group++) {
            size_t low = group * TEXT_KEYS * 2 + i, high = low + TEXT_KEYS;
            fprintf(file, "%stype[Group%zu] = \"TEXT\", symbols[Group%zu] = [0x%x, 0x%x]", group ? ", " : "", group + 1, group + 1,
                    low < map->count ? map->keys[low].symbol : XKB_KEY_NoSymbol,
                    high < map->count ? map->keys[high].symbol : XKB_KEY_NoSymbol);
        }
        fputs("};\n", file);
    }
    fputs("}; };", file);
    fputc(0, file);
    if (fflush(file) || ferror(file)) { fclose(file); return NULL; }
    return file;
}

#ifndef VOXA_KEYBOARD_TEST
typedef struct {
    struct wl_seat *seat;
    struct zwp_virtual_keyboard_manager_v1 *manager;
} Registry;
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    Registry *r = data;
    (void)version;
    if (!strcmp(interface, wl_seat_interface.name) && !r->seat)
        r->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) && !r->manager)
        r->manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }

static int type_text(const char *text) {
    Keymap map = {0};
    Registry globals = {0};
    struct wl_display *display = NULL;
    struct wl_registry *registry = NULL;
    struct zwp_virtual_keyboard_v1 *keyboard = NULL;
    FILE *file = NULL;
    int result = -1;
    if (keymap_build(text, &map)) goto done;
    file = keymap_file(&map);
    if (!file || !(display = wl_display_connect(NULL))) goto done;
    registry = wl_display_get_registry(display);
    if (!registry) goto done;
    const struct wl_registry_listener listener = {registry_global, registry_remove};
    wl_registry_add_listener(registry, &listener, &globals);
    if (wl_display_roundtrip(display) < 0 || !globals.seat || !globals.manager) goto done;
    keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(globals.manager, globals.seat);
    if (!keyboard) goto done;
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(file), (uint32_t)ftell(file));
    zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
    if (wl_display_roundtrip(display) < 0) goto done;
    for (size_t i = 0; i < map.length; i++) {
        Key key = map.keys[map.sequence[i]];
        zwp_virtual_keyboard_v1_modifiers(keyboard, key.level, 0, 0, key.group);
        zwp_virtual_keyboard_v1_key(keyboard, (uint32_t)now_ms(), key.code, WL_KEYBOARD_KEY_STATE_PRESSED);
        if (wl_display_roundtrip(display) < 0) goto done;
        usleep(2000);
        zwp_virtual_keyboard_v1_key(keyboard, (uint32_t)now_ms(), key.code, WL_KEYBOARD_KEY_STATE_RELEASED);
        if (wl_display_roundtrip(display) < 0) goto done;
        usleep(2000);
    }
    zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
    if (wl_display_roundtrip(display) < 0) goto done;
    result = 0;
done:
    if (keyboard) zwp_virtual_keyboard_v1_destroy(keyboard);
    if (globals.manager) zwp_virtual_keyboard_manager_v1_destroy(globals.manager);
    if (globals.seat) wl_seat_destroy(globals.seat);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
    if (file) fclose(file);
    keymap_free(&map);
    return result;
}

int keyboard_insert(const char *text) {
    pid_t parent = getpid(), pid = fork();
    if (!pid) { child_signals(); parent_guard(parent, SIGKILL); _exit(type_text(text) ? 1 : 0); }
    if (pid < 0) return -1;
    size_t characters = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) if ((*p & 0xc0) != 0x80) characters++;
    double deadline = now_ms() + 2000 + (characters < 10000 ? characters : 10000) * 6;
    while (!quitting && now_ms() < deadline) {
        int status;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
        if (done < 0 && errno != EINTR) break;
        usleep(2000);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    return -1;
}

#else
#include <assert.h>
static void check_keymap(const char *text) {
    Keymap map = {0};
    assert(!keymap_build(text, &map));
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(context);
    FILE *file = keymap_file(&map);
    assert(file);
    size_t size = (size_t)ftell(file);
    char *contents = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fileno(file), 0);
    assert(contents != MAP_FAILED);
    struct xkb_keymap *parsed = xkb_keymap_new_from_string(context, contents, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(parsed);
    munmap(contents, size);
    struct xkb_state *state = xkb_state_new(parsed);
    assert(state);
    for (size_t i = 0; i < map.count; i++) {
        unsigned expected_code = map.keys[i].symbol == XKB_KEY_Return ? 28 : map.keys[i].symbol == XKB_KEY_Tab ? 15 : text_codes[i % TEXT_KEYS];
        assert(map.keys[i].code == expected_code);
        const xkb_keysym_t *symbols;
        assert(xkb_keymap_key_get_syms_by_level(parsed, map.keys[i].code + 8, map.keys[i].group, map.keys[i].level, &symbols) == 1);
        assert(symbols[0] == map.keys[i].symbol);
        xkb_state_update_mask(state, map.keys[i].level, 0, 0, 0, 0, map.keys[i].group);
        assert(xkb_state_key_get_one_sym(state, map.keys[i].code + 8) == map.keys[i].symbol);
        if (map.keys[i].level) assert(xkb_state_mod_index_is_consumed(state, map.keys[i].code + 8, 0));
    }
    mbstate_t decoder = {0};
    size_t length = 0, bytes = strlen(text);
    while (*text) {
        wchar_t ch;
        size_t width = mbrtowc(&ch, text, bytes, &decoder);
        assert(width > 0 && width <= bytes);
        xkb_keysym_t expected = ch == '\n' || ch == '\r' ? XKB_KEY_Return : ch == '\t' ? XKB_KEY_Tab : xkb_utf32_to_keysym((uint32_t)ch);
        assert(map.keys[map.sequence[length++]].symbol == expected);
        text += width; bytes -= width;
    }
    assert(length == map.length);
    xkb_state_unref(state); xkb_keymap_unref(parsed); xkb_context_unref(context); fclose(file); keymap_free(&map);
}
int main(void) {
    check_keymap("รู้สึกว่าตัว transcribe ที่ Linux จะทำงานได้ไม่ค่อยถูกต้องเท่าไหร่ น้ำ กำ ทำ\n😀\t");
    char text[(SYMBOL_LIMIT + 1) * 4 + 1];
    size_t length = 0;
    for (unsigned i = 0; i <= SYMBOL_LIMIT; i++) {
        int width = snprintf(text + length, sizeof(text) - length, "%lc", (wint_t)(0x400 + i));
        assert(width > 0); length += (size_t)width;
        if (i + 1 == SYMBOL_LIMIT) check_keymap(text);
    }
    Keymap map = {0};
    assert(keymap_build(text, &map)); keymap_free(&map);
    const char *invalid[] = {"\xff", "\xe0\xb8", "\033"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        map = (Keymap){0};
        assert(keymap_build(invalid[i], &map)); keymap_free(&map);
    }
    puts("keyboard keymap checks passed");
}
#endif
#endif
