#!/bin/bash
# Runs tests/usb-e2e/usb_e2e.py: a real USB UVC PTZ camera, through real OBS and
# the obs-ptz plugin built from this checkout. See README.md.
#
# Like scripts/run-macos-integration-tests.sh, this has to give OBS a config of
# its own, because on macOS OBS ignores every documented way of doing that (see
# scripts/macos-integration-test-obs-wrapper.py). So it backs up what a run can
# change, moves the real profiles and scene collections out of the way, swaps in
# the plugin from build_macos, and puts everything back afterwards, whatever
# happens. Unlike that script it does NOT assume the profile is named
# "Untitled": every profile and scene collection is moved aside.
#
# usage: tests/usb-e2e/run-macos.sh [--skip-build] [--camera <AVFoundation unique ID>]

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OBS_APP="${PTZSIM_OBS_APP:-/Applications/OBS.app}"
OBS_BINARY="$OBS_APP/Contents/MacOS/OBS"
CFG="$HOME/Library/Application Support/obs-studio"
VENV="${XDG_CACHE_HOME:-$HOME/Library/Caches}/obs-ptz-macos-integration-venv"
PLUGIN_BUILD="$REPO_ROOT/build_macos/RelWithDebInfo/obs-ptz.plugin"
POSITION_TOOL="$REPO_ROOT/build_macos/tests/usb-e2e/RelWithDebInfo/usb-uvc-position"

# What a run can change. Everything in the config directory is backed up and
# restored except OBS's browser cache (plugin_config/obs-browser, which is large
# and only a cache) and the other plugins (large binaries this never touches);
# only obs-ptz.plugin, which the run swaps, is kept from plugins/.
FILTERS=(
    --include='/plugins/' --include='/plugins/obs-ptz.plugin/' --include='/plugins/obs-ptz.plugin/**'
    --exclude='/plugins/*' --exclude='/plugin_config/obs-browser/'
)

die() { echo "error: $*" >&2; exit 1; }

SKIP_BUILD=0
CAMERA="${USB_E2E_CAMERA:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1 ;;
        --camera) shift; CAMERA="${1:-}" ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done

[ -x "$OBS_BINARY" ] || die "OBS not found at $OBS_BINARY (set PTZSIM_OBS_APP to override)"
pgrep -x OBS >/dev/null && die "OBS is running -- quit it first (this needs to control its config, and the camera)"

if [ -z "$CAMERA" ]; then
    # A USB camera's AVFoundation unique ID is "0x" and hex digits; the built-in
    # camera and virtual cameras have UUIDs.
    CANDIDATES="$(system_profiler SPCameraDataType 2>/dev/null | sed -n 's/^ *Unique ID: \(0x[0-9a-fA-F]*\)$/\1/p')"
    [ -n "$CANDIDATES" ] || die "no USB camera found (system_profiler SPCameraDataType lists none)"
    [ "$(echo "$CANDIDATES" | wc -l)" -eq 1 ] || die "more than one USB camera; pick one with --camera:
$CANDIDATES"
    CAMERA="$CANDIDATES"
fi
echo "==> Camera: $CAMERA"

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "==> Building the plugin and usb-uvc-position (build_macos must be configured with -DENABLE_USB_TESTS=ON -DENABLE_UI_TESTS=ON)"
    cmake --build "$REPO_ROOT/build_macos" --config RelWithDebInfo --target obs-ptz usb-uvc-position -- -quiet || die "build failed"
fi
[ -d "$PLUGIN_BUILD" ] || die "no plugin at $PLUGIN_BUILD"
[ -x "$POSITION_TOOL" ] || die "no $POSITION_TOOL (configure with -DENABLE_USB_TESTS=ON)"
strings -a "$PLUGIN_BUILD/Contents/MacOS/obs-ptz" | grep -q 'UVC PTZ ranges' || die "the built plugin has no USB camera support"

if ! "$VENV/bin/python" -c "import websockets" >/dev/null 2>&1; then
    echo "==> Setting up the Python environment"
    python3 -m venv --clear "$VENV"
    "$VENV/bin/pip" install -q -r "$REPO_ROOT/tests/obs-integration/requirements.txt" || die "pip install failed"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/obs-ptz-usb-e2e.XXXXXX")"
BACKUP="$WORK/backup"
ASIDE="$WORK/aside"
mkdir -p "$BACKUP" "$ASIDE" "$CFG"

echo "==> Backing up $CFG"
rsync -a "${FILTERS[@]}" "$CFG/" "$BACKUP/" || die "backup failed"
# rsync -c compares contents; anything it lists is a difference
DIFFS="$(rsync -rnc --delete --itemize-changes "${FILTERS[@]}" "$BACKUP/" "$CFG/")"
[ -z "$DIFFS" ] || die "the backup doesn't match the original:
$DIFFS"
echo "    backed up and verified ($(du -sh "$BACKUP" | cut -f1))"

restore() {
    echo "==> Restoring your OBS config"
    pkill -TERM -x OBS 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x OBS >/dev/null || break; sleep 1; done
    pgrep -x OBS >/dev/null && { pkill -9 -x OBS; sleep 1; }
    LOG="$(ls -t "$CFG"/logs/*.txt 2>/dev/null | head -1)"
    [ -n "$LOG" ] && cp "$LOG" "$WORK/obs.log"
    rsync -a --delete "${FILTERS[@]}" "$BACKUP/" "$CFG/"
    DIFFS="$(rsync -rnc --delete --itemize-changes "${FILTERS[@]}" "$BACKUP/" "$CFG/")"
    if [ -z "$DIFFS" ]; then
        echo "    restored, and verified identical to the backup"
        rm -rf "${BACKUP:?}" "${ASIDE:?}"
    else
        echo "    !! the config still differs from the backup, which is kept in $BACKUP:"
        echo "$DIFFS"
    fi
    echo "    logs from this run are in $WORK"
}
trap restore EXIT

echo "==> Moving every profile and scene collection aside so OBS starts with a fresh one"
for entry in "$CFG"/basic/profiles/* "$CFG"/basic/scenes/*; do
    [ -e "$entry" ] && mv "$entry" "$ASIDE/$(basename "$(dirname "$entry")")-$(basename "$entry")"
done
# A leftover from a crash would make OBS stop at a Safe Mode dialog nobody can dismiss
if [ -d "$CFG/.sentinel" ]; then
    for entry in "$CFG"/.sentinel/*; do [ -e "$entry" ] && mv "$entry" "$ASIDE/sentinel-$(basename "$entry")"; done
fi

echo "==> Putting this checkout's plugin in place"
mkdir -p "$CFG/plugins"
[ -e "$CFG/plugins/obs-ptz.plugin" ] && mv "$CFG/plugins/obs-ptz.plugin" "$ASIDE/obs-ptz.plugin.installed"
cp -R "$PLUGIN_BUILD" "$CFG/plugins/obs-ptz.plugin" && xattr -cr "$CFG/plugins/obs-ptz.plugin" || die "could not install the plugin"

echo "==> Running the test"
PTZSIM_REAL_OBS_BINARY="$OBS_BINARY" "$VENV/bin/python" "$REPO_ROOT/tests/usb-e2e/usb_e2e.py" \
    "$REPO_ROOT" "$WORK" "$CAMERA" "$POSITION_TOOL"
RESULT=$?
echo "==> Test exit code: $RESULT"
exit $RESULT
