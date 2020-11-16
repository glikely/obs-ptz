OBS PTZ Todo List
=================

This is a partial list of the features I'd like to add to this project.
Anybody who wants to pitch in and help implement these is most welcome.
Feel free to send me patches by email, or pull requests via github.

Multiple Camera Support
-----------------------

Nothing at the moment!

PTZ Backend
-----------

- Refactor libvisca into libptz with VISCA as backend protocol
- Add support for VISCA over IP
- Add support for other camera control protocols
- Refactor backend to run in separate thread, or at least be non-blocking

User Interface
--------------

- Add configuration dialog or other way to add/remove/edit cameras
- Replace discrete buttons with a "virtual joystick" panel
- Add focus control
- Add configuration dialog for adding/removing/modifying cameras
- Enhance gamepad support
  - Add zoom control
  - Add cycling through cameras
  - Add pan/tilt speed control
  - Add gamepad configuration (enable/disable, select gamepads)

Wishlist
--------
- Virtual PTZ for any source - use PTZ to translate & scale a source.
