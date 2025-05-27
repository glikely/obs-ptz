# Pan Tilt Zoom (PTZ) Controls for OBS Studio

[![Push](https://github.com/glikely/obs-ptz/actions/workflows/push.yaml/badge.svg)](https://github.com/glikely/obs-ptz/actions/workflows/push.yaml)

This is a plugin for controlling PTZ Cameras from OBS studio.

This plugin adds a new control dock window that can be used to control pan,
tilt, zoom camera directly from the OBS Studio main window.
It also tracks the current active scenes to automatically select the
correct camera for control and can be automated by adding PTZ Actions sources
to trigger camera actions when scenes change.

![PTZ Controls Screenshot](/doc/ptz-controls-screenshot.png?raw=true "OBS Studio PTZ Controls")

![PTZ Controls Screenshot](/doc/ptz-settings-screenshot.png?raw=true "OBS Studio PTZ Device Settings")

Features:

- Pan, Tilt, and Zoom controls
- Auto and manual focus
- Hotkeys control
- Joystick control
- Save and recall camera presets
- Control any number of cameras
- Auto select active camera based on active scene
- Control camera power
- Whitebalance control
- Supported protocols
  - VISCA
  - VISCA over UDP (Sony protocol)
  - VISCA over TCP (PTZ Optics and others)
  - Pelco-P
  - Pelco-D
  - ONVIF (experimental)
  - USB Cameras (Windows and Linux only)

[OBS project resource page](https://obsproject.com/forum/resources/ptz-controls.1284/)

[#obsptz on Twitter](https://twitter.com/hashtag/obsptz?s=09)

# User Guide

## Installation

Go to the releases page to find the latest binary release for your platform.
Binaries are created for Windows (x64), MacOS (Arm, x86_64, and Universal),
and Ubuntu Linux 22.04 (x86_64).
Download the package for your platform and install it.
If you need support for a different platform (e.g. Linux Arm) then you'll need
to follow the building from source instructions below.

[OBS PTZ Releases](https://github.com/glikely/obs-ptz/releases)

## Configuration

To show the controls dock, in the `Docks` menu select `PTZ Controls`.
The PTZ Controls window should appear.
You can drag the window to any side of the OBS Studio main window to dock it
into place, or just leave it floating.

Initially no PTZ cameras will be configured.
To add a camera, click on the gear icon at the bottom of the dock window,
or in the `Tools` menu select `PTZ Controls`.
The PTZ Settings window will appear.

The Settings window has three tabs, `General`, `Cameras` and `About`.
The `General` tab has settings that affect every camera.
The `Cameras` tab is where you add and remove cameras,
and change individual camera settings.
The `About` tab give some details about the plugin and what version is installed.

### Adding a Camera
The camera controller only handles the PTZ control of a video source.
You should add the camera's video source in OBS before adding PTZ control
to the plugin.

To add a camera, select the `Cameras` tab and click the `+` button at the bottom
of the window.
It will expand to a list of camera control connections that are available.
Select the control protocol that is used by your camera.
A new camera instance will be added to the list above.
Click on the new camera and the camera settings will appear on the right hand
side of the window.
You'll need to enter the camera connection details, either the network address
or serial port used for control.
Click the `Apply` button to connect to the camera.

Finally associate the camera with an OBS source by using the `Source` combo box.
This lets the plugin automatically select the right camera for control when
the preview or program scene changes in OBS.

### Removing a camera

To remove a camera, select the camera in the settings dialog can click the `-`
button in the toolbar.

## Controlling Cameras

Cameras are controlled with the arrow buttons in the control dock.
To move a camera, select it in the list of cameras
(bottom left of the control dock) and then click the control buttons above.
The arrow buttons will pan/tilt the camera,
The magnifing glass buttons will zoom in and out,
and the small/large buttons will chagne the focus.
You can also toggle autofocus on and off with the `AF` button and trigger
a one-touch refocus action.

Presets are listed on the right hand side of the dock.
Presets can be saved, recalled, and renamed from the dock window.

To save a preset, right click on the preset that you want to change and select
`Save Preset`.
Similarly, to rename a preset, right click and select `Rename Preset`,
or select `Clear Preset` to reset the name back to default.

Double click to recall a preset.

### Joystick Control

To enable joystick control, select the `Joystick Control` check box on the
`general` tab of the settings dialog.
All of the connected joysticks will be shown in the list box.
Click on the joystick that you want to use for camera control.
The mapping of controls to PTZ actions is shown in the settings dialog
to the right of the joystick list.

## Advanced Features

### Block Moves on Live Camera

In Studio mode, camera adjustments are usually set up with the source visible
in the Preview scene before being transitioned over to the live Program scene.
Manual camera movements are avoided on the Program scene because they can
be quite abrupt and unpleasant to watch.

OBS PTZ can by default block out manual moves of cameras visible in Program.
To enable this feature, check the `Lockout live PTZ moves in studio mode`
checkbox in the Setting dialog `About` tab.

With the feature enabled the pan, tilt, zoom and preset controls will be
disabled for any camera visible in Program, preventing live moves.
If you need to override the block and do a live movement anyway
then you can temporarily override the block by clicking the lock icon in
the toolbar.

### Debugging data

The plugin can generate a large amount of debug data with all the protocol
messages sent to and received by the cameras.
Debug logs appear in the main obs-studio log, but are disable by default.
To enable verbose debug logs, select `Enable debug logging` in the settings
dialog.

# Build Instructions

The build infrastructure for this project comes from the
[OBS Plugin Template](https://https://github.com/obsproject/obs-plugintemplate]
repo. To build this plugin, follow the instructions in the plugin template
[README.md](doc/plugin-template-README.md)

# Contributing

Contributions welcome!
You can submit changes as GitHub pull requests.
Or email patches to me at mailto:grant.likely@secretlab.ca

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

# Acknowledgements

Thank you to everyone who has contributed to his project, either with filing issues,
asking questions, or contributing to the code.
All code and documentation contributors are listed in [AUTHORS](AUTHORS).

Thank you also to the OBS Project developers, and the
[OBS Plugin Template](https://https://github.com/obsproject/obs-plugintemplate]
repo that they maintain. This plugin leans heavily on that project.

Joystick support uses the
[QJoystick library](https://github.com/alex-spataru/QJoysticks).

And finally, thank you to everyone who contributes to the Free and Open Source
Software that this project is built upon, including OBS Studio, Qt, Simple
DirectMedia Layer (SDL), Linux, and countless libraries and tools.
