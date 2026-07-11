# Project Status

Current status of the ESPHome Maytronics Dolphin BLE integration.

## Working Features

- RTC synchronization sends a four-byte big-endian Unix timestamp to `FFF9` with opcode `0x09` on connection and hourly thereafter.
- MU telemetry publishes PCB runtime, impeller runtime, boot count, incomplete cycles, software version, and packed LED state. It is refreshed every 30 seconds after connection.
- PWS capabilities are decoded from the compact three-byte reply, including in-water and cellular support.
- The cleaning-mode select no longer emits the ESPHome `Invalid option none` warning.
- Cleaning mode is exposed as a single select entity. Telemetry updates its current state and user changes send the corresponding mode command.
- Cycle duration is derived from the local `SM/62/1` cleaning-duration table, and cycle time remaining is derived from active-cycle start time plus that duration.
- Verbose protocol logging is runtime-controlled by the `Protocol Debug Logging` switch. Normal operation suppresses repetitive poll queue/send/ACK/frame logs.
- BLE advertisement relay is enabled with passive scanning and without Home Assistant active GATT proxy connections.
- The custom LED light supports on/off, brightness from 0-100%, and the `Blinking`, `Constant`, and `Disco` effects.
- Status, SM, and MU requests are coordinated by the C++ component. Temperature polling remains disabled pending a valid, non-disruptive response from this unit.

## Protocol Layouts

All response payloads described here begin with a one-byte response ACK. The documented data offsets begin after that byte; raw ESPHome frame offsets are therefore one greater.

- `system_status`: data bytes 0-3 are robot state, PWS state, filter state, and active cleaning mode. Data bytes 4-5 are the cycle type/time field, bytes 6-9 are device uptime, bytes 10-13 are the UTC cycle start time, and bytes 30-51 contain the cleaning-duration estimate table. Multi-byte status values are big-endian.
- `get_sm_data`: timezone is at data bytes 63-64; quick features at byte 65; weekly schedule at bytes 72-107; start delay at bytes 108-113; SSID at bytes 118-150; and configured cycle times at bytes 217-236.
- `get_mu_data`: robot type is at bytes 132-133; flash counter at 134-137; cycle time at 138-139; PCB runtime at 140-142; impeller runtime at 143-145; turn-on count at 146-147; incomplete cycles at 148-149; packed LED state at data byte 155 (verified by live matrix testing; bytes 156-157 remain `0xFF`); clean mode at 167; software-version/checksum at 168-169; and climb period at 170. The multi-byte MU fields are little-endian.

## Device Observations

The phone reports the following configuration for the reference unit:

- Device presentation: `Crazy Climber`; description: `DOLPHIN NAUTILUS CC PRO IOT`.
- Power-supply serial: `Z4868YMRBM`; motor-unit serial: `Z4868YMR`.
- Daily schedule at 09:00 with a two-hour duration.
- LEDs enabled with the `Disco` effect.
- Physical quick-button setting: weekly timer, every two days. This is separate from the regular weekly schedule.

The phone reported 1% progress at 14:46 and 29% at 15:19, with an estimated finish at 16:44. The device data and ESPHome entities should be compared against these values when validating active-cycle timing.

## Known Discrepancies

### Configured cycle duration

The status estimate table and SM bytes 217-236 are not used by the reference protocol implementation. The component uses the `SM/62/1` cleaning-duration properties: regular 120, short 60, cove 120, floor 120, waterline 120, ultra 120, spot 120, wall 120, TicTac 600, custom 120, pickup 5, and stairs 120 minutes.

### Cleaning mode

When idle, the status frame can report active cleaning mode `0x00`. The integration does not publish `None` into the cleaning-mode select because that is not a valid command option; the select retains the last valid mode reported by telemetry or selected by the user.

### Cycle start time

The integration reads the UTC cycle start time from raw payload bytes 11-14, big-endian, with no epoch adjustment. A zero or unset field is published as `NA` when no active cycle is present. The value is a fixed start timestamp rather than the current PWS clock. If its RTC has been stale, the controller can report an old cycle start; after RTC synchronization, it recomputes the active cycle's start from the corrected clock and elapsed run time.

Cycle Time Remaining is computed locally as configured duration minus elapsed time since the active cycle start timestamp. It is published as `NA` when the cycle is inactive, the timestamp is invalid, or local time is not available.

### Quick-button configuration

Quick Features is a capability bitmask. It reports support for timer intervals, start delay, filter indication, cleaning modes, and pickup mode; it does not describe the physical power-supply button's current action. The integration does not currently expose physical-button configuration.

### Cleaning state

PWS state alone is not sufficient to identify an active cleaning cycle on the reference unit. A future binary cleaning sensor should combine cycle timing with state transitions and, when available, motor-unit and in-water status.

### MU runtime values

The current live response reports approximately 828 hours 49 minutes for PCB runtime, 828 hours 46 minutes for impeller runtime, 457 turn-ons, and 68 incomplete cycles. These values are plausible and should be retained as the current reference until compared with device diagnostics.

### Filter status

Raw filter value `0x66` is a not-available indication rather than 102 percent. The integration publishes `NA` numerically and `not_available` textually for `0x66` and `0xff`; zero represents an empty or clear filter.

### In-water status and temperature

The PWS advertises in-water support, but its temperature request has not yielded a valid response on the reference unit and coincides with malformed short-ACL traffic. Temperature polling is disabled until a valid response can be captured without disrupting status traffic.

### LED readback

Live matrix testing with the official app and ESPHome verified that data byte 155 is the active packed LED byte (e.g. `0xA2` for Constant, `0xA3` for Disco, `0x42` for 40% Constant), while bytes 156 and 157 remain `0xFF`. 

Although the PWS processes LED control writes (`FFF7/10`) and manual drive writes (`FFF7/03`), it does not return a clean ACK response on this unit (often causing link-layer BLE aborts/short-ACL warnings). To prevent command queue blockage, these `FFF7` commands are now treated as write-only (`expects_response = false`) and popped immediately after transmission, resolving the 4-second command queue timeouts. The integration now defaults `mu_led_data_offset` to `155`.

Start and stop cleaning commands are also treated as write-only on the reference unit: they can take effect physically without producing the expected clean response frame. After control writes, the component queues an immediate `system_status` refresh and then continues the normal 2-second status polling cadence.

Timezone and RTC sync writes are also treated as write-only. The reference unit applies them without returning a clean response frame, so waiting for ACKs only delays initialization.

## Future Work

- Add read-only schedule and start-delay entities.
- Add validated schedule and start-delay write controls.
- Add a derived binary `Cleaning in Progress` sensor.
- Separate the public importable ESPHome template from the local development configuration.
