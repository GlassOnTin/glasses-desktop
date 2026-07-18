#!/bin/bash
# glasses-desktop — launch the SBS glasses desktop (sway) on this seat.
#
# Run from a TTY (e.g. Ctrl+Alt+F3, log in): any existing desktop session
# on another VT pauses while sway owns the display; exit sway with
# Super+Shift+e and switch back.
#
# Software cursors: the wlroots headless backend accepts a hardware-cursor
# plane and discards it, so the cursor never reaches the desktop frames
# that get mirrored to the glasses. Forcing software cursors draws it into
# every frame — screencopy then shows it in both eyes.
export WLR_NO_HARDWARE_CURSORS=1
exec sway -c "${XDG_CONFIG_HOME:-$HOME/.config}/sway-glasses/config"
