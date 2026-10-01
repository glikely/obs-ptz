# Plan: per-camera VISCA command sets

Status: proposal. Nothing here is implemented yet.

## Where the driver is today

Everything a VISCA camera can be told or asked lives in `src/ptz-visca.cpp`
as file-scope `const PTZCmd`/`PTZInq` globals, and four separate, static
tables decide how they are used:

| Table | What it decides |
|---|---|
| `PTZVisca::inquires` | the inquiry a state key is read with |
| `PTZVisca::inquiresFallback` | a second inquiry, if the first gets a syntax error |
| `visca_state_commands` | the command `requestState()` sends for a key, and the order |
| `PTZCmd::affects` | the (one) key a command makes stale |

And then a set of hand-written exceptions in the code:

- `requestState()`: `focus_af_move_time`/`focus_af_interval_time` (one
  command, two keys), `menu_on` (off only), `tally_on`/`tally_preview`.
- `set_autofocus()`: `focus_af_enabled` is set by two fixed commands
  (`Focus_Auto`/`Focus_Manual`), not by a value, so it isn't in
  `visca_state_commands`, and it writes the state itself.
- `visca_tally_key()` and `update_tally_state()`: recognise tally commands
  by comparing bytes, and read the value back out of `cmd[6]`.
- `receive()`: an error for a tally command, or any inquiry, adds it to
  `unsupported_requests`; only an inquiry triggers the fallback.
- Movement, presets, home, one-push AF, power at shutdown and the
  `wb_onepush` trigger name their command directly.
- `discover_limits()` masks `focus_pos & 0xffff` because the block
  inquiry decodes it signed and `CAM_FocusPosInq` unsigned.

Consequences:

1. **One state value is spread over 2–4 places.** Adding a setting means
   a command global, an inquiry global, an entry in `inquires`, maybe one
   in `inquiresFallback`, and one in `visca_state_commands` in the right
   position. Nothing checks they agree (e.g.
   `AFMode_ActiveIntervalTime` only marks `focus_af_move_time` stale;
   `ZoomFocus_Direct` marks nothing).
2. **Every camera gets the same command set.** The vendor/model the
   version inquiry reads (`viscaVendors`/`viscaModels`) is only shown in
   the UI. Per-camera differences are discovered at runtime by syntax
   errors, which only works for inquiries and tally, costs a round trip
   per unsupported request after every reconnect/settings change, and
   can't express *"this camera has it, but differently"*.
3. **Commands are identified by their bytes**, globally. Two cameras
   that use the same bytes for different things can't both be supported;
   the tally code depends on the byte layout.
4. **There's nowhere to put user commands.** The tables are static,
   compile-time, and the `datagram_field`s are leaked `new`s that only
   work because they're never freed.

## Target model

Three layers, each a plain data structure the driver interprets:

```
ViscaProfile            "sony-srg-120dh", extends "sony-base"
 ├─ controls            one per state key
 │   ViscaControl       key, type, setters, reads, flags
 ├─ actions             named commands the driver itself sends
 └─ triggers            named fire-and-forget commands (ptz_trigger)
```

### `ViscaControl`: a state value and everything about it

```cpp
struct ViscaControl {
	QString key;                  // state key, e.g. "focus_af_enabled"
	ViscaValueType type;          // Bool, Int, Enum (for coercion & UI later)

	/* How it is set. Exactly one of: */
	std::optional<PTZCmd> set;    // value encoded into the command's args
	QMap<int, PTZCmd> setTo;      // value -> fixed command (AF Auto/Manual,
	                              // tally on/off; menu_on has only {0: Off})

	/* How it is read, best first. An inquiry may be shared by many controls
	 * (the block inquiries); the next is tried when one is unsupported. */
	QList<PTZInq> reads;

	/* No read at all: the state follows the setter once the camera ACKs it
	 * (tally on a BirdDog). */
	bool assumeOnAck = false;

	/* Set after these, when several are requested together (a mode before
	 * the value that only exists in it). */
	QStringList after;
};
```

A command that sets several keys (AF move/interval time, zoom+focus
direct) gets each argument bound to a key (`datagram_field::name` already
is the key); a request that names only some of them fills the rest from
current state. That replaces the AF-time special case and gives
`ZoomFocus_Direct` correct `affects` for free. `affects` becomes the list
of keys the command's args name, plus any declared extra (movement drives
affect `*_pos`).

Everything the driver does per key goes through the control:
`requestState()` iterates controls in `after` order; `send_pending()` and
the poll pick `reads.first()` not yet in `unsupported_requests`;
`receive()` on a syntax error re-marks every key whose `reads` contained
the failed inquiry (N-level fallback instead of exactly two); the tally
"last value set" logic is `assumeOnAck` on a command that carries its
`(key, value)` instead of being decoded from `cmd[6]`.

### Actions and triggers

Driver behaviour that isn't a state value looks its command up by name:
`pantilt_drive`, `pantilt_abs`, `pantilt_rel`, `pantilt_home`,
`zoom_drive`, `zoom_abs`, `focus_drive`, `zoom_focus_abs`,
`focus_onetouch`, `memory_reset/set/recall`. A missing action is a no-op
(and, later, can hide the corresponding UI). Triggers (`wb_onepush`, plus
user ones) are the same with a public name, checked by `runTrigger()`
before falling back to `PTZDevice::runTrigger()`. Diagnostics
(`scan_inquiries`, `replies_to_log`, `discover_limits`) stay in code.

### `ViscaProfile`: a camera's command set

- A profile is controls + actions + triggers keyed by name, with an
  optional parent. Lookup walks the chain; a child entry **replaces** the
  parent's by key, and can explicitly remove one. Two cameras can then
  use the same bytes for different keys, because nothing is global.
- Built-in profiles:
  - `generic`: exactly today's superset, so unknown cameras behave as now
    (syntax-error discovery stays as the safety net).
  - `sony-srg` (block inquiries first), `birddog-p100` (no block
    inquiries, green tally, no tally inquiry), and whatever else the
    model table names, each `extends: generic`.
- Selection: a new per-device setting `visca_profile`, default `auto`.
  `auto` starts on `generic`; when `CAM_VersionInq` answers, the driver
  looks up `(vendor_id << 16 | model_id)` and switches if it maps to a
  profile, clearing `unsupported_requests` and marking every control's key
  stale. An explicit choice skips the lookup (for cameras that misreport,
  or report a vendor ID shared by rebadged hardware).
- Ownership: profiles are immutable once built and held by
  `std::shared_ptr<const ViscaProfile>`, so a device keeps its profile
  alive while a reload builds a new one.

### User custom commands

The same structures, loaded from JSON. The model above is designed so
nothing in it needs code that a user can't express:

```json
{
  "id": "my-ptzoptics",
  "extends": "generic",
  "name": "PTZOptics 30X (custom)",
  "match": [{"vendor": "0x0001", "model": "0x0513"}],
  "controls": {
    "user_auto_tracking": {
      "type": "bool",
      "set":  {"cmd": "81010a1100ff", "args": [{"type": "flag", "offset": 4}]},
      "reads": [{"cmd": "81090a11ff", "results": [{"type": "flag", "offset": 2}]}]
    }
  },
  "remove": ["low_latency"],
  "triggers": {
    "user_ir_reset": {"cmd": "8101060505ff"}
  }
}
```

- A field factory maps type names (`u4`, `u7`, `u8`, `u15`, `u16`, `s16`,
  `flag`, `s4`, `s7`, and raw `int`/`bool` with `mask`) to the existing
  `visca_*` classes; for those, a result's key defaults to the control's.
- Validation on load: hex parses, ends in `ff`, no `ff` before the end,
  byte 1 is `01` for a command and `09` for an inquiry, every field fits
  inside the template, no field overlaps another. Errors are logged with
  file and key and the profile is skipped, never half-applied.
- Where they live: `obs-ptz/visca-profiles/*.json` under the module's
  config dir, so one file serves every device using that camera, and
  people can share them. They show up in the `visca_profile` list and
  take part in `auto` matching (user `match` wins over built-in).
- State keys a user defines must start with `user_`, so they can never
  collide with a key the plugin later adds. That prefix is documented in
  `docs/ptz-device-api.md` once (minor API bump); the individual keys
  aren't part of the API. Triggers likewise.

Deliberately **not** in the first pass: making the built-in profiles JSON
too (it's attractive, since contributors could add cameras without C++,
but it's a separate decision once the format has been lived with), and
having the state view render controls from profile metadata (it hardcodes
choices per key today; `ViscaValueType` leaves room for a range/choices
description to be added and exposed through a new proc later).

## Commit series

Each commit builds, keeps every existing test passing, and changes
behaviour in at most one visible way, named in its subject. Following
the repo's habit, new tests go in a separate `tests:` commit straight
after the commit they cover. Pure moves are never mixed with changes.

### Part 1: groundwork (no structural change)

1. **protocol-helpers: own datagram fields instead of leaking them.**
   `QList<std::shared_ptr<datagram_field>>` in `PTZCmd`, with a
   `field<T>(...)` helper so the definitions only change `new X(` →
   `field<X>(`. Required before anything is built or freed at runtime.
2. **protocol-helpers: let a command affect more than one key.**
   `QString affects` → `QStringList`. Mechanical; no behaviour change.
3. **visca: mark both AF times stale after setting them**, and zoom and
   focus after `ZoomFocus_Direct`. Small fix using (2).
4. **protocol-helpers: add `PTZCmd::isInquiry()`**, replacing the
   `cmd[1] == 0x09` tests in `send_packet()`, `receive()`.
5. **visca: read focus as unsigned from the lens block inquiry**, so both
   inquiries agree; drop the `& 0xffff` masks in `discover_*`.
6. `tests:` focus position read the same with and without block
   inquiries (ptzsim `--visca-no-block-inquiries` already exists).

### Part 2: group commands and inquiries by state value

7. **visca: move the command definitions to `ptz-visca-commands.cpp`.**
   Pure move; the globals the driver still names directly get `extern`
   declarations in `ptz-visca-commands.hpp`.
8. **visca: describe each state value with a ViscaControl.** Add the
   struct and one `visca_controls` table built from the existing globals,
   reproducing `inquires`, `inquiresFallback` and `visca_state_commands`
   exactly (same order). Nothing uses it yet; a startup self-check
   (debug builds) asserts it matches the old tables.
9. **visca: set state through the controls.** `requestState()` iterates
   the controls; `visca_state_commands` goes. Same commands, same order.
10. **visca: read state through the controls.** `send_pending()`,
    `update_timer_callback()`, `cmd_get_camera_info()` and the fallback in
    `receive()` use `reads`; `inquires`/`inquiresFallback` go. Fallback is
    now N-deep; with today's tables that is identical behaviour. Drop the
    self-check from (8).
11. **visca: set autofocus and the menu with value-to-command setters.**
    `setTo` for `focus_af_enabled` and `menu_on`; `set_autofocus()`
    (which `PTZDevice::requestState()` already calls for
    `focus_af_enabled`) sends through the control, and stops writing
    the state itself: the read after completion does that. No API change.
12. **visca: let tally state follow a command the camera ACKs.**
    `assumeOnAck` and per-command `(key, value)`; `visca_tally_key()`,
    the `cmd[6]` decode, and `sendTally()`'s lookup go. Tally becomes two
    `setTo` controls.
13. **visca: bind a command's arguments to state keys.** Multi-key
    setters fill missing args from state; the AF-time special case goes.
    `affects` is derived from the bound keys (absorbs (3)'s explicit
    lists).
14. `tests:` requesting one AF time keeps the other; closing the menu.

### Part 3: named actions and triggers

15. **visca: look up movement and preset commands by name.** An
    `actions` map; the `PTZVisca` movement methods, `pantilt_home()`,
    `focus_onetouch()`, `memory_*()`, `send_pending()`'s drive commands,
    `powerOnAtStartup()`/`onOBSShutdown()` and `discover_finish()` use it.
16. **visca: look up triggers by name.** `wb_onepush` moves into a
    `triggers` map consulted by `runTrigger()`.

### Part 4: profiles

17. **visca: gather the controls, actions and triggers into a profile.**
    `ViscaProfile` with parent chain, replace and remove; one built-in
    `generic` profile holding everything from Part 2/3. `PTZVisca` holds
    a `shared_ptr<const ViscaProfile>`. No behaviour change.
18. **visca: choose the profile from the camera's model ID.** Registry of
    built-in profiles with `match` lists; switch on the version reply;
    clear `unsupported_requests` and mark keys stale on a switch. Only
    `generic` is registered, so still no visible change.
19. **visca: add a profile setting.** `visca_profile` (`auto` default) in
    `defaults()`/`update()`/`save()` and the advanced group of
    `get_obs_properties()`, plus locale strings. Existing configs load
    as `auto`.
20. **visca: add a profile for the Sony SRG/BRC block-inquiry cameras.**
21. **visca: add a profile for the BirdDog P100.** Single-value inquiries
    only, green tally, no tally inquiry: what it currently has to learn
    from syntax errors on every connect becomes declared.
22. **ptzsim: report a configurable vendor and model.** `--visca-model`.
23. `tests:` auto selection (BirdDog sim gets `birddog-p100` and sends no
    block inquiries at all), explicit override, unknown model stays on
    `generic` and still falls back on syntax errors.

### Part 5: user command sets

24. **visca: build datagram fields from a type name.** Field factory and
    the field/command JSON reader, with validation. Not yet wired in.
25. **visca: load user profiles from the config directory.** Read
    `visca-profiles/*.json` at module load, register them after the
    built-ins (user `match` wins), log and skip invalid files.
26. **visca: pass user_ state keys and triggers through.** `requestState`,
    state reporting and `runTrigger` accept keys/triggers defined by the
    active profile with the `user_` prefix; document the prefix and bump
    the API minor version (`scripts/gen-api-docs.py`).
27. `tests:` a user profile adds a `user_` control that's set and read
    back via ptzsim, removes a built-in control, and an invalid file is
    rejected without affecting other profiles.
28. **doc: describe VISCA profiles and the user profile format** in
    `doc/visca-protocol.md`, and remove this plan.

Later, separately: built-in profiles as JSON shipped in `data/`; profile
metadata (ranges, choices, labels) driving the state view, and a UI for
`user_` controls; a reload trigger instead of a restart for edited files.

## Decisions to make before starting

1. **Where user profiles live**: shared files in the config directory
   (proposed), or a per-device JSON setting (simpler, but duplicated per
   device and stored in scene collections).
2. **Auto-selection**: switch automatically on the model ID with a
   manual override (proposed), or manual only.
3. **`user_` prefix** for user-defined state keys and triggers, or another
   namespace.
4. **Built-in profiles in C++** for now (proposed: type-checked, and the
   series stays bisectable), with JSON for built-ins as a later decision.
