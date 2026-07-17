#!/bin/bash
# glasses-desktop installer.
#   curl -fsSL https://raw.githubusercontent.com/GlassOnTin/glasses-desktop/main/install.sh | bash
set -e

echo "==> Installing dependencies"
sudo apt-get install -y --no-install-recommends git sway wl-mirror foot python3

SRC=$(mktemp -d)
trap 'rm -rf "$SRC"' EXIT
echo "==> Fetching source"
git clone --depth 1 https://github.com/GlassOnTin/glasses-desktop "$SRC"

echo "==> Installing commands to /usr/local/bin"
sudo install -Dm755 "$SRC/start-glasses-desktop.sh" /usr/local/bin/glasses-desktop
sudo install -Dm755 "$SRC/setup-mirror.sh" /usr/local/bin/glasses-desktop-setup

echo "==> Registering login-manager session"
sudo install -Dm644 "$SRC/glasses-desktop.desktop" \
    /usr/share/wayland-sessions/glasses-desktop.desktop

CFG="${XDG_CONFIG_HOME:-$HOME/.config}/sway-glasses/config"
if [ -e "$CFG" ]; then
    echo "==> Keeping existing config: $CFG"
else
    echo "==> Installing config: $CFG"
    install -Dm644 "$SRC/sway-config" "$CFG"
fi

if ! command -v wf-panel-pi >/dev/null; then
    echo "note: wf-panel-pi (Raspberry Pi OS panel) not found; the desktop runs without a panel"
fi

echo "==> Done. From a TTY (Ctrl+Alt+F3), run: glasses-desktop"
