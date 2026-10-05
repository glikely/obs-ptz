OBS PTZ Todo List
=================

This is a partial list of the features I'd like to add to this project.
Anybody who wants to pitch in and help implement these is most welcome.
Feel free to send me patches by email, or pull requests via github.

Core
----

- Remove the legacy self-managed device migration, two releases after the
  first one that has it. Delete `src/ptz-legacy-migration.*`, its calls in
  `PTZControls::LoadConfig()`, `SaveConfig()` and `handleFrontendEvent()`,
  `ptz_device_startup()`. Before that, the integration suite's devices
  (`write_ptz_plugin_config()` in `tests/obs-integration/conftest.py`, which
  writes the old `config.json`) have to be made as filters in a scene
  collection instead, and `test_legacy_migration.py` goes
- Replace PTZControls->cameras with a QAbstractListModel
- Add generic properties infrastructure so each camera can expose
  different settings
- Fix display of transient window on startup

PTZ Backend
-----------

- Add support for other camera control protocols

VISCA
-----

- Add badge showing when a camera is non-responsive
- Reorganize settings dialog to show VISCA-over-SERIAL hierarchy

User Interface
--------------

- Add a virtual joystick alternative to the discrete direction buttons
- Add focus control
- Implement gamepad support
  - Find replacement for QGamePad
  - Add zoom control
  - Add cycling through cameras
  - Add pan/tilt speed control
  - Add gamepad configuration (enable/disable, select gamepads)
- Show more of the camera's state on the settings dialog's status view (picture,
  exposure, etc). Connection, live/preview, position and white balance are there

Wishlist
--------

- Virtual PTZ for any source - use PTZ to translate & scale a source.
- Spacemouse support
- VISCA controller input support
