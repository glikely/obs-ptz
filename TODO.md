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
- Improve gamepad support
  - Add a button mapping configuration.
  - Add a modifier button to add more presets.
  - Add focus controls as an option.
  - Add deadzone support.
  - Support multiple gamepads.
  - Support different types of gamepads. (DirectInput)
  - Add support for linux and mac.
- Display current camera info in settings dialog (pan, tilt, picture, focus, etc)

Wishlist
--------

- Virtual PTZ for any source - use PTZ to translate & scale a source.
- Spacemouse support
- VISCA controller input support
