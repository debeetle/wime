CC ?= gcc
CFLAGS ?= -Wall -Wextra -O3 -flto -march=native -fno-plt -ffunction-sections -fdata-sections -D_GNU_SOURCE
LDFLAGS ?= -fuse-ld=mold -Wl,--gc-sections -Wl,-O3
WAYLAND_FLAGS = $(shell pkg-config --cflags --libs wayland-client)

XIAOHE = ..
# 协议生成产物统一放 wime 根目录,所有输入法共用一份
PROTO = ../..

IM_PROTO_C = $(PROTO)/input-method-unstable-v2-protocol.c
IM_PROTO_H = $(PROTO)/input-method-unstable-v2-client-protocol.h
VK_PROTO_C = $(PROTO)/virtual-keyboard-unstable-v1-protocol.c
VK_PROTO_H = $(PROTO)/virtual-keyboard-unstable-v1-client-protocol.h

all: xhup

$(IM_PROTO_H): $(PROTO)/input-method-unstable-v2.xml
	wayland-scanner client-header $< $@

$(IM_PROTO_C): $(PROTO)/input-method-unstable-v2.xml
	wayland-scanner private-code $< $@

$(VK_PROTO_H): $(PROTO)/virtual-keyboard-unstable-v1.xml
	wayland-scanner client-header $< $@

$(VK_PROTO_C): $(PROTO)/virtual-keyboard-unstable-v1.xml
	wayland-scanner private-code $< $@

xhup.dict: $(XIAOHE)/gen_dict.py dict.txt $(XIAOHE)/word_freq.txt $(wildcard user_dict.txt)
	python ../gen_dict.py xhup.dict .

gen:
	python ../gen_dict.py xhup.dict .

xhup: main.c $(IM_PROTO_C) $(IM_PROTO_H) $(VK_PROTO_C) $(VK_PROTO_H) $(XIAOHE)/dict.h xhup.dict
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(XIAOHE) -I$(PROTO) -o $@ main.c $(IM_PROTO_C) $(VK_PROTO_C) $(WAYLAND_FLAGS)

clean:
	rm -f xhup

.PHONY: all clean gen
