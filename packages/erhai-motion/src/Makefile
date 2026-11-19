# SPDX-License-Identifier: GPL-2.0-only
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags libssc glib-2.0)
LDLIBS += $(shell pkg-config --libs libssc glib-2.0) -lm

erhai-motion: erhai-motion.c

install: erhai-motion
	install -D -m 0755 erhai-motion $(DESTDIR)/usr/libexec/erhai-motion

clean:
	rm -f erhai-motion

.PHONY: install clean
