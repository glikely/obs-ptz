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

## Rework commit

The commit on top of this one removes `QSerialPortPrivate`/
`QSerialPortInfoPrivate`'s dependency on Qt's private, ABI-unstable
`QIODevicePrivate`/`QObjectPrivate`/`Qt::CorePrivate` - the actual root
cause of obs-ptz's historical Qt-uprev breakage with real `QSerialPort`,
independent of static vs. dynamic linking. Technique: `Q_DECLARE_PRIVATE`/
`Q_D`/`Q_Q` are generic macros that just call hand-writable `d_func()`/
`q_func()` methods - those got hand-written against an ordinary owned
`std::unique_ptr<QSerialPortPrivate>` member instead of `QObject::d_ptr`,
so every existing `Q_D`/`Q_Q` call site needed no changes. `QRingBuffer`
(upstream's private chunked ring buffer backing
`QIODevicePrivate::buffer`/`writeBuffer`) is reimplemented locally as a
plain `QByteArray`-backed stand-in
(`qtserialport_ringbuffer_compat_p.h`); `<private/qcore_unix_p.h>` and
`<private/qcore_mac_p.h>` get similar local stand-ins
(`qtserialport_unix_compat_p.h`, `qtserialport_mac_compat_p.h`).

Genuine cuts (not just relocated): Qt6 BINDABLE properties
(`QObjectCompatProperty`/`Q_OBJECT_COMPAT_PROPERTY_WITH_ARGS`) on
`dataBits`/`parity`/`stopBits`/`flowControl`/`error`/`isBreakEnabled` -
needs `qproperty_p.h` machinery this fork has no access to, and obs-ptz
has no use for declarative property bindings on a serial port. Everything
else - full read/write/error/config API surface, real termios/
`QSocketNotifier` async I/O on macOS - is preserved.

**Known miscompilation, worth reading before touching
`qtserialport_ringbuffer_compat_p.h` or vendoring a newer upstream**:
AppleClang silently miscompiled calls to member functions on the local
`QRingBuffer` stand-in named `free`, `reserve`, `chop`, or `append` -
code for these methods was present in the compiled binary (confirmed via
`strings`) but its effects at the call site never happened, with no
compiler warning or error. This produced a very hard to diagnose infinite
loop. Renaming to `freeBytes`/`reserveBytes`/`chopBytes`/`appendBytes`
fixed it immediately and reproducibly; see the comment at the top of
`qtserialport_ringbuffer_compat_p.h` for the full account. Root cause is
not confirmed with certainty, but strongly suspected to be AppleClang
treating these specific names as builtin/library functions (`free` in
particular) even when called as `object.methodname(...)` on a
user-defined C++ class.

**Windows rework technique note**: `QWinOverlappedIoNotifierPrivate`
derives from `QObjectPrivate` (not `QIODevicePrivate` - `QSerialPort` is a
`QObject` in its own right, unlike `QSerialPortPrivate`), de-privatized
the same way - hand-written `d_func()`/`q_func()` backed by an ordinary
`std::unique_ptr` member. Two `QObjectPrivate::connect(...)` call sites
(one in `qserialport_win.cpp`, one in `qwinoverlappedionotifier.cpp`)
connected a signal directly to a member-function pointer on a
`QObjectPrivate`-derived Private instance - not available without that
base class, so both became ordinary lambda-capturing connects instead
(see the comments at each call site). This let the corresponding
`Q_PRIVATE_SLOT` declarations in `qserialport.h`/`qwinoverlappedionotifier_p.h`
be deleted outright, rather than kept working through the same mechanism.
`qserialportinfo_win.cpp`'s `<private/qwinregistry_p.h>` (`QWinRegistryKey`)
and `<private/quniquehandle_types_p.h>` (`QUniqueHandle`,
`QUniqueWin32Handle`) got local stand-ins in `qtserialport_win_compat_p.h`,
same pattern as the macOS/Unix compat headers.

**Include-shadowing bug, worth knowing about before adding any new file
here**: `qserialport.h`/`qserialportinfo.h` originally included their own
sibling `qserialportglobal.h` via `<QtSerialPort/qserialportglobal.h>`
(upstream's own module-style angle-bracket form). The prebuilt Qt6 SDK
this project vendors from (see the top of this file) ships a real,
complete QtSerialPort module's headers alongside the ones actually used
here, under that same relative path - so the angle-bracket form silently
resolved to *that* real header instead of this fork's own (empty)
`Q_SERIALPORT_EXPORT` override. Harmless-looking on macOS (no visible
build failure), but a hard MSVC error on Windows (`C2491`: dllimport/
dllexport mismatch between declaration and definition). Fixed by quoting
both includes (`"qserialportglobal.h"`), which always resolves to this
directory's own file regardless of what else is on the include path. Any
new file here that includes another vendored header should use the
quoted form for the same reason.
