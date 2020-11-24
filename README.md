# Pan Tilt Zoom (PTZ) Controls for OBS Studio

Plugin for OBS Studio to add a PTZ Camera control dock.

![PTZ Controls Screenshot](/doc/ptz-controls-screenshot.png?raw=true "OBS Studio PTZ Controls")

This plugin is a work in progress!
It is not feature complete and not very useful yet.
Feel free to help out!
Patches can be submitted as Github pull requests.

Based on: https://github.com/obsproject/obs-studio/pull/2380

## Build

### Linux

Building libvisca
-----------------
obs-ptz depends on libvisca to provide the backend VISCA protocol
interface library.
Fetch and build libvisca before building obs-ptz.

```
git clone https://git.code.sf.net/p/libvisca/git libvisca
cd libvisca
autoreconf --install
./configure
make
```

To build this plugin
--------------------

### Build as standalone plugin (recommended)

- Install OBS Studio including headers
  - Follow instructions for your distribution: https://obsproject.com/wiki/install-instructions
- clone this repository and build:

```
git clone https://github.com/glikely/obs-ptz
mkdir obs-ptz/build
cd obs-ptz/build
cmake ..
```

Copy or symlink ptz-controls.so into the OBS plugins directory.
Typically `/usr/lib/obs-plugins`.

### Build in OBS source tree

- Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
- Check out this repository to UI/frontend-plugins/ptz-controls
- Add `add_subdirectory(ptz-controls)` to UI/frontend-plugins/CMakeLists.txt
- Rebuild OBS Studio

### Windows

- Build OBS Studio using instructions on OBS-Studio Wiki:
  https://obsproject.com/wiki/Install-Instructions
- Clone this repository into a working directory
- Modify (or copy and modify) `CI\install-script-win.cmd`, changing values of
  `DepsPath`, `QT_DIR`, and `LibObs_DIR` to match your local environment
- Run `CI\install-script-win.cmd` to invoke cmake
- Run `CI\build-script-win.cmd` to make the binary
- Copy resulting `build\Debug\ptz-controls.dll` and `Qt5Gamepadd.dll` from QT directory binaries into OBS plugins directory

## Contributing

Contributions welcome!
You can submit changes as GitHub pull requests.
Or email patches to me at mailto:grant.likely@secretlab.ca

See CONTRIBUTING.md for details.
