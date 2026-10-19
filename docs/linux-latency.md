# Linux v8 input latency

Stop the old bridge with Ctrl+C and start `bin/ApexSenseBridge bridge` again.
The optimizations are enabled by default; no launch-option change or special
latency flag is needed. Features depend on the [controller model](linux-models.md).
The v8 model expansion retains this path and preserves buffered evdev frames
on Apex 6, including adjacent button press/release edges.

## What changed

- Each fresh physical sample is decoded and sent immediately to UHID. The old
  1 ms forwarding gate is gone. A press followed by a release in the same burst
  produces two reports in the original order, including sticks and motion.
- Physical input is drained in bounded batches of up to 32 packets before one
  game-feedback event is serviced. The previous loop handled up to 64 feedback
  events before reading one physical packet. Both directions still progress
  continuously during heavy traffic.
- One event-driven wait watches the physical and virtual controller together.
  Ready input wakes it immediately. A 1 ms timer only maintains quiet-period
  heartbeats and synthetic touch gestures; fresh input never waits for it.
- Nonblocking reads avoid a redundant readiness check for each queued packet.
  Input writes include the complete 64-byte HID report in a 70-byte UHID event,
  avoiding the event union's unused tail. Linux accepts these short, zero-filled
  events; see the kernel's [UHID implementation](https://github.com/torvalds/linux/blob/master/drivers/hid/uhid.c)
  and [UHID API documentation](https://docs.kernel.org/hid/uhid.html).
- Trace file writes, trigger debug messages, periodic terminal output and JSON
  publication run on a background worker. Its queue holds at most 256 records;
  if busy/full it skips diagnostic records instead of waiting in the input loop.
  `diagnostic_drops` reports this in status snapshots, periodic output and the
  final trace line. This queue never stores controller input or game feedback.
- Healthy iterations avoid acquiring trigger/rumble/RGB error-string locks.
  Physical bow-trigger updates are queued after the corresponding virtual
  button/axis report has already been sent.

The existing independent physical input queue and asynchronous trigger, rumble
and RGB writes are retained. Wireless command pacing remains unchanged;
removing firmware pacing could make feedback unreliable and does not speed
up this independent input path. The normal runtime sleeps while idle and does
not require administrator access, busy polling or a real-time scheduling policy.

The default touchpad profile is `none` and has no hold-to-decide button delay.
Manually selected gesture profiles still intentionally consume or defer their
documented shortcuts (for example the 400 ms View hold threshold).

## Verification and measurement

All 33 automated tests pass. The input regression test uses the same
`InputPump`, mapper and UHID report writer as the app. It verifies:

- 64 adjacent input samples preserve all press/release edges, axes and gyro;
- the input is already delivered before physical trigger-position work runs;
- physical input takes priority over a feedback flood and feedback still advances;
- 256 input samples continue while the diagnostic worker and its queue are blocked;
- synthetic touch taps finish through heartbeats even without new physical input;
- physical disconnection is detected, and asynchronous I/O failures are reported.

The release-build benchmark injects 2,000 ordered controller samples per second
through local packet sockets, then receives the resulting UHID events. Each
sample alternates a button press/release and carries an ID to detect loss or
reordering. One scenario is input alone; another floods feedback and submits
diagnostic work that takes 2 ms per record. That overload deliberately fills
the diagnostic queue. Its skipped-record count is expected.

| Release simulation | Samples | Median (µs) | 95th (µs) | 99th (µs) | Maximum (µs) | Missing/reordered |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Input only | 2000 | 14.25 | 26.12 | 41.56 | 398.93 | 0 |
| Feedback flood + slow diagnostics | 2000 | 13.06 | 16.62 | 17.81 | 20.18 | 0 |

See [recorded measurements](linux-v8-latency-results.json). These are local
software enqueue-to-receive timings, not a physical end-to-end measurement or
a before/after hardware comparison. They exclude USB/radio/firmware delay,
Proton, game polling, rendering and display delay. The saturated scenario keeps
the simulated loop runnable, so it can measure faster than the sleeping input-only
scenario; that is not a recommendation to generate extra traffic.

Reproduce from the repository directory, without a connected controller:

```sh
cmake -S . -B build-latency -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-latency --target ApexSenseBridgeLinuxLatencyTests --parallel
./build-latency/ApexSenseBridgeLinuxLatencyTests
./build-latency/ApexSenseBridgeLinuxLatencyTests --benchmark
```

Actual button-to-game latency still needs measurement on the Apex 5 and the
game. The bridge cannot reduce latency that occurs before a physical report
arrives or after the kernel receives the virtual report.
