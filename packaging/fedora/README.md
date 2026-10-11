# Fedora packaging

`obs-ptz.spec` builds the plugin against Fedora's `obs-studio-devel`. It builds,
passes its unit tests and installs in containers (see below), but has not been
run through `fedora-review`, and the packaged plugin has not been loaded in a
real OBS; do both before submitting.

## Test in Docker

```
packaging/fedora/docker-test.sh [fedora-version]   # default: rawhide
packaging/fedora/install-test.sh [fedora-version]
```

`docker-test.sh` builds an image from `Dockerfile` (build dependencies are
installed from the spec with `dnf builddep`), then builds the RPM from the
upstream tag that `Source0` downloads (`v<upstream_version>`, read from the
spec; set `SOURCE_REF=HEAD` to build your checkout with the same spec) and runs
`rpmlint`. The spec itself is taken from your working copy. The RPMs end up in
`packaging/fedora/rpms-out/`.

`install-test.sh` then installs those RPMs into a clean Fedora container
together with `obs-studio` and checks that `obs-ptz.so` lands in the same
directory as OBS's own plugins (`%{_libdir}/obs-plugins`) and resolves all its
symbols, and that the data directory (`/usr/share/obs/obs-plugins/obs-ptz`) is
installed.

Both default to the host's architecture. Set
`PLATFORM=linux/amd64|linux/arm64|linux/ppc64le|linux/s390x` to build and test
another one under emulation (slow). Set `BASE_IMAGE=docker.io/library/fedora`
if `registry.fedoraproject.org` is not reachable.

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
- **Version and pre-releases.** The spec packages the tag `v0.20.0-pre4` as
  `Version: 0.20.0~pre4` (`%global upstream_version` holds the tag spelling;
  the tilde sorts it before the final release). For v1.0.0 set both to `1.0.0`.
- **License tag.** Upstream relicensed to GPL-2.0-or-later and fixed the
  invalid `GPLv2` SPDX ids in `8fe88490`, and v0.20.0-pre4 has no GPL-2.0-only
  text left, so the tag is `GPL-2.0-or-later AND MIT AND Zlib`. Earlier tags,
  such as v0.20.0-pre2, are still GPL-2.0-only.
- **`rpmlint`.** Clean on rawhide and F43, with no filters. The spec has a
  `%check` that runs the soft-PTZ and USB-backend unit tests and checks the
  shipped VISCA command sets with the plugin's own reader. They are built in a
  second CMake tree (`redhat-linux-build-tests`) with the `ENABLE_*_TESTS` and
  `ENABLE_VISCA_PROFILE_CHECK` options, so none of those options, test sources
  or the Catch2 copy vendored in `shared/catch2` (BSL-1.0) reach the shipped
  plugin. Tests that need OBS, a camera or a serial port can't run in a build
  root and are not enabled.
- **Architectures.** The spec has `ExcludeArch: %{ix86}`, the same as
  `obs-studio`, which is built for x86_64, aarch64, ppc64le and s390x. Tested
  by building, running `%check` and installing the RPM on: aarch64 (rawhide,
  F43), ppc64le and s390x (rawhide, under QEMU; s390x is big-endian), and
  x86_64 (F43, under emulation; rawhide's `tar` hits `ENOSYS` under Docker
  Desktop's Rosetta layer, an emulation artifact).
- **x86_64 and SIMDe.** obs-studio's `util/sse-intrin.h` always includes SIMDe
  with native aliases, and x86 intrinsics headers (pulled in by Qt's `qsimd.h`
  from moc-generated files) then fail to compile when included after
  `obs.h`: `redeclared as different kind of entity` in `smmintrin.h`.
  `CMakeLists.txt` now includes `<immintrin.h>` first on x86 Linux (since
  v0.20.0-pre4), so the spec needs no workaround for it. A release older than
  that fails to build on x86_64 without `-include immintrin.h` in the C++ flags.
- **Build type.** The spec passes `-DCMAKE_BUILD_TYPE=None`: the project
  otherwise defaults to `RelWithDebInfo`, and sdl2-compat's imported
  `SDL2::SDL2` only has a "noconfig" location, so configure fails.
- **Release tag.** `Source0` assumes tags named `vX.Y.Z`. If upstream tags
  differ, adjust it.
- **Version string.** The build runs `git describe`, but a release tarball has
  no `.git`, so it falls back to the version in `buildspec.json` (here
  `0.20.0-pre4`, which the build log confirms). That must match the tag, so
  bump it before tagging a release.
- **`obs-studio` dependency.** The ABI is not stable between OBS releases, so
  you will probably need to rebuild this package whenever `obs-studio` updates.
