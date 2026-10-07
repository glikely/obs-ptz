# AGENTS.md

Operational notes for working on obs-ptz (PTZ Camera Control for OBS Studio). See
[README.md](README.md) for what the plugin does; this file is about how the build,
packaging, and release machinery actually works, and the traps that aren't obvious
from just reading the code.

## Build system

- `buildspec.json` at repo root declares versions/hashes for `obs-studio`, `prebuilt`
  (obs-deps), `qt6`, and `sdl`. Per-platform `cmake/{macos,windows}/buildspec.cmake`
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

## PTZDevice settings vs. state

A `PTZDevice` has two separate halves, each with its own data, hooks and proc
handlers (see `docs/ptz-device-api.md`):

- **Settings**: persisted, in the "PTZ Control" filter's own settings, the way
  any `obs_source_info` does it. `defaults()`, `update()`, `save()`,
  `get_obs_properties()`. `update()` is handed a complete settings object and
  doesn't modify it. `save()` may add runtime identity (name, id) for the
  plugin's own use; the filter's `.save` strips it again.
- **State**: transient, never saved. `saveState()` (read through the
  `ptz_get_state` proc) and `requestState()` (through `ptz_request_state`),
  which issues the camera command and lets the camera's report make it state.
  It has no properties tree: nothing OBS persists may be bound to it.

Where the camera is pointing is state too, and not a driver's own: the state has
"pan" and "tilt" in [-1, 1] and "zoom" and "focus" in [0, 1], the units of the
movement API. A driver that can read a position back records it with
`setPosition()`; one that can't leaves those keys out.

Never put state, or a button that acts on the camera, in
`get_obs_properties()`: that tree is also shown in OBS's Filters dialog, bound
to the persisted filter settings, so OBS would save whatever it edits and hand
it back to `update()` on load. An action on the camera that isn't a state
change (a one-push white balance, a diagnostic) goes through the `ptz_trigger`
proc instead.

## Legacy self-managed devices

Before devices were owned by filters, they were kept in the plugin's own
`config.json`, bound to a source by name. `src/ptz-legacy-migration.cpp` turns
what an old `config.json` still has into filters when a scene collection loads,
and is to be removed two releases after the first one that has it (see
TODO.md, which lists what goes with it).

- It copies `config.json` to `config.json.pre-filter-migration`, and the
  scene collection to `<name>.json.pre-ptz-filter-migration`, before
  changing either, and never overwrites a copy. They are a user's way back.
- A device whose source is in the collection becomes a filter on it. One whose
  source no collection has, or that never had one, gets a hidden colour
  source in the current scene. So does a source of the user's that refuses the
  filter, which OBS does without saying.
- Devices have no ids. A device is its filter, which is named by its UUID
  (`obs_source_get_uuid()`), kept in the scene collection by OBS. Action sources
  and the selected camera that named an old device by its id are changed to
  name its filter by UUID.
- An entry is kept in `config.json`, with the collections it was made into,
  until every collection that has its source has it, so a source that is in
  more than one collection gets a device in each.

## What a device can do

A `PTZDevice` says what it can do with `features()`, flags the UI enables
controls by: the dock's buttons, the camera list's menu, the state view's
diagnostics. They are in its state as `"features"`, an object with the name of
each it can do true (`PTZDevice::featureNames()`). A new driver overrides
`features()`, with only what it really does: a control for something it doesn't
is shown disabled, not left to do nothing. One that only finds out once it is
talking to the camera (ONVIF's imaging service, a UVC camera's controls, a VISCA
camera's command set) calls `featuresChanged()` when it does. A device without
`"features"` predates them, and the UI takes it to do anything. A new feature is
an addition to the API, as a state key is.

## PTZ API version

The procs and signals in `docs/ptz-device-api.md` are an API other plugins
and scripts call, versioned by `PTZ_API_VERSION_MAJOR`/`_MINOR` in
`src/ptz.h` and reported by the `ptz_get_api_version` proc on each device's own proc_handler
(another plugin can implement the per-device API, at another version, so
callers check per device). Any change to
them, or to the calldata fields, state keys or trigger names they take,
bumps it: the minor version for an addition, the major version (minor back
to 0) for a removal, rename or change of meaning. Then change
`docs/ptz-device-api.md` to match, which is written by hand: it is the
specification, and `tests/obs-integration/test_api_doc.py` fails when it and the
plugin disagree. That test compares it with what `ptz_proc_add()` and
`ptz_signal_add()` in `src/ptz-device.cpp` log, so register every proc and signal
of the API through them, not with `proc_handler_add()` directly.

A device is named by its filter, and found again by the filter's UUID; the
plugin has no ids, and no handlers of its own for a caller to reach a device
through. The device list finds devices the way any other plugin could, from OBS's
`source_filter_add` signal and a filter's answer to `ptz_get_api_version`,
with nothing private between the plugin's devices and it.

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

## Git/PR conventions

- Prefer a clean, minimal-diff, logically-ordered commit history over incremental
  fixup commits — squash/reorder before landing, but only when explicitly asked to;
  don't rewrite history proactively.
- Write commit messages that explain **why**, not just what as described
  in CONTRIBUTING.md.
- Keep source comments short, and avoid rewording within a patch series.
  Reworded patches cause conflicts when reordering commits
