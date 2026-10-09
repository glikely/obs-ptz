#!/bin/bash
# Runs inside the container: build the RPM from the checkout mounted at /src.
set -euo pipefail

spec=/src/packaging/fedora/obs-ptz.spec
version=$(rpmspec -q --qf '%{version}\n' "$spec" | head -1)
top=$HOME/rpmbuild

# Source0 is a GitHub tag tarball; make the same thing from the checkout
git config --global --add safe.directory /src
git -C /src archive --prefix="obs-ptz-$version/" HEAD \
    -o "$top/SOURCES/obs-ptz-$version.tar.gz"
cp "$spec" "$top/SPECS/"

rpmlint "$top/SPECS/obs-ptz.spec" || true
rpmbuild -ba "$top/SPECS/obs-ptz.spec"

echo "=== rpmlint on built packages ==="
rpmlint "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm || true

echo "=== package contents ==="
rpm -qlp "$top"/RPMS/*/obs-ptz-[0-9]*.rpm

# Hand the RPMs back to the host
mkdir -p /out && cp -v "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm /out/
