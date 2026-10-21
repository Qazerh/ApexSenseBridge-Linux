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
