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
| `read_only` | The property can't be set |
| `bad_value` | A value can't be what the property or argument is, or a model has no such role |
| `no_method` | The object has no such method, or signal |
| `secret` | It would give away what is in a secret (see [Secrets](#secrets)) |
| `timeout` | OBS's main thread did not get to it within 5 seconds |
| `unavailable` | OBS is exiting |

| Request | Request JSON | Answer JSON |
| --- | --- | --- |
| `ping` | `{}` | `{"driver": 1}` |
| `find` | `{"path", "all"?: false}` | `{"objects": [object...]}`, each described as below |
| `get` | `{"path", "properties": [name...]}` | `{"properties": {name: value...}}` |
| `tree` | `{"path"?, "depth"?: 3}` | `{"tree": object}`, the object with its `"children"`, and theirs, to `depth` levels; with no path, `{"tree": [object...]}`, every window so |
| `clipboard` | `{}` | `{"text"}`, the text on the clipboard |
| `set` | `{"path", "properties": {name: value...}}` | `{}`. Each value is made the property's type, as Qt converts it |
| `invoke` | `{"path", "method", "args"?: [], "queued"?: false}` | `{"return": value}`, or `{}` for a method that returns nothing. It calls the first slot or invokable method of that name that takes that many arguments, up to 4, each made the parameter's type (a `QPoint` from `[x, y]`). With `"queued"`, the call is left for the main thread to make after the answer, `{}`, is given: a method that opens a modal dialog doesn't return until the dialog closes |
| `activate` | `{"path", "data" \| "text", "role"?: 256}` | `{"index"}`. The first item of a `QComboBox` whose data, or text, is the one given is chosen as a user chooses it: it is made current, and the combo box says `activated` and `textActivated`, which it doesn't for a change made in code |
| `select` | `{"path", "value", "role"?: "display", "column"?: 0}` | `{"row"}`. The first row of a `QAbstractItemView` whose `role` (a name from its model's `roleNames()`, `accessibleText` or `accessibleDescription`, which a model answers without naming, or a role's number) is the value is made current and selected, as a click does. With a `value` of `null`, no row is current or selected |
| `rows` | `{"path", "roles"?: ["display"], "column"?: 0}` | `{"rows": [{role: value...}...], "current": row or -1, "selected": [row...]}`. The rows of a `QAbstractItemView` or a `QComboBox`, as its model gives them for the roles asked for (by name, as for `select`), with the row that is current and those that are selected. A role named `flags` is the row's item flags, and one named `rect` the row's place in a view, `[x, y, width, height]` in its viewport. A view with no model has no rows |
| `setdata` | `{"path", "row", "value", "role"?: "edit", "column"?: 0}` | `{}`. Sets the data of that role of the row of a `QAbstractItemView`'s model, as an editor or a click on a control in the row does (the model's `setData()`); `bad_value` if the model does not take it |
| `moverow` | `{"path", "from", "to"}` | `{"moved"}`. Moves a row of a `QAbstractItemView`'s model to before row `to`, as a view's own moving of a row does (the model's `moveRow()`), and says whether the model moved it |
| `key` | `{"path", "key"}` | `{}`. Presses and releases a key in the widget, as typed: the first key of a key sequence's text, `Escape`, `Return` or `Ctrl+A`, say |
| `watch` | `{"path", "signal"}` | `{"watch": id}`. Counts how often the object sends the signal, by its name, from now. Watching it again counts again from nothing, under the same id |
| `count` | `{"watch"}` | `{"count"}`, or `not_found` once the object watched has gone |
| `hold_source` | `{"name"}` | `{}`. Keeps a reference to the source by that name until the driver unloads, inside OBS's own shutdown, as a plugin with an output on a scene does: the source outlives OBS clearing its scenes as it exits |
| `duplicate_source` | `{"name", "new_name", "scene"}` | `{"uuid"}`. Makes a copy of the source by that name, with copies of its filters, as OBS's Paste (Duplicate) does (`obs_source_duplicate()`, not private), and adds it to the scene |
| `filter_links` | `{"name", "filter"}` | `{"source", "parent", "target", "device_parent"}`, each `{"uuid", "name", "private"}` or `null`: the source by that name, its filter by that name's parent and target as libobs has them, and, for a PTZ device's filter, the source its `ptz_get_parent_source` says |

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
numbers, as true or false if both are, and otherwise as text. A list, an
object or null matches nothing.

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

### Secrets

A password, a stream key or any other secret typed into OBS is in a widget the
driver can reach, and an answer can end up in a test's output, or a log. So
the driver gives nothing away of what is in a **secret**:

- a `QLineEdit` that hides what is typed in it: one whose `echoMode` is
  anything but `Normal`, as OBS's stream key, obs-websocket's password and
  every `OBS_TEXT_PASSWORD` property are;
- any object whose dynamic property `uiDriverSecret` is true, which code may
  give any widget whose content is a secret.

Once an object is a secret it stays one for as long as it lives, although it
is then shown by its "Show" button, or by code: a `QLineEdit` is seen to be one
when it is made ready to show (`QEvent::Polish`), by when what made it has set
its `echoMode`, and before anything can have shown its text. One made to hide
its text only after that is a secret once the driver finds it hiding it.

Of a secret:

- an answer describing it leaves out `"text"`, and says `"secret": true`;
- `get` reads only `objectName`, `enabled`, `visible`, `hidden`,
  `echoMode`, `readOnly`, `toolTip`, `geometry` and `placeholderText`, and
  answers `secret` for anything else;
- a path's criteria match it on those alone, and on `class`, `name` and
  `id`: any other property is one it hasn't, so a guess at its text is
  `not_found`, as a wrong one is;
- `set` sets anything but its `echoMode`, so that a test can type into it, but
  can't have it show what is in it;
- `invoke` calls a method that gives nothing back, or is `queued`, but not
  `copy` or `cut`;
- `activate` and `select` don't choose from it, as which item there is says
  what it holds.

`invoke` calls no object's `paste`, whether it is a secret or not: what is
pasted may be one, copied by a button that copies it (obs-websocket's Connect
Info has one).

What isn't in a widget is beyond this: a secret in an object's own Qt
property, read by `get` of that object, or one that a method of an object that
isn't marked gives back. Nor does it make the driver safe to start on an OBS
whose secrets matter: it can still press any button, and change any setting.

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
