#!/usr/bin/env python3
"""PTZSIM_OBS_BINARY wrapper for scripts/run-macos-integration-tests.sh.

tests/obs-integration/conftest.py isolates its OBS instance by setting
$HOME to a throwaway directory and passing OBS_WEBSOCKET_SERVER_* env
vars -- both of which real OBS on macOS ignores. It resolves its config
directory through a Qt/Cocoa API that doesn't consult $HOME, and this
obs-websocket build only honors those env vars on a genuinely first-ever
launch (once its own config.json exists, further launches use the saved
values instead). obs-ptz's device config and obs-websocket's server
config both end up living in the REAL, shared
~/Library/Application Support/obs-studio directory regardless of what
conftest.py intended.

This wrapper bridges that gap: it copies the obs-ptz device config
conftest.py wrote under the isolated $HOME into the real path, writes
obs-websocket's config directly with the port/password conftest.py
picked, and then execs the real OBS binary. The calling script is
responsible for backing up and restoring the real directory around
this -- this wrapper only ever overwrites two config.json files that
get restored afterward.
"""
import json
import os
import pwd
import sys

isolated_home = os.environ["HOME"]
# os.path.expanduser("~") consults $HOME first, which is the isolated
# dir here -- the opposite of what's needed. Go straight to the passwd
# db, the same source OBS's own NSHomeDirectory()-based resolution uses.
real_home = pwd.getpwuid(os.getuid()).pw_dir
real_obs_binary = os.environ.get(
    "PTZSIM_REAL_OBS_BINARY", "/Applications/OBS.app/Contents/MacOS/OBS")

isolated_ptz_config = os.path.join(
    isolated_home, "Library", "Application Support", "obs-studio",
    "plugin_config", "obs-ptz", "config.json")
real_ptz_config = os.path.join(
    real_home, "Library", "Application Support", "obs-studio",
    "plugin_config", "obs-ptz", "config.json")

with open(isolated_ptz_config) as f:
    ptz_config = f.read()
os.makedirs(os.path.dirname(real_ptz_config), exist_ok=True)
with open(real_ptz_config, "w") as f:
    f.write(ptz_config)

real_ws_config = os.path.join(
    real_home, "Library", "Application Support", "obs-studio",
    "plugin_config", "obs-websocket", "config.json")
ws_config = {
    "alerts_enabled": False,
    "auth_required": False,
    "first_load": False,
    "server_enabled": True,
    "server_password": os.environ.get("OBS_WEBSOCKET_SERVER_PASSWORD", ""),
    "server_port": int(os.environ["OBS_WEBSOCKET_SERVER_PORT"]),
}
os.makedirs(os.path.dirname(real_ws_config), exist_ok=True)
with open(real_ws_config, "w") as f:
    json.dump(ws_config, f)

os.execv(real_obs_binary, [real_obs_binary] + sys.argv[1:])
