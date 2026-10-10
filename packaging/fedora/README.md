# Fedora packaging

`obs-ptz.spec` builds the plugin against Fedora's `obs-studio-devel`. It has
not been built or run through `fedora-review` yet; do that before submitting.

## Test in Docker

```
packaging/fedora/docker-test.sh [fedora-version]   # default: rawhide
```

This builds an image from `Dockerfile` (build dependencies are installed from
the spec with `dnf builddep`), then builds the RPM from the committed `HEAD` of
your checkout and runs `rpmlint`. The RPMs end up in `packaging/fedora/rpms-out/`.
After a build, `packaging/fedora/install-test.sh [fedora-version]` installs
the RPMs from `rpms-out/` into a clean Fedora container together with
`obs-studio` and checks that `obs-ptz.so` lands in the same directory as OBS's
own plugins (`%{_libdir}/obs-plugins`), resolves all its symbols, and that the
data directory (`/usr/share/obs/obs-plugins/obs-ptz`) is installed.
Commit your spec changes first; uncommitted changes aren't included. Set
`BASE_IMAGE=docker.io/library/fedora` if `registry.fedoraproject.org` is not
reachable.

## Test locally

```
sudo dnf install fedora-packager fedora-review rpmlint mock
sudo usermod -aG mock $USER   # log out and back in
spectool -g obs-ptz.spec      # downloads Source0 (the v<version> tag must exist)
rpmlint obs-ptz.spec
fedpkg --release rawhide mockbuild   # or: mock -r fedora-rawhide-x86_64 --buildsrpm ...
fedora-review -b <bugzilla-id>       # once the review ticket exists
```

Also run the plugin in OBS on Fedora: the dock appears, a camera can be added,
and the joystick and USB camera paths load.

## Submit to Fedora

1. Create a Fedora Account (accounts.fedoraproject.org), sign the FPCA, and add
   an SSH key.
2. Read the Packaging Guidelines and "Joining the Package Collection
   Maintainers". A first-time packager needs a **sponsor**: say so in the
   review request and introduce yourself on the Fedora devel list.
3. Push the spec and an SRPM somewhere public (a COPR project is easiest:
   `copr-cli build`), and confirm it builds for all supported Fedora releases.
4. File a **Package Review** request in Bugzilla (product Fedora, component
   Package Review) with the spec and SRPM URLs, a short description, and
   "I need a sponsor".
5. Address the reviewer's findings (`fedora-review` output is a good checklist).
   When approved, the review bug gets `fedora-review+`.
6. Request the repo: `fedpkg request-repo obs-ptz <review-bug-id>`, then
   `fedpkg request-branches` for the release branches you want.
7. `fedpkg clone obs-ptz`, import the spec and sources (`fedpkg new-sources`),
   `fedpkg build` on rawhide, then submit updates for stable branches in Bodhi.
8. For each new upstream release: bump `Version`, upload the new tarball, build,
   and file the Bodhi update.

## Points a reviewer will check

- **Bundled code.** `Provides: bundled(...)` lists what is copied into the
  plugin. Confirm the entries and licenses against the review guidelines, or
  patch the build to use system copies where Fedora ships them.
- **License tag.** Upstream relicensed to GPL-2.0-or-later and fixed the
  invalid `GPLv2` SPDX ids (commit `8fe88490`, newer than v0.19.0), so the tag
  is `GPL-2.0-or-later AND MIT AND Zlib`. A tarball of v0.19.0 itself is still
  GPL-2.0-only; the tag must be `GPL-2.0-only AND ...` for that release.
- **Known `rpmlint` output.** Tested clean on rawhide (F46) and F43 apart from
  `no-%check-section` (the plugin only loads inside OBS, so there are no
  runnable tests) and `incorrect-fsf-address`, which comes from the stock GPLv2
  text in `LICENSE`; report it upstream, don't patch it in the package.
- **Build type.** The spec passes `-DCMAKE_BUILD_TYPE=None`: the project
  otherwise defaults to `RelWithDebInfo`, and sdl2-compat's imported
  `SDL2::SDL2` only has a "noconfig" location, so configure fails.
- **Release tag.** `Source0` assumes tags named `vX.Y.Z`. If upstream tags
  differ, adjust it.
- **Version string.** The build runs `git describe`, but a release tarball has
  no `.git`, so it falls back to `buildspec.json` (`0.19.0`). That's intended.
- **`obs-studio` dependency.** The ABI is not stable between OBS releases, so
  you will probably need to rebuild this package whenever `obs-studio` updates.
