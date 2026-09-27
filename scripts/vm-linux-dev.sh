#!/bin/bash
# Build and run obs-ptz on the Parallels "Ubuntu 24.04 ARM64" VM, from
# whatever checkout (branch, worktree, even uncommitted changes) this
# script is run from on the Mac host.
#
# Why this exists: getting a branch onto the VM and actually running it
# against a real OBS + obs-websocket is fiddly. This script handles
# everything needed to run it against the current tree.
#
# Usage (run from anywhere; it locates the repo root from its own path):
#   scripts/vm-linux-dev.sh setup              # one-time per checkout: share it to the VM
#   scripts/vm-linux-dev.sh build [--ui-tests] [extra cmake args...]
#   scripts/vm-linux-dev.sh install            # install the freshly built .so as the VM's system obs-ptz plugin
#   scripts/vm-linux-dev.sh test [pytest args] # run tests/obs-integration against it (implies install)
#   scripts/vm-linux-dev.sh run                # install the plugin and print how to launch OBS yourself on the VM
#   scripts/vm-linux-dev.sh clean              # kill stray obs/Xvfb/ptzsim processes left over from a previous run
#   scripts/vm-linux-dev.sh restore            # put the VM's original obs-ptz.so back
#
# Every subcommand implies `setup`. `build`/`install`/`test`/`run` always
# reflect this checkout's *current on-disk* files -- including uncommitted
# changes -- since the "push" is a live Parallels shared folder, not a git
# push. That's normally what you want when iterating on a branch; if you
# specifically need a clean-tree build, commit/stash first.
#
# Env overrides: VM (Parallels VM name, default below).

set -eu

VM="${VM:-Ubuntu 24.04 ARM64}"
SRC_MAC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHARE_NAME="ptzdev-$(basename "$SRC_MAC")"
SRC_VM="/media/psf/$SHARE_NAME"
BUILD_VM="$SRC_VM/build_vm"
VENV_VM="/home/parallels/.venvs/$SHARE_NAME"
PLUGIN_PATH="/usr/lib/aarch64-linux-gnu/obs-plugins/obs-ptz.so"

ensure_shared_folder() {
	if ! prlctl list -i "$VM" 2>/dev/null | grep -qF "$SHARE_NAME (+) path='$SRC_MAC'"; then
		echo "vm-linux-dev: sharing $SRC_MAC to \"$VM\" as \"$SHARE_NAME\"" >&2
		prlctl set "$VM" --shf-host-add "$SHARE_NAME" --path "$SRC_MAC" >&2
	fi
}

# Writes a script (its stdin) into the shared folder -- so both sides can
# see it, and so we dodge prlctl exec's documented quoting gotchas with
# inline bash -c strings -- and runs it on the VM as the given user
# (default root). Extra args are passed through to that script.
run_vm_script() {
	name="$1"; user="$2"; shift 2
	path="$SRC_VM/.vm-linux-dev-$name.sh"
	cat > "$SRC_MAC/.vm-linux-dev-$name.sh"
	if [ "$user" = "root" ]; then
		prlctl exec "$VM" bash "$path" "$@"
	else
		prlctl exec "$VM" runuser -u "$user" -- bash "$path" "$@"
	fi
}

cmd_build() {
	cmake_extra=""
	for a in "$@"; do
		if [ "$a" = "--ui-tests" ]; then
			cmake_extra="$cmake_extra -DENABLE_UI_TESTS=ON"
		else
			cmake_extra="$cmake_extra $a"
		fi
	done
	run_vm_script build root <<EOF
set -e
cmake -S "$SRC_VM" -B "$BUILD_VM" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo $cmake_extra
cmake --build "$BUILD_VM"
echo BUILD_OK
EOF
}

cmd_install() {
	run_vm_script install root <<EOF
set -e
if [ ! -f "$PLUGIN_PATH.orig-backup" ]; then
	cp "$PLUGIN_PATH" "$PLUGIN_PATH.orig-backup"
fi
cp "$BUILD_VM/obs-ptz.so" "$PLUGIN_PATH"
chmod 755 "$PLUGIN_PATH"
echo INSTALL_OK
EOF
}

cmd_restore() {
	run_vm_script restore root <<EOF
set -e
if [ -f "$PLUGIN_PATH.orig-backup" ]; then
	cp "$PLUGIN_PATH.orig-backup" "$PLUGIN_PATH"
	echo RESTORE_OK
else
	echo "no $PLUGIN_PATH.orig-backup found -- nothing to restore" >&2
	exit 1
fi
EOF
}

cmd_clean() {
	run_vm_script clean root <<'EOF'
pkill -9 -x obs 2>/dev/null || true
pkill -9 -x Xvfb 2>/dev/null || true
pkill -9 -f "python3? -m ptzsim" 2>/dev/null || true
sleep 1
ps -ef | grep -E "obs$|Xvfb|ptzsim" | grep -v grep || true
echo CLEAN_OK
EOF
}

cmd_venv() {
	run_vm_script venv parallels <<EOF
set -e
if [ ! -x "$VENV_VM/bin/pytest" ]; then
	python3 -m venv "$VENV_VM"
	"$VENV_VM/bin/pip" install -q -r "$SRC_VM/tests/obs-integration/requirements.txt"
fi
echo VENV_OK
EOF
}

cmd_test() {
	cmd_install
	cmd_venv
	# No logged-in GNOME session is the normal state of this VM, so this
	# falls back to xvfb-run, same as tests/obs-integration/conftest.py
	# itself does on Linux with no DISPLAY set. That's fine for these
	# tests (confirmed: a full run completes in well under a minute) --
	# but it's software-rendered under this VM's 2 vCPUs, a known-flaky
	# combination for anything heavier (see this script's own header, and
	# CLAUDE.md's "Building and running on the Ubuntu 24.04 ARM64 VM"
	# section, for why `run` below hands you the real-session command
	# instead of also defaulting to Xvfb). Always `clean` afterward: a
	# killed/timed-out run leaves Xvfb/obs orphans that starve the *next*
	# run instead of just failing it outright.
	status=0
	run_vm_script test parallels <<EOF || status=$?
export HOME=/home/parallels
cd "$SRC_VM/tests/obs-integration"
"$VENV_VM/bin/python" -m pytest $*
EOF
	cmd_clean
	return "$status"
}

cmd_run() {
	cmd_install
	cat <<EOF
vm-linux-dev: plugin installed on "$VM". No GUI is provided by this
script -- log into the VM's own console (open its Parallels window, sign
in as parallels) and run this yourself there, so OBS gets the real
GPU-accelerated session instead of a starved software-rendered Xvfb:

    PTZ_UI_TEST_HARNESS=1 OBS_WEBSOCKET_SERVER_ENABLE=true obs --disable-updater

ptzsim, if you need a simulated camera, is at $SRC_VM/scripts/ptzsim
(run \`python3 -m ptzsim\` from $SRC_VM/scripts).
EOF
}

main() {
	cmd="${1:-}"
	[ -n "$cmd" ] && shift || true
	ensure_shared_folder
	case "$cmd" in
	setup) echo "vm-linux-dev: $SRC_MAC is shared to \"$VM\" as \"$SHARE_NAME\" ($SRC_VM)" ;;
	build) cmd_build "$@" ;;
	install) cmd_install ;;
	restore) cmd_restore ;;
	clean) cmd_clean ;;
	test) cmd_test "$@" ;;
	run) cmd_run ;;
	*)
		echo "usage: $0 {setup|build [--ui-tests]|install|test [pytest args]|run|clean|restore}" >&2
		exit 2
		;;
	esac
}

main "$@"
