# Linux: Apex 4, 5, 6 and 6 Pro

This port adds a foreground command-line program for **Flydigi Apex 4, 5, 6 and 6 Pro**.
See the [model support table](linux-models.md) before using model-specific features.
The Apex 5 path described below retains triggers, rumble, gyro and RGB.
Start it before your game and leave it running. It forwards the controller's
buttons, sticks, independent LT/RT axes and six-axis motion to a virtual USB
DualSense. It translates the game's DualSense trigger commands into Apex 5
FORCEADAPT effects, forwards standard rumble to the grip motors, and synchronizes
the Apex 5 RGB with the game's lightbar color.
No GUI, game detection, Windows drivers, VIIPER server or account is required.

The **Linux v10** [feature guide](linux-features.md) covers manual touchpad
shortcuts, temporary onboard profiles, battery updates, expanded hardware
tests, JSON diagnostics and session telemetry. Run `--version` to check the build.
The v7 [input latency changes](linux-latency.md) are enabled automatically:
every fresh sample is forwarded immediately and diagnostic I/O runs separately.

**Status:** the user has tested this port on an Apex 5 in **Overwatch and
Warframe**. Automated protocol and simulated UHID integration tests pass.
Additional models still need physical Linux validation. Normal bridge runs are
quiet after startup; add `--verbose` for live statistics and trigger diagnostics.
Explicit input/motion test commands still display their requested samples.
Tracing and telemetry files remain opt-in and independent of verbosity.

## Requirements

- Apex 4/5/6 or 6 Pro by USB/2.4 GHz: Apex 4 in DInput, Apex 6 family in PC/XInput.
  Apex 5 supports PC/XInput or DInput. Bluetooth and the Switch profile bank are not supported.
- Linux with `hidraw`, `uhid`, `evdev` and `hid-playstation` support. A current
  desktop distribution is recommended.
- C++20 compiler, CMake 3.20+, and Make or Ninja. There are **no extra library
  dependencies** for this port: it uses Linux device interfaces directly.
- A game that emits native DualSense adaptive-trigger effects. Standard XInput
  rumble alone cannot provide weapon/vehicle-specific trigger effects.

## Build

The supplied Linux package also includes `bin/ApexSenseBridge`, built for this
x86-64 Arch Linux system. You can use it directly after setting up permissions:
replace `./build-linux/ApexSenseBridge` in the examples with `./bin/ApexSenseBridge`.
Build from source on other systems if the binary's C++/glibc runtime is unavailable.

From the repository directory:

```sh
# Arch Linux (if the tools are missing)
sudo pacman -S --needed base-devel cmake ninja

# Debian/Ubuntu alternative
# sudo apt install build-essential cmake ninja-build

cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

## One-time device permissions

Install the supplied rule and load UHID:

```sh
sudo install -m 0644 packaging/linux/70-apexsensebridge.rules /etc/udev/rules.d/70-apexsensebridge.rules
sudo modprobe uhid
sudo modprobe hid-playstation
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --sysname-match=uhid
sudo udevadm settle
```

Unplug and reconnect the Apex 5 cable/receiver after installing the rule. Run
the bridge as your normal desktop user. The rules grant access to the active
local desktop session through `uaccess`; they do not make every input device
world-writable. The UHID rule permits that session to create virtual HID devices.
Headless/SSH sessions need a separately managed group/ACL policy.

If UHID is absent after a reboot, run `sudo modprobe uhid` again. To load it at
boot, create `/etc/modules-load.d/apexsensebridge.conf` containing `uhid`.

## Test the triggers, then run a game

```sh
./build-linux/ApexSenseBridge list
./build-linux/ApexSenseBridge test-triggers
./build-linux/ApexSenseBridge
```

The test applies mild resistance to **both** triggers for five seconds, then
clears the test and reloads your original onboard profile. Ctrl+C ends it early.
It does not require UHID or a game. You can shorten it with `--seconds 2` or
reduce its force with `--strength 50`.

## Gyroscope and RGB

Normal `bridge` mode enables six-axis motion, standard rumble and game-controlled
RGB by default.
Restart an already-running bridge to use the updated executable. The usual game
launch options and controller permission rules still apply.

```sh
./bin/ApexSenseBridge test-gyro
./bin/ApexSenseBridge test-rgb
./bin/ApexSenseBridge test-rumble
./bin/ApexSenseBridge bridge
```

`test-gyro` displays rotation and acceleration samples for five seconds; rotate
and tilt the controller while it runs. `test-rgb` cycles red, green and blue for
two seconds each, then restores the original profile lighting. Neither test
needs a game or virtual controller. `--seconds N` sets the motion-test duration
or the time per RGB color (1–30 seconds).

`test-rumble` drives both grip motors mildly for five seconds, then stops them.
Use `--seconds 2` for a shorter test. Normal game rumble uses the DualSense's
compatible-vibration output commands, with independent left/heavy and
right/light motor levels. `bridge --rumble-strength 50` reduces rumble to 50%;
`0` disables it, and values up to 200 are supported. This is standard motor
rumble; the Linux virtual controller has no USB audio-haptics endpoint.

Motion passes through the native DualSense sensor reports and Linux sensor
device, including signed axes, acceleration calibration and the sensor clock.
The existing Apex 5 decoder supplies raw rotation and converts acceleration
from 4096 to 10000 counts per g to match the advertised DualSense calibration.
Use `bridge --gyro-strength 150` for 150% rotation sensitivity, or `0` to suppress
rotation; acceleration stays unchanged. Values from 0 to 400 are accepted, and
large rotations saturate rather than wrapping. Sensitivity defaults to 100%.
Exposing a gyroscope does not add motion aiming to a game: the game or input
mapper must consume those sensors. This bridge does not map gyro to a mouse.

RGB follows valid DualSense lightbar color commands, including black for LEDs
off. Live colors use a single displayed frame: each update transfers six USB
reports, rather than the previous 22. Updates are coalesced, with a minimum
20 ms between transfer starts; the receiver's actual command/ACK speed can
reduce the frame rate. This substantially reduces stepping in animations but
does not promise a particular hardware refresh rate. The current profile's full lighting configuration is
backed up before accepting game output and restored on normal shutdown or
errors. Use `bridge --no-lightbar` to keep the original lighting throughout.
If the initial backup fails, the bridge waits before changing RGB; reconnect
the controller or run with `--no-lightbar` while investigating.

Input has its own read-only hidraw open and report queue. RGB command exchanges
cannot drain button/stick/motion samples, and trigger/rumble callbacks enqueue
their writes instead of waiting on an RGB transfer. Keeping a WebHID tester
open during bridge startup or using a continuous rainbow should therefore not
cause a false stale-input timeout. A missing raw stream is detected after 1.5 seconds and the bridge then waits
for the controller, keeping the virtual pad connected with neutral input. The input isolation fix is retained in the current `v10` build.

When the bridge prints `Bridge running`, launch the game:

1. In Steam, set the game's **Properties → Controller → Disable Steam Input**.
2. Enable adaptive triggers in the game, if it has a separate setting.
3. Select the virtual DualSense/PlayStation controller if the game offers a choice.
4. Leave the terminal running. Press Ctrl+C after playing.

For SDL-based applications that also open the physical Flydigi through HIDAPI,
try this Steam launch option, in addition to disabling Steam Input:

```text
SDL_JOYSTICK_HIDAPI_FLYDIGI=0 %command%
```

The bridge exclusively grabs gamepad **evdev** nodes belonging to the selected
USB device, preventing their events from reaching other evdev/joydev consumers.
This is not a universal device-hiding driver: the physical controller can remain
listed, and direct HIDAPI readers bypass evdev grabs. Close other remappers and
controller tools. `--no-grab` is available when you manage isolation separately;
it can cause duplicate input otherwise. Only matching gamepad nodes are grabbed,
not keyboard/mouse interfaces or unrelated controllers.

### Steam sees two controllers, or only one button works in a Proton game

The physical Flydigi appearing unresponsive in Steam's input tester while the
bridge runs is expected: its evdev events are held by the bridge. Test the
**virtual DualSense** instead. If its sticks and buttons work in Steam, input
has reached the virtual controller; this does not yet verify Proton's native
HID path.

If the virtual DualSense works in Steam but a game has missing/misassigned
controls, first exclude the physical controller from that game's enumeration
and explicitly enable the virtual DualSense's native HID interface. For an
Apex 5 reporting `37d7:2501`, use these Steam launch options:

```text
SDL_GAMECONTROLLER_IGNORE_DEVICES=0x37d7/0x2501 PROTON_ENABLE_HIDRAW=0x054c/0x0ce6 %command%
```

Replace `0x2501` with the actual product ID from `list` if different. Keep any
existing launch wrappers immediately before `%command%`, for example
`gamemoderun mangohud %command%`. Keep Steam Input disabled, fully exit the game,
and relaunch it while the bridge is running. Steam itself can still list both
controllers: the launch environment applies to the game and Proton.

This is an optional troubleshooting step. The confirmed Overwatch fix was
accepting its host setup feature report `0x08`; the working run needed no launch
option changes. Proton's
[controller documentation](https://github.com/ValveSoftware/Proton/blob/proton_11.0/docs/CONTROLLERS.md)
describes the different native HID and translated input paths. Its Wine bus
[honors the SDL ignore list](https://github.com/ValveSoftware/wine/blob/proton_10.0/dlls/winebus.sys/bus_udev.c)
and supports device-specific `PROTON_ENABLE_HIDRAW` selection. If controls
remain wrong, record the Proton version and capture a Proton HID log before
changing the bridge's button mapping.

If that does not help, use the [Overwatch diagnostic run](overwatch-diagnostics.md).
It checks the native Windows HID interface through the same Proton selected for
the game and collects a bounded bridge HID trace. The updated Linux bridge also
answers input/output snapshot requests; the first version only answered feature
requests and streamed input.

If multiple candidates appear, explicitly choose the vendor interface:

```sh
./build-linux/ApexSenseBridge bridge --device /dev/hidraw7 --strength 75
```

Use the actual path printed by `list`; the number can change after reconnecting.
The program verifies the controller's model before writing any trigger effects,
and refuses a second bridge process holding the same HID interface.

## Feedback and recovery

Every five seconds the bridge reports physical input, virtual output, trigger
translations, reports containing nonzero rotation, RGB update/write/error
counts, and rumble update/write counts. Feedback counts refer to accepted
updates; background USB write errors are checked separately and stop the session.
Kernel LED/setup writes can increment the output count, so
look for **trigger translations** changing during gameplay.
Battery metadata is refreshed every 30 seconds without blocking input.
Use `--telemetry-json status.json` for atomic status/error snapshots, and
`diagnose --json` or `input-status --json` for machine-readable diagnostics.

| Symptom | Next step |
| --- | --- |
| No candidates | Use USB/2.4 GHz, wake the pad, check PC mode; Bluetooth is unsupported. |
| Permission denied | Install the udev rule, reconnect, and use the active desktop session. |
| `/dev/uhid` missing | Load `uhid`; check that the kernel includes `CONFIG_UHID`. |
| Grab fails/busy | Close Steam/controller tools and restart the bridge before the game. Check permissions on the listed event node. |
| Raw input did not start | Reconnect the pad/receiver; close any other program configuring Flydigi routing. |
| Test works, game has no effects | Check native DualSense support, Steam Input and the game's settings. Proton/Wine must pass HID output to the virtual pad. This varies by game. |
| Controller sleeps/unplugs | The bridge waits and resumes automatically; wake/reconnect the same controller. |
| Trigger effect remains after a forced kill | Run `./build-linux/ApexSenseBridge reset`, or power-cycle the controller. |
| No rumble | Run `test-rumble`; in the tester select the virtual DualSense and use its heavy/light rumble controls. Check `rumble_updates` / `rumble_writes` in a v10 trace. |
| Old build stops while RGB updates | Restart with the current build, which separates input from RGB command replies; `--no-lightbar` remains available for diagnosis. |

On Ctrl+C, SIGTERM, SIGHUP, startup failure or normal errors, the program removes
the virtual controller, stops rumble, restores RGB, clears trigger effects,
reapplies the original saved
onboard profile, and restores the original input-routing flags. This takes about
one to a few seconds. Profile effects may therefore return after the session.
It does not write saved profiles to flash. The kernel releases evdev grabs and
destroys UHID devices when the process exits, including after a crash.

SIGKILL/power loss cannot restore physical controller settings. Reconnect or
power-cycle it if input routing or resistance remains changed. If you stop while the controller is absent, the warning states that physical
settings could not be restored. While the bridge remains running, its in-memory
checkpoint restores the original profile, routing and lighting before resuming.

## Installation, Steam and reconnection

See the [README install and launch instructions](../README.md#build-and-install)
for `make`, `sudo make install`, `sudo make install-udev`, and the one-time module
and permission setup. Native Steam launch options after installation:

```text
/usr/local/bin/apexsensebridge-steam -- %command%
```

Keep Steam Input disabled. The launcher waits for bridge readiness and owns its
bridge process until the game command exits. It does not attach to or terminate
a separately running bridge. See the [Steam examples](../README.md#start-automatically-with-a-steam-game)
for settings, logging, timeouts and custom paths.

Default bridge mode scans every 500 ms if no controller is present. On physical
input failure it sends neutral virtual input, answers UHID requests while waiting,
and retries the same verified USB controller. Replugging can change hidraw
numbers; serial/USB location matching follows it. Without a unique serial, use
the same USB port. Switching connection transport or controller model may require
a restart. The same UHID device survives each reconnect, preserving game handles.
Game trigger/lightbar state is cached; transient rumble is not replayed. The
active input pump still forwards fresh samples immediately without a scan timer.
Use `--once` for fail-fast diagnostics; other test/identify commands remain
one-shot. A timed bridge includes waiting time in its limit.

## Scope and limitations

- Apex 4/5/6/Pro feature differences and connection requirements are listed in [model support](linux-models.md).
- Dynamic trigger effects, normal gamepad input, Back/View → touchpad click,
  and manually selected upstream touchpad gesture profiles are included.
  Input comes from the **raw** vendor stream;
  onboard button remaps, macros, custom curves and extra paddles are not reproduced.
- Six-axis gyro/accelerometer passthrough and game lightbar RGB synchronization
  are included. RGB writes use the active profile slot and do not save to flash.
- Standard grip rumble is included, with explicit stop/reset handling.
- No audio haptics, GUI, auto-start or
  automatic game detection. The virtual controller has no USB audio interface;
  games that require a composite USB/audio device may reject it.
- A virtual DualSense does not add adaptive-trigger support to games that do not
  send those effects. Anti-cheat or sandbox restrictions may also prevent use.
- Start with the direct trigger test before testing game compatibility. Passing
  automated tests is not a claim of physical/gameplay validation.

## Implementation and validation

The Linux CMake path builds `src/platform/linux/LinuxMain.cpp` instead of the
Windows command suite. It retains the existing `Apex5Device` identity guard,
input decoder and `AdaptiveTriggerBridge`. HID report sizes come from the
device's actual report descriptor; transport uses hidraw reads/writes and ioctls.
The virtual USB DualSense uses [Linux UHID](https://docs.kernel.org/hid/uhid.html).
The descriptor and firmware report are reused from the existing VIIPER patch.
Feature/input layouts follow the
[Linux PlayStation HID driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c).
Separate input opens follow the kernel's
[per-open hidraw report queues](https://github.com/torvalds/linux/blob/master/drivers/hid/hidraw.c).
Live RGB uses the single-frame geometry and display latch described in
[OpenFlydigi's Apex 5 hardware findings](https://github.com/mkaliaha/openflydigi/blob/main/docs/device-settings.md#rgb-the-led-config-blob).

The hardware-free Linux test checks descriptor parsing, malformed reports,
DualSense feature replies, both control/interrupt output paths, simultaneous
LT/RT input, trigger mechanism state, effect clearing, deduplication, and
physical-write/disconnection errors. It also checks signed six-axis samples,
gyro gain/clipping, RGB output gating, simultaneous color/trigger feedback,
RGB deduplication/restoration, and serialization of command-channel reads.
An independent input stream stays responsive beyond the 1.5-second deadline
while RGB is blocked, with trigger and rumble feedback arriving simultaneously.
A rainbow-rate test offers colors every 33 ms with simulated USB reply latency,
and rumble tests verify nonzero motor levels and explicit stops. Socketpairs
substitute for physical HID and `/dev/uhid`; the real trigger translator,
lighting worker and Apex packet encoders are exercised.
Additional tests cover Linux option/JSON contracts, synthetic touch reports,
battery updates, temporary profile restoration after a lost ACK and atomic
telemetry. Python 3 runs the CLI/launcher tests and the Steam launcher; the native bridge
itself still requires no extra runtime libraries.

For a hardware acceptance run, verify: direct resistance on LT and RT; all
buttons/sticks and simultaneous LT+RT in a controller tester; game-trigger
translations; Ctrl+C restoring ordinary input; unplug/sleep releasing all virtual controls;
and input/feedback resuming in the same game after reconnecting. Record the controller firmware, connection mode,
kernel and game/Proton version when reporting a failure.

See `THIRD_PARTY_NOTICES.md` for the Linux executable's GPL-3.0 license and the
licenses retained by the existing source files.
