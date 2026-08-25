# Vendored qtserialport (unmodified)

Source: `qtserialport-everywhere-src-6.11.1` (see `.deps/`), the version of
Qt's `qtserialport` submodule this project's `buildspec.json` pins for its
Qt6 baseline.

License: upstream `qtserialport` is multi-licensed
(`LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR
GPL-3.0-only`, see each file's own `SPDX-License-Identifier` line) -
copyright notices are preserved as shipped; do not remove them.

This commit is an unmodified copy of the files below - real upstream
class/file names throughout (`QSerialPort`, `QSerialPortPrivate`,
`QWinOverlappedIoNotifier`, `QSerialPortInfo`, `QSerialPortInfoPrivate`),
deliberately kept exactly as upstream ships them. The point of vendoring
this way, in its own commit, is that a later rework commit can then be
diffed directly against this one (`git diff <this-commit>..<rework-commit>
-- shared/qtserialport/src/`) to see precisely what changed and nothing
else - not obscured by a wholesale rename or restructure.

## What's here vs. upstream

Only the Windows and macOS backends - no Linux support is planned for
this fork (see the follow-up rework commit for why): `qserialport.cpp`/
`.h`/`_p.h`, `qserialport_unix.cpp` (the POSIX/termios backend - used by
macOS here; also by Linux upstream, not relevant to this fork),
`qserialport_win.cpp`, `qwinoverlappedionotifier.cpp`/`_p.h`,
`qserialportglobal.h`, `qserialportinfo.cpp`/`.h`/`_p.h`,
`qserialportinfo_osx.cpp`, `qserialportinfo_win.cpp`.

Not vendored: `qserialportinfo_unix.cpp` (Linux libudev/sysfs
enumeration) and `qtudev_p.h` (Linux only), `qserialportinfo_freebsd.cpp`
(FreeBSD only), `removed_api.cpp` (a source/binary-compatibility shim for
one bindable-property overload removed in Qt 6.7 - not applicable, this
fork isn't a versioned drop-in replacement library), upstream's own
`CMakeLists.txt`/`configure.cmake`/`qt_cmdline.cmake`/`doc/` (this
project writes its own, matching the pattern already used for the other
vendored dependencies under `shared/`), and examples/tests.

## Updating

To pick up a newer upstream version: replace the files above with the
new version's copies (unmodified, same as this commit), then manually
reconcile the rework commit's changes against the new base.
