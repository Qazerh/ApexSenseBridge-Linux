PREFIX ?= /usr/local
DESTDIR ?=
BUILD_DIR ?= build-linux
BUILD_TYPE ?= Release
CMAKE ?= cmake
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

.PHONY: all build test install install-udev clean
all: build

build:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)"
	$(CMAKE) --build "$(BUILD_DIR)" --parallel "$(JOBS)"

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

# Build first as your normal user; installing never compiles as root.
install:
	@test -x "$(BUILD_DIR)/ApexSenseBridge" || { echo 'Run make first (with the same BUILD_DIR).' >&2; exit 1; }
	DESTDIR="$(DESTDIR)" $(CMAKE) --install "$(BUILD_DIR)" --prefix "$(PREFIX)"

# Optional system integration, separate from the binary installation.
install-udev:
	install -D -m 0644 packaging/linux/70-apexsensebridge.rules "$(DESTDIR)/etc/udev/rules.d/70-apexsensebridge.rules"
	install -D -m 0644 packaging/linux/apexsensebridge.conf "$(DESTDIR)/etc/modules-load.d/apexsensebridge.conf"

clean:
	$(CMAKE) --build "$(BUILD_DIR)" --target clean
