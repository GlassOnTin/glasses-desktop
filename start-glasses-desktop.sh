#!/bin/bash
# Launch the SBS glasses desktop (sway) on this seat.
#
# Run this from a TTY: press Ctrl+Alt+F3, log in as ian, then run
#   ~/viture-xr/glasses-desktop/start-glasses-desktop.sh
# The labwc session on the other VT pauses while sway owns the display;
# switch back to it with Ctrl+Alt+F7 (or wherever labwc lives) after
# exiting sway ($mod+Shift+e or Ctrl+C here).
exec sway -c ~/.config/sway-glasses/config
