# glasses-desktop — SBS Wayland desktop for the Viture glasses

The whole desktop (any window: terminal, browser, ...) visible in **both
eyes** of the glasses, not just the left. labwc composites windows onto one
wide canvas, so a window at x<1920 lands only in the left eye; this uses
**sway** instead:

- The desktop lives on a **1920×1080 headless virtual output** (HEADLESS-1).
- The physical glasses output shows **wl-mirror** copies of it:
  - glasses in SBS mode (3840×1080): two mirrors side by side = left | right eye
  - glasses in 2D mode (1920×1080): one fullscreen mirror
- `setup-mirror.sh` is a reconciliation loop (verified on hardware
  2026-07-17, stable through repeated 2D↔SBS toggles). Every second it
  enforces: desktop workspaces on HEADLESS-1 (an EDID flap makes sway
  scatter them), workspace "mirror" visible on the physical output, the
  right mirror count, widest available mode driven (sway doesn't always
  adopt 3840×1080 after the flip), and wf-panel-pi alive (it can crash on
  hotplug). After repairs it wiggles the cursor a few px to force fresh
  frames — wlroots renders on damage only, so new mirrors on a static
  desktop would otherwise stay black.
- The desktop shell is the Pi's own `wf-panel-pi` (start menu, taskbar,
  clock); app windows open floating with titlebars for mouse-first use.

## Run it

The real session needs the GPU, so it can't start while labwc is showing.
From a TTY:

1. `Ctrl+Alt+F3`, log in as ian
2. `~/viture-xr/glasses-desktop/start-glasses-desktop.sh`
3. labwc's VT pauses while sway owns the display; switch back after
   exiting sway (`Super+Shift+e`).

Keys (Super = mod): `Super+Enter` terminal, `Super+Shift+q` close window,
`Super+1..4` workspaces, `Super+space` float, `Super+f` fullscreen.

Config: `sway-config` (symlinked from `~/.config/sway-glasses/config`).
Setup log: `~/viture-xr/glasses-desktop/setup.log`.

Nested smoke test (safe, inside the running desktop session):
`WLR_BACKENDS=wayland,headless WLR_HEADLESS_OUTPUTS=1 sway -c ~/.config/sway-glasses/config`

## Next idea: per-window depth (v1)

Replace the two wl-mirror instances with a custom presenter that:
1. captures HEADLESS-1 via wlr-screencopy (dmabuf → GL texture),
2. reads window bounding boxes + stacking/focus order from sway IPC
   (`swaymsg -t get_tree` / subscribe to window events),
3. paints a synthetic depth map from the window rects (focused nearest,
   background further) and warps per eye — the same disparity math as
   gles-stereo-shim's Phase 4 depth shader.

Result: the focused window pops toward you, others sit deeper. Modest
disparities (5–15 px) keep edge artifacts small.
