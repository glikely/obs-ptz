# Preset export/import unit tests

Exercises `ptz_preset_io::exportPresets()`/`importPresets()`
(`src/ptz-preset-io.cpp`) - the (de)serialization behind the "Export
Presets.../Import Presets..." feature (issue #78) and behind
`PTZDevice::save()`/`update()`, which delegate to the same two functions.

Pure `OBSData`-in/`OBSData`-out logic against real `libobs`: no `PTZDevice`,
no `ptzDeviceList`, no `obs-frontend-api`, no running OBS, and (unlike
`tests/uart-hil`) no hardware. Safe to build and run anywhere, including CI.

`test_preset_io.cpp` covers the round trip a real export-then-import does:
ordering, field values, and starting from an empty preset list.
`test_preset_io_edge_cases.cpp` covers what a hand-edited or corrupted file
can do to it (duplicate ids, out-of-range/missing `preset_max`), plus one
test pinning down a pre-existing `protocol-helpers.cpp` gap that's directly
relevant to preset export fidelity: a preset field that's a `bool` or a
plain C++ `double` silently doesn't survive the round trip. Real presets
only ever store strings today, so this doesn't bite in practice, but it's
worth knowing about before adding a new preset field of either type.

## Building

```
cmake --preset macos -DENABLE_PRESET_IO_TESTS=ON   # or your platform's preset
cmake --build build_macos --target preset-io-tests
```

## Running

No arguments needed:

```
./preset-io-tests
```

Catch2 tag filtering works normally, e.g. to run only the edge-case tests:

```
./preset-io-tests [preset-io][import]
```
