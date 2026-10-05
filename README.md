# Pan Tilt Zoom (PTZ) Controls for OBS Studio

[![Push](https://github.com/glikely/obs-ptz/actions/workflows/push.yaml/badge.svg)](https://github.com/glikely/obs-ptz/actions/workflows/push.yaml)
[![Crowdin](https://badges.crowdin.net/obs-ptz/localized.svg)](https://crowdin.com/project/obs-ptz)

This is a plugin for controlling PTZ Cameras from OBS Studio.

It adds a control dock to the OBS Studio main window for pan, tilt, zoom,
focus and presets, and tracks the active scenes to select the right camera
automatically. Cameras are added as a `PTZ Control` filter on the source that
shows the camera's video, so each camera's settings and presets stay with its
source. Camera actions can also be automated with PTZ Actions sources that
trigger when scenes change.

![PTZ Controls Screenshot](/docs/ptz-controls-screenshot.png?raw=true "OBS Studio PTZ Controls")

![PTZ Controls with thumbnail presets](/docs/ptz-presets-screenshot.png?raw=true "Presets shown as thumbnails")

![PTZ Controls Screenshot](/docs/ptz-settings-screenshot.png?raw=true "OBS Studio PTZ Device Settings")

Features:

- Pan, tilt, zoom and focus control
- Save and recall presets, shown as a list or as a grid of thumbnails
- Control multiple cameras, and auto select the camera for the active scene
- Lock out moves on cameras that are live in Studio Mode
- Control power, white balance and tally lamps
- Assign hotkeys to camera controls, or use a joystick or the onscreen joystick
- Detect cameras on the network (ONVIF and Sony VISCA over IP)
- Supports multiple camera control protocols, including:
  - VISCA (RS232, RS422, UDP and TCP)
  - Pelco-P
  - Pelco-D
  - ONVIF (experimental)
  - USB Cameras (UVC)

## Websites
- [OBS project resource page](https://obsproject.com/forum/resources/ptz-controls.1284/)
- [PTZ Controls on GitHub](https://github.com/glikely/obs-ptz)
- [PTZ Controls on Crowdin (translations)](https://crowdin.com/project/obs-ptz)

# User Guide

## Installation

Go to the releases page to find the latest binary release for your platform.
Binaries are created for Windows (x64 and arm64), MacOS (Universal), and
Ubuntu Linux 24.04 and 26.04 (x86_64 and aarch64).
Download the package for your platform and install it.
If you need support for a different platform or distro then you'll need
to follow the building from source instructions below.

[OBS PTZ Releases](https://github.com/glikely/obs-ptz/releases)

## Configuration

To show the controls dock, in the `Docks` menu select `PTZ Controls`.
You can drag the window to any side of the OBS Studio main window to dock it
into place, or just leave it floating.

To open the settings, click on the gear icon at the bottom of the dock,
or in the `Tools` menu select `PTZ Controls`.
The settings window has three tabs.
`General` has settings that affect every camera,
`Cameras` is where you add, remove and configure cameras,
and `About` shows which version of the plugin is installed.

### Adding a Camera

First add a source for the camera's video feed in the OBS Studio `Sources`
dock. Then, on the `Cameras` tab, click the `+` button.
Choose the source, then choose what the new device starts from:
a new device using one of the control protocols,
or a camera detected on the network.
The device is added as a `PTZ Control` filter on that source, so it also
appears in the source's `Filters` dialog.

Select the new camera in the list and enter its connection details on the
`Settings` tab, either the network address or the serial port used for
control. Click `Apply` to connect, or `Revert` to discard your edits.
The `Status` tab shows what the camera is doing, and the `Diagnostics` tab
shows statistics about its connection.

Because the camera is tied to its source, the plugin selects the right
camera when the preview or program scene changes in OBS.

### Removing a camera

Select the camera on the `Cameras` tab and click the `-` button.
The camera's settings and presets are kept as a backup.
To bring them back, click `+` and pick the removed device from the
`Restore a removed device` list.
If you recreate a source with the same name, its backup is picked by default.

## Controlling Cameras

Cameras are controlled with the arrow buttons in the control dock.
To adjust a camera, it needs to be selected from the camera list in the PTZ
dock .
If `Auto select active camera` is enabled on the `General` tab,
the plugin will automatically select the correct camera when the current scene
changes.
Then, clicking the camera control buttons will adjust the camera position.
The arrow buttons will pan/tilt the camera,
The magnifying glass buttons will zoom in and out,
and the small/large buttons will change the focus.
You can also toggle autofocus on and off with the `AF` button and trigger
a one-touch refocus action.
Buttons for controls that the selected camera doesn't have are disabled or
hidden. Right click anywhere in the dock for a menu of the other actions,
such as showing the presets as thumbnails.

Presets are listed on the right hand side of the dock.
Double click a preset to recall it.
To save the camera's current position, right click a preset and select
`Save Preset`. A thumbnail of the camera's view is saved with it.
Right click and select `Rename Preset` to rename it,
or `Clear Preset` to reset it.

Select `Show Presets as Thumbnails` for a grid of thumbnails,
sized with the slider. By default a preset's thumbnail is refreshed
whenever it is recalled; turn that off with `Refresh preset thumbnail when
recalled` on the `General` tab.
Presets can be exported to a file and imported again.

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
checkbox on the settings `General` tab.

With the feature enabled the pan, tilt, zoom and preset controls will be
disabled for any camera visible in Program, preventing live moves.
If you need to override the block and do a live movement anyway
then you can temporarily override the block by clicking the lock icon in
the toolbar.

### Tally lamps and power

Cameras with tally lamps (VISCA) light red when their source is live and
green when it is in preview. Cameras can also be turned on when OBS starts
and put in standby when it closes; see the camera's `Settings` tab.

### VISCA command sets

Not every VISCA camera follows the Sony standard, so the plugin picks a
command set for the camera's model, with built in sets for the Sony SRG-120DH
and BirdDog P100. You can choose one yourself in the camera's advanced
settings, or add your own as a JSON file.
See [doc/visca-protocol.md](doc/visca-protocol.md) for the format.

If your camera isn't handled well, the `Diagnostics` tab can create a
`Camera Report` of what the camera answers, including a draft command set.
It doesn't contain the camera's address or anything about you. Look it over,
then send it in with the `Open Issue Page` button.

### Debugging data

The plugin can generate a large amount of debug data with all the protocol
messages sent to and received by the cameras.
Debug logs appear in the main obs-studio log, but are disabled by default.
To enable debug logs, select `Write protocol trace to OBS log file` in the
camera's advanced settings, and run OBS Studio with the --verbose command line option.

### Testing without a camera

`scripts/ptzsim` is a simulated camera that speaks VISCA, Pelco and ONVIF.
See [scripts/ptzsim/README.md](scripts/ptzsim/README.md).

# Building from Source

The build infrastructure for this project comes from the
[OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate)
repo. To build this plugin, follow the instructions in the plugin template
[README.md](docs/plugin-template-README.md)

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
$ sudo cp -r release/RelWithDebInfo/* /usr/
```

## MacOS Quickstart

Use `scripts/macos-dev.sh` to configure, build, sign and install the plugin,
and to run OBS with it:

```
$ scripts/macos-dev.sh setup     # configure, build and sign
$ scripts/macos-dev.sh run       # install, then run OBS with the plugin
$ scripts/macos-dev.sh restore   # put back the plugin it replaced
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

The screenshots in `doc/` are retaken with `scripts/update-screenshots.sh`,
which runs OBS Studio with a simulated camera in the Parallels Windows VM.

# Acknowledgements

Thank you to everyone who has contributed to this project, either with filing issues,
asking questions, or contributing to the code.
All code and documentation contributors are listed in [AUTHORS](AUTHORS).

Thank you also to the OBS Project developers, and the
[OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate)
repo that they maintain. This plugin leans heavily on that project.

Joystick support uses the
[QJoystick library](https://github.com/alex-spataru/QJoysticks).

And finally, thank you to everyone who contributes to the Free and Open Source
Software that this project is built upon, including OBS Studio, Qt, Simple
DirectMedia Layer (SDL), Linux, and countless libraries and tools.
