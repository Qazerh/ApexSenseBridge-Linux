# AppImage packaging

The Debian 13 container keeps PySide6, Qt and KDE QML plugins on the same ABI.
The AppDir includes the Python standard library, selected Python packages,
all installed Qt QML modules/plugins, the engine, source GUI and licenses.
Dynamic dependencies are resolved with ldd. Host glibc and graphics drivers are
not bundled. The minimum runtime glibc is 2.41; no claim is made for older Linux.
The default Qt Quick software renderer avoids host GPU/libstdc++ mismatches;
advanced users can override QT_QUICK_BACKEND.

AppImage tools are official upstream downloads checked against tools.lock.json.
The stable appimagetool version is pinned. The upstream type2 runtime currently
uses a continuous release with a pinned SHA-256: if that asset is replaced, the
build fails safely and the lock must be updated from verified upstream metadata.
Distribution packages are security-updated from the Debian trixie repositories.

## Build

    ./packaging/appimage/build.sh

Results are written to dist/. The script does not modify host packages or udev
rules. Docker must be usable by the calling user. Package installation and all
compilation happen inside the container.

## Validation

The container runs all C++ tests, simulated GUI integration tests and an AppDir
smoke test loading twelve device pages without writing to real hardware.
For an additional check on the produced file:

    QT_QPA_PLATFORM=offscreen ./dist/ApexSenseBridge-Linux-x86_64.AppImage --appimage-extract-and-run --smoke-test

--version works without a display. --install-device-rules invokes the desktop
administrator prompt and installs only the bundled controller/UHID and dock/screen
udev rules and module-load configuration. Normal GUI operation is unprivileged.

Redistribution notices for system libraries are copied from Debian packages.
The root GPL license, vendor licenses and source revision list are included in
the image. Corresponding application source is the repository revision used to
build the artifact; bundled Debian library source is available from Debian.

## Rendered UI regression checks

Every build also runs `check-ui.sh` against the **bundled** Python/Qt/QML runtime.
It renders all 16 sections at 1060×740, 1280×880 and 1600×1000 logical pixels,
at 100% and 150% scaling. A simulated controller and dock are the only devices
visible to the tests; user configuration is isolated in a temporary directory.

Checks reject controls outside card bounds, zero-width switch hit areas,
overlapping labels, clipped descriptions and thumbs outside their tracks.
Pointer clicks on both the switch and the whole row, keyboard activation and
subsequent model updates are exercised. These checks reproduce the old Basic
style switch regression, rather than merely checking that a page loads.

PNG screenshots and JSON results are written to `dist/visual-tests/`. GitHub
Actions uploads these as `visual-layout-regressions`, including on test failure.
An AppImage release is blocked if any layout/interaction assertion fails.

To run against host Qt during development:

    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software python3 gui/tests/test_visual_layout.py

For the exact shipped runtime (after building the Docker image):

    ./packaging/appimage/check-ui.sh

Screenshots are diagnostic artifacts, not pixel-exact golden images: font
rasterization differences do not hide geometry or input regressions.

Successful `main` builds publish or update the `preview` GitHub Release, including
AppImage and checksums. Superseded builds cannot roll the preview back. Version
tags (`v*`) create versioned releases; existing version tags are never moved.
Release publishing is exercised with a fake GitHub CLI before the container
build (`python3 scripts/test_publish_appimage.py`), including update, stale-run,
failed-upload, checksum and pull-request cases. No test publishes real releases.

Device photos and fallback SVG artwork are local app resources, independent of
the host icon theme. Visual checks decode every bundled photo/SVG and check the
rendered device/profile images. Routine backend status is journal-only and is
asserted absent from the shared page header.
