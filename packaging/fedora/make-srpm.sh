#!/bin/bash
# Build a source RPM from obs-studio-plugin-ptz.spec and the tarball its Source0 downloads
# (the real GitHub archive of the tag, not one made from a checkout), for
# uploading to COPR or attaching to a Fedora Package Review. The SRPM lands in
# ./srpm-out.
#   packaging/fedora/make-srpm.sh [fedora-version]   (default: rawhide)
# PLATFORM and BASE_IMAGE work as for docker-test.sh.
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${PLATFORM:-}" ]; then
    case $(docker info --format '{{.Architecture}}') in
        aarch64|arm64) PLATFORM=linux/arm64 ;;
        x86_64|amd64)  PLATFORM=linux/amd64 ;;
        *) echo "set PLATFORM to the docker platform to use" >&2; exit 1 ;;
    esac
fi
ver=${1:-rawhide}
base=${BASE_IMAGE:-registry.fedoraproject.org/fedora}

mkdir -p srpm-out && chmod 777 srpm-out
docker run -i --rm --platform "$PLATFORM" -v "$PWD":/spec:ro -v "$PWD/srpm-out":/out \
    "$base:$ver" bash -s <<'INNER'
set -euo pipefail
dnf -y -q install rpm-build rpmdevtools rpmlint glibc-langpack-en >/dev/null
rpmdev-setuptree
cp /spec/obs-studio-plugin-ptz.spec ~/rpmbuild/SPECS/
spectool -g -R ~/rpmbuild/SPECS/obs-studio-plugin-ptz.spec
rpmbuild -bs ~/rpmbuild/SPECS/obs-studio-plugin-ptz.spec
srpm=$(ls ~/rpmbuild/SRPMS/obs-studio-plugin-ptz-*.src.rpm)
rpmlint "$srpm" || true
cp -v "$srpm" /out/
echo "Source0 sha256: $(sha256sum ~/rpmbuild/SOURCES/*.tar.gz | cut -d' ' -f1)"
INNER
