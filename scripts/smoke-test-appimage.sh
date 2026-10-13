#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Starts an LCAD AppImage on a virtual display with a document, and checks that it stays up,
# draws a window, and logs no warnings or errors. CI runs this on a distribution other than the
# one the AppImage was built on, with neither Qt nor Open CASCADE installed.
#
# Usage: scripts/smoke-test-appimage.sh <LCAD.AppImage> <document.lcad> <screenshot.png>
#
# Needs Xvfb, ImageMagick, and Mesa's software OpenGL driver.
set -eu

if [ $# -ne 3 ]; then
    sed -n '8p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi
appimage=$(realpath "$1")
document=$(realpath "$2")
screenshot=$3

work=$(mktemp -d)
display=":98"
xvfb=""
app=""
cleanup() {
    [ -n "$app" ] && kill "$app" 2>/dev/null
    [ -n "$xvfb" ] && kill "$xvfb" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    echo "smoke-test-appimage.sh: $1" >&2
    if [ -f "$work/output.txt" ]; then
        echo "--- what LCAD printed:" >&2
        cat "$work/output.txt" >&2
    fi
    if [ -f "$work/state/lcad/lcad.log" ]; then
        echo "--- its log:" >&2
        cat "$work/state/lcad/lcad.log" >&2
    fi
    exit 1
}

# With -fbdir, Xvfb keeps the screen's pixels in a file, so no screenshot tool has to connect.
Xvfb "$display" -screen 0 1280x800x24 -fbdir "$work" >/dev/null 2>&1 &
xvfb=$!
sleep 2
kill -0 "$xvfb" 2>/dev/null || fail "Xvfb did not start."

# No FUSE in a container, so let the AppImage unpack itself. The private state directory keeps
# the log of this run apart from any other.
DISPLAY="$display" XDG_STATE_HOME="$work/state" LIBGL_ALWAYS_SOFTWARE=1 \
    APPIMAGE_EXTRACT_AND_RUN=1 APPIMAGELAUNCHER_DISABLE=1 \
    "$appimage" "$document" >"$work/output.txt" 2>&1 &
app=$!

sleep 15
kill -0 "$app" 2>/dev/null || fail "LCAD exited within 15 seconds of starting."

convert "xwd:$work/Xvfb_screen0" "$screenshot" || fail "The screen could not be captured."
colors=$(identify -format '%k' "$screenshot")
echo "The screenshot has $colors distinct colors."
# An empty screen is one color and a window without the 3D view a few dozen. A lit, shaded
# model with smoothed edges has thousands.
[ "$colors" -ge 1000 ] || fail "The window does not show a rendered model."

log="$work/state/lcad/lcad.log"
[ -f "$log" ] || fail "LCAD did not write its log."
if grep -E '^[^ ]+ (warning|error) ' "$log"; then
    fail "LCAD logged warnings or errors."
fi
echo "LCAD started from $(basename "$appimage"), opened $(basename "$document"), and drew it."
