FROM debian:trixie-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build python3 python3-pil ca-certificates curl file \
    squashfs-tools patchelf fonts-dejavu-core \
    python3-pyside6.qtcore python3-pyside6.qtgui python3-pyside6.qtwidgets \
    python3-pyside6.qtnetwork python3-pyside6.qtqml python3-pyside6.qtquick \
    python3-pyside6.qtquickwidgets python3-pyside6.qtquickcontrols2 python3-pyside6.qttest \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-layouts \
    qml6-module-qtquick-controls qml6-module-qtquick-templates \
    qml6-module-qtquick-dialogs qml6-module-qtquick-shapes qml6-module-qtqml-workerscript \
    qml6-module-qt-labs-platform qml6-module-org-kde-kirigami \
    qml6-module-org-kde-kirigamiaddons-formcard \
    qml6-module-org-kde-kirigamiaddons-components qt6-wayland \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt LICENSE THIRD_PARTY_NOTICES.md /src/
COPY src /src/src
COPY tests /src/tests
COPY scripts /src/scripts
COPY packaging/linux /src/packaging/linux
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel 4 \
    && ctest --test-dir /build --output-on-failure
COPY . /src
ENV QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
RUN python3 gui/tests/test_unified_session.py && python3 gui/tests/test_unified_window.py
RUN python3 packaging/appimage/download_tools.py /tools
RUN python3 packaging/appimage/bundle.py --appdir /AppDir --engine /build/ApexSenseBridge
RUN /AppDir/AppRun --smoke-test
RUN mkdir /out && ARCH=x86_64 /tools/appimagetool/AppRun \
    --runtime-file /tools/runtime /AppDir /out/ApexSenseBridge-Linux-x86_64.AppImage \
    && chmod +x /out/*.AppImage \
    && cd /out && sha256sum *.AppImage > SHA256SUMS
CMD ["sh", "-c", "cp /out/* /export/"]
