# AGENTS.md

Operational notes for working on obs-ptz (PTZ Camera Control for OBS Studio). See
[README.md](README.md) for what the plugin does; this file is about how the build,
packaging, and release machinery actually works, and the traps that aren't obvious
from just reading the code.

## Build system

- `buildspec.json` at repo root declares versions/hashes for `obs-studio`, `prebuilt`
  (obs-deps), `qt6`, `qtserialport`, and `sdl`. Per-platform `cmake/{macos,windows}/buildspec.cmake`
  map those entries to actual filenames/URLs and drive `_check_dependencies()` in
  `cmake/common/buildspec_common.cmake`.
- macOS and Windows **vendor-build** SDL and Qt's SerialPort submodule from source
  (`_setup_sdl()`, `_setup_qt_submodule()`) rather than using prebuilt binaries. SDL has
  **no skip-if-cached logic** — it reconfigures/rebuilds on every configure, unlike
  `prebuilt`/`qt6` which check an installed VERSION file first.
- `_setup_sdl()` deliberately trims SDL to a minimal build: joystick/gamepad + haptic
  rumble only. `SDL_VIDEO` stays **on** even though nothing is ever rendered — SDL's
  joystick event pump needs it initialized on some platforms. `SDL_AUDIO` is off, so
  don't add `SDL_INIT_AUDIO` to `SDL_Joysticks.cpp` without also flipping that build
  flag, or `SDL_Init()` will fail on every launch.
- Linux does **not** vendor-build anything — it's the only platform with no
  `cmake/linux/buildspec.cmake`, and gets SDL2, Qt SerialPort, obs-studio dev headers,
  etc. straight from apt (see `.github/scripts/utils.zsh/setup_ubuntu`). Keep this in
  mind before assuming a buildspec.json bump (e.g. SDL) affects Linux — it doesn't.

## Linux packaging

- `.deb`/`.ddeb` files use Debian Policy's standard `name_version_arch.deb` naming
  (`CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT"` in `cmake/linux/defaults.cmake`), with the
  Ubuntu release **codename** folded into the version as a `~` suffix (e.g.
  `obs-ptz_0.19.0~noble_arm64.deb`) — the same convention the obsproject PPA itself
  uses. This exists so builds for different Ubuntu releases on the same architecture
  don't collide/overwrite each other in a release (this happened for real once —
  see `4099d4e`).
- `obs-ptz` depends on the **`obs-studio` package**, not `libobs`/`libobs0`. This is a
  deliberate post-build fixup, not a CMake option: CPack's
  `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` is all-or-nothing (auto-detected deps always get
  appended to, never replace, manually-specified ones — there's no CMake-level hook to
  exclude just `libobs`), so `.github/scripts/package-ubuntu` extracts the built `.deb`
  (`dpkg-deb -R`), rewrites the dependency line with `sed`, and repacks it
  (`dpkg-deb -b`). **Why it matters**: the obsproject PPA's `obs-studio` package
  `Conflicts: libobs0`, so a plain `libobs0` dependency gets `obs-ptz` silently
  auto-removed by `apt` whenever a user upgrades to the PPA build. Depending on the
  `obs-studio` package name instead survives that upgrade cleanly — verified against
  the real PPA package, not just reasoned about (see `b665184`).

## Releases

- `action-gh-release` + `draft: true` means GitHub doesn't attach a real git tag to
  the release while it's a draft — it shows under an auto-generated `untagged-<hash>`
  slug in the releases list until a maintainer manually publishes it. This is expected
  behavior, not a bug to chase.
- `release/vX.Y.x-for-obs-A.B.C` branches exist to keep shipping updates against an
  older pinned OBS Studio version (older `buildspec.json`) after `main` has moved on.
  Platform build jobs can be selectively turned off per branch with `if: false` on the
  job in `.github/workflows/build-project.yaml` (see `ubuntu-build`) — nothing else in
  the workflows references a platform job by name, so this is safe without touching
  `push.yaml`'s `needs:`.
- When drafting/updating release notes, don't retroactively rewrite an
  already-published (non-draft) release's notes without asking — treat those as a
  historical record. Only the current draft should get updated.

## Translations

- `crowdin.yml` + `.github/workflows/crowdin.yml` sync `data/locale/*.ini` with
  Crowdin on every push to `main`. `upload_translations: true` — local translation
  files (including AI/machine-generated ones, see `8de12df`) get uploaded as
  unapproved suggestions (`update_option: update_as_unapproved` in `crowdin.yml`) for
  human review, not just pulled down. If a locale file has a block that needs human
  review, comment it as such so it's easy to find later.

## Verification practices

- Prefer testing real built artifacts over reasoning about CMake/CPack behavior from
  docs alone — several bugs here (the filename collision, the PPA conflict) were only
  actually confirmed/fixed by installing real `.deb`s against a real OBS Studio.
- The obsproject PPA is **amd64-only** — there's no arm64 build, so an arm64 Linux VM
  can't reproduce PPA-specific scenarios. Needs an amd64 environment (e.g. emulation)
  instead.
- `ldd path/to/obs-ptz.so | grep -E 'not found|libobs'` is a quick, real check that
  symbol resolution works against whatever `libobs`/`libobs-frontend-api` is actually
  installed (distro vs. PPA).

## PTZDevice / PTZListModel decoupling

`PTZListModel` (`src/ptz-list-model.*`, the frontend Qt model) never holds a
`PTZDevice*` or includes a driver header. It only knows a device by its
integer `device_id`, plus the `proc_handler_t*`/`signal_handler_t*` pair
handed to it once, on creation, over the global PTZ signal_handler's
`"ptz_device_create"` signal. All control goes out through
`proc_handler_call()`; all notification comes back through
`signal_handler_connect()` callbacks. `PTZListModel` keeps a small
per-device cache (name, status flags, preset list) purely so
`QAbstractItemModel::data()` stays synchronous; the cache is seeded from a
`ptz_get_state()`/`ptz_preset_get_list()` proc_handler call and kept in sync
solely by signals, never by a direct method call on a `PTZDevice`.

### API surface

Everything below is declared once, not per device: the global signals in
`ptz_load_devices()`; the per-device proc_handler/signal_handler entries in
`PTZDevice::registerFilterHandlers()`, called once per *filter* from
`ptz_filter_create()` (the per-device `handler`/`sigs` pointers are borrowed
from the owning filter's `obs_source_t*`, not allocated per `PTZDevice`
instance -- see the constructor's comment in `ptz-device.cpp`).

**Global PTZ signal_handler** (`ptz_get_signal_handler()`) -- device
lifecycle, connected once in `PTZListModel`'s constructor:

| Signal | Payload | Fired by |
| --- | --- | --- |
| `ptz_device_create(int device_id, ptr proc_handler, ptr signal_handler)` | the new device's own proc/signal handlers | `PTZDevice::announceCreated()`, called by `ptz_filter_create()`/`ptz_filter_update()` once the full (base+derived) object exists and is assigned -- deliberately not from the constructor itself, see that method's comment |
| `ptz_device_destroy(int device_id)` | -- | `PTZDevice::~PTZDevice()` |

`device_create_cb()`/`device_destroy_cb()` route these into
`PTZListModel::deviceCreated()`/`deviceDestroyed()`, which append/remove the
row and (on create) connect the per-device entries below.

**Per-device proc_handler** -- the subset `PTZListModel` actually calls
(`entry->ph` is already bound to one specific device, so unlike the signals
below, calldata's `device_id` field is informational, not load-bearing for
dispatch):

| Method | In | Out | Called from |
| --- | --- | --- | --- |
| `ptz_get_state()` | -- | `ptr return`: caller-owned `obs_data_t` (name, description, type, connected/live/preview/locked, supports_set_home, max_presets) | `refreshDeviceState()` -- seeds/refreshes the row cache |
| `ptz_preset_get_list()` | -- | `ptr return`: caller-owned `obs_data_array_t` of `{id, name, token}` | `refreshPresetList()` |
| `ptz_get_config(ptr config)` | `ptr config`: caller-owned `obs_data_t` to fill | -- | `save()`, wraps `PTZDevice::save()` |
| `ptz_set_name(string name)` | -- | -- | `renameDevice()` |
| `ptz_set_locked(bool locked)` | -- | -- | `setData()` for `IsLockedRole` |
| `ptz_scene_changed()` | -- | -- | `onSceneChanged()`, fanned out to every device |
| `ptz_preset_new(int row)` | -- | `int return`: new preset id | `insertRows()` |
| `ptz_preset_remove(int row)` | -- | -- | `removeRows()` |
| `ptz_preset_move(int src_row, int dest_row)` | -- | -- | `moveRows()` |
| `ptz_preset_set_name(int id, string name)` | -- | -- | `setData()` on a preset row's `Qt::EditRole` |

`ptz_set_config(ptr config)` and `ptz_get_properties()` are also registered
(wrapping `update()`/`get_obs_properties()`), but `PTZListModel` no longer
calls either -- `PTZSettings` and the source's own Filters dialog both go
through the filter's `obs_source_update()`/`obs_source_properties()`
instead (see "PTZ Control filter update path" below), which reaches the
same `PTZDevice` methods but via `ptz_filter_update()`/
`ptz_filter_get_properties()`, bypassing `PTZListModel` entirely. The
proc_handler also carries the pre-existing movement API (`ptz_stop`,
`ptz_move`, `ptz_preset_recall`, etc.) for the dock/joystick/scripting --
`PTZListModel` doesn't call those either.

**Per-device signal_handler** -- everything `PTZListModel` listens to,
connected once per device in `deviceCreated()`:

| Signal | Meaning |
| --- | --- |
| `state_changed(int device_id)` | something about this device changed -- status, rename, or settings alike, one signal for all three (see below) |
| `preset_insert`/`preset_inserted(int device_id, int row)` | before/after a preset insert |
| `preset_remove`/`preset_removed(int device_id, int row)` | before/after a preset removal |
| `preset_move(int device_id, int src_row, int dest_row)` (`bool return`) / `preset_moved(...)` | before/after a preset move; the "before" signal's `return` lets the caller veto, mirroring `beginMoveRows()`'s own bool result |
| `preset_renamed(int device_id, int id)` | a preset's name changed |

`status_changed`, `renamed`, and `settings_changed` used to be three
separate signals; collapsed into the single `state_changed` above because
none of them carried enough of a payload for a listener to act on
selectively -- every one of them just means "re-read this device's state",
so there was nothing a separate signal per change kind let a listener do
differently.

### Notes

- `ptzDeviceList` (the `PTZListModel` singleton) is constructed by
  `PTZListModel::create()`, called from `ptz_load_devices()` -- i.e. at
  `obs_module_load()` time, not as a plain static-storage global. A plain
  global's constructor runs at plugin-library-load time, before
  `obs_module_load()` gets to run anything, which would be a trap the first
  time a `PTZListModel` constructor needs something module-load sets up
  (as it now does: the PTZ signal_handler it connects to).
- The real `PTZDevice*` objects, the id-uniqueness registry, and the driver
  factory (`ptz_device_create()`/`ptz_device_destroy()`, dispatching on
  `config["type"]`) all live in `src/ptz-device.cpp`, private to that
  translation unit -- neither is exported via `ptz.h`, and neither is
  called by `PTZListModel` or `settings.cpp` at all. A device's lifetime is
  entirely driven by its owning "PTZ Control" filter's own OBS callbacks
  (`ptz_filter_create()`/`ptz_filter_update()`/`ptz_filter_destroy()`, same
  file): adding/removing the filter, or changing its `"type"`, is what
  creates or destroys the `PTZDevice` -- `PTZListModel` only ever finds out
  about it afterward, via the global `ptz_device_create`/`_destroy` signals
  above.
- The begin/end preset-mutation signal pairs exist because
  `signal_handler_signal()` dispatches to connected callbacks synchronously
  (same thread, no queueing) -- exactly like the direct method calls they
  replaced -- so `PTZListModel`'s "before" callback can still call
  `beginInsertRows()`/etc. ahead of the mutation actually happening, and its
  cache refresh on the "after" callback happens before `endInsertRows()`/etc.
  return, satisfying `QAbstractItemModel`'s contract that row data is
  already updated by the time `end*Rows()` is called.
- Trap: don't write `begin*/end*` in a `/* */` block comment in this file --
  the literal `*/` silently ends the comment early and the rest becomes
  live (broken) code. Spell it `begin.../end...` instead. Hit this twice
  while building this design.

## PTZ Control filter update path (Cameras tab / Filters dialog)

`PTZSettings`'s Cameras tab and the "PTZ Control" filter's own native
Filters-dialog properties are two different UI surfaces over the *same*
underlying state, and getting the plumbing between them right had several
non-obvious traps:

- Key everything on the filter's `obs_source_t*` (via
  `ptz_device_find_filter_source(device_id)`), not on `device_id`, for any
  operation that might change the device's type. `device_id` is not a
  stable identity across a type change: `ptz_filter_update()`
  (`ptz-device.cpp`) destroys the old `PTZDevice` and constructs a new one
  via `ptz_device_create()`, which -- since the old device isn't actually
  gone yet (`deleteLater()`, so its id is still taken) -- assigns the
  replacement a *different* id. A `device_id` captured before an
  `obs_source_update()` call may already be stale by the time it returns.
- `OBSPropertiesView`'s update callback (`update_cb`/`updateProperties()`)
  is called with `new_settings` pointing at the *same* `OBSData` object the
  view renders from -- the widget already wrote the new value into it
  before the callback runs. Comparing "old" vs "new" by reading both out of
  that object doesn't work; they're aliased. If you need the pre-edit
  value, capture it earlier (e.g. when the row was selected), not inside
  the update callback itself.
- `ptz_filter_update()` must forward ordinary field edits (host, port,
  speeds, ...) to the live device (`ptzf->ptz->update(settings)`) even when
  `"type"` didn't change -- it originally only ever handled the type-change
  case, so edits made while the type stayed the same were silently dropped
  on the floor. Whichever surface (Filters dialog or `PTZSettings`) didn't
  originate an edit is showing a stale settings snapshot afterward and
  needs `obs_source_update_properties()` (via `PTZDevice::notify_properties_changed()`,
  queued -- see the comment on why: calling it synchronously mid-`.update()`
  reenters the properties dialog that's still on the call stack) to know to
  refresh, regardless of which branch of `ptz_filter_update()` ran.
- `PTZDevice::update()` needs to fire `notifySettingsChanged()` itself so
  `PTZListModel`'s cache stays live for *any* caller, not just ones that
  happen to remember to force a refresh afterward -- `PTZListModel::update()`
  used to do that refresh manually as a caller-side workaround, and once it
  was removed (nothing called it anymore) that workaround went with it, so
  the signal has to come from `update()` itself or the row's cached values
  go stale silently.
- `QItemSelectionModel::reset()` -- which `QAbstractItemView::reset()`
  runs automatically in response to a model's `modelReset` signal -- clears
  the view's current index *without* emitting `currentChanged`. A slot
  connected to `modelReset` that tries to reselect a row by name after a
  device is destroyed-and-replaced (a type change) must explicitly call
  its own refresh logic when there's nothing left to reselect (e.g. after
  picking "unset"); relying on Qt to emit `currentChanged` on its own in
  that case doesn't happen, and a properties panel is left showing stale
  values for a device that's already gone.

## Git/PR conventions

- Prefer a clean, minimal-diff, logically-ordered commit history over incremental
  fixup commits — squash/reorder before landing, but only when explicitly asked to;
  don't rewrite history proactively.
- Write commit messages that explain **why**, not just what as described
  in CONTRIBUTING.md.
