# OBS + ptzsim integration tests

Boots a real, plugin-loaded OBS Studio against a real `ptzsim` instance
and drives PTZ commands through the actual plugin code, not just the
emulator. Where `scripts/ptzsim`'s own manual testing confirms the wire
protocols are decoded correctly, this suite confirms obs-ptz's real
`PTZDevice` implementations produce those wire commands in the first
place, and that OBS's device-config loading and `ptz_action_source`
actually work end to end.

## How it works

1. **`ptzsim`** is started as a subprocess on fixed local ports, one
   camera exposing every backend under test (VISCA TCP/UDP/serial,
   Pelco), plus `--debug-http-port` so the tests can read back its state
   as JSON without decoding a camera protocol themselves.
2. **obs-ptz's own device config** -- the JSON file
   `PTZControls::LoadConfig()` reads from `obs_module_config_path
   ("config.json")` (see `src/ptz-controls.cpp`) -- is pre-written with
   one device per backend, each given an explicit `"id"` so the test
   knows each device's `device_id` up front (`PTZDevice::PTZDevice()`
   reads `id` straight from config; there's no auto-assignment to rely
   on). This is the *only* obs-ptz/OBS file this suite hand-writes --
   everything else about OBS's state (scenes, sources) is driven live
   over obs-websocket once OBS is running, specifically to avoid
   depending on OBS's own scene-collection JSON schema, which is
   internal and changes between versions.
3. **OBS Studio** is launched with an isolated `$HOME` (so it doesn't
   touch your real profile) and obs-websocket enabled via its documented
   environment variables (`OBS_WEBSOCKET_SERVER_ENABLE`,
   `_PORT`, `_PASSWORD`) -- no manual "enable websocket server" click
   needed.
4. Each test uses `obsws.py` (a from-scratch, dependency-light
   obs-websocket v5 client -- just the Hello/Identify handshake and
   Request/RequestResponse, see its docstring) to create a scene, add a
   `ptz_action_source` (`src/ptz-action-source.c`) configured for one
   device + one action, and switch to that scene. A `ptz_action_source`
   fires its configured action the moment it's added to what's already,
   or becomes, the current program scene (`ptz_action_source_activate()`
   with the default `PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE` trigger) --
   which calls straight into `proc_handler_call(..., "ptz_move_continuous"
   / "ptz_preset_recall" / "ptz_preset_save", ...)`, the same entry
   point a joystick move or a hotkey uses.
5. The test polls `ptzsim`'s `/state` endpoint until it sees the
   expected pan/tilt speed or position, or times out.

## Running locally

Needs a real OBS Studio with this plugin built and installed into it --
see the top-level `CMakeLists.txt` options; you'll want
`-DENABLE_SERIALPORT=ON` so the VISCA-serial and Pelco device types
exist at all (off by default). The CI workflow
(`.github/workflows/ptz-emulator-tests.yml`) shows the exact steps for
Ubuntu (`apt install obs-studio libobs-dev ...`, plain `cmake` build,
`cmake --install` straight into `/usr`) and macOS (`brew install --cask
obs`, `cmake --preset macos`, copy the built `.plugin` bundle into
`~/Library/Application Support/obs-studio/plugins/`).

Once OBS has the plugin installed:

```
pip install -r tests/obs-integration/requirements.txt
PTZSIM_OBS_BINARY=obs pytest tests/obs-integration -v
```

`PTZSIM_OBS_BINARY` defaults to `obs` on Linux and OBS.app's real binary
path on macOS; override it if yours lives elsewhere.

## Known limitations

This suite was written by tracing the plugin's actual source (device
config schema, `ptz_action_source`'s trigger behavior, the proc handler
signatures) and obs-websocket's documented protocol, but **it has not
been run against a real OBS Studio process** -- the environment this was
developed in has no OBS binary or display server available. The parts
most likely to need a first-run fix:

- **A first-run OBS dialog** (auto-config wizard, "check for updates"
  prompt, etc.) could block headless startup indefinitely. The fixture
  fails with a clear timeout rather than hanging forever, but doesn't
  attempt to suppress any such dialog -- OBS's exact first-run behavior
  varies by version and wasn't verified here.
- **The macOS plugin bundle path**: `cmake --preset macos` and the
  `.plugin` bundle's exact output location under `build_macos/` were
  read from `cmake/macos/helpers.cmake` and the existing
  `.github/scripts/build-macos`/`package-macos` scripts, not built.
- **Timing constants** (`wait_for_state` timeouts, the `time.sleep(0.5)`
  move durations in `test_preset_save_and_recall`) are reasonable
  guesses, not tuned against real hardware timing.

Treat the first CI run (or a local run) as the real validation pass, and
expect to adjust the OBS bootstrap step if it doesn't come up cleanly.
