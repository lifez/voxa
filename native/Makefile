CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl json-c)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl json-c)

all: voxa-c
voxa-c: main.c session.c config.c osd.c voxa.h
	$(CC) $(CPPFLAGS) $(CFLAGS) main.c session.c config.c osd.c -o $@ $(LDFLAGS) $(LDLIBS)
voxa-c-test: main.c session.c config.c osd.c voxa.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DVOXA_TESTING main.c session.c config.c osd.c -o $@ $(LDFLAGS) $(LDLIBS)
test: voxa-c voxa-c-test
	node --test test-native.mjs
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
clean:
	rm -f voxa-c voxa-c-test
.PHONY: all test sanitize clean
