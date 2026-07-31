CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -fPIC
FCITX_CFLAGS := $(shell pkg-config --cflags Fcitx5Core)
FCITX_LIBS := $(shell pkg-config --libs Fcitx5Core)

all: libxhup.so

libxhup.so: xhup.cpp dict.h
	$(CXX) $(CXXFLAGS) $(FCITX_CFLAGS) -shared -o $@ xhup.cpp $(FCITX_LIBS)

install: libxhup.so
	sudo install -Dm755 libxhup.so /usr/lib/fcitx5/libxhup.so
	install -Dm644 xhup-addon.conf "$(HOME)/.local/share/fcitx5/addon/xhup.conf"
	install -Dm644 xhup-im.conf "$(HOME)/.local/share/fcitx5/inputmethod/xhup.conf"
	install -Dm644 ../xiaohe.dict "$(HOME)/.local/share/fcitx5/xhup.dict"

uninstall:
	sudo rm -f /usr/lib/fcitx5/libxhup.so
	rm -f "$(HOME)/.local/share/fcitx5/addon/xhup.conf"
	rm -f "$(HOME)/.local/share/fcitx5/inputmethod/xhup.conf"
	rm -f "$(HOME)/.local/share/fcitx5/xhup.dict"

clean:
	rm -f libxhup.so

.PHONY: all install uninstall clean
