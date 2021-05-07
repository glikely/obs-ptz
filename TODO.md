OBS PTZ Todo List
=================

This is a partial list of the features I'd like to add to this project.
Anybody who wants to pitch in and help implement these is most welcome.
Feel free to send me patches by email, or pull requests via github.

Core
----

- Fix all the memory leaks (obs_data allocations, PTZDevice instances, etc.)
- Replace PTZControls->cameras with a QAbstractListModel

PTZ Backend
-----------

- Add support for VISCA over IP
- Add support for other camera control protocols

User Interface
--------------

- Replace discrete buttons with a "virtual joystick" panel
- Add focus control
- Enhance gamepad support
  - Add zoom control
  - Add cycling through cameras
  - Add pan/tilt speed control
  - Add gamepad configuration (enable/disable, select gamepads)

Wishlist
--------

- Virtual PTZ for any source - use PTZ to translate & scale a source.
