#!/bin/sh
# tests/run-display-tier.sh — the display tier under a throwaway
# Xvfb (xvfb-run needs xauth, which minimal images lack).
#
# 1. window smoke test (opens, paints, presents)
# 2. STEP-2 app-level UI audit at 3 sizes x 2 scales, plus an
#    alternate font face when HAL_UI_ALT_FONT points at one —
#    every draw_text's ink rect is checked against the clip stack
#    and every menu popup against its widest item.
set -u

APP="$1"
SUPP="$2"
ALT_FONT="${HAL_UI_ALT_FONT:-}"

if ! command -v Xvfb >/dev/null 2>&1; then
    echo "test-display: Xvfb not found, skipping (install xvfb to run)"
    exit 0
fi

disp=90
while [ -e "/tmp/.X$disp-lock" ] || [ -e "/tmp/.X11-unix/X$disp" ]; do
    disp=$((disp + 1))
done
Xvfb ":$disp" -screen 0 1920x1080x24 >/dev/null 2>&1 &
xvfb_pid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -e "/tmp/.X11-unix/X$disp" ] && break
    sleep 0.2
done

status=0
DISPLAY=":$disp" LSAN_OPTIONS="suppressions=$SUPP" "$APP" --smoke \
    || status=$?

for cfg in "1920 1080 1.0" "1920 1080 2.0" "1366 768 1.0" \
           "1366 768 2.0" "1280 720 1.0" "1280 720 2.0"; do
    set -- $cfg
    echo "test-display: ui-audit $1x$2 @$3"
    DISPLAY=":$disp" LSAN_OPTIONS="suppressions=$SUPP" \
        "$APP" --ui-audit "$1" "$2" "$3" || status=$?
done

if [ -n "$ALT_FONT" ] && [ -f "$ALT_FONT" ]; then
    echo "test-display: ui-audit alternate font $ALT_FONT"
    DISPLAY=":$disp" HAL_UI_FONT="$ALT_FONT" \
        LSAN_OPTIONS="suppressions=$SUPP" \
        "$APP" --ui-audit 1920 1080 1.0 || status=$?
fi

kill "$xvfb_pid" 2>/dev/null
wait "$xvfb_pid" 2>/dev/null
exit "$status"
