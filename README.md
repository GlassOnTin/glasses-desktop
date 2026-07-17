# glasses-desktop

A full Linux desktop in **both eyes** of SBS display glasses (Viture,
XREAL, …), using sway. Regular compositors put a window at x<1920 in the
left eye only; here the desktop lives on a virtual 1920×1080 output and the
physical output shows one mirrored copy per eye.

- Glasses in **SBS mode** (3840×1080): two mirrors, left | right eye.
- Glasses in **2D mode** (1920×1080): one fullscreen mirror.
- Toggling the glasses reconfigures live — a watcher re-checks every second
  and also repairs output/workspace scrambles, crashed mirrors, and the
  panel after display hotplugs.

Developed on a Raspberry Pi 5 (Wayland/labwc based Raspberry Pi OS); should
apply to any wlroots-capable box. Uses the Pi's `wf-panel-pi` for a start
menu/taskbar when available.

## Install

```
curl -fsSL https://raw.githubusercontent.com/GlassOnTin/glasses-desktop/main/install.sh | bash
```

## Run

**From the login screen (recommended):** log out, pick **Glasses Desktop
(SBS)** from the session menu, log in. Log out (`Super+Shift+e`) to return
to your normal desktop. If your machine auto-logs-in (default on Raspberry
Pi OS), the session menu appears after logging out once.

**Without logging out:** switch to a TTY (`Ctrl+Alt+F3`), log in, run
`glasses-desktop`. Your existing session pauses on its VT while sway owns
the display; exit sway and switch back (`Ctrl+Alt+F7` or similar).

Put the glasses in SBS mode whenever — the desktop follows the toggle live.

Windows open floating with titlebars — use the panel's start menu and
mouse, or: `Super+Enter` terminal, `Super+Shift+q` close, `Super+1..4`
workspaces, `Super+f` fullscreen, `Super+space` toggle floating.

## Files

- `glasses-desktop` — session launcher (`start-glasses-desktop.sh`)
- `glasses-desktop-setup` — output/mirror reconciliation watcher
  (`setup-mirror.sh`); log: `~/.local/state/glasses-desktop/setup.log`
- `~/.config/sway-glasses/config` — sway config (`sway-config`)

## 3D apps

For stereo 3D of individual GLES apps (not just a flat desktop), see
[gles-stereo-shim](https://github.com/GlassOnTin/gles-stereo-shim).
Planned here: replacing wl-mirror with a presenter that assigns per-window
depth from stacking order, so the focused window pops toward you.

## License

MIT
