# SPDX-License-Identifier: GPL-2.0-only
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(shell pkg-config --cflags alsa)
LDLIBS += $(shell pkg-config --libs alsa) -lm

erhai-speakers: erhai-speakers.c

install: erhai-speakers
	install -D -m 0755 erhai-speakers $(DESTDIR)/usr/libexec/erhai-speakers

clean:
	rm -f erhai-speakers

.PHONY: install clean
