#!/bin/bash
# Build obs-ptz.spec (the working copy's) in a Fedora container. RPMs land in
# ./rpms-out.
#   packaging/fedora/docker-test.sh [fedora-version]   (default: rawhide)
# PLATFORM=linux/amd64|linux/arm64|linux/ppc64le|linux/s390x builds for another
# architecture under emulation (slow). The sources are the tag the spec's Source0 downloads (v<upstream_version>);
# set SOURCE_REF=HEAD (or any ref) to build another tree with the same spec.
set -euo pipefail
cd "$(dirname "$0")"
# Always pass an explicit platform: pulling another architecture's image under
# the same tag otherwise makes a later plain `docker run` use the wrong one.
if [ -z "${PLATFORM:-}" ]; then
    case $(docker info --format '{{.Architecture}}') in
        aarch64|arm64) PLATFORM=linux/arm64 ;;
        x86_64|amd64)  PLATFORM=linux/amd64 ;;
        *) echo "set PLATFORM to the docker platform to use" >&2; exit 1 ;;
    esac
fi
repo=$(git rev-parse --show-toplevel)
ver=${1:-rawhide}
image=obs-ptz-fedora-$ver-${PLATFORM//\//-}

docker build --platform "$PLATFORM" --build-arg FEDORA_VERSION="$ver" ${BASE_IMAGE:+--build-arg BASE_IMAGE="$BASE_IMAGE"} -t "$image" .
mkdir -p rpms-out && chmod 777 rpms-out
# Tarball is made on the host: a worktree's .git file points outside the mount
upstream_version=$(sed -n 's/^%global upstream_version[[:space:]]*//p' obs-ptz.spec)
ref=${SOURCE_REF:-v$upstream_version}
srcdir=$(mktemp -d)
trap 'rm -rf "$srcdir"' EXIT
git -C "$repo" archive --prefix="obs-ptz-$upstream_version/" "$ref" \
    -o "$srcdir/obs-ptz-$upstream_version.tar.gz"
docker run --platform "$PLATFORM" --rm -v "$srcdir":/sources:ro -v "$PWD":/spec:ro -v "$PWD/rpms-out":/out \
    "$image"
