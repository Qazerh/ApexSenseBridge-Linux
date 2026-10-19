# Overwatch: native controller input diagnosis

Use this when the virtual DualSense's sticks/buttons work in Steam's input
tester but Overwatch misreads them (for example, physical Select becomes LT).
Steam's physical Flydigi entry being unresponsive while the bridge grabs it is
expected. That entry alone does not establish the cause of an in-game failure.

The updated bridge supports synchronous input/output report snapshots as well
as streamed input. The first Linux version returned an unsupported error for
input snapshot requests. This is corrected, but has not yet been confirmed as
the cause of the Overwatch problem.

The next captured trace showed 99 rejected writes to the declared host setup
feature `0x08`. This report is now acknowledged and retained for virtual
readback, without changing the Apex's configuration. A regression test reproduces
the request shape and verifies that trigger output can follow a successful
setup. The user subsequently confirmed working input and adaptive triggers in
Overwatch without launch-option changes. The successful trace showed feature
`0x08` accepted and no unsupported trigger effects. The current build's trace
header is `v8`, which retains the working setup support, independent input,
smaller live RGB transfers and rumble, and adds battery/touchpad counters.

## Capture a diagnostic run

1. Fully exit Overwatch and stop the old bridge with Ctrl+C.
2. Use the updated executable. Start it with HID tracing:

   ```sh
   ./bin/ApexSenseBridge bridge --trace-hid bridge-hid.log
   ```

3. Keep Steam Input disabled for Overwatch. For one diagnostic launch, set Steam
   launch options to the following. Replace `/path/to/ApexSenseBridge` with the
   absolute directory of this checkout:

   ```text
   SDL_GAMECONTROLLER_IGNORE_DEVICES=0x37d7/0x2501 PROTON_ENABLE_HIDRAW=0x054c/0x0ce6 python3 "/path/to/ApexSenseBridge/scripts/linux-proton-probe.py" gamemoderun mangohud %command%
   ```

4. Launch Overwatch. The native HID checker runs first through the game's
   selected Proton. During its approximately 12-second input phase, move both
   sticks and press Select, A, and both triggers. Overwatch launches afterward.
   Test its controls again, then exit the game and stop the bridge.
5. Provide `bridge-hid.log` from the directory where you started the bridge and
   `proton-hid-probe.log` from the checkout's root directory. The checker only
   reads controller information/input; it does not send trigger effects or
   change controller settings. The logs include controller identifiers and
   controller input samples.

Afterward, remove the `python3 .../linux-proton-probe.py` portion of the launch
options so subsequent starts go straight into the game.

## What the checks establish

- A native USB DualSense should expose input/output/feature report lengths
  **64/48/64**, with input report ID `01` and usage page/usage `0001/0005`.
- The checker prints raw input bytes and values parsed by Windows' HID API.
  This separates errors in Proton's report/layout handling from an error in
  the game's choice of API or controller.
- `GET_INPUT_REPORT` and `ReadFile` are checked independently. A working
  streamed report does not guarantee that synchronous requests are supported.
- The bridge trace shows virtual HID feature/control requests, rejected
  requests, feedback packet bytes and translation counters. It is bounded to
  8192 request/feedback events, plus periodic status counters.
- `OPEN`/`CLOSE` events in that trace count aggregate kernel consumers. They
  do not identify which process opened the controller.

The diagnostic executable is provided as `bin/ProtonHidProbe.exe`. Its complete
source is `src/tools/ProtonHidProbe.c`. Rebuilding the optional checker requires
Wine development headers/import libraries plus GCC and binutils:

```sh
bash scripts/build-proton-hid-probe.sh
```

The ordinary Linux bridge still has no Wine build dependency.

The checker also writes `bin/proton-hid-probe-native.log` because Steam can
detach a child Windows program's console from launcher output. The wrapper
appends this native report to `proton-hid-probe.log`. If the native file is
missing, the wrapper reports that capture failed rather than interpreting the
launcher's exit code as a successful HID check.
