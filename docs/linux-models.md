# Linux v9 controller support

The Linux foreground bridge now detects Apex 4, Apex 5, Apex 6 and Apex 6 Pro.
Start `bin/ApexSenseBridge bridge`; the model selects the input/feedback path.
If several controllers are connected, use `list` and `--device /dev/hidrawN`.
Keep Steam Input disabled for native DualSense feedback.

| Feature | Apex 4 | Apex 5 | Apex 6 standard | Apex 6 Pro / PBZ |
| --- | --- | --- | --- | --- |
| Buttons, sticks and independent LT/RT | Yes, V1 vendor stream | Yes, V2 vendor stream | Yes, kernel gamepad input | Yes, kernel gamepad input |
| DualSense trigger effects | FORCEADAPT resistance | FORCEADAPT resistance | No supported trigger hardware | Translated voice-coil trigger haptics |
| Standard grip rumble | V1 motor commands | V2 motor commands | Kernel `FF_RUMBLE`, when exposed | Voice-coil renderer |
| Gyro and accelerometer | Upstream V1 decoder/calibration | V2 decoder | Not decoded | Not decoded |
| Game RGB / temporary onboard profiles | Not implemented | Yes | Not implemented | Not implemented |
| Battery percentage | Unavailable | Yes, background refresh | Unavailable | Unavailable |
| Manual touchpad profiles | Yes | Yes | Yes | Yes |
| Native DualSense audio haptics | Not implemented | Not implemented | Not implemented | Not implemented |

These are implemented capabilities, not a claim that the additional models
have passed physical Linux acceptance testing. Apex 5 is the model previously
confirmed working by the user. The other models are covered by simulated
integration tests and still need USB/receiver/game validation.

The [original project](https://github.com/ReynArts/ApexSenseBridge#supported-controllers)
supplies the V1 FORCEADAPT and V3 Pro haptic renderers. The Pro's voice-coil
trigger sensations use a different renderer from Apex 4/5 resistance effects.
The Linux UHID device still has no USB audio endpoint, so this port does not
receive the game's native PCM audio-haptic stream, including on Pro hardware.

## Connections and permissions

- Apex 4: USB or 2.4 GHz receiver in **DInput** mode. The controller must expose
  vendor interface `04b4:2412`, normally USB interface 2. The V1 decoder forwards
  available motion samples using upstream's capture-derived experimental gains.
  If the firmware outputs zero motion, the bridge cannot synthesize real motion.
- Apex 5: USB or 2.4 GHz receiver, as in the previous release. RGB, profile
  restoration, background battery metadata and the Overwatch setup fix remain.
- Apex 6 / Pro: USB or 2.4 GHz receiver in **PC/XInput** mode, vendor interface
  `37d7:2502`. A matching Linux gamepad node must expose two sticks and separate
  trigger axes. A combined-axis-only collection is rejected instead of losing
  simultaneous LT/RT. No unrelated controller or virtual pad is used as input.
- Bluetooth and the Switch profile bank are outside this Linux port.

Existing Apex 5 permission rules already cover the Apex 6 USB family. **Apex 4
users must install the updated rule** from the repository directory:

```sh
sudo install -m 0644 packaging/linux/70-apexsensebridge.rules /etc/udev/rules.d/70-apexsensebridge.rules
sudo udevadm control --reload-rules
```

Reconnect the cable/receiver afterward. Continue running the bridge as your
normal desktop user. The existing UHID/hid-playstation setup still applies;
see [Linux setup](linux.md). No driver installation or system setting is changed
automatically by this update.

## Model safeguards and cleanup

Model identity is verified before effects are enabled. The Linux path accepts
the standard Apex 6 identity `0x95` for input and kernel rumble only. Pro IDs
`0x96` and `0x98` additionally need both advertised haptic capability bits.
Unknown identities and corrupt V3 checksums are rejected. A standard model
cannot receive Pro `0x53`/`0x57` commands even if its feature byte resembles a Pro.
Existing Windows identity checks retain their stricter default behavior.

V1 short commands are padded to the declared 32/64-byte report size. V3
unnumbered output uses the zero report-ID prefix required by hidraw; input
replies may omit it. The two formats are handled separately.

On Apex 6 the input reader owns its exclusive event-device grab. Other gamepad
collections on the same USB device are also isolated; keyboard/mouse interfaces
are not grabbed. Both xpad's historical button codes and hid-generic's contiguous
button usages are mapped, including their differing trigger-axis order. This
follows the kernel's [xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)
and [generic HID](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-input.c)
implementations.

Fresh input retains the v7 immediate-forwarding path. Evdev frames preserve each
`SYN_REPORT`, including press/release edges held in the reader's local buffer.
Idle Apex 6 input is event-driven and does not trip the raw-stream 1.5-second
watchdog. Removal is detected through the kernel event node; wireless sleep
behavior depends on what the receiver/driver reports. `SYN_DROPPED` triggers a
kernel state refresh instead of retaining potentially stuck button state.

Cleanup stops the model's feedback worker before releasing the device. Pro
cleanup sends neutral frames and disables haptic routing using the upstream
sequence. Standard Apex 6 rumble is uploaded/played outside the input loop,
renewed only while active, explicitly stopped on exit and removed with its
kernel client. Each kernel effect expires after one second without renewal.
No Apex 5 profile, RGB or input-routing command is sent to Apex 4 or Apex 6.

## Commands and status

```sh
./bin/ApexSenseBridge --version
./bin/ApexSenseBridge list
./bin/ApexSenseBridge identify --json
./bin/ApexSenseBridge bridge
./bin/ApexSenseBridge input-status --seconds 10 --json
./bin/ApexSenseBridge test-triggers --mode weapon --side rt --seconds 2
./bin/ApexSenseBridge test-rumble --left 80 --right 0 --seconds 2
```

Trigger tests work on Apex 4/5 and Pro; the standard Apex 6 returns an explanatory
error. Standard rumble tests require the kernel's `FF_RUMBLE` capability. When
that capability is absent, normal bridging still forwards controls and announces
that feedback is unavailable. `test-gyro` is available on Apex 4/5. `test-rgb`,
`--sync-lightbar`, `--apex-profile` and `test-profile-switch` are Apex 5 only;
explicit requests on other models fail before motor/profile changes. Default
RGB synchronization automatically skips models without a supported protocol.

Telemetry includes `model`, `feedback_backend`, `motion_supported`,
`rgb_supported`, `battery_available` and `haptic_frames`. Profile value zero
means the bridge does not manage that model's onboard profiles. Battery fields
are meaningful only when `battery_available` is true; other virtual reports
retain the existing synthetic full-battery fallback.

All 33 tests include the original portable protocols plus Linux model routing,
V1 motion, raw V3 identity replies, Pro/PBZ capability checks, standard-model
write rejection, kernel rumble scaling/stop, buffered input edges, trigger-axis
mapping and idle/disconnect behavior. Physical testing should confirm controls,
simultaneous LT/RT, trigger effects, grip rumble and cleanup on each connection.
