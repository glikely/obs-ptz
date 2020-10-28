OBS PTZ Todo List
=================

This is a partial list of the features I'd like to add to this project.
Anybody who wants to pitch in and help implement these is most welcome.
Feel free to send me patches by email, or pull requests via github.

Multiple Camera Support
-----------------------

- Add multiple camera instances
- Add support for multiple VISCA interfaces (multiple UARTs)
- Add support for linking cameras to scenes
  - Choose PTZ target based on scene visibility in program or preview
  - User control for selecting PTZ target (preview/program/manual)

PTZ Backend
-----------

- Refactor libvisca into libptz with VISCA as backend protocol
- Add support for VISCA over IP
- Add support for other camera control protocols

User Interface
--------------

- Replace discrete buttons with a "virtual joystick" panel
- Add joystick support
- Add zoom control
- Add focus control
- Add configuration dialog for adding/removing/modifying cameras
