# Linux feature guide

See [model support](linux-models.md): Apex 4/5 use FORCEADAPT, Pro uses voice-coil
trigger feedback, and standard Apex 6 uses kernel input/rumble. RGB, profiles
and battery refresh in the table below apply to Apex 5.

This checkout keeps the foreground Linux workflow: start the bridge, use the
virtual DualSense, and stop it with Ctrl+C. Upstream game and onboard profiles
are selected explicitly. Normal bridge mode still starts with touchpad profile
`none`, your current onboard slot, 100% strengths, and RGB/rumble enabled.

Linux v9 forwards each fresh button/stick/motion sample immediately, preserves
adjacent press/release edges, and moves diagnostic I/O to a bounded background
queue. These optimizations are always enabled. See [input latency](linux-latency.md)
for details, the software-only measurement and its limits.

## Features carried over from upstream

| Original feature | Linux use |
| --- | --- |
| Adaptive trigger translation | Enabled in `bridge`; `--strength 0..100` or `--trigger-strength` |
| Grip rumble amplification | Enabled; `--rumble-strength 0..200` or `--vibration-strength` |
| RGB/lightbar synchronization | Enabled; `--no-lightbar` keeps your lighting |
| Native gyro and accelerometer | Enabled; `--gyro-strength 0..400` and additional `--gyro-yaw 0..400` |
| Touchpad gesture profiles | Manual `--touchpad-profile NAME`, listed below |
| Onboard Apex profiles 1–4 | `bridge --apex-profile N`; original slot restored on exit |
| Temporary profile test | `test-profile-switch --target N --seconds 3` |
| Battery reporting | Initial identity plus a background metadata request every 30 seconds |
| Model/connection identification | `identify`, with optional `--json` |
| Interface/access diagnostics | `diagnose`, with optional `--json` |
| Full input diagnostics | `input-status --seconds 10`, with optional `--json` |
| Trigger tests | `test-triggers --side lt\|rt\|both --mode resistance\|weapon\|vibration\|bow\|normal --level 1..4` |
| Independent grip-motor tests | `test-rumble --left 0..255 --right 0..255` |
| Custom RGB test | `test-rgb --color 33AAFF --seconds 2` |
| Session telemetry | `bridge --telemetry-json status.json` |
| Bounded bridge runs | `bridge --seconds 60`; normal cleanup runs at the end |

`bridge-triggers`, `test-trigger`, `test-input` and `clear` are aliases for
`bridge`, `test-triggers`, `input-status` and `reset`. `--version` identifies
this build as **Linux v9**. Hardware tests last 1–30 seconds; a timed bridge
may run for 1–86400 seconds. Trigger tests default to mild level 1.

## Touchpad shortcuts

These reuse the original repository's mapper. Only the selected profile
consumes its documented shortcuts; the default `none` keeps ordinary controls.

| Profile name | Physical shortcut | Virtual gesture |
| --- | --- | --- |
| `none` | View/Back | Ordinary touchpad click |
| `legacy-view-hold-swipe-up` | Tap / hold View | Click / upward swipe |
| `spider-man-2` | Hold View / hold D-pad Up | Left swipe / alternating up-down camera swipes |
| `miles-morales` | Hold View | Left swipe |
| `ghost-of-tsushima` | Hold D-pad Right and flick right stick | Directional wind swipe |
| `warframe` | Hold RB and press A/X/B/Y | Up/down/left/right ability swipe |
| `death-stranding-2` | View/Back / LB + Menu | Right-side click or hold / left-side photo click |

Example, from this checkout:

```sh
./bin/ApexSenseBridge bridge --touchpad-profile death-stranding-2
./bin/ApexSenseBridge bridge --apex-profile 2 --touchpad-profile spider-man-2
```

`--view-hold-swipe-up` selects the legacy profile. Shortcuts advance on each
virtual report using fresh physical state, so holding a button does not feed
the previously synthesized swipe back into the mapper. No game is detected.

Selecting an onboard profile temporarily applies that slot before raw input
starts and backs up that slot's RGB when synchronization is enabled. The
restore guard is armed before the switch, including when the command takes
effect but its acknowledgement is lost. No profile is saved to flash. Input
still uses the raw vendor stream: custom onboard button remaps, curves and
extra paddles are not reproduced by this Linux port.

## Battery, diagnostics and telemetry

Battery percentages are the firmware's coarse estimate. Metadata replies
update the virtual controller's battery/charge state independently of sticks
and buttons. Requests run outside the input loop, and only replies matching
the verified model and connection are accepted. Request failures appear in
the trace/telemetry counters; they do not invent a new battery value.

`list`, `diagnose` and `identify` do not change controller settings. `identify`
sends the identity query. `diagnose` checks hidraw candidates, read/write
access to their nodes, UHID access, and PlayStation HID driver registration.
It can report an empty device list when no hidraw subsystem is present.
`input-status` and the motion test temporarily enable the raw stream and
restore settings afterward. JSON input samples are one object per line.

```sh
./bin/ApexSenseBridge diagnose --json
./bin/ApexSenseBridge identify --json
./bin/ApexSenseBridge input-status --seconds 10 --json
./bin/ApexSenseBridge bridge --telemetry-json status.json --trace-hid bridge-hid.log
```

Telemetry is a single JSON snapshot, replaced atomically at startup, when the
bridge becomes ready, every five seconds, and on final stop/error. Ready/periodic
updates run in the background and are best effort under diagnostic overload;
startup and final/error snapshots are written synchronously. It includes
phase, error, cleanup result, battery, profile selection, input/feedback counts,
USB writes/coalescing/retries, touchpad activity and battery-request failures.
`input_forwarded` counts fresh samples sent immediately to UHID;
`input_heartbeats` counts repeats during quiet periods. `diagnostic_drops` counts
records skipped when the logging/status queue is busy or full; input and game
feedback are not dropped by this queue.
The directory must exist. Its write access is checked before controller
settings are touched. The trace and telemetry cannot share a file, including
existing hard-link/symlink aliases. `cleanup_ok` is true only after restoration
has been confirmed or no settings required restoration.

Ctrl+C, SIGTERM, SIGHUP, closed output pipes, normal errors and timed completion
use the cleanup path. Actual disconnects or SIGKILL/power loss can prevent
hardware restoration; wake/reconnect and restart the bridge or use `reset`.

## Deliberate limits

There is no GUI, game detection, Playnite integration, Windows driver installer
or automatic game selection. Model-specific limitations are listed in [model support](linux-models.md).
UHID supplies no USB audio endpoint, so native DualSense audio haptics are
not implemented; standard grip rumble is supported. A motion sensor does not
add gyro aiming to games that do not consume it, and touchpad profile names
do not certify each game's Proton compatibility.

The Linux tests cover the original mapper, its Linux USB report path, profile
restoration after a lost ACK, battery updates, gain clipping, CLI contracts,
JSON snapshots, rapid input edges, feedback fairness, a blocked diagnostic
worker and the existing busy RGB/trigger/rumble input tests. Added Apex 4/6 model paths still require physical/game acceptance testing.
