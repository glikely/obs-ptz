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
6. `test_preset_import_export.py` covers the preset export/import feature.
   It triggers the import/export context menu action. However, it passes
   a filename at call time as the tests cannot drive the file dialog.
   Needs the plugin built with `-DENABLE_UI_TESTS=ON` (`tests/ui-harness/`
   is entirely inert otherwise) and OBS launched with
   `PTZ_UI_TEST_HARNESS=1`, which `conftest.py`'s `obs_world` fixture
   always sets -- a no-op if the binary wasn't built with that option, so
   it doesn't affect the rest of the suite.
7. `test_device_status.py` covers camera connect/disconnect detection via
   `tests/ui-harness/device-status-test.cpp`'s `get_device_status` test
   (same `-DENABLE_UI_TESTS=ON` requirement as above), using its own
   disposable `flaky_ptzsim` fixture -- a second, VISCA-TCP-only `ptzsim`
   instance a test can kill and restart -- rather than the session-scoped
   `ptzsim` every other test shares, since stopping *that* one would break
   the rest of the suite.
8. `test_absolute_relative_moves.py` covers absolute/relative pan-tilt
   moves and absolute zoom (`PTZDevice::move_abs()`/`move_rel()`), via
   `tests/ui-harness/move-device-test.cpp`'s `move_device` test (same
   `-DENABLE_UI_TESTS=ON` requirement as above) -- there's no
   `ptz_action_source` action type for either, so obs-websocket alone
   can't reach them.
9. `test_autofocus_white_balance.py` covers autofocus on/off
   (`PTZDevice::set_autofocus()`) and VISCA white balance mode
   (`PTZVisca::set()`'s `"wb_mode"` handling), via
   `tests/ui-harness/set-device-test.cpp`'s `set_device` test (same
   `-DENABLE_UI_TESTS=ON` requirement as above), read back through
   `get_device_status`.

## Running locally

Needs a real OBS Studio with this plugin built and installed into it --
see the top-level `CMakeLists.txt` options; you'll want
`-DENABLE_SERIALPORT=ON` so the VISCA-serial and Pelco device types
exist at all (off by default), and `-DENABLE_UI_TESTS=ON` if you want
`test_preset_import_export.py` to actually find anything to drive
(`test_ptz_backends.py` works fine without it).

**macOS**: real OBS on macOS ignores every isolation mechanism this
suite relies on and always loads your live profile, so don't run pytest
against it directly -- use `scripts/run-macos-integration-tests.sh`
instead, which builds the plugin, backs up and restores your whole OBS
profile around the run, and redirects obs-ptz's/obs-websocket's config to
where real OBS actually reads it (see that script's and
`scripts/macos-integration-test-obs-wrapper.py`'s docstrings for why).
Last verified end to end this way (20/20 passing, against OBS 32.2.1)
before the preset export/import suite dropped its vendor-request tests
in favor of testing only through the real UI action, and
on_actionPresetImport_triggered() was fixed to restore device selection
after its model reset (see `src/ptz-controls.cpp`'s own comment there) --
not yet re-run against that change; expect 14/14 (the vendor-request
tests are gone, see `test_preset_import_export.py`'s own docstring) if
it holds.

**Linux**: `$HOME` isolation actually works, so a plain

```
apt install obs-studio libobs-dev  # or your distro's equivalents
cmake --preset ubuntu-<arch> -DENABLE_SERIALPORT=ON -DENABLE_ONVIF=ON -DENABLE_UI_TESTS=ON
cmake --build --preset ubuntu-<arch>
cp build_<arch>/obs-ptz.so /usr/lib/<arch>-linux-gnu/obs-plugins/  # real file, not a symlink -- OBS's plugin scan skips symlinks
pip install -r tests/obs-integration/requirements.txt
PTZSIM_OBS_BINARY=obs pytest tests/obs-integration -v
```

is enough -- no wrapper script needed. Verified end to end this way
(14/14 passing) against Ubuntu 24.04's packaged OBS 30.0.2, run under a
throwaway `Xvfb` (confirmed a viable fallback for this suite
specifically, since every test here drives OBS purely over
obs-websocket and never needs synthetic X11 input) in 22s; `Xvfb`'s
software (`llvmpipe`) rendering is a known way to starve OBS's main
thread on a low-core-count VM (see the top-level `CLAUDE.md`), so prefer
a real accelerated GNOME/Xwayland session where one's available.

`PTZSIM_OBS_BINARY` defaults to `obs` on Linux and OBS.app's real binary
path on macOS; override it if yours lives elsewhere.

## Known limitations

- **Timing constants** (`wait_for_state` timeouts, the `time.sleep(0.5)`
  move durations in `test_preset_save_and_recall`) are reasonable
  guesses tuned against `ptzsim`, not real camera hardware timing.
