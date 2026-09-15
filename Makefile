# Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
# This software is released under the GNU General Public License v3.0. See LICENSE file for details.
# This header needs to maintain in any file it is present in, as per the GPL license terms.

CC = x86_64-boredos-gcc

DESTDIR ?= $(abspath build/dist)

CFLAGS  = -Wall -Wextra -std=gnu11 -O2 -fno-stack-protector \
          -fno-stack-check -m64 -march=x86-64

LDFLAGS = -Wl,-z,max-page-size=0x1000 -Wl,-dynamic-linker,/usr/lib/ld.so -Wl,-rpath,/usr/lib:/lib -lm

APPS    = bpm

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,obj/%.o,$(SRCS))

all: $(APPS)

bpm: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

obj/%.o: src/%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

install: all
	mkdir -p $(DESTDIR)/bin
	cp $(APPS) $(DESTDIR)/bin/
	mkdir -p $(DESTDIR)/Library/AppData/org.boredos.bpm
	cp bpmconf.toml $(DESTDIR)/Library/AppData/org.boredos.bpm/bpmconf.toml 2>/dev/null || true
	mkdir -p $(DESTDIR)/etc/skel/Library/AppData/org.boredos.bpm
	cp bpmconf.toml $(DESTDIR)/etc/skel/Library/AppData/org.boredos.bpm/bpmconf.toml 2>/dev/null || true

clean:
	rm -rf obj build $(APPS)
