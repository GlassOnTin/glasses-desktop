# glasses-desktop

A full Linux desktop in **both eyes** of SBS display glasses (Viture,
XREAL, …), using sway. Regular compositors put a window at x<1920 in the
left eye only; here the desktop lives on a virtual 1920×1080 output and the
physical output shows one copy per eye — with **per-window stereo depth**:
each window is drawn at a depth given by focus order, so the focused window
pops toward you and the rest sit further back.

- Glasses in **SBS mode** (3840×1080): both eyes, windows at depth.
- Glasses in **2D mode** (1920×1080): one flat fullscreen mirror.
- Toggling the glasses reconfigures live — the presenter follows the
  resolution change per frame, and a watcher repairs output/workspace
  scrambles, crashed clients, and the panel after display hotplugs.

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

## Depth tuning

`glasses-presenter` reads these environment variables (pixels of horizontal
disparity; set them in `~/.config/sway-glasses/config` via
`exec env ... glasses-desktop-setup` or your session environment):

| var | default | meaning |
|---|---|---|
| `GLASSES_DEPTH_POP` | 8 | disparity of the focused window |
| `GLASSES_DEPTH_STEP` | 3 | how much less each step down the focus order gets |
| `GLASSES_DEPTH_FLOOR` | 0 | minimum disparity (0 = screen plane) |
| `GLASSES_SWAP_EYES` | 0 | set 1 if depth looks inverted |

Set `GLASSES_DEPTH_POP=0 GLASSES_DEPTH_STEP=0` for a flat mirror. Known
cosmetic artifact: a few-pixel "echo" at window edges where a shifted
window exposes its own copy in the flat desktop capture behind it.

If `glasses-presenter` is missing (e.g. its build failed), the watcher
falls back to flat wl-mirror copies automatically.

## Files

- `glasses-desktop` — session launcher (`start-glasses-desktop.sh`)
- `glasses-desktop-setup` — output/mirror reconciliation watcher
  (`setup-mirror.sh`); log: `~/.local/state/glasses-desktop/setup.log`
- `glasses-presenter` — per-window depth SBS presenter (`presenter/`)
- `~/.config/sway-glasses/config` — sway config (`sway-config`)

## 3D apps

For stereo 3D of individual GLES apps (true depth from the app's depth
buffer, not just window-level depth), see
[gles-stereo-shim](https://github.com/GlassOnTin/gles-stereo-shim).

## License

MIT
