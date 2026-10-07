# Fedora packaging

`obs-ptz.spec` builds the plugin against Fedora's `obs-studio-devel`. It has
not been built or run through `fedora-review` yet; do that before submitting.

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
- **License tag.** Source files carry `SPDX-License-Identifier: GPLv2`, which
  isn't a valid SPDX id, and `LICENSE` is the plain GPLv2 text. Ask upstream to
  switch to `GPL-2.0-only` or `GPL-2.0-or-later` so the `License:` field is
  defensible.
- **Release tag.** `Source0` assumes tags named `vX.Y.Z`. If upstream tags
  differ, adjust it.
- **Version string.** The build runs `git describe`, but a release tarball has
  no `.git`, so it falls back to `buildspec.json` (`0.19.0`). That's intended.
- **`obs-studio` dependency.** The ABI is not stable between OBS releases, so
  you will probably need to rebuild this package whenever `obs-studio` updates.
