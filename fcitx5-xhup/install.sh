#!/bin/bash
# Install the standalone fcitx5 xhup addon.
# Only /usr/lib/fcitx5/libxhup.so requires root; everything else goes to
# the user's own fcitx5 data dirs.
set -euo pipefail

cd "$(dirname "$0")"

make libxhup.so

# 1. addon shared library (system-wide, needs sudo)
sudo install -Dm755 libxhup.so /usr/lib/fcitx5/libxhup.so

# 2. addon descriptor (user data dir, no root needed)
install -Dm644 xhup-addon.conf "$HOME/.local/share/fcitx5/addon/xhup.conf"

# 3. input method entry (user data dir)
install -Dm644 xhup-im.conf "$HOME/.local/share/fcitx5/inputmethod/xhup.conf"

# 4. dictionary (user data dir)
install -Dm644 ../xiaohe.dict "$HOME/.local/share/fcitx5/xhup.dict"

echo "Installed. Restart fcitx5 (fcitx5 -r or log out/in)."
