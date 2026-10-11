# PTZUARTWrapper hardware-in-the-loop test suite

Exercises `PTZUARTWrapper` (`src/drivers/serial/uart-wrapper.cpp`/`.hpp`) end to end: data
transfer correctness, `send()` latency under concurrent reader-thread
activity (written specifically to characterize a reported Windows
blocking-write symptom), and disconnect/reconnect behavior. Not part of the
`obs-ptz` plugin build, not CI-integrated (needs real serial hardware, or
`socat` on macOS/Linux) - build it explicitly when you want to run it.

Every test file (`test_harness.hpp/.cpp`, `test_data_transfer.cpp`,
`test_concurrent_rw.cpp`, `test_reconnect.cpp`) touches only
`PTZUARTWrapper`'s public interface - never the backend library directly.

## Building

```
cmake --preset macos -DENABLE_UART_TESTS=ON   # or your platform's preset
cmake --build build_macos --target uart-hil-tests
```

`ENABLE_UART_TESTS` requires `ENABLE_SERIALPORT=ON` (it compiles
`uart-wrapper.cpp`, which needs it) and is never on by default - always
pass it explicitly.

## Running

No arguments needed on macOS/Linux with `socat` installed - defaults to a
self-managed virtual port pair, zero hardware required, and every category
including reconnect runs fully unattended (kill/restart the managed `socat`
process simulates unplug/replug):

```
./uart-hil-tests
```

Against real wired-back-to-back hardware (needed on Windows; optional
elsewhere to validate actual driver/timing behavior, which virtual ports
can't):

```
./uart-hil-tests --port-a=<DUT port> --port-b=<peer port> [--baud=9600]
```

Env var equivalents: `OBS_PTZ_TEST_PORT_A`, `OBS_PTZ_TEST_PORT_B`,
`OBS_PTZ_TEST_BAUD` (CLI flags win if both are given). Example port values:

- **Windows**: `COM3` / `COM4`
- **Linux**: prefer a stable `/dev/serial/by-id/usb-<vendor>_<model>-if00-port0`
  symlink over a raw `/dev/ttyUSBN` - replugging a USB-serial adapter can
  re-enumerate under a different node.
- **macOS**: `/dev/cu.usbserial-XXXXXXXX` - the `cu.*` node, not `tty.*`
  (which can block on `open()` waiting for carrier-detect).

In hardware mode, `[reconnect]` prompts you to physically unplug and later
replug the DUT's adapter (port A only - leave the peer's adapter alone).

Catch2 tag filtering works normally, e.g. to skip the reconnect test:
```
./uart-hil-tests [data-transfer],[concurrent]
```
