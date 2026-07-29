CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -D_GNU_SOURCE
WAYLAND_FLAGS = $(shell pkg-config --cflags --libs wayland-client)

all: xiaohe

input-method-unstable-v2-client-protocol.h: input-method-unstable-v2.xml
	wayland-scanner client-header $< $@

input-method-unstable-v2-protocol.c: input-method-unstable-v2.xml
	wayland-scanner private-code $< $@

virtual-keyboard-unstable-v1-client-protocol.h: virtual-keyboard-unstable-v1.xml
	wayland-scanner client-header $< $@

virtual-keyboard-unstable-v1-protocol.c: virtual-keyboard-unstable-v1.xml
	wayland-scanner private-code $< $@

dict.h xiaohe.dict: gen_dict.py dict.txt $(wildcard user_dict.txt)
	python gen_dict.py

dict:
	python gen_dict.py

xiaohe: main.c \
            input-method-unstable-v2-protocol.c input-method-unstable-v2-client-protocol.h \
            virtual-keyboard-unstable-v1-protocol.c virtual-keyboard-unstable-v1-client-protocol.h \
            dict.h
	$(CC) $(CFLAGS) -o $@ main.c input-method-unstable-v2-protocol.c virtual-keyboard-unstable-v1-protocol.c $(WAYLAND_FLAGS)

clean:
	rm -f xiaohe *-protocol.h *-protocol.c

.PHONY: all clean dict
