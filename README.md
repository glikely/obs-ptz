# Pan Tilt Zoom (PTZ) Controls for OBS Studio

Plugin for OBS Studio to add a PTZ Camera control dock.

![PTZ Controls Screenshot](/doc/ptz-controls-screenshot.png?raw=true "OBS Studio PTZ Controls")

This plugin is a work in progress!
It is not feature complete and not very useful yet.
Feel free to help out!
Patches can be submitted as Github pull requests.

Based on: https://github.com/obsproject/obs-studio/pull/2380

## Build

So far has only been built on Linux.
It should work on other platforms, but none of them have been tested.

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
- Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
- Check out this repository to UI/frontend-plugins/ptz-controls
- Add `add_subdirectory(ptz-controls)` to UI/frontend-plugins/CMakeLists.txt
- Rebuild OBS Studio

## Contributing

Contributions welcome!
You can submit changes as GitHub pull requests.
Or email patches to me at mailto:grant.likely@secretlab.ca

See CONTRIBUTING.md for details.
