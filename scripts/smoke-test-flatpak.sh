#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Starts the installed LCAD Flatpak on a virtual display with a document, and checks that it
# stays up, draws a window, and logs no warnings or errors. CI runs this after installing the
# bundle that scripts/build-flatpak.sh makes.
#
# Usage: scripts/smoke-test-flatpak.sh <document.lcad> <screenshot.png>
#
# Needs Xvfb and ImageMagick. The document has to be where the Flatpak may read it: in the home
# folder.
set -eu

if [ $# -ne 2 ]; then
    sed -n '8p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi
document=$(realpath "$1")
screenshot=$2
app=io.github.cheapazhobbies.LCAD
# Where the Flatpak keeps LCAD's state, the log among it.
log="$HOME/.var/app/$app/.local/state/lcad/lcad.log"

work=$(mktemp -d)
display=":97"
xvfb=""
started=""
cleanup() {
    [ -n "$started" ] && flatpak kill "$app" 2>/dev/null
    [ -n "$xvfb" ] && kill "$xvfb" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    echo "smoke-test-flatpak.sh: $1" >&2
    if [ -f "$work/output.txt" ]; then
        echo "--- what LCAD printed:" >&2
        cat "$work/output.txt" >&2
    fi
    if [ -f "$log" ]; then
        echo "--- its log:" >&2
        cat "$log" >&2
    fi
    exit 1
}

rm -f "$log"
# With -fbdir, Xvfb keeps the screen's pixels in a file, so no screenshot tool has to connect.
Xvfb "$display" -screen 0 1280x800x24 -fbdir "$work" >/dev/null 2>&1 &
xvfb=$!
sleep 2
kill -0 "$xvfb" 2>/dev/null || fail "Xvfb did not start."

flatpak run --env=DISPLAY="$display" --env=LIBGL_ALWAYS_SOFTWARE=1 "$app" "$document" \
    >"$work/output.txt" 2>&1 &
started=yes

sleep 20
flatpak ps --columns=application | grep -q "^$app\$" ||
    fail "LCAD exited within 20 seconds of starting."

convert "xwd:$work/Xvfb_screen0" "$screenshot" || fail "The screen could not be captured."
colors=$(identify -format '%k' "$screenshot")
echo "The screenshot has $colors distinct colors."
# An empty screen is one color and a window without the 3D view a few dozen. A lit, shaded
# model with smoothed edges has thousands.
[ "$colors" -ge 1000 ] || fail "The window does not show a rendered model."

[ -f "$log" ] || fail "LCAD did not write its log."
if grep -E '^[^ ]+ (warning|error) ' "$log"; then
    fail "LCAD logged warnings or errors."
fi
echo "LCAD started from the Flatpak, opened $(basename "$document"), and drew it."
