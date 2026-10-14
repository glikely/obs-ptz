# obs-ui-driver

An OBS plugin that lets a client of obs-websocket find and drive OBS's Qt
widgets: open a dialog, read what a widget shows, click it, choose an item in
a list. It is generic. It knows nothing of obs-ptz and uses nothing of this
repository's code, so it drives OBS's own widgets as well as any plugin's.

It is for tests, and is built only with `-DENABLE_UI_TESTS=ON`, as a plugin
of its own, `obs-ui-driver`, next to `obs-ptz`. It does nothing unless OBS is
started with `OBS_UI_DRIVER=1` in its environment. The suite's scripts install
it, and `tests/obs-integration/conftest.py` sets that variable. Its Python
client is `tests/obs-integration/uidriver.py`.

## Requests

The requests are on obs-websocket's vendor `ui-driver`, made with
`CallVendorRequest`. Each request's data, and each answer, is **one string
field, `json`, holding a JSON object**: obs-websocket hands a vendor its data
as `obs_data`, which can't hold an array of plain values.

```json
{"vendorName": "ui-driver", "requestType": "ping", "requestData": {"json": "{}"}}
```

A request is done on OBS's main thread, and answered once it has been. One
that could not be done answers `{"error": code, "message": text}`:

| Error | |
| --- | --- |
| `bad_request` | The request's `json` is not a JSON object, or is missing what it needs |
| `not_found` | Nothing is what a segment of the path names |
| `ambiguous` | More than one thing is what a segment names |
| `no_property` | The object has no property by that name |
| `timeout` | OBS's main thread did not get to it within 5 seconds |
| `unavailable` | OBS is exiting |

| Request | Request JSON | Answer JSON |
| --- | --- | --- |
| `ping` | `{}` | `{"driver": 1}` |
| `find` | `{"path", "all"?: false}` | `{"objects": [object...]}`, each described as below |
| `get` | `{"path", "properties": [name...]}` | `{"properties": {name: value...}}` |
| `tree` | `{"path"?, "depth"?: 3}` | `{"tree": object}`, the object with its `"children"`, and theirs, to `depth` levels; with no path, `{"tree": [object...]}`, every window so |

### Paths

An object is named by a **path**, a list of **segments**. Each segment
names one object among everything inside what the segment before it named:
its children, their children, and so on. The first segment names it among
every window and everything in them. A segment is:

- a string: the object's `objectName`;
- or an object of criteria, all of which the object meets:
  - `"class"`: it is of that class, or one derived from it (`inherits()`);
  - `"name"`: its `objectName`;
  - `"id"`: the id an earlier answer gave it;
  - any other key: it has a property by that name, with that value.

For example, `[{"class": "OBSBasic"}, "menuTools", {"class": "QAction",
"text": "Settings"}]`.

Each segment has to name exactly one object, or the answer is `not_found` or
`ambiguous`. The exception is the last segment of a `find` with `"all":
true`, which names every object it matches.

A value in a request is compared with an object's as numbers if both are
numbers, as true or false if both are, and otherwise as text.

### Objects and their properties

An object in an answer is `{"id", "class", "name"}`, with `"hidden"`,
`"visible"` and `"enabled"` if it is a widget, and `"text"` if it has a
`text` property. Its **id** stays the same for as long as the object lives,
and is never given to another. A test can compare ids to tell whether a widget
was kept or replaced.

`get` reads an object's Qt properties (`Q_PROPERTY`, and dynamic ones), each
as JSON. It also reads three that widgets have no Qt property for:

- `"hidden"` of any widget: whether it is hidden (`isHidden()`), which
  `"visible"` isn't for a widget on a tab that isn't showing;
- `"tabs"` of a `QTabWidget`: `[{"text", "visible", "page"}...]`, where
  `page` is the tab's page's `objectName`;
- `"items"` of a `QComboBox`: `[{"text", "data"}...]`.

## Building and running it

`cmake --preset macos -DENABLE_UI_TESTS=ON` (or another platform's preset)
builds it with obs-ptz, into the same `rundir`. Then:

- **macOS:** `scripts/macos-dev.sh test` signs it and loads it.
- **Linux VM:** `scripts/vm-linux-dev.sh build --ui-tests`, then `test`,
  copies it next to the system's obs-ptz.
- **Windows VM:** `scripts/vm-windows-dev.sh test <arch>` copies it into
  `C:\OBS-Test\<arch>`.

It is tested by `tests/obs-integration/test_ui_driver.py` on OBS's own
widgets. These are the OBS versions the suite runs on, which set the oldest
Qt and OBS APIs it may use:

| Platform | OBS | Qt |
| --- | --- | --- |
| macOS | 32.2.2 | 6.11 |
| Linux (Ubuntu 26.04 package) | 32.1.0 | 6.10.2 |
| Windows (arm64, `C:\OBS-Test`) | 32.2.2 | 6.11.1 |
