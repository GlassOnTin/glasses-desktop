#!/bin/bash
# glasses-desktop — launch the SBS glasses desktop (sway) on this seat.
#
# Run from a TTY (e.g. Ctrl+Alt+F3, log in): any existing desktop session
# on another VT pauses while sway owns the display; exit sway with
# Super+Shift+e and switch back.
exec sway -c "${XDG_CONFIG_HOME:-$HOME/.config}/sway-glasses/config"
