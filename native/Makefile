CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl json-c)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl json-c)
SOURCES = main.c session.c config.c osd.c platform.c settings.c

all: voxa
voxa: $(SOURCES) voxa.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)
voxa-test: $(SOURCES) voxa.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DVOXA_TESTING $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)
test: voxa voxa-test
	python3 test_native.py
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
clean:
	rm -f voxa voxa-test voxa-c voxa-c-test
.PHONY: all test sanitize clean
