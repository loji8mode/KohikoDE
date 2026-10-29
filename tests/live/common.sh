# Shared helpers for the live tests (tests/test_*_live.sh): the REAL
# kohiko binary running as an actual window manager under Xvfb, driven
# through its real IPC (kohikoctl) with real X11 clients
# (tests/live/x11_test_client) - nothing mocked.
#
# Source this after setting KOHIKO_BIN, KOHIKOCTL and TEST_CLIENT (and
# optionally CHOOSER). Everything a test creates lives under one
# throwaway $WORKDIR, including $HOME and every XDG directory, so a run
# can never read or modify a real user's config, session or data.

pass=0
fail=0

check()
{
    # check <actual> <expected> <message>
    if [ "$1" = "$2" ]; then
        pass=$((pass + 1)); echo "  PASS: $3"
    else
        fail=$((fail + 1)); echo "  FAIL: $3 (expected [$2], got [$1])"
    fi
}

check_true()
{
    # check_true <message> <command...>   - passes when the command succeeds
    msg=$1; shift
    if "$@"; then
        pass=$((pass + 1)); echo "  PASS: $msg"
    else
        fail=$((fail + 1)); echo "  FAIL: $msg"
    fi
}

check_lt()
{
    # check_lt <value> <limit> <message> - integer compare, decimals truncated
    v=${1%%.*}
    if [ -n "$v" ] && [ "$v" -lt "$2" ] 2>/dev/null; then
        pass=$((pass + 1)); echo "  PASS: $3 ($1 < $2)"
    else
        fail=$((fail + 1)); echo "  FAIL: $3 (got [$1], limit $2)"
    fi
}

live_require()
{
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not available."; exit 0; }
    done

    [ -x "$KOHIKO_BIN" ]  || { echo "SKIP: $KOHIKO_BIN not built.";  exit 0; }
    [ -x "$KOHIKOCTL" ]   || { echo "SKIP: $KOHIKOCTL not built.";   exit 0; }
    [ -x "$TEST_CLIENT" ] || { echo "SKIP: $TEST_CLIENT not built."; exit 0; }
}

LIVE_PIDS=""
XVFB_PID=""
WM_PID=""

live_cleanup()
{
    for p in $LIVE_PIDS; do kill "$p" 2>/dev/null; done
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    sleep 0.2
    [ -n "${WORKDIR:-}" ] && rm -rf "$WORKDIR"
}

# live_init <display base> - fresh X server + isolated environment.
live_init()
{
    WORKDIR=$(mktemp -d)
    DISPLAY_NUM=$(($1 + ($$ % 40)))
    export DISPLAY=":$DISPLAY_NUM"

    export HOME="$WORKDIR/home"
    export XDG_RUNTIME_DIR="$WORKDIR/runtime"
    export XDG_DATA_HOME="$WORKDIR/data"
    export XDG_CONFIG_HOME="$WORKDIR/home/.config"
    # Nothing from the real machine's /etc/xdg/autostart (flameshot,
    # nm-applet, ...) may start inside a test.
    export XDG_CONFIG_DIRS="$WORKDIR/no-system-autostart"
    unset XDG_SESSION_ID

    mkdir -p "$HOME/.config/kohiko" "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME"
    chmod 700 "$XDG_RUNTIME_DIR"

    CONF="$HOME/.config/kohiko/kohiko.conf"
    DATA_DIR="$XDG_DATA_HOME/kohiko"

    # The window manager spawns kohiko-choose-window / kohikoctl by name.
    PATH="$(dirname "$KOHIKO_BIN"):$(dirname "$KOHIKOCTL"):$PATH"
    export PATH

    trap live_cleanup EXIT INT TERM

    # Also remove the socket: a previous run that reused this display
    # number leaves /tmp/.X11-unix/X<n> behind, and the readiness wait
    # below (which looks for exactly that file) would then return at
    # once, before the NEW server exists.
    rm -f "/tmp/.X${DISPLAY_NUM}-lock" "/tmp/.X11-unix/X${DISPLAY_NUM}"
    Xvfb "$DISPLAY" -screen 0 1920x1080x24 > "$WORKDIR/xvfb.log" 2>&1 &
    XVFB_PID=$!

    waited=0
    while [ ! -e "/tmp/.X11-unix/X${DISPLAY_NUM}" ] && [ "$waited" -lt 50 ]; do
        sleep 0.1; waited=$((waited + 1))
    done

    # The socket file exists a moment BEFORE the server accepts
    # connections; a client that connects in that gap is refused and
    # exits. When a probe is available (x11_probe), wait until it can
    # really connect.
    if [ -n "${PROBE:-}" ] && [ -x "${PROBE:-}" ]; then
        waited=0
        while ! "$PROBE" focus >/dev/null 2>&1 && [ "$waited" -lt 50 ]; do
            sleep 0.1; waited=$((waited + 1))
        done
    fi
}

live_write_config()
{
    printf '%s\n' "$1" > "$CONF"
}

# Starts kohiko in the background (extra args: VAR=value env assignments).
live_start_wm()
{
    : > "$WORKDIR/kohiko.log"
    env "$@" "$KOHIKO_BIN" >> "$WORKDIR/kohiko.log" 2>&1 &
    WM_PID=$!

    waited=0
    while ! grep -q '\[INFO\] ready' "$WORKDIR/kohiko.log" 2>/dev/null && [ "$waited" -lt 80 ]; do
        sleep 0.1; waited=$((waited + 1))
        kill -0 "$WM_PID" 2>/dev/null || break
    done

    if ! kill -0 "$WM_PID" 2>/dev/null; then
        echo "FAIL: kohiko did not start"; cat "$WORKDIR/kohiko.log"; exit 1
    fi
}

# Asks kohiko to quit through its normal path (saves the session).
live_quit_wm()
{
    "$KOHIKOCTL" dispatch quit >/dev/null 2>&1
    i=0
    while kill -0 "$WM_PID" 2>/dev/null && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    WM_PID=""
}

# SIGKILL: a crash - no Shutdown(), nothing saved.
live_crash_wm()
{
    kill -9 "$WM_PID" 2>/dev/null
    wait "$WM_PID" 2>/dev/null
    WM_PID=""
}

clients_lines() { "$KOHIKOCTL" clients 2>/dev/null | sed 's/},{/}\n{/g'; }

# live_field <class> <field> - first managed window of that class.
live_field()
{
    clients_lines | grep "\"class\":\"$1\"" | head -1 \
        | sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p" | tr -d '"'
}

# live_title_field <title> <field> - same, but picks the window by title.
live_title_field()
{
    clients_lines | grep "\"title\":\"$1\"" | head -1 \
        | sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p" | tr -d '"'
}

# Smallest / largest x among the tiled (not floating, not fullscreen)
# windows on a workspace.
live_extreme_x()
{
    clients_lines | grep "\"workspace\":$2," | grep '"floating":false' | grep '"fullscreen":false' \
        | sed -n 's/.*"x":\([-0-9]*\),.*/\1/p' | sort -n | { [ "$1" = min ] && head -1 || tail -1; }
}
live_extreme_y()
{
    clients_lines | grep "\"workspace\":$2," | grep '"floating":false' | grep '"fullscreen":false' \
        | sed -n 's/.*"y":\([-0-9]*\),.*/\1/p' | sort -n | { [ "$1" = min ] && head -1 || tail -1; }
}

# live_wait_for <seconds*10> <command...> - polls until it succeeds.
live_wait_for()
{
    n=$1; shift
    i=0
    while [ "$i" -lt "$n" ]; do
        "$@" >/dev/null 2>&1 && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

class_listed() { [ -n "$(live_field "$1" id)" ]; }

# live_open <class> [client args...] - starts a window with that class
# and title and waits until kohiko manages it. PID in LAST_PID.
live_open()
{
    cls=$1; shift
    "$TEST_CLIENT" --title "$cls" --class "$cls" --width 400 --height 300 "$@" \
        > "$WORKDIR/client_$cls.log" 2>&1 &
    LAST_PID=$!
    LIVE_PIDS="$LIVE_PIDS $LAST_PID"
    live_wait_for 60 class_listed "$cls" || echo "  (note: $cls never appeared in kohikoctl clients)"
    sleep 0.15
}

# live_close <pid> <class> - closes it and waits until kohiko forgets it.
live_close()
{
    kill "$1" 2>/dev/null
    wait "$1" 2>/dev/null
    i=0
    while class_listed "$2" && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
}

# --- X-server-side observation and real input (tests/live/x11_probe.cpp,
# tests/live/x11_input.cpp). Set PROBE / INPUT before calling these; both
# are plain Xlib clients, so they report what the X server actually has
# rather than what Kohiko believes. -----------------------------------------

# live_stack_names - mapped, non-override-redirect top-level windows by
# name, BOTTOM TO TOP (XQueryTree order is the stacking order).
live_stack_names()
{
    "$PROBE" tree | grep ' state=mapped or=0 ' | sed -n 's/.* name=\(.*\)$/\1/p'
}

# live_stack_pos <name> - 1-based position from the bottom, empty if absent.
live_stack_pos()
{
    live_stack_names | grep -nx "$1" | head -1 | cut -d: -f1
}

# live_is_above <upper> <lower> - true when <upper> is higher in the real X
# stacking order than <lower>.
live_is_above()
{
    up=$(live_stack_pos "$1"); lo=$(live_stack_pos "$2")
    [ -n "$up" ] && [ -n "$lo" ] && [ "$up" -gt "$lo" ]
}

# live_pixel <x> <y> - the colour actually on screen there, rrggbb.
live_pixel() { "$PROBE" pixel "$1" "$2"; }

# live_hex <r,g,b> - the rrggbb the test client's --color paints.
live_hex() { printf '%02x%02x%02x' $(echo "$1" | tr ',' ' '); }

live_finish()
{
    echo
    echo "$pass passed, $fail failed"
    [ "$fail" -eq 0 ]
}
