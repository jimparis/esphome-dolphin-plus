# Maytronics Dolphin Plus BLE Notes

## Observed Peripheral

- MAC: `AC:74:C4:0E:D8:6E`
- Name: `Z4868YMR`
- Advertised service UUID: `fd5abba0-3935-11e5-85a6-0002a5d5c51b`
- Manufacturer data:
  - company/id: `0x0600`
  - payload: `d6b2f005f0f8`

## Protocol Families

Two BLE protocol families are currently known:

### IOT

Used for the observed advertised service UUID.

- Service UUID: `fd5abba0-3935-11e5-85a6-0002a5d5c51b`
- Characteristic UUID: `fd5abba1-3935-11e5-85a6-0002a5d5c51b`
- CCCD UUID: `00002902-0000-1000-8000-00805f9b34fb`

### UART / POP

Known secondary family, but not the path for the observed advertisement.

- Service UUID: `fd5abca0-3935-11e5-85a6-0002a5d5c51b`
- Write characteristic UUID: `fd5abca1-3935-11e5-85a6-0002a5d5c51b`
- Notify characteristic UUID: `fd5abca2-3935-11e5-85a6-0002a5d5c51b`
- CCCD UUID: `00002902-0000-1000-8000-00805f9b34fb`

## BLE Connection Sequence

The working connection sequence does more than a normal GATT client connection:

1. Open a local GATT server on the ESP32.
2. Add local service `fd5abba0-3935-11e5-85a6-0002a5d5c51b`.
3. Add local characteristic `fd5abba1-3935-11e5-85a6-0002a5d5c51b` with notify property.
4. Add CCCD descriptor `00002902-0000-1000-8000-00805f9b34fb`.
5. Connect as a GATT client to the robot.
6. On client connection, discover remote services.
7. Find remote service/characteristic `fd5abba0/fd5abba1`.
8. Enable notifications on remote `fd5abba1` by writing its CCCD.
9. Request MTU `512`.
10. Treat BLE as connected after MTU success.

This dual role is the likely reason a generic nRF Connect client can see the device but does not complete the same connection workflow.

## Message Direction Hypothesis

The robot subscribes to the ESP32's local characteristic by writing `0100` to the local CCCD. That suggests the robot receives commands as notifications from the ESP32, while robot-to-ESP32 data arrives as notifications from the robot's remote characteristic.

The ESPHome prototype now validates connection setup, can send text-formatted framed probes as local GATT notifications, and logs robot notifications.

## IOT Packet Framing

The observed command frames use this layout:

- SOP preamble: `ab`
- Source: `03`
- Destination: 2 bytes from the command entry
- Opcode: 1 byte from the command entry
- Payload length: 2 bytes, big-endian
- Payload: command-specific bytes
- Checksum: 2-byte unsigned sum of every preceding frame byte, big-endian

Outbound IOT frames are transported as ASCII text notifications in this form:

- `03:<lowercase_frame_hex>`

The `03` prefix matches the IOT source/key byte used by the command frames below. Sending the raw binary frame bytes as the local notification payload completed at the BLE layer but produced no observed robot responses.

Robot responses are also ASCII text notifications. Response frames observed so far start with `:<lowercase_frame_hex>` and may be split across several BLE notifications. The frame length field is sufficient to reassemble the fragments and validate the trailing checksum.

Internal parameter read requests use these payloads:

- `get_mu_data`: `0100ff`
- `get_sm_data`: `0200ff`

## Initial Read Probe Frames

These are read-only probes currently configured in `dolphin_ble.yaml`:

- `pws_features`: `ab03fffa1a000002c1`
- `system_status`: `ab03fff807000002ac`
- `temperature`: `ab03fff809000002ae`
- `cloud_connection_status`: `ab03fffedf0000038a`
- `get_mu_data`: `ab03fffd0100030100ff03ae`
- `get_sm_data`: `ab03fffd0200030200ff03b0`

Expected response content:

- `pws_features`: feature bits for network sensing, in-water, cellular, OTA, PSC.
- `system_status`: MU state, SM state, filter state, cleaning mode, cycle info, next cycle info, faults, cleaning modes.
- `temperature`: in-water status, temperature, measurement flags, timestamp.
- `cloud_connection_status`: one-byte error/status field.
- `get_mu_data`: 256-byte MU configuration/history block including faults, robot type, flash counter, cycle time, PCB/impeller runtime, turn-on count, software version, LEDs, clean mode, climb period.
- `get_sm_data`: 256-byte SM/PWS configuration block including timezone, quick features, PWS software version, weekly schedule, trigger source, delay, cycle times, Wi-Fi SSID.

`MU` and `SM` appear to refer to different controllers or controller-side data blocks inside the system. That is still a hypothesis; the names line up with the separate 256-byte read requests and different sets of state/configuration fields.

Write/control commands identified but intentionally not sent yet:

- `start_up_dolphin`: opcode `06`, destination `FFF8`, no payload.
- `shutdown_dolphin`: opcode `05`, destination `FFF8`, no payload.
- `set_cleaning_mode`: opcode `03`, destination `FFE9`, 1-byte payload.
- `manual_drive`: opcode `03`, destination `FFF7`, 2-byte payload:
  - byte 0: direction (`01` stop, `02` forward, `03` backward, `04` right, `05` left)
  - byte 1: speed (`0`-`100`)
- `quit_manual_drive`: opcode `04`, destination `FFF7`, no payload.
- `set_cycle_time`: opcode `01`, destination `FFE9`, 2-byte payload.
- `set_robot_leds`: opcode `10`, destination `FFF7`, 3-byte payload.
- `reset_filter_indicator`: opcode `0a`, destination `FFF7`, no payload.
- `quick_features`: opcode `0b`, destination `FFF9`, 1-byte payload.

## ESPHome Exposure

The ESP32 bridge now exposes:

- current robot state, PWS state, cleaning mode, in-water status, and filter state
- raw status blocks for system, temperature, MU, and SM
- robot type, turn-on count, MU firmware fields, temperature, and cycle timing fields that have been decoded so far
- cleaning-mode control, start/stop, pickup, refresh, and manual drive controls
- manual drive direction and speed as separate controls, with a direct `manual_drive` packet assembled from them

`get_sm_data` is still mostly exposed as a raw 256-byte blob in the bridge. The decoded protocol notes suggest it contains timezone, quick-features, PWS SW version, weekly schedule, delay, cycle times, and Wi-Fi SSID fields, but those offsets have not yet been lifted into individual ESPHome entities.

The next decode pass should focus on these SM fields:

- `wifi.netName`
- `systemState.timeZone` and `systemState.timeZoneName`
- `weeklySettings`
- `delay`
- `cleaningModes`
- `led`
- `featureEn`

The app-side parser also shows the current top-level `system_status` layout:

- `cleaning_mode`
- `cleaning_modes`
- `cycle_info`
- `filter_state`
- `next_cycle_info`
- `sm_state`
- `mu_state`

The app models map those to:

- `sm_state` -> `PwsState` (`off`, `on`, `hold_weekly`, `hold_delay`, `programing`, `on_clean_mode`, `sleep`, `unknown`)
- `mu_state` -> `RobotState` (`init`, `mapping`, `cleaning`, `recovery`, `finished`, `programing`, `fault`, `not_connected`, `unknown`)
- `cleaning_mode` -> `CleanMode` (`regular`, `all_surfaces`, `fast`, `cove`, `floor_only`, `water_line`, `ultra_clean`, `spot`, `wall_only`, `tic_tac`, `custom`, `pick_up`, `empty`, `unknown`)
- `filter_state` -> filter bag indication byte
- `cycle_info` and `next_cycle_info` -> mode plus duration/time fields
- `cleaning_modes` -> 32-byte mode estimate table
- `systemStateBuoyData` -> `batteryPercentage` and `isCharging` when present

## ESPHome Prototype Validation

On 2026-07-09, the ESPHome prototype was built and flashed to an ESP32 on `/dev/ttyUSB0`. The runtime log showed:

1. ESPHome enabled Bluedroid.
2. The custom component registered local GATTS and GATTC apps.
3. The local IOT GATT service/notify characteristic/CCCD were created.
4. The robot connected back to the ESP32's local GATT server and wrote `0100` to the local CCCD.
5. The ESP32 opened a direct GATT client connection to `AC:74:C4:0E:D8:6E`.
6. The ESP32 found remote service `fd5abba0-3935-11e5-85a6-0002a5d5c51b`, characteristic `fd5abba1-3935-11e5-85a6-0002a5d5c51b`, and CCCD.
7. Register-for-notify completed with status `0`.
8. MTU negotiation completed with status `0`, MTU `512`.

The initial 2026-07-09 probe firmware sent all six configured read probes as raw local GATT notifications. Each local notification completed with confirmation status `0`.

No robot notification payloads were observed during the 60-second post-flash capture.

A later 2026-07-09 validation pass sent the same frames using the text notification envelope above. The robot responded to:

- `pws_features`: response source `0x0d`, destination `0xfffa`, opcode `0x1a`, payload length `3`, payload `000003`, checksum OK.
- `system_status`: response source `0x0e`, destination `0xfff8`, opcode `0x07`, payload length `53`, checksum OK.
- `get_mu_data`: response source `0x0d`, destination `0xfffd`, opcode `0x01`, payload length `256`, checksum OK.
- `get_sm_data`: response source `0x0d`, destination `0xfffd`, opcode `0x02`, payload length `256`, checksum OK.

No response was observed for `temperature` or `cloud_connection_status` in the same idle-state capture.
