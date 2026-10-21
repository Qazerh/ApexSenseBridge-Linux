#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$repo_dir/dist"
docker build --file "$repo_dir/packaging/appimage/Dockerfile" --tag apexsense-appimage-builder "$repo_dir"
docker run --rm --user "$(id -u):$(id -g)" --volume "$repo_dir/dist:/export" apexsense-appimage-builder
printf '\nAppImage and checksum: %s/dist\n' "$repo_dir"
