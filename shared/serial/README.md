# Vendored gbionics/serial_cpp

Source: https://github.com/gbionics/serial_cpp
Version: `main` branch tip, commit `2ab9e20388a6efca8540e5ae8910e9e2c42a71f4`
License: MIT (see `COPYING` in this directory) - upstream copyright notices
in each file are preserved as-is; do not remove them.

## Why this fork, not wjwwood/serial itself

`gbionics/serial_cpp` is a maintained fork of the original
[wjwwood/serial](https://github.com/wjwwood/serial) (itself dormant since
2022, and the last tagged release, `v1.0.1`, is from 2015): active PR-based
development from April 2025 through at least mid-2026, including fixes
directly relevant here - `Serial::reconfigurePort()`'s `double`/`stopbits_t`
arithmetic warning is fixed with an explicit cast (upstream still triggers
it), and Windows' `list_ports_win.cc` was changed to also enumerate virtual
COM ports, which the original `SetupDiGetClassDevs` GUID missed. It builds
on top of `wjwwood/cxx_serial`, the original author's own from-2019
conversion of the library to a plain CMake package (no catkin/ROS
dependency), then continues from there under the `serial_cpp` namespace
(renamed from `serial` partway through this fork's history) with its own
modern install/FetchContent-friendly `CMakeLists.txt` - not used directly
here (see below), but a good sign of real upkeep.

## Why vendored (all platforms, unlike shared/libserialport)

obs-ptz previously depended on Qt's `QSerialPort` module, which `obs-deps`
does not ship prebuilt - the plugin had to build the whole Qt `qtserialport`
submodule from source on macOS and Windows, which was the source of repeated
packaging/build breakage (see the commit history around `1418dac`, `9073764`,
`486c697`, `85a4023`, `fefef79`). `serial_cpp` has no Qt dependency and is
small enough to vendor and statically link directly, which avoids all of
that: no separate shared library to discover, build from source, or package
alongside the plugin.

Vendored on Windows, macOS, *and* Linux: unlike `libserialport` (see
`shared/libserialport/README.md` on the `libserialport-migration` branch -
this is a parallel evaluation of that same QSerialPort replacement problem
via a different library), there's no `serial-cpp-dev`-style distro package
to link against instead on Linux, so this directory's `CMakeLists.txt`
vendors and statically links the same way on all three platforms.

## What's here vs. upstream

`src/serial.cc` (the shared pimpl-forwarding glue), `include/serial_cpp/serial.h`,
`include/serial_cpp/v8stdint.h` (all platforms), plus one backend pair per
platform (see `CMakeLists.txt`): `src/impl/unix.cc` +
`include/serial_cpp/impl/unix.h` (Linux and macOS both use this - it's the
same POSIX termios backend on both, only port *enumeration* differs) with
either `src/impl/list_ports/list_ports_linux.cc` or `..._osx.cc`, or
`src/impl/win.cc` + `include/serial_cpp/impl/win.h` +
`src/impl/list_ports/list_ports_win.cc` on Windows. Not vendored:
`include/serial_cpp/serial_compat.h` (a `namespace serial { ... }` shim
this fork provides for projects migrating off vanilla `wjwwood/serial`'s
namespace - not applicable here, since `uart-wrapper.cpp`/`.hpp` are
written directly against `serial_cpp::`), examples, tests, upstream's own
`CMakeLists.txt`/`package.xml`/`pixi.toml`, Visual Studio project files -
none of it is needed for this build.

## Updating

To pick up a newer commit: replace the files listed above from the new
commit, re-check `CMakeLists.txt` against the new commit's own
`CMakeLists.txt` for any new required source files or link dependencies,
and update the commit hash noted above.
