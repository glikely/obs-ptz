#!/bin/bash
# Load the RPMs in ./rpms-out into a real OBS Studio in a Fedora container,
# on a virtual display (Xvfb), and check that OBS loads the plugin and creates
# its dock. Run docker-test.sh first. Screenshots land in ./load-test-out.
#   packaging/fedora/obs-load-test.sh [fedora-version]   (default: rawhide)
# Set KEEP=1 to leave OBS running afterwards and watch it over VNC at
# localhost:${VNC_PORT:-5901} (e.g. `open vnc://localhost:5901`); stop it with
# `docker rm -f obs-ptz-view`. PLATFORM and BASE_IMAGE work as for docker-test.sh.
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${PLATFORM:-}" ]; then
    case $(docker info --format '{{.Architecture}}') in
        aarch64|arm64) PLATFORM=linux/arm64 ;;
        x86_64|amd64)  PLATFORM=linux/amd64 ;;
        *) echo "set PLATFORM to the docker platform to use" >&2; exit 1 ;;
    esac
fi
ver=${1:-rawhide}
base=${BASE_IMAGE:-registry.fedoraproject.org/fedora}
name=obs-ptz-view
want=$(sed -n 's/^%global upstream_version[[:space:]]*//p' obs-ptz.spec)
mkdir -p load-test-out
dx() { docker exec "$name" bash -c "$1"; }
shot() { dx "DISPLAY=:99 import -window root /tmp/$1.png" && docker cp "$name:/tmp/$1.png" load-test-out/"$1.png"; }
fail() { echo "FAIL: $*" >&2; docker logs "$name" 2>&1 | tail -5 >&2 || true
         [ -n "${KEEP:-}" ] || docker rm -f "$name" >/dev/null 2>&1; exit 1; }

docker rm -f "$name" >/dev/null 2>&1 || true
docker run -d --name "$name" --platform "$PLATFORM" --user root --shm-size=1g \
    -p "127.0.0.1:${VNC_PORT:-5901}:5900" \
    -v "$PWD/rpms-out":/rpms:ro -v "$PWD/obs-load-in-container.sh":/setup.sh:ro \
    --entrypoint bash "$base:$ver" /setup.sh >/dev/null

echo "Installing obs-studio and the RPM, starting OBS..."
for _ in $(seq 1 120); do
    state=$(dx 'cat /tmp/state 2>/dev/null' || true)
    [ -n "$state" ] && break
    sleep 5
done
[ "$state" = READY ] || { dx 'tail -20 /tmp/setup.log' || true; fail "container setup ($state)"; }

echo "Waiting for OBS to load the plugin..."
# OBS names its log "YYYY-MM-DD HH-MM-SS.txt", with a space, so keep it quoted
log='"$(ls -t /home/obs/.config/obs-studio/logs/*.txt 2>/dev/null | head -1)"'
for _ in $(seq 1 60); do
    dx "grep -q 'plugin loaded successfully' $log" 2>/dev/null && break
    sleep 3
done
loaded=$(dx "grep -m1 'plugin loaded successfully' $log" || true)
[ -n "$loaded" ] || fail "OBS did not load the plugin"
echo "  $loaded"
case $loaded in *"(version $want)"*) ;; *) fail "plugin version is not $want";; esac

# The dock is created hidden; OBS lists it as a window named "PTZ Controls"
sleep 3
dx 'DISPLAY=:99 xdotool search --name "PTZ Controls"' >/dev/null || fail 'no "PTZ Controls" dock'
echo '  "PTZ Controls" dock present'
if dx "grep -qiE 'crash|segfault|abort' $log"; then fail "crash reported in the OBS log"; fi
echo "  no crash in the OBS log"

# Screenshots: the main window, then the dock enabled from the Docks menu. The
# coordinates assume the fixed 1600x900 display, so these steps are best effort
# and not part of the pass/fail result.
shot obs-main
dx 'export DISPLAY=:99; xdotool mousemove 146 35 click 1; sleep 1; xdotool mousemove 189 322 click 1; sleep 3' || true
shot obs-dock
echo "Screenshots in packaging/fedora/load-test-out/"

if [ -n "${KEEP:-}" ]; then
    echo "OBS is still running: open vnc://localhost:${VNC_PORT:-5901}   (stop: docker rm -f $name)"
else
    docker rm -f "$name" >/dev/null
fi
echo "OBS load test passed"
