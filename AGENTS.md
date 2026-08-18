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
handed to it once, on creation, over `ptz_get_signal_handler()`'s
`"ptz_device_create"` signal. All control goes out through
`proc_handler_call()`; all status/list-mutation notification comes back
through `signal_handler_connect()` callbacks on that device's own signal
handler (a single `state_changed` signal covering status/rename/settings
changes alike, plus the paired `preset_insert`+`preset_inserted`-style
before/after signals/etc. -- see `PTZDevice::PTZDevice()` for the full
list). `PTZListModel` keeps a small
per-device cache (name, status flags, preset list) purely so
`QAbstractItemModel::data()` stays synchronous; the cache is seeded from a
`ptz_get_state()`/`ptz_preset_get_list()` proc_handler call and kept in sync
solely by those signals, never by a direct method call on a `PTZDevice`.
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
  translation unit. `PTZListModel`/`settings.cpp` call the factory
  functions by `OBSData`/`device_id`; they never `new` a driver class.
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

## Git/PR conventions

- Prefer a clean, minimal-diff, logically-ordered commit history over incremental
  fixup commits — squash/reorder before landing, but only when explicitly asked to;
  don't rewrite history proactively.
- Write commit messages that explain **why**, not just what as described
  in CONTRIBUTING.md.
