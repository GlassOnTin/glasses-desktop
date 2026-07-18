#!/bin/bash
# Runs inside the sway-glasses session (exec'd from its config, or launched
# manually with SWAYSOCK set). Reconciliation loop enforcing, every second:
#   - every desktop workspace (everything except "mirror") on HEADLESS-1
#     (an output EDID flap makes sway evacuate/steal workspaces)
#   - workspace "mirror" on the physical output and visible there
#   - the mirror client: one glasses-presenter (draws both eyes itself and
#     follows 2D/SBS live), or without it N wl-mirror windows (2 when the
#     output is double-wide, else 1)
#   - wf-panel-pi alive in this session (it can crash on hotplug)
# After repairs, cursor wiggles force fresh frames (wlroots renders on
# damage only; a static desktop leaves new mirrors black).
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/glasses-desktop"
mkdir -p "$STATE"
LOG="$STATE/setup.log"
exec >>"$LOG" 2>&1
echo "=== setup $$ $(date) ==="

# only one watcher may run. flock, not pgrep: command-substitution
# subshells and launching shells share this script's cmdline, so any
# pattern match kills innocents (including the launcher itself).
LOCK="$STATE/watcher.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
    oldpid=$(cat "$LOCK.pid" 2>/dev/null)
    [ -n "$oldpid" ] && kill "$oldpid" 2>/dev/null && echo "killed stale watcher $oldpid"
    sleep 1
    flock -n 9 || { echo "another watcher holds the lock, exiting"; exit 1; }
fi
echo $$ >"$LOCK.pid"

# prefer the depth presenter; wl-mirror is the fallback
if command -v glasses-presenter >/dev/null; then
    MIRROR_APP=glasses-presenter
    MIRROR_CMD=glasses-presenter
else
    MIRROR_APP=at.yrlf.wl_mirror
    MIRROR_CMD='wl-mirror HEADLESS-1'
fi
export MIRROR_APP

if ! swaymsg -t get_outputs -r | grep -q '"HEADLESS-1"'; then
    swaymsg create_output
fi
swaymsg output HEADLESS-1 enable
# single quoted argument so swaymsg's own getopt never sees '--custom'
swaymsg 'output HEADLESS-1 mode --custom 1920x1080'
swaymsg output HEADLESS-1 position 0 0
for i in 1 2 3 4; do swaymsg "workspace $i output HEADLESS-1"; done
swaymsg workspace 1
swaymsg exec foot

# Prints one line:
#   NOPHYS
#   MODESET <phys> <WxH@Hz>
#   OK|FIX <phys> <W> <H> <want> <have> <mirror_ok:1|0> <stray_ws_csv|->
state() {
    swaymsg -t get_outputs -r >/tmp/gd-outputs.json 2>/dev/null || { echo IPCFAIL; return; }
    swaymsg -t get_workspaces -r >/tmp/gd-ws.json 2>/dev/null
    swaymsg -t get_tree -r >/tmp/gd-tree.json 2>/dev/null
    python3 - <<'PYEOF'
import json
try:
    outs = json.load(open('/tmp/gd-outputs.json'))
    wss = json.load(open('/tmp/gd-ws.json'))
    tree = json.load(open('/tmp/gd-tree.json'))
except Exception:
    print('NOPHYS'); raise SystemExit
phys = next((o for o in outs
             if not o['name'].startswith('HEADLESS') and o.get('active')), None)
if not phys:
    print('NOPHYS'); raise SystemExit
w, h = phys['rect']['width'], phys['rect']['height']
best = max(phys.get('modes', []) or [{'width': w, 'height': h, 'refresh': 60000}],
           key=lambda m: m['width'])
if best['width'] > w:
    print(f"MODESET {phys['name']} {best['width']}x{best['height']}@{round(best['refresh']/1000)}Hz")
    raise SystemExit
import os
app = os.environ.get('MIRROR_APP', 'at.yrlf.wl_mirror')
# one presenter handles both modes; wl-mirror needs one window per eye
want = 1 if app == 'glasses-presenter' else (2 if w >= 2 * h else 1)

# workspace placement: 'mirror' on phys AND visible there; all others on HEADLESS-1
mirror_ok, strays = False, []
for ws in wss:
    if ws['name'] == 'mirror':
        mirror_ok = (ws['output'] == phys['name'] and ws.get('visible', False))
    elif not ws['name'].startswith('__') and ws['output'] != 'HEADLESS-1':
        strays.append(ws['name'])

have = 0
def count(n):
    global have
    if n.get('app_id') == app:
        have += 1
    for c in n.get('nodes', []) + n.get('floating_nodes', []):
        count(c)
for out in tree.get('nodes', []):
    for ws in out.get('nodes', []):
        if ws.get('type') == 'workspace' and ws.get('name') == 'mirror':
            count(ws)

ok = mirror_ok and have == want and not strays
print('OK' if ok else 'FIX', phys['name'], w, h, want, have,
      int(mirror_ok), ','.join(strays) or '-')
PYEOF
}

panel_alive() {
    for pid in $(pgrep -x wf-panel-pi); do
        if grep -qz "WAYLAND_DISPLAY=$WAYLAND_DISPLAY" "/proc/$pid/environ" 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

ipc_fails=0
while :; do
    read -r status a b c d e f g <<< "$(state)"
    case "$status" in
    IPCFAIL|'')
        ipc_fails=$((ipc_fails + 1))
        if [ "$ipc_fails" -ge 5 ]; then
            echo "$(date +%T) sway gone, watcher exiting"
            exit 0
        fi
        ;;
    MODESET)
        ipc_fails=0
        echo "$(date +%T) modeset: $a -> $b"
        swaymsg "output $a mode $b"
        sleep 2
        ;;
    FIX)
        ipc_fails=0
        echo "$(date +%T) fix: phys=$a ${b}x${c} want=$d have=$e mirror_ok=$f strays=$g"
        # 1. send stray desktop workspaces home
        if [ "$g" != "-" ]; then
            IFS=, read -ra STRAYS <<< "$g"
            for ws in "${STRAYS[@]}"; do
                swaymsg "workspace $ws"
                swaymsg 'move workspace to output HEADLESS-1'
            done
        fi
        # 2. mirror workspace on the physical output, visible
        swaymsg "workspace mirror output $a"
        swaymsg 'workspace mirror'
        swaymsg "move workspace to output $a"
        # 3. right number of mirror windows (comm is truncated to 15
        #    chars, so "glasses-presenter" matches as "glasses-present")
        pkill -x wl-mirror
        pkill -x glasses-present
        sleep 0.5
        for _ in $(seq "$d"); do
            swaymsg exec "$MIRROR_CMD"
            sleep 0.7
        done
        # 4. focus back to the desktop (mirror stays visible on phys)
        swaymsg 'workspace 1'
        # 5. force fresh frames (single-arg quoting: swaymsg must not
        #    parse '-3' as its own option)
        for _ in 1 2 3 4 5; do
            swaymsg 'seat seat0 cursor move 3 0'
            swaymsg 'seat seat0 cursor move -3 0'
            sleep 0.6
        done
        sleep 1
        ;;
    OK)
        ipc_fails=0
        if command -v wf-panel-pi >/dev/null && ! panel_alive; then
            echo "$(date +%T) panel dead, respawning"
            swaymsg exec wf-panel-pi
            sleep 2
        fi
        ;;
    esac
    sleep 1
done
