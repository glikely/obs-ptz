OBS PTZ Todo List
=================

This is a partial list of the features I'd like to add to this project.
Anybody who wants to pitch in and help implement these is most welcome.
Feel free to send me patches by email, or pull requests via github.

Core
----

- Replace PTZControls->cameras with a QAbstractListModel
- Add generic properties infrastructure so each camera can expose
  different settings
- Fix display of transient window on startup
- Fix gamepad detection on Windows
  - Ideally get QGamepad to work with DirectInput instead of XInput

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
- Enhance gamepad support
  - Add zoom control
  - Add cycling through cameras
  - Add pan/tilt speed control
  - Add gamepad configuration (enable/disable, select gamepads)
- Display current camera info in settings dialog (pan, tilt, picture, focus, etc)

Wishlist
--------

- Virtual PTZ for any source - use PTZ to translate & scale a source.
- Spacemouse support
- VISCA controller input support
