# Pan Tilt Zoom (PTZ) Controls for OBS Studio

![build status](https://github.com/glikely/obs-ptz/actions/workflows/main.yml/badge.svg)

Plugin for OBS Studio to add a PTZ Camera control dock.

Latest Version: v0.7.0

![PTZ Controls Screenshot](/doc/ptz-controls-screenshot.png?raw=true "OBS Studio PTZ Controls")

![PTZ Controls Screenshot](/doc/ptz-settings-screenshot.png?raw=true "OBS Studio PTZ Device Settings")

Features:

- Pan, Tilt, and Zoom controls
- Save and recall camera presets
- Control any number of cameras
- Auto select active camera based on active scene
- Supported protocols
  - VISCA
  - VISCA-over-IP
  - Pelco-P

[OBS project resource page](https://obsproject.com/forum/resources/ptz-controls.1284/)

[#obsptz on Twitter](https://twitter.com/hashtag/obsptz?s=09)

# Build Instructions

## Build as standalone plugin (recommended for faster build turnaround)

### Standalone Build for Linux

- Install OBS Studio including headers
  - Follow instructions for your distribution: https://obsproject.com/wiki/install-instructions
- clone this repository and build:

```
git clone https://github.com/glikely/obs-ptz
mkdir obs-ptz/build
cd obs-ptz/build
cmake ..
make
```

Copy or symlink ptz-controls.so into the OBS plugins directory.
Typically `/usr/lib/obs-plugins`.

### Standalone Build for Windows

This project contains a helper script for building the plugin that assumes that
`obs-studio` and `obs-ptz` share the same top level directory.
It also assumes that Qt5 is installed under `c:\Qt\`.
If you have Qt5 installed or obs-studio checked out somewhere else then you'll
need to modify the `winbuild` and `winrun` scripts.

- Build OBS Studio using instructions on OBS-Studio Wiki:
  https://obsproject.com/wiki/Install-Instructions
- Clone this repository into a working directory
- Run `scripts\winbuild.cmd` to build the plugin
- Run `scripts\winrun32.cmd` or `scripts\winrun64.cmd` to run OBS either the
  32bit or 64bit version of the plugin

From Powershell:

```
git clone https://github.com/glikely/obs-ptz
cd obs-ptz
.\scripts\winbuild.cmd
.\scripts\winrun64.cmd
```

# Contributing

Contributions welcome!
You can submit changes as GitHub pull requests.
Or email patches to me at mailto:grant.likely@secretlab.ca

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.
