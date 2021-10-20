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

- Build OBS Studio using instructions on OBS-Studio Wiki:
  https://obsproject.com/wiki/Install-Instructions
- Clone this repository into a working directory
- Modify (or copy and modify) `ci\install-script-win.cmd`, changing values of
  `DepsPath`, `QTDIR`, and `LibObs_DIR` to match your local environment
- Create a build directory under obs-ptz
- Run `ci\windows-configure.cmd` to invoke cmake
- Run `ci\windows-build.cmd` to make the binary

```
git clone https://github.com/glikely/obs-ptz
cd obs-ptz
mkdir build
cd build
..\ci\windows-configure.cmd
..\ci\windows-build.cmd
```

- Copy the following files into OBS plugins directory
  - `build\Debug\ptz-controls.dll`
  - `%QTDIR%\bin\Qt5SerialPortd.dll`
  - `%QTDIR%\bin\Qt5Gamepadd.dll`

## Build inside OBS source tree

- Build OBS according to official instructions:
  https://obsproject.com/wiki/Install-Instructions
- Check out this repository to UI/frontend-plugins

```
cd obs-studio/UI/frontend-plugins
git clone https://github.com/glikely/obs-ptz
```

- Add obs-ptz to the OBS build scripts using included patch

```
cd obs-studio
git am < UI/frontend-plugins/obs-ptz/patches/0001-Add-obs-ptz-plugin-to-OBS-Studio.patch
```

- Rebuild OBS Studio

# Contributing

Contributions welcome!
You can submit changes as GitHub pull requests.
Or email patches to me at mailto:grant.likely@secretlab.ca

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.
