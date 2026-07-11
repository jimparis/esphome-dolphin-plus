# Project Status

Current status of the ESPHome Maytronics Dolphin BLE integration.

## Working Features

- RTC synchronization sends a four-byte big-endian Unix timestamp to `FFF9` with opcode `0x09` on connection and hourly thereafter.
- MU telemetry publishes PCB runtime, impeller runtime, boot count, incomplete cycles, software version, and packed LED state. It is refreshed every 30 seconds after connection.
- PWS capabilities are decoded from the compact three-byte reply, including in-water and cellular support.
- The cleaning-mode select no longer emits the ESPHome `Invalid option none` warning.
- The custom LED light supports on/off, brightness from 0-100%, and the `Blinking`, `Constant`, and `Disco` effects.
- Status, temperature, SM, and MU requests are coordinated by the C++ component. Temperature polling is enabled only when the PWS advertises in-water support.

## Protocol Layouts

All response payloads described here begin with a one-byte response ACK. The documented data offsets begin after that byte; raw ESPHome frame offsets are therefore one greater.

- `system_status`: data bytes 0-3 are robot state, PWS state, filter state, and active cleaning mode. Data bytes 4-5 are the cycle type/time field, bytes 6-9 are device uptime, bytes 10-13 are the UTC cycle start time, and bytes 30-51 contain the cleaning-duration estimate table. Multi-byte status values are big-endian.
- `get_sm_data`: timezone is at data bytes 63-64; quick features at byte 65; weekly schedule at bytes 72-107; start delay at bytes 108-113; SSID at bytes 118-150; and configured cycle times at bytes 217-236.
- `get_mu_data`: robot type is at bytes 132-133; flash counter at 134-137; cycle time at 138-139; PCB runtime at 140-142; impeller runtime at 143-145; turn-on count at 146-147; incomplete cycles at 148-149; packed LED state at 155; clean mode at 167; and climb period at 170. The multi-byte MU fields are little-endian.

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

The status estimate table contains repeated 120-minute values, while the cycle type/time field contains values such as `0x0100`, `0x0200`, and `0x0300`. The implementation therefore uses SM data bytes 217-236 for configured cycle durations. The current live table is `120, 60, 120, 120, 120, 120, 120, 120, 120, 120` minutes.

### Cleaning mode

When idle, the status frame can report active cleaning mode `0x00`, which the integration publishes as `None`. The configured or next cleaning mode may instead be represented by the MU block, schedule, start-delay data, or the last mode command. Active and configured modes should remain separate entities.

### Cycle start time

The integration reads the UTC cycle start time from raw payload bytes 11-14, big-endian, with no epoch adjustment. A zero or unset field is published as `NA` when no active cycle is present. Phone-reported cycle start times provide the comparison point for validating this entity.

### Quick-button configuration

Quick Features is a capability bitmask. It reports support for timer intervals, start delay, filter indication, cleaning modes, and pickup mode; it does not describe the physical power-supply button's current action. The integration does not currently expose physical-button configuration.

### Cleaning state

PWS state alone is not sufficient to identify an active cleaning cycle on the reference unit. A future binary cleaning sensor should combine cycle timing with state transitions and, when available, motor-unit and in-water status.

### MU runtime values

The current live response reports approximately 828 hours 49 minutes for PCB runtime, 828 hours 46 minutes for impeller runtime, 457 turn-ons, and 68 incomplete cycles. These values are plausible and should be retained as the current reference until compared with device diagnostics.

### Filter status

Raw filter value `0x66` is a not-available indication rather than 102 percent. The integration publishes `NA` numerically and `not_available` textually for `0x66` and `0xff`; zero represents an empty or clear filter.

### In-water status

The reference unit does not currently provide a usable in-water measurement. If the capability is absent, temperature polling is suppressed and the entity remains `NA`.

### LED readback

Raw MU payload byte 156 (data byte 155) is a packed LED state: bits 3-7 hold intensity in 5% increments, bit 0 selects Disco, bit 1 selects Constant, and neither mode bit selects Blinking. A live `0xa2` value decoded to 100% Constant while the physical LED was solid blue, confirming the mapping. The previously used data byte 157 is not the LED field.

## Future Work

- Add read-only schedule and start-delay entities.
- Add validated schedule and start-delay write controls.
- Add a derived binary `Cleaning in Progress` sensor.
- Separate the public importable ESPHome template from the local development configuration.
