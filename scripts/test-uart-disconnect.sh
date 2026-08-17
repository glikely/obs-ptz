#!/usr/bin/env bash
#
# Exercises PTZUARTWrapper's UART disconnect/reconnect handling
# (src/uart-wrapper.cpp) against a real, running OBS on macOS or Linux, using
# a socat-created virtual serial port pair to simulate a device disappearing
# and later reappearing mid-session.
#
# The test device is configured with socat's stable symlink path (not the
# /dev/ttysNNN or /dev/pts/N it resolves to, which usually changes on every
# socat restart) so that killing and restarting socat simulates the same
# physical device being unplugged and replugged at the same path, the same
# way a real USB-serial adapter's path is normally stable across a
# reconnect.
#
# PTZUARTWrapper's reconnect timer (see src/uart-wrapper.cpp) retries open()
# directly rather than gating on serial_cpp::list_ports() first, specifically
# so this kind of test works: list_ports_osx.cc enumerates via IOKit
# (IOServiceMatching(kIOSerialBSDServiceValue)), the mechanism real
# USB-serial adapters register through, and a plain BSD pty from socat never
# registers there - with a list_ports()-gated design this script could only
# ever test disconnect, never reconnect, since a port list_ports() can't see
# would never get open() called on it again.
#
# There is no working way to fully sandbox OBS's config directory on macOS
# ($HOME overrides are ignored - OBS resolves it via a Cocoa API, not the
# environment), so on both platforms this script temporarily adds one test
# device to your REAL plugin_config/obs-ptz/config.json alongside your
# existing devices, and always restores the original afterwards (verified
# byte-for-byte) no matter how the script exits. OBS is only ever killed
# with SIGKILL, never a graceful quit, so it can never autosave over the
# restore.
#
# Prerequisites:
#   - socat (macOS: brew install socat; Linux: apt install socat)
#   - macOS: the plugin already built and installed to
#     release/RelWithDebInfo/obs-ptz.plugin, with your real
#     ~/Library/Application Support/obs-studio/plugins/obs-ptz.plugin
#     symlinked to it (a one-time setup - so a rebuild+reinstall here is
#     immediately what OBS loads, no copy step needed):
#       cmake --build build_macos --target obs-ptz --config RelWithDebInfo
#       cmake --install build_macos --prefix "$(pwd)/release/RelWithDebInfo" --config RelWithDebInfo
#   - Linux: the plugin already built and copied over your real installed
#     obs-ptz.so (wherever `dpkg -L obs-ptz` or your distro's package
#     manager put it, typically
#     /usr/lib/<multiarch-triplet>/obs-plugins/obs-ptz.so). Unlike the
#     macOS symlink above, this has to be a real copy, and redone after
#     every rebuild: OBS's plugin directory scan on Linux skips symlinks
#     (readdir()'s d_type reports a symlink as DT_LNK, not DT_REG, and the
#     scan doesn't follow it - a symlinked-in build silently never loads,
#     confirmed the hard way). Keep the copy world-readable, since it's
#     normally root:root from the system package but OBS runs as your
#     regular desktop user:
#       cmake --build "build_$(uname -m)" --target obs-ptz
#       sudo cp "$(pwd)/build_$(uname -m)/rundir/RelWithDebInfo/obs-ptz.so" /usr/lib/"$(dpkg-architecture -qDEB_HOST_MULTIARCH)"/obs-plugins/obs-ptz.so
#       sudo chmod 755 /usr/lib/"$(dpkg-architecture -qDEB_HOST_MULTIARCH)"/obs-plugins/obs-ptz.so

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$(uname -s)" in
Darwin)
	OS=macos
	PLUGIN_CHECK="$REPO_ROOT/release/RelWithDebInfo/obs-ptz.plugin"
	PLUGIN_BUILD_HELP="  cmake --build build_macos --target obs-ptz --config RelWithDebInfo
  cmake --install build_macos --prefix \"$REPO_ROOT/release/RelWithDebInfo\" --config RelWithDebInfo"
	CONFIG="$HOME/Library/Application Support/obs-studio/plugin_config/obs-ptz/config.json"
	SENTINEL_DIR="$HOME/Library/Application Support/obs-studio/.sentinel"
	OBS_APP="/Applications/OBS 32.2.1.app/Contents/MacOS/OBS"
	SOCAT_INSTALL_HELP="brew install socat"
	;;
Linux)
	OS=linux
	PLUGIN_CHECK="$REPO_ROOT/build_$(uname -m)/rundir/RelWithDebInfo/obs-ptz.so"
	PLUGIN_BUILD_HELP="  cmake --build \"$REPO_ROOT/build_$(uname -m)\" --target obs-ptz"
	CONFIG="$HOME/.config/obs-studio/plugin_config/obs-ptz/config.json"
	SENTINEL_DIR="$HOME/.config/obs-studio/.sentinel"
	OBS_APP="obs"
	SOCAT_INSTALL_HELP="apt install socat"
	;;
*)
	echo "Unsupported OS: $(uname -s)" >&2
	exit 1
	;;
esac

TEST_DEVICE_ID=999999
TEST_DEVICE_NAME="SOCAT-DISCONNECT-TEST-DELETE-ME"
# The reconnect timer's retry interval (reconnect_poll_interval_ms in
# src/uart-wrapper.cpp) - how long to allow for a reconnect to be noticed.
RECONNECT_TIMEOUT_S=30

SCRATCH="$(mktemp -d)"
BACKUP="$SCRATCH/config.json.backup"
OBS_LOG="$SCRATCH/obs_stdout.log"
VPORT1="$SCRATCH/vport1"
VPORT2="$SCRATCH/vport2"

SOCAT_PID=""
OBS_PID=""

cleanup() {
	echo "--- cleaning up ---"
	if [[ -n "$OBS_PID" ]] && kill -0 "$OBS_PID" 2>/dev/null; then
		kill -9 "$OBS_PID" 2>/dev/null || true
	fi
	if [[ -n "$SOCAT_PID" ]] && kill -0 "$SOCAT_PID" 2>/dev/null; then
		kill "$SOCAT_PID" 2>/dev/null || true
	fi
	# A SIGKILL never runs OBS's own clean-shutdown handler, which is what
	# normally deletes its crash-sentinel file (see
	# CrashHandler::applicationShutdownHandler() in frontend/utility/
	# CrashHandler.cpp) - left behind, that sentinel is exactly what makes
	# the *next* launch show the "crash or unclean shutdown detected"
	# dialog. Delete it ourselves so repeated runs of this script don't
	# require clicking through that dialog every time.
	rm -f "$SENTINEL_DIR"/run_* 2>/dev/null || true
	if [[ -f "$BACKUP" ]]; then
		cp -p "$BACKUP" "$CONFIG"
		if ! cmp -s "$BACKUP" "$CONFIG"; then
			echo "FATAL: config restore verification failed - your original is safe at: $BACKUP" >&2
			echo "Restore it manually with: cp \"$BACKUP\" \"$CONFIG\"" >&2
			exit 1
		fi
		echo "Real obs-ptz config restored and verified byte-for-byte."
	fi
	rm -rf "$SCRATCH"
}
trap cleanup EXIT INT TERM

if ! command -v socat >/dev/null 2>&1; then
	echo "socat not found - install with: $SOCAT_INSTALL_HELP" >&2
	exit 1
fi
if [[ ! -f "$CONFIG" ]]; then
	echo "No existing obs-ptz config found at: $CONFIG" >&2
	exit 1
fi
if [[ ! -e "$PLUGIN_CHECK" ]]; then
	echo "Plugin not built/installed yet. Run:" >&2
	echo "$PLUGIN_BUILD_HELP" >&2
	exit 1
fi
if pgrep -x obs >/dev/null 2>&1 || pgrep -f "OBS.app/Contents/MacOS/OBS" >/dev/null 2>&1; then
	echo "OBS already appears to be running - quit it first so this script controls a single, known instance." >&2
	exit 1
fi

# Starts (or restarts) socat with a stable symlink pair at $VPORT1/$VPORT2,
# waiting for the symlinks to appear. Safe to call again after a previous
# socat instance has exited - stale symlinks are removed first so socat
# doesn't balk at the paths already existing.
start_socat() {
	rm -f "$VPORT1" "$VPORT2"
	socat -d -d "pty,raw,echo=0,link=$VPORT1" "pty,raw,echo=0,link=$VPORT2" >"$SCRATCH/socat.log" 2>&1 &
	SOCAT_PID=$!
	for _ in $(seq 1 20); do
		[[ -L "$VPORT1" ]] && return 0
		sleep 0.25
	done
	echo "FAIL: socat did not create the virtual port in time" >&2
	cat "$SCRATCH/socat.log" >&2
	exit 1
}

# Polls `lsof -p $OBS_PID` for up to $2 seconds for the port $VPORT1
# currently resolves to (re-read each call, since it changes across socat
# restarts) to show up as an open fd. Prints PASS/FAIL with the label $1.
wait_for_port_open() {
	local label="$1" timeout_s="$2" target resolved elapsed=0
	target="$(readlink "$VPORT1")"
	while (( elapsed < timeout_s )); do
		resolved="$(lsof -p "$OBS_PID" 2>/dev/null | grep -oF "$target" || true)"
		if [[ -n "$resolved" ]]; then
			echo "PASS: $label (open on $target after ${elapsed}s)"
			return 0
		fi
		sleep 1
		elapsed=$((elapsed + 1))
	done
	echo "FAIL: $label - port never opened within ${timeout_s}s (check $OBS_LOG)" >&2
	return 1
}

echo "--- backing up real obs-ptz config ---"
cp -p "$CONFIG" "$BACKUP"

echo "--- starting socat virtual serial port pair ---"
start_socat
echo "stable port path: $VPORT1 -> $(readlink "$VPORT1")"

echo "--- adding test device to real config ---"
python3 - "$CONFIG" "$VPORT1" "$TEST_DEVICE_ID" "$TEST_DEVICE_NAME" <<'PYEOF'
import json, sys
path, port, device_id, name = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
with open(path) as f:
    data = json.load(f)
data.setdefault("devices", []).append({
    "name": name,
    "id": device_id,
    "type": "visca",
    "port": port,
    "baud_rate": 9600,
    "address": 1,
})
with open(path, "w") as f:
    json.dump(data, f, indent=4)
PYEOF

echo "--- launching OBS ---"
"$OBS_APP" --disable-updater --disable-missing-files-check >"$OBS_LOG" 2>&1 &
if [[ "$OS" == linux ]]; then
	# obs on Linux re-execs/forks internally, so $! here is not reliably
	# the long-running process - find the real one by name instead.
	disown
	for _ in $(seq 1 20); do
		OBS_PID="$(pgrep -x obs || true)"
		[[ -n "$OBS_PID" ]] && break
		sleep 0.25
	done
	if [[ -z "$OBS_PID" ]]; then
		echo "FAIL: obs process never appeared" >&2
		exit 1
	fi
else
	OBS_PID=$!
fi

echo "--- waiting for obs-ptz to load ---"
plugin_loaded=0
for _ in $(seq 1 60); do
	if grep -q "obs-ptz\] plugin loaded successfully" "$OBS_LOG" 2>/dev/null; then
		plugin_loaded=1
		break
	fi
	sleep 0.5
done
if [[ "$plugin_loaded" -ne 1 ]]; then
	echo "FAIL: obs-ptz plugin never reported loaded (check $OBS_LOG)" >&2
	exit 1
fi

wait_for_port_open "initial open" 10 || exit 1

echo "--- killing socat to simulate the device disappearing ---"
kill "$SOCAT_PID"
SOCAT_PID=""

echo "--- waiting for the disconnect to be logged ---"
disconnect_logged=0
for _ in $(seq 1 20); do
	if grep -qF "UART $VPORT1 disappeared" "$OBS_LOG" 2>/dev/null; then
		disconnect_logged=1
		break
	fi
	sleep 0.5
done
if [[ "$disconnect_logged" -eq 1 ]]; then
	echo "PASS: disconnect was logged:"
	grep -F "UART $VPORT1 disappeared" "$OBS_LOG"
else
	echo "FAIL: no disconnect log message seen within timeout" >&2
	echo "--- relevant OBS log lines ---" >&2
	grep "obs-ptz\]" "$OBS_LOG" >&2 || true
	exit 1
fi

echo "--- restarting socat at the same path to simulate the device returning ---"
start_socat
echo "stable port path: $VPORT1 -> $(readlink "$VPORT1")"

wait_for_port_open "reconnect" "$RECONNECT_TIMEOUT_S" || exit 1

echo "--- checking the reconnect was logged ---"
# "UART $VPORT1 connected" logs on every successful open(), including the
# initial one earlier in this script, so a reconnect needs a *second*
# occurrence, not just any match.
reconnect_logged=0
for _ in $(seq 1 10); do
	if [[ "$(grep -cF "UART $VPORT1 connected" "$OBS_LOG" 2>/dev/null || true)" -ge 2 ]]; then
		reconnect_logged=1
		break
	fi
	sleep 0.5
done
if [[ "$reconnect_logged" -eq 1 ]]; then
	echo "PASS: reconnect was logged:"
	grep -F "UART $VPORT1 connected" "$OBS_LOG"
else
	echo "FAIL: no second 'connected' log message seen after reconnect (check $OBS_LOG)" >&2
	exit 1
fi

echo ""
echo "Both disconnect and reconnect handling verified."
