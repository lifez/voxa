.DEFAULT_GOAL := all
CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl json-c)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl json-c)
SOURCES = main.c session.c config.c osd.c platform.c settings.c
ifeq ($(shell uname -s),Linux)
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags wayland-client xkbcommon)
LDLIBS += $(shell $(PKG_CONFIG) --libs wayland-client xkbcommon)
SOURCES += keyboard.c virtual-keyboard-protocol.c
GENERATED = virtual-keyboard-client.h virtual-keyboard-protocol.c
KEYBOARD_TEST = voxa-keyboard-test
virtual-keyboard-client.h: virtual-keyboard-unstable-v1.xml
	wayland-scanner client-header $< $@
virtual-keyboard-protocol.c: virtual-keyboard-unstable-v1.xml
	wayland-scanner private-code $< $@
voxa-keyboard-test: keyboard.c voxa.h $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DVOXA_KEYBOARD_TEST keyboard.c -o $@ $(LDFLAGS) $(LDLIBS)
endif

all: voxa
voxa: $(SOURCES) voxa.h $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)
voxa-test: $(SOURCES) voxa.h $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DVOXA_TESTING $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)
test: voxa voxa-test $(KEYBOARD_TEST)
	$(if $(KEYBOARD_TEST),./$(KEYBOARD_TEST),:)
	python3 test_native.py
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
clean:
	rm -f voxa voxa-test voxa-c voxa-c-test voxa-keyboard-test $(GENERATED)
.PHONY: all test sanitize clean
