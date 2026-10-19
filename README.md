# ApexSenseBridge for Linux

[![Linux build](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/actions/workflows/linux.yml/badge.svg)](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/actions/workflows/linux.yml)

A foreground Linux bridge that makes a Flydigi Apex controller appear as a USB
DualSense and translates game feedback back to the physical controller. Start
it before playing and leave it running. There is no GUI or game detection.

Based on **[ReynArts/ApexSenseBridge](https://github.com/ReynArts/ApexSenseBridge)**,
starting from upstream commit [`1adf78d`](https://github.com/ReynArts/ApexSenseBridge/commit/1adf78dacbfcc6eb6f2f41ae7cc8060962e148b6).
The original project supplies the controller protocols, trigger translators and
Apex 6 Pro haptic renderer. This repository adds the Linux HID/UHID backend,
input isolation, command-line tools and Linux integration tests. Upstream history
and source notices are retained. The exact baseline commit is linked in the
[protocol documentation](docs/wiki/Controller-Protocol.md).

**Tested by the user on an Apex 5 in Overwatch and Warframe.** Apex 4 and the
Apex 6 family have automated protocol/integration coverage, but have not yet
been physically validated with this Linux port. Firmware and connection mode
can affect compatibility; the table describes implemented support.

## Supported devices and features

| Feature | Apex 4 | Apex 5 | Apex 6 | Apex 6 Pro / PBZ |
| --- | --- | --- | --- | --- |
| USB / 2.4 GHz receiver | DInput mode | PC/XInput or DInput | PC/XInput mode | PC/XInput mode |
| Buttons, sticks, independent LT/RT | Yes | Yes | Yes | Yes |
| Game adaptive-trigger effects | FORCEADAPT resistance | FORCEADAPT resistance | No supported trigger hardware | Translated voice-coil trigger haptics |
| Standard grip rumble | Yes | Yes | If kernel driver exposes `FF_RUMBLE` | Yes, voice-coil renderer |
| Gyroscope and accelerometer | Experimental upstream calibration | Yes | Not decoded | Not decoded |
| Game-controlled RGB | Not implemented | Yes | Not implemented | Not implemented |
| Temporary onboard profile selection | Not implemented | Slots 1–4, restored on exit | Not implemented | Not implemented |
| Battery reporting | Unavailable | Yes | Unavailable | Unavailable |
| Manual touchpad gesture profiles | Yes | Yes | Yes | Yes |

Other implemented features include adjustable trigger/rumble/gyro strength,
manual controller selection, direct hardware tests, JSON diagnostics, optional
HID tracing and status snapshots. Physical gamepad events are isolated while
the virtual controller is active. Fresh inputs are forwarded immediately;
feedback writes and diagnostic I/O run separately to keep them out of the input
path. See [model details](docs/linux-models.md), [features](docs/linux-features.md)
and [latency notes](docs/linux-latency.md).

RGB follows colors sent by the game to the virtual DualSense lightbar. It does
not add effects to games that do not send lightbar commands. The bridge backs
up the Apex 5 lighting configuration and restores it on exit.

## Build and install

Requirements: Linux with `hidraw`, `uhid`, `evdev` and `hid-playstation`, a C++20
compiler, CMake 3.20+, Make or Ninja, and Python 3 for the test suite and Steam
launcher. The native bridge needs only the C/C++ system runtime.

```sh
git clone https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux.git
cd ApexSenseBridge-Linux
make
make test
sudo make install
sudo make install-udev
```

`make install` installs `ApexSenseBridge` and `apexsensebridge-steam` in
`/usr/local/bin`, plus licenses and setup files in `/usr/local/share/ApexSenseBridge`.
Build as your normal user first; the install target never compiles as root.
`install-udev` installs the device permission rule and a module-load file for
future boots. Complete the one-time setup below before running.

For a user-only binary installation, use `make install PREFIX="$HOME/.local"`;
the system device-permission setup still needs administrator access. CMake users
can keep using `cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release`,
`cmake --build build-linux --parallel` and `cmake --install build-linux --prefix PATH`.
`BUILD_DIR`, `BUILD_TYPE`, `JOBS`, `PREFIX` and staging `DESTDIR` are configurable
in the Makefile. Use `-DBUILD_TESTING=OFF` with CMake to build only the native
bridge without the Python test dependency.

The [build action](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/actions/workflows/linux.yml)
compiles and tests Debug and Release builds on Ubuntu 24.04. Its downloadable
artifact contains the Release binary, setup files, documentation, licenses and
the corresponding source. Build locally if your distribution's runtime is
older than the artifact's runtime. CI tests use simulated devices; they do not
replace hardware testing.

## Create a release manually

Open **[Actions → Linux release](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/actions/workflows/release.yml)
→ Run workflow**. Select `main`, enter a new tag such as `linux-v10`, optionally
mark it as a prerelease, and run it. The workflow builds and tests the selected
revision, then creates the tag and publishes a GitHub release with the Linux
package and SHA-256 checksum. Existing tags/releases are never overwritten.
It uses GitHub's built-in token; no additional secret is needed. Releases only
run manually, not on pushes or tags. Keep version identifiers in `--version`
aligned with your chosen release series when preparing later versions.

## One-time device permissions

From the repository directory:

```sh
sudo make install-udev
sudo modprobe uhid
sudo modprobe hid-playstation
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --sysname-match=uhid
sudo udevadm settle
```

Reconnect the controller/receiver, then run the bridge as your normal desktop
user. The rules grant the active local desktop session access, rather than
making every input device world-writable. Headless sessions need their own
ACL/group configuration. `install-udev` also loads the required modules at
future boots through `/etc/modules-load.d/apexsensebridge.conf`. It does not reload rules or reconnect
devices automatically.

## Play

```sh
ApexSenseBridge list
ApexSenseBridge test-triggers
ApexSenseBridge
```

Start the bridge before the game. In Steam, choose the game's **Properties →
Controller → Disable Steam Input** so native DualSense feedback can reach the
bridge. Enable adaptive triggers in the game if it has a setting. Keep the
bridge running; **Ctrl+C stops it and restores controller settings**. No special
HID override launch options were needed in the confirmed Overwatch setup.

The bridge waits quietly when no controller is connected, rescanning every
half-second. If the controller sleeps or disconnects, it releases virtual
buttons/triggers, keeps the same virtual DualSense alive, and resumes after
reconnection. Game trigger effects and the last lightbar color are retained;
old rumble is not replayed. Original Apex 5 settings are restored before starting
a fresh session, so an interrupted RGB/profile change does not become the new
baseline. No per-input scan delay is added.

After the first connection, selection follows the same USB device by serial
or USB location, even if its `/dev/hidrawN` number changes. For devices without a
unique serial, reconnect to the same USB port. Switching from cable to receiver,
pairing a different model, or choosing another controller may require a restart.
`--once` retains the old one-shot behavior for troubleshooting. `--seconds N`
limits the entire bridge lifetime, including time spent waiting.

## Start automatically with a Steam game

After installation and device setup, open the game's **Properties → General →
Launch Options** and enter:

```text
/usr/local/bin/apexsensebridge-steam -- %command%
```

Also set **Properties → Controller → Disable Steam Input**, and enable native
adaptive triggers in the game when available. The launcher starts Steam's
game/Proton command immediately and runs the bridge in the background. It does not wait for a controller or bridge readiness. The
bridge keeps scanning and connects when you turn the controller on. Even a
bridge startup failure does not prevent the game from starting. When the game
command exits, the launcher stops its bridge with normal settings restoration.
Steam's Stop button stops both. Close any manually running bridge before using
this launcher. Games must support controller hotplug to pick up a controller
that appears after they start.

Bridge settings go before `--`. Existing game wrappers go after it:

```text
/usr/local/bin/apexsensebridge-steam --touchpad-profile warframe -- %command%
/usr/local/bin/apexsensebridge-steam --strength 75 -- gamemoderun %command%
```

Use an absolute path for a custom installation, for example
`/home/you/.local/bin/apexsensebridge-steam`. Quote paths containing spaces. Logging is disabled by default; bridge output is
discarded. Add `--log /absolute/path.log` before `--` to save bridge output to a
file, overwritten each run. The launcher keeps Steam's environment for the game and removes runtime library overrides only from the
native bridge. Games whose launch command exits immediately after spawning a
separate launcher may need the bridge run manually instead. Flatpak Steam needs
access to the installed binaries, Python and controller nodes; these host launch
instructions assume a native Steam installation. Valve documents the
[Launch Options field](https://help.steampowered.com/en/faqs/view/7D01-D2DD-D75E-2955).

Normal bridge runs print startup, shutdown, warnings and errors. They do not
continually print statistics or trigger effects. Add `--verbose` to see those:

```sh
ApexSenseBridge --verbose
ApexSenseBridge --strength 75 --rumble-strength 50
ApexSenseBridge --no-lightbar
ApexSenseBridge --touchpad-profile warframe
ApexSenseBridge --telemetry-json status.json
ApexSenseBridge --trace-hid bridge-hid.log
ApexSenseBridge --help
```

Tracing and JSON status files are explicit opt-ins and work without `--verbose`.
Explicit diagnostic commands such as `input-status` and `test-gyro` display their
requested samples; `--json` provides machine-readable diagnostic output.

```sh
ApexSenseBridge diagnose --json
ApexSenseBridge identify --json
ApexSenseBridge input-status --seconds 5
ApexSenseBridge test-rumble --seconds 2
ApexSenseBridge test-gyro
ApexSenseBridge test-rgb --color 00AAFF
```

Consult [Linux setup and troubleshooting](docs/linux.md) if permissions, Proton
or duplicate device enumeration cause trouble. While the bridge is running,
test the **virtual DualSense** in Steam's input tester. The physical Flydigi may
still be listed but appear unresponsive because its gamepad events are grabbed.
Direct HIDAPI consumers can bypass that isolation. Use `--device /dev/hidrawN`
when several supported controllers are connected.

## Scope and differences from the original project

This Linux port does **not** implement the original Windows tray/GUI, game
process detection, automatic game profiles, Playnite integration, Windows
installer/driver management.
The checkout contains only the Linux runtime, tests, diagnostics, setup files
and documentation. Upstream history and attribution remain available in Git.

Native DualSense **PCM/audio haptics are not implemented**: the virtual UHID
controller has no USB audio endpoint. Standard motor rumble and supported
adaptive-trigger translations do work; they are separate from audio haptics.
Bluetooth and Switch mode are unsupported. Apex 6 motion and non-Apex-5 RGB and
profile control are not implemented. There is no gyro-to-mouse mapper, arbitrary
button remapper or extra physical touchpad. Apex 4/5 use raw controls, so custom
onboard remaps, curves and extra paddles are not fully reproduced by this port.

Games must actually emit native DualSense effects. A game that only sends
XInput rumble cannot provide weapon-specific adaptive triggers through this
bridge. This project does not inject into game processes.

Cleanup handles normal exit, supported stop signals and errors. A power loss or
`SIGKILL` cannot run cleanup; reconnect the controller if settings or effects
remain active. Run `reset` to stop supported trigger/rumble effects. If you exit while the
controller is absent, its settings cannot be restored; reconnect or power-cycle
it. Reconnect checkpoints last only while the bridge process stays alive.

## Controller protocol and contributing

The [controller protocol wiki](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/wiki/Controller-Protocol)
documents V1/V2/V3 framing, model identification, input layouts, triggers,
rumble, RGB and routing. Its [versioned source](docs/wiki/Controller-Protocol.md)
is kept with the code. These are reverse-engineered implementation notes,
not an official Flydigi specification.

Useful reports include the model, firmware, connection/mode, bridge version,
`diagnose --json` and the exact failing feature. Capture traces only when needed
and inspect them before sharing; they may contain controller activity or device
identifiers. Please distinguish physical results from simulated test results.

## License and credits

Distributed under [GPL-3.0-or-later](LICENSE); the full GPL text is included in
[packaging/linux/VIIPER-LICENSE.txt](packaging/linux/VIIPER-LICENSE.txt). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for upstream, VIIPER and other
component notices. Existing per-file licenses are retained. This is an
independent community project, unaffiliated with Flydigi, Sony or the games.
