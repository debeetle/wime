CC ?= gcc
CFLAGS ?= -Wall -Wextra -O3 -flto -march=native -fno-plt -ffunction-sections -fdata-sections -D_GNU_SOURCE
LDFLAGS ?= -fuse-ld=mold -Wl,--gc-sections -Wl,-O3
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

xiaohe.dict: gen_dict.py dict.txt $(wildcard user_dict.txt)
	python gen_dict.py

dict:
	python gen_dict.py

xiaohe: main.c \
            input-method-unstable-v2-protocol.c input-method-unstable-v2-client-protocol.h \
            virtual-keyboard-unstable-v1-protocol.c virtual-keyboard-unstable-v1-client-protocol.h \
            dict.h xiaohe.dict
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c input-method-unstable-v2-protocol.c virtual-keyboard-unstable-v1-protocol.c $(WAYLAND_FLAGS)

clean:
	rm -f xiaohe *-protocol.h *-protocol.c

.PHONY: all clean dict
