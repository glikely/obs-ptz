# Pan Tilt Zoom (PTZ) Controls for OBS Studio

[![Push](https://github.com/glikely/obs-ptz/actions/workflows/push.yaml/badge.svg)](https://github.com/glikely/obs-ptz/actions/workflows/push.yaml)
[![Crowdin](https://badges.crowdin.net/obs-ptz/localized.svg)](https://crowdin.com/project/obs-ptz)

This is a plugin for controlling PTZ Cameras from OBS studio.

This plugin adds a new control dock window that can be used to control pan,
tilt, zoom camera directly from the OBS Studio main window.
It also tracks the current active scenes to automatically select the
correct camera for control and can be automated by adding PTZ Actions sources
to trigger camera actions when scenes change.

![PTZ Controls Screenshot](/doc/ptz-controls-screenshot.png?raw=true "OBS Studio PTZ Controls")

![PTZ Controls Screenshot](/doc/ptz-settings-screenshot.png?raw=true "OBS Studio PTZ Device Settings")

Features:

- Adjuts camera Pan, Tilt, Zoom and Focus settings
- Toggle between manual and auto focus modes
- Assign hotkeys to camera controls
- Use a joystick to control comera position
- Save and recall camera presets
- Control multiple cameras from OBS
- Auto select active camera based on active scene
- Control camera power
- Adjust camera whitebalance
- Supports multiple camera control protocols, including:
  - VISCA (RS232, RS422, UDP and TCP)
  - Pelco-P
  - Pelco-D
  - ONVIF (experimental)
  - USB Cameras (Windows and Linux only)

## Websites
- [OBS project resource page](https://obsproject.com/forum/resources/ptz-controls.1284/)
- [PTZ Controls on GitHub](https://github.com/glikely/obs-ptz)
- [PTZ Controls on Crowdin (translations)](https://crowdin.com/project/obs-ptz)

# User Guide

## Installation

Go to the releases page to find the latest binary release for your platform.
Binaries are created for Windows (x64), MacOS (Arm, x86_64, and Universal),
and Ubuntu Linux 24.04 (x86_64).
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
In this plugin, cameras are associated with OBS Studio video sources.
To add a camera, you should first add a source for the camera's video feed in
the OBS Studio `Sources` dock.
Once you've got the camera video working, add PTZ controls to the source in the
PTZ Controls settings dialog `Tools->PTZ Devices'.

On the `Cameras` tab, click the `+` button in the bottom toolbar to add a
device.
It will expand to a list of camera control connections that are available.
Select the control protocol that is used by your camera.
A new entry will be added to the device list.
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
To adjuat a camera, you needs to be selected from the camera list in the PTZ
dock (bottom left of the control dock).
By default (if `Auto Select Active Camera` is enabled in settings), then the
plugin will automatically select the correct camera when the current scene
changes.
Then, clicking the camera control buttons will adjust the camera position.
The arrow buttons will pan/tilt the camera,
The magnifing glass buttons will zoom in and out,
and the small/large buttons will change the focus.
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
Joystick axis can be mapped to Pan, Tilt, Zoom or Focus.
Joystick buttons can be mapped to any OBS hotkey action.

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
To enable debug logs, select `Write protocol trace to OBS log file` in the
device settings, and run OBS Studio with the --verbose command line option.

# Building from Source

The build infrastructure for this project comes from the
[OBS Plugin Template](https://https://github.com/obsproject/obs-plugintemplate]
repo. To build this plugin, follow the instructions in the plugin template
[README.md](doc/plugin-template-README.md)

## Linux Quickstart

This project should be easy to build on any Linux distro once OBS Studio and
all of the build dependencies are installed on your machine.
Check your distributions documentation for instructions on how to do this.
Then you can build the plugin with cmake commands:

```
$ cmake -B build
$ cmake --build build --config RelWithDebInfo
$ cmake --install build --config RelWithDebInfo
```

On Ubuntu 24.04, you can also use the GitHub action CI script to build the
plugin, which will also download and install all the build dependencies for you.

```
$ ./.github/scripts/build-ubuntu
$ cp release/RelWithDebInfo /usr/
```

## MacOS Quickstart

The easiest way to build the plugin is to use the CMake presets.
Use the following commands to configure, build and install the plugin in your
OBS Studio plugins directory.

```
$ cmake -B build_macos --preset macos
$ cmake --build build_macos --config RelWithDebInfo
$ cmake --install build_macos --config RelWithDebInfo
```

## Windows Quickstart

Easiest way to build for windows is to use the GitHub actions build script.
First install Visual Studio and CMake as described in the obs-plugintemplate
documentation linked above.
Then open the `x64 Native Tools Command Prompt for VS 2022` and run the following commands:

```
c:\> pwsh
PS > cd path/to/obs-ptz
PS > $env:ci=1
PS > .github/scripts/Build-Windows.ps1
```

# Contributing

Contributions welcome!
You can submit changes as GitHub pull requests.
See the github pull request page for details.
https://github.com/glikely/obs-ptz/pulls

Help is also needed to translate into other languages.
Go to the Crowdin project page to help: [PTZ Controls on Crowdin](https://crowdin.com/project/obs-ptz)

See [CONTRIBUTING.md](CONTRIBUTING.md) for more details.

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
