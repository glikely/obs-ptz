# Code signing policy

Release builds for Windows and macOS are signed, so that the operating system
can show they come from this project and haven't been changed since.

- **Windows**: free code signing provided by [SignPath.io](https://about.signpath.io/),
  certificate by [SignPath Foundation](https://signpath.org/).
  The plugin binaries and the installer are signed.
- **macOS**: the plugin and its `.pkg` installer are signed with an Apple
  Developer ID certificate, and the installer is notarized by Apple.

Linux packages are not signed.

Only binaries built by this repository's GitHub Actions workflows, from its own
source code, are signed; nothing is signed on a developer's machine. Releases
are built from a signed release tag. On Windows, the signing key never leaves
SignPath's hardware security module, and only builds of a release tag are
submitted for signing with the release certificate; other builds are signed
with SignPath's test certificate, which Windows doesn't trust. On macOS, the
Developer ID certificate is kept as an encrypted GitHub Actions secret, and is
loaded into a temporary keychain on the CI runner only for the build.

## Team roles

- Committers and reviewers: [Grant Likely](https://github.com/glikely)
- Approvers: [Grant Likely](https://github.com/glikely)

Committers may change the source code without further review. Every change
from anyone else, such as a pull request, must be reviewed by a team member.
Every signing request must be approved by a team member trusted by the entire
team. All team members use multi-factor authentication for both GitHub and
SignPath.

## Privacy policy

This program will not transfer any information to other networked systems
unless specifically requested by the user or the person installing or
operating it.

The plugin talks only to the cameras you add. While the Add Device dialog is
open, or when an obs-websocket client asks it to, it also looks for cameras by
sending discovery requests on your local network. It also answers
requests through obs-websocket, if you have it enabled in OBS. It has no
telemetry, and doesn't check for updates. A camera report stays on your
computer unless you send it: you can save it to a file or copy it, or open a
GitHub issue form for it in your browser, with the camera's name and the
plugin's version in the link, and the report copied for you to paste in.
