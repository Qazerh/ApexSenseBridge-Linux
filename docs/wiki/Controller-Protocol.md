# Flydigi Apex controller protocol

These notes describe the **reverse-engineered subset implemented by this Linux
bridge**, not an official or complete Flydigi specification. Packet offsets are
zero-based, hex values use `0x`, and multi-byte numeric fields are little-endian
unless stated otherwise. Reserved/unknown fields should not be invented or
written. Hardware validation is currently limited to the user's Apex 5 tests
in Overwatch, Warframe and a WebHID tester; other model paths have simulated
coverage.

The implementation derives from [ReynArts/ApexSenseBridge](https://github.com/ReynArts/ApexSenseBridge)
at [commit 1adf78d](https://github.com/ReynArts/ApexSenseBridge/commit/1adf78dacbfcc6eb6f2f41ae7cc8060962e148b6).
The source links below point into
[ApexSenseBridge-Linux](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux).
They are the authority for the current implementation when firmware differs.

## 1. Transport and model selection

The bridge opens the vendor HID collection through Linux `hidraw`. Physical
input and command/reply traffic use **separate opens and report queues**, so
RGB/identity exchanges cannot steal button samples. USB and the 2.4 GHz receiver
are supported; Bluetooth and Switch mode are outside this implementation.

| Family | USB identification | Mode | Vendor protocol |
| --- | --- | --- | --- |
| Apex 4 | `04b4:2412`, usage page `ffa0`, normally interface 2 | DInput | V1, numbered output `05` |
| Apex 5 | VID `37d7`, PID family `2xxx` (tested receiver `2501`), usage page `ffa0` | PC/XInput or DInput | V2, output `03`, input `04` |
| Apex 6 / Pro | `37d7:2502`, usage page `ffa0` | PC/XInput | V3, unnumbered 32-byte frame |

USB IDs are candidates, **not sufficient identity verification**. Before motor
commands the bridge sends GetInfo, verifies the returned model, and selects the
protocol. V2-family enumeration must not make an Apex 6 receive Apex 5 packets.

Accepted model IDs:

- Apex 4: decimal `84, 86, 87, 92, 93, 102, 103, 104`.
- Apex 5: decimal `128, 129, 133, 134, 135, 136`.
- Apex 6 standard: `0x95`, controls and kernel rumble only.
- Apex 6 Pro / Phantom Blade Zero: `0x96` / `0x98`; real-time haptics also require
  feature bits `0x80` (grip) and `0x10` (trigger).

**Never send Pro motor-routing or real-time commands to standard Apex 6**, even
if its feature byte resembles a Pro. Unknown IDs and bad V3 checksums fail
verification. See [identity code](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/flydigi/Apex5Identity.cpp).

## 2. Apex 5 V2 framing

Reports are 32 bytes, zero-padded. Offsets include the HID report ID.

| Offset | Output field |
| --- | --- |
| 0 | Report ID `03` |
| 1–2 | Magic `5a a5` |
| 3 | Command |
| 4 | Length; interpretation depends on command family below |
| 5 onward | Payload |

Replies start `04 5a a5 <command>`. **V2 does not have one universal checksum
rule**:

- GetInfo, routing, profiles and RGB: byte 4 is `payload_size + 2`; checksum goes
  at offset `3 + length`, and is the sum of bytes `[3, 3 + length)` modulo 256.
- FORCEADAPT `51`: byte 4 describes the effect payload; the raw builder writes
  `0a` and five parameter bytes. It does not append the checksum above.
- Rumble `12`: byte 4 is `06`, motor bytes are at 5 and 6, remaining bytes zero.

Examples below show the nonzero prefix; pad each output to **32 bytes**:

```text
GetInfo:                 03 5a a5 01 02 03
Read input routing:      03 5a a5 10 02 12
Controller + raw input:  03 5a a5 11 07 01 01 ff ff ff 17
Read active profile:     03 5a a5 a1 02 a3
Apply profile slot 0:    03 5a a5 a2 03 00 a5
Grip rumble 80/80:       03 5a a5 12 06 50 50
Stop grip rumble:        03 5a a5 12 06 00 00
Normal left trigger:     03 5a a5 51 03 01 01 00
Normal right trigger:    03 5a a5 51 03 01 02 00
```

See [packet builders](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/flydigi/Apex5Protocol.cpp)
and [protocol tests](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/tests/test_protocol.cpp).

### Identity, routing and profiles

GetInfo reply fields: model at 6, connection at 7 (`1` = wired), battery at 12.
The low battery nibble is a coarse level 0–6; a nonzero high nibble indicates
charging. The bridge clamps levels above 6, handles charging-with-zero-level as
level 5, and converts the coarse value for the virtual controller. This is not
a precise battery gauge. Identity requests are refreshed in the background
every 30 seconds during an Apex 5 session.

Routing commands are `10` (read) and `11` (set). The set payload is:
`controllerData, rawData, keyboard, mouse, thirdPartyOwnership`. For the last
three bytes the bridge uses `ff`, meaning **keep current value**. It enables
controller/raw data, checks the reply and readback, and restores the original
routing on exit. The routing read reply has five flags at offsets 6–10.

Profile commands are `a1` (status) and `a2` (apply slot). Status byte 6 is a bank
and slot: 0–3 = PC slots; 4–7 = Switch bank. The CLI presents PC slots 1–4 and
rejects the Switch bank. These operations apply an existing runtime profile;
the bridge does not save edits to flash. A profile application can change
routing/effects after its ACK, so cleanup allows it to settle before restoring
routing. See [device exchanges and restore handling](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/flydigi/Apex5Device.cpp).

### Raw input: `04 5a a5 ef`

| Offset | Field |
| --- | --- |
| 4–5 / 6–7 | Left X / Y, signed 16-bit |
| 8–9 / 10–11 | Right X / Y, signed 16-bit |
| 12 | Primary button bits |
| 13 | Secondary button bits |
| 15 bit `08` | Guide / PS |
| 16 / 17 | Independent LT / RT, 0–255 |
| 18–19 / 20–21 / 22–23 | Gyro X / Y / Z, signed 16-bit |
| 24–25 / 26–27 / 28–29 | Accelerometer X / Y / Z, signed 16-bit |

Primary bits `01,02,04,08` = D-pad up/right/down/left; `10,20,40,80` = A/B/Back/X.
Secondary bits `01,02,04,08,40,80` = Y/Menu/LB/RB/left stick click/right stick click.
Other bits are not assigned by this decoder.

Stick values are normalized to DualSense byte axes. Acceleration is scaled by
`10000 / 4096` (saturating) to match the virtual sensor calibration. Gyro samples
are forwarded with optional strength/yaw gain; the virtual calibration is
synthetic, not controller-specific factory calibration. Raw input does not
fully reproduce onboard remaps, curves or extra paddles. See
[input decoder](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/flydigi/Apex5Input.cpp).

### FORCEADAPT trigger command `51`

The raw command used by the translator is:

```text
03 5a a5 51 0a APPLY SIDE MODE P0 P1 P2 P3 P4 [zero padding]
```

`APPLY=01` on Apex 5. `SIDE=01` left, `02` right. The effect block is five bytes;
meanings depend on the mode. The enum follows the upstream wire values, which
can differ from Space Station UI names:

| Mode | Bridge name | Parameters from the structured builder |
| --- | --- | --- |
| `00` | Normal / release | No effect parameters |
| `01` | Race / resistance | start, force, match-input |
| `02` | RecoilRattle | start, p1, p2, p3, match-input |
| `03` | SniperBreak | start, p1, p2, zero, match-input |
| `04` | Lock | start, `ff`, match-input |
| `05` | Vibration | start, p1, p2, p3, match-input; under-documented |

The structured builder clamps relevant force/rate parameters to at least 1.
The raw translator preserves its five-byte block. Both clear match-input for
Race when start is zero and match-input is 1 (firmware quirk). Travel and force
are not assumed to be in physical units. Use the supplied mild diagnostic
levels rather than sending unbounded motor experiments. The translator supports
common DualSense resistance, weapon, vibration and bow effects, with
approximations where controller hardware differs. See
[translation code](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/dualsense/AdaptiveTriggerTranslation.cpp).

### RGB commands `a7`, `a8`, `a9`

The Apex 5 lighting configuration is **380 bytes = 19 packets × 20 bytes** and
contains a header plus animation frames for 12 LEDs. The bridge backs up the
entire configuration before modifying it.

| Command | Payload |
| --- | --- |
| `a7` read | slot, packet size (`14` hex = 20) |
| `a8` start write | slot, first destination packet, count, packet size |
| `a9` write packet | transfer-relative packet index, up to 20 bytes |

Read replies contain a packet index at offset `magic_start + 4` and 20 data
bytes at `magic_start + 6`. The device exchange collects all 19 distinct packets
with a timeout. Writes wait for each command ACK. A partial write uses packet
indices starting at **zero for that transfer**, even when the destination
range starts elsewhere.

Live colors use a single frame: header byte 2 latches updates, byte 4 = 0,
byte 5 = 1, byte 6 = brightness 100, byte 7 = LED count 12, byte 8 = 5;
bytes 10–19 are `ff`. RGB triplets begin at offset 20. The bridge writes packets
0–2 while latched, then packet 0 with byte 2 cleared to display the new frame.
That is **six HID reports** including the two start-write commands. Pending
colors are coalesced with a minimum 20 ms between update starts; ACK speed can
make the actual refresh rate lower. Shutdown restores the full backed-up
configuration. Black is a valid game color. See
[RGB bridge](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/dualsense/LightbarBridge.cpp).

## 3. Apex 4 V1

V1 uses a different layout: **do not reuse V2 offsets**. Builders emit short
commands which the Linux transport pads to the declared output size (32 or
64 bytes). Output report ID is `05`.

```text
Identity:   05 ec [zeros]
Rumble:     05 0f LOW HIGH [zeros]
Trigger:    05 a0 01 APPLY SIDE MODE P0 P1 P2 P3 P4 [zeros]
```

The `01` before APPLY is the effect-family selector; omitting it shifts the
payload and firmware ignores it. The bridge uses **APPLY=00**: setting it on the
receiver can stall the controller loop for about a second, while clearing it
still applies/releases effects. Side, mode and the Race match-input quirk use
the same meanings as V2. Sending Normal to each side clears trigger effects.

Identity replies are 32 bytes: command `ec` at 15, model at 3, firmware at 9–10,
connection at 13 (`1` = wired). State reports start `04 fe`:

| Offset | Field |
| --- | --- |
| 17 / 19 / 21 / 22 | LX / LY / RX / RY, bytes; center `7f` normalized to `80` |
| 23 / 24 | LT / RT |
| 9 / 10 | Primary / secondary button bits, same meanings as V2 |
| 8 bit `08` | Guide / PS |
| 26–27 | Pitch, signed 16-bit |
| **18 and 20** | Yaw low/high bytes, **not contiguous** |
| 29–30 | Roll, signed 16-bit |
| 11–12 / 13–14 / 15–16 | Physical acceleration X / Y / Z |

Upstream's experimental conversion is gyro `(pitch*9, yaw*5, -roll*12/5)` and
Sony acceleration `(-X*25/2, Z*25/2, Y*25/2)`, with saturation. These are
capture-derived gains, not factory calibration. Firmware may emit zero motion;
the bridge cannot synthesize it. Sources:
[V1 protocol](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/flydigi/Apex4Protocol.cpp),
[V1 input](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/flydigi/Apex4Input.cpp).

## 4. Apex 6 V3 and Pro real-time haptics

V3 uses an **unnumbered 32-byte protocol frame**. Linux hidraw output needs a
leading `00` report-ID byte, so the write buffer is 33 bytes. Replies can arrive
with or without that prefix. **Offsets in this section exclude the prefix.**

| Frame offset | Output field |
| --- | --- |
| 0–1 | `5a a5` |
| 2 | Command |
| 3 | `payload_size + 2` |
| 4 onward | Payload, at most 27 bytes |
| 31 | Sum of frame bytes 2–30 modulo 256 |

GetInfo `01` has no payload. Its reply uses a different header: frame 3 =
package count, 4 = package index, payload begins at 5. Model is at 5, connection
at 6 (`0` = USB), capability bits at 29. Checksums are validated before use.

### Pro motor routes `53`

Enable with two commands, payloads `01 02 03 00 40` then `01 12 02 40 00`.
Disable with `01 02 00` then `01 12 00`. The renderer waits 94 ms after enabling
before streaming. These are the implemented firmware sequence; other field
meanings are not established here.

### Pro real-time motor `57`

Payload is 25 bytes: one control byte, then eight subframes, each containing
**trigger, left grip, right grip**. At 1,000 samples/second this is an 8 ms block.
The control byte is:

```text
80 | (rightGripEnabled ? 10 : 00) | (leftGripEnabled ? 08 : 00)
   | (triggerEnabled ? 04 : 00) | ROUTE
ROUTE: 0 left, 1 right, 2 both, 3 mute
```

Each signed 8-bit sample is encoded as `sample + 128`; **neutral is `80`, not
`00`**. Disabled channels also carry `80`. The upstream renderer translates
trigger effects into these voice-coil samples; they are not the mechanical
resistance commands used by Apex 4/5. Idle keepalives are 50 ms. Cleanup sends
neutral output and disables routing through the renderer's stop sequence.

Standard Apex 6 never uses `53` or `57`; the bridge forwards standard rumble
through Linux `FF_RUMBLE` if supported by its kernel gamepad driver. The Apex 6
family's controls use a matching kernel evdev node with separate trigger axes;
V3 vendor motion is not decoded. Sources:
[V3 packets](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/flydigi/Apex6Protocol.cpp),
[Pro renderer](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/dualsense/Apex6HapticBridge.cpp),
[Linux feedback](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/platform/linux/FeedbackBridge.cpp).

## 5. Virtual DualSense side

The bridge creates a UHID USB controller with Sony VID/PID `054c:0ce6`.
Input uses report `01` (64 bytes), including buttons, sticks, independent
triggers, sensor timestamp/motion, battery and synthetic touch contacts. Native
USB feedback report `02` is accepted at 48–64 bytes and decoded into separate
trigger, standard rumble and lightbar updates, honoring their validity flags.
Feature replies include calibration `05`, address `09`, firmware `20`, and the
`08` setup exchange needed by the Proton/Overwatch initialization path. Unknown
feature requests fail rather than reporting fabricated success.

This is **not a complete physical DualSense**: there is no USB audio interface,
so native PCM haptics cannot reach the bridge. USB/HID support in the game and
Proton is still required; Steam Input translation can hide native feedback.
Sources:
[USB reports](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/platform/linux/DualSenseReports.cpp),
[UHID handling](https://github.com/RafaelDiasCampos/ApexSenseBridge-Linux/blob/main/src/platform/linux/UhidDevice.cpp).

## 6. Timing, cleanup and further work

Input is forwarded on arrival with button edges preserved. Feedback commands
are queued/coalesced away from the input loop. Current pacing is 25 ms for
Apex 4 vendor writes; Apex 5 receiver writes use 10 ms spacing and a 30 ms
rumble holdoff after trigger writes. Wired Apex 5 uses the low-delay asynchronous
path. These are firmware/transport policies, not claims of end-to-end input
latency. See the repository's latency guide for simulated measurements.

The bridge preserves original profile/routing/lighting where supported, stops
motors, then removes the virtual controller on exit/errors. Cleanup cannot run
after `SIGKILL` or power loss. No flash-writing or firmware-update protocol is
implemented. Use `--verbose`, `--trace-hid FILE` and `--telemetry-json FILE`
for deliberate diagnostics. Traces can reveal device identifiers and control
activity; inspect them before publishing.

Unknown work includes Apex 6 vendor motion, additional model RGB/profile
formats, Bluetooth and factory sensor calibration. Contributions should include
model/firmware/connection details and distinguish real captures from synthetic
tests. Never infer support only from a successful HID write or a shared USB ID.
