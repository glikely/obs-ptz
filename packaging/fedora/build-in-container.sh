#!/bin/bash
# Runs inside the container: build the RPM from the checkout mounted at /src.
set -euo pipefail

spec=/spec/obs-ptz.spec
top=$HOME/rpmbuild

# Source0 is a GitHub tag tarball; docker-test.sh makes the same thing from
# the checkout and mounts it at /sources
cp /sources/*.tar.gz "$top/SOURCES/"
cp "$spec" "$top/SPECS/"

rpmlint "$top/SPECS/obs-ptz.spec" || true
rpmbuild -ba "$top/SPECS/obs-ptz.spec"

echo "=== rpmlint on built packages ==="
rpmlint "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm || true

echo "=== package contents ==="
rpm -qlp "$top"/RPMS/*/obs-ptz-[0-9]*.rpm

# Hand the RPMs back to the host
mkdir -p /out && cp -v "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm /out/
