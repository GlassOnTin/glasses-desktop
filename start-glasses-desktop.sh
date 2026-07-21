#!/bin/bash
# glasses-desktop — launch the SBS glasses desktop (sway) on this seat.
#
# Safe as the login-manager default session: when no SBS glasses are
# attached, it hands over to the stock desktop instead. Detection reads
# the EDID of connected outputs from sysfs, before any compositor runs.
#
#   GLASSES_DESKTOP_FORCE=1   always start the glasses desktop
#   GLASSES_DESKTOP_FORCE=0   always start the fallback session
#   GLASSES_FALLBACK_SESSION  fallback command (default: /usr/bin/labwc-pi)
#   GLASSES_EDID_MATCH        EDID regex (default: VITURE|XREAL|ROKID)

FALLBACK="${GLASSES_FALLBACK_SESSION:-/usr/bin/labwc-pi}"
MATCH="${GLASSES_EDID_MATCH:-VITURE|XREAL|ROKID}"

glasses_connected() {
    local c
    for c in /sys/class/drm/card*-*; do
        [ -f "$c/status" ] && [ "$(cat "$c/status")" = connected ] || continue
        grep -aqiE "$MATCH" "$c/edid" 2>/dev/null && return 0
    done
    return 1
}

want_glasses=false
case "${GLASSES_DESKTOP_FORCE:-}" in
1) want_glasses=true ;;
0) ;;
*) glasses_connected && want_glasses=true ;;
esac
if ! $want_glasses && [ -x "$FALLBACK" ]; then
    exec "$FALLBACK"
fi

# Software cursors: the wlroots headless backend accepts a hardware-cursor
# plane and discards it, so the cursor never reaches the desktop frames
# that get mirrored to the glasses. Forcing software cursors draws it into
# every frame — screencopy then shows it in both eyes.
export WLR_NO_HARDWARE_CURSORS=1
export XDG_CURRENT_DESKTOP=sway
exec sway -c "${XDG_CONFIG_HOME:-$HOME/.config}/sway-glasses/config"
