#!/bin/bash
# Build obs-ptz.spec in a Fedora container from the current checkout (HEAD
# plus nothing uncommitted: commit first). RPMs land in ./rpms-out.
#   packaging/fedora/docker-test.sh [fedora-version]   (default: rawhide)
set -euo pipefail
cd "$(dirname "$0")"
repo=$(git rev-parse --show-toplevel)
ver=${1:-rawhide}
image=obs-ptz-fedora-$ver

docker build --build-arg FEDORA_VERSION="$ver" ${BASE_IMAGE:+--build-arg BASE_IMAGE="$BASE_IMAGE"} -t "$image" .
mkdir -p rpms-out && chmod 777 rpms-out
docker run --rm -v "$repo":/src:ro -v "$PWD/rpms-out":/out \
    "$image"
