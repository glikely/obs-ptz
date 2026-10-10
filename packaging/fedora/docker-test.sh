#!/bin/bash
# Build obs-ptz.spec (the working copy's) in a Fedora container. RPMs land in
# ./rpms-out.
#   packaging/fedora/docker-test.sh [fedora-version]   (default: rawhide)
# The sources are the tag the spec's Source0 downloads (v<upstream_version>);
# set SOURCE_REF=HEAD (or any ref) to build another tree with the same spec.
set -euo pipefail
cd "$(dirname "$0")"
repo=$(git rev-parse --show-toplevel)
ver=${1:-rawhide}
image=obs-ptz-fedora-$ver

docker build --build-arg FEDORA_VERSION="$ver" ${BASE_IMAGE:+--build-arg BASE_IMAGE="$BASE_IMAGE"} -t "$image" .
mkdir -p rpms-out && chmod 777 rpms-out
# Tarball is made on the host: a worktree's .git file points outside the mount
upstream_version=$(sed -n 's/^%global upstream_version[[:space:]]*//p' obs-ptz.spec)
ref=${SOURCE_REF:-v$upstream_version}
srcdir=$(mktemp -d)
trap 'rm -rf "$srcdir"' EXIT
git -C "$repo" archive --prefix="obs-ptz-$upstream_version/" "$ref" \
    -o "$srcdir/obs-ptz-$upstream_version.tar.gz"
docker run --rm -v "$srcdir":/sources:ro -v "$PWD":/spec:ro -v "$PWD/rpms-out":/out \
    "$image"
