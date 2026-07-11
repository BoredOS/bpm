CC = x86_64-boredos-gcc
LD = x86_64-boredos-ld

ifneq ($(BOREDOS_SDK),)
  ifeq ($(wildcard $(BOREDOS_SDK)/lib/libc.a),)
    BOOTSTRAP_SDK = $(BOREDOS_SDK)
    SDK_PATH      = $(BOREDOS_SDK)
  else
    SDK_PATH      = $(BOREDOS_SDK)
  endif
endif

ifeq ($(SDK_PATH),)
  SDK_PATH = $(abspath build/sdk)
  ifeq ($(wildcard $(SDK_PATH)/lib/libc.a),)
    BOOTSTRAP_SDK = $(SDK_PATH)
  endif
endif

DESTDIR ?= $(abspath build/dist)

CFLAGS  = -Wall -Wextra -std=gnu11 -ffreestanding -O2 -fno-stack-protector \
          -fno-stack-check -fno-lto -fno-pie -m64 -march=x86-64 -mno-red-zone \
          -isystem $(SDK_PATH)/include

LDFLAGS = -m elf_x86_64 -nostdlib -static -no-pie -Ttext=0x40000000 \
          --no-dynamic-linker -z text -z max-page-size=0x1000 -e _start \
          -L$(SDK_PATH)/lib

APPS    = bpm.elf

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,obj/%.o,$(SRCS))

all: $(APPS)

bpm.elf: $(OBJS)
	$(LD) $(LDFLAGS) $(SDK_PATH)/lib/crt0.o $(SDK_PATH)/lib/crti.o $(OBJS) -lc $(SDK_PATH)/lib/crtn.o -o $@

obj/%.o: src/%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

install: all
	mkdir -p $(DESTDIR)/bin
	cp $(APPS) $(DESTDIR)/bin/
	mkdir -p $(DESTDIR)/Library/AppData/org.boredos.bpm
	cp bpmconf.toml $(DESTDIR)/Library/AppData/org.boredos.bpm/bpmconf.toml 2>/dev/null || true

clean:
	rm -rf obj build $(APPS)
