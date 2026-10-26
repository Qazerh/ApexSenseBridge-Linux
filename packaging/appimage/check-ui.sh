#!/bin/sh
# Test the same bundled Qt/QML runtime users receive, not the host's Qt.
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$repo_dir/dist/visual-tests"
result=0
for scale in 1 1.5; do
    mkdir -p "$repo_dir/dist/visual-tests/scale-$scale"
    docker run --rm --network none --user "$(id -u):$(id -g)" \
        --env QT_QPA_PLATFORM=offscreen --env QT_SCALE_FACTOR="$scale" \
        --volume "$repo_dir/dist/visual-tests/scale-$scale:/artifacts" \
        apexsense-appimage-builder /AppDir/AppRun --visual-test --output-dir /artifacts || result=1
done
exit "$result"
