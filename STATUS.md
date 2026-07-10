# Project Status & Handover Notes

This document provides a summary of the current state of the ESPHome Maytronics Dolphin BLE integration, recent protocol discoveries, verified fixes, and the next steps for debugging and feature implementation.

---

## 1. What is Working & Verified

- **RTC / NTP Clock Synchronization**: On connection and hourly thereafter, the ESP32 successfully syncs the power supply clock by sending a 4-byte big-endian Unix timestamp to `FFF9` with opcode `0x09`.
- **Motor Unit (MU) Telemetry (Opcode 0x01)**:
  - Confirmed as **Little-Endian** (the new `phone configuration` JSON mapping lists `"big_indian": false` for `get_mu_data`).
  - Correctly parses and publishes:
    - **PCB Runtime Hours** (`824` hours)
    - **Impeller Runtime Hours** (`824` hours)
    - **Total Boot Count** (`452` boots)
    - **Not Completed/Aborted Cycles** (`64` cycles)
    - **Active LED State / Telemetry**: Read from byte 157 of MU data block.
- **Cleaning Mode Select Component**:
  - The warning `Invalid option none` in ESPHome logs has been resolved at the ESPHome select layer, but the underlying mode reporting is **not resolved**; see the tracked issues below.
- **PWS Features**:
  - Successfully decodes capabilities: `in_water=true`, `cellular=false`.
- **Cycle Start Time & Configured Cycle Duration**:
  - The frame is parsed as big-endian at payload offset 10 and the duration matrix is currently read at payload offset 30. These are implementation hypotheses, not verified facts; see the tracked issues below.
- **LED Light Control (Opcode 0x10)**:
  - Implemented as a custom ESPHome `light` entity which allows toggle, brightness control (0-100% mapped to byte 1), and mode effects (`Blinking` mapped to mode 1, `Constant/None` mapped to mode 2, and `Disco` mapped to mode 3).

---

## 2. Next Steps / Future Work
- **Importable Template Split**:
  - Split `dolphin_ble.yaml` into a public importable ESPHome template (e.g. `dolphin_ble_core.yaml`) and a private local development overlay once the configuration is thoroughly validated.

## 3. Active Issues — Do Not Treat These as Resolved

These are explicit investigation items. Each one needs packet evidence and a reproducible before/after observation before it is moved to resolved. In particular, do not change the epoch interpretation again without recording the raw bytes and comparing all candidate fields.

### ISSUE-1: Configured cycle duration is invalid

- **Observed:** Home Assistant reports `3916800 s` (65,280 minutes / raw `0xff00`), which is not a plausible cleaning duration.
- **Current code path:** `publish_status_from_frame_()` uses status payload byte 3 as the mode, falls back to `selected_cleaning_mode_` when that byte is zero, then reads a big-endian 16-bit value from payload offset `30 + 2 * (mode - 1)` and multiplies by 60.
- **Evidence:** The captured idle frame contains a duration table with repeated `0x0078` (120-minute) values and `0xff` bytes before it. The `0xff00` result proves that an invalid/sentinel pair or an incorrectly aligned field is being treated as minutes; it is not a real duration.
- **History / finding:** The table start was changed from 31 to 30, and the idle fallback was added, but neither change was validated against a known active-cycle frame. This is not resolved.
- **Next proof required:** Capture a complete status frame before starting, immediately after starting, during cleaning, and after stopping. Print payload indices 0–52 plus the selected source offset/raw 16-bit value. Compare that with the configured mode from the MU/SM blocks. Reject `0xff`, `0xffff`, and implausible values rather than publishing them.

### ISSUE-2: Cleaning Mode reports `None`

- **Observed:** The Cleaning Mode text sensor shows `None`; the captured idle status frame has payload byte 3 equal to `0x00`.
- **Current behavior:** `0x00` is intentionally mapped to `None`, because it appears to mean “no active cycle,” and the select entity suppresses that value. That only hides the invalid select update; it does not answer which program is configured for the next run.
- **Likely distinction:** The status frame’s `cleaning_mode` is an **active-cycle** field. When idle it may legitimately be zero. The configured/last-selected mode may instead come from the MU block (documented byte 167), the SM schedule/start-delay block, or a previously issued mode command. The current implementation does not expose those as separate entities.
- **Next proof required:** Compare status byte 3, MU byte 167, SM schedule mode bytes, and the mode command response while idle and while a cycle is running. Keep “active cleaning mode” and “configured/next cleaning mode” as separate concepts; do not simply map zero to mode 1.

### ISSUE-3: Cycle start time is about six months in the future / sometimes `NA`

- **Observed:** The displayed timestamp is about six months ahead. Before a cycle starts it has also appeared as `NA`.
- **What has actually been tried:** The parser has been changed repeatedly between raw 32-bit values, little-endian, big-endian, and a hard-coded `1559347200` epoch offset. The current code reads payload bytes 10–13 big-endian and adds that offset. The protocol notes call bytes 6–9 a monotonic PWS uptime and bytes 10–13 a UTC Unix timestamp, but this field assignment is not verified by a captured active-cycle packet.
- **Important conclusion:** The six-month error is evidence that either (a) the selected bytes are not a Unix timestamp, (b) the device uses a May-2019-relative epoch and the offset/units are wrong, (c) the timestamp is in local/device time and is being treated as UTC, or (d) the frame offsets are wrong. It is not evidence that another blind epoch constant is needed.
- **`NA` interpretation:** A zero/unset start field before a cycle can be legitimate. Publishing `NA` while idle is preferable to inventing a timestamp, but we need to determine whether Home Assistant should retain the last completed start time or clear it.
- **Next proof required:** Record raw bytes 4–13 and the ESPHome SNTP Unix timestamp at the same moment before start, immediately after start, during the cycle, and after stop. Decode all four candidate combinations (bytes 6–9 and 10–13, BE and LE) and compare with the observed wall clock. Only then choose the field and conversion; document the arithmetic and remove the hard-coded offset unless packet evidence proves it.

### ISSUE-4: No schedule viewing or control is exposed

- **Current state:** `dolphin_ble.yaml` exposes start/stop/pickup buttons and a cleaning-mode select, but no weekly schedule entities, start-delay entities, or schedule readback.
- **What the protocol supports:** The SM block contains a weekly schedule and start-delay settings. The protocol notes also describe write commands `0x45` (weekly schedule), `0x47` (single-day schedule), and `0x46` (start delay).
- **Gap:** The command formats are documented only; there is no parser or ESPHome entity model for reading or writing them. This is a real feature gap, not a Home Assistant display problem.
- **Next proof required:** First publish the raw/read-only schedule (repeat flag, seven day entries, start delay) from the SM response. Then add validated controls and writes, gated by the advertised quick-feature capability bits.

### ISSUE-5: “Quick button features” are being misunderstood

- **What it means:** The current `Quick Features` text sensor is a capability bitmask, not the power-supply button configuration and not a control. It reports which features the PWS claims to support: weekly timer intervals, start delay, filter LED, Floor Only, Fast, and Pickup.
- **Offset warning:** The protocol notes label this byte 65, while the current parser reads `payload[66]`. That one-byte disagreement must be resolved from an indexed SM packet before trusting the displayed capability names.
- **What it does not do:** It does not configure what pressing the physical power-supply button does, and the YAML has no entity for that. The current code only formats the bitmask into text.
- **Physical button control status:** No command or response mapping for “physical button action” has been established in this repository. We must not claim that the button can be configured until an app trace or protocol mapping identifies that setting.
- **Next proof required:** Identify the app’s setting/API and corresponding packet, if one exists. Separately expose capability flags and actual schedule/button configuration so they cannot be conflated.

### ISSUE-6: Which state indicates that cleaning is in progress?

- **Best current answer:** The protocol defines PWS state byte 1 value `0x05` (`onCleanMode`) as the power supply’s direct “cleaning” state. Robot/MU state byte 0 value `0x02` (`scanning`) also indicates the robot is cleaning, while `0x01` (`mapping`) may be part of startup/cleaning behavior.
- **Therefore:** `PWS State == Cleaning` is a strong primary indicator for the power supply cycle, but it should be corroborated with `Robot State` and, when available, In-Water status. `PWS State` alone can be transitional or stale if the MU is disconnected.
- **Current implementation gap:** There is no dedicated binary `Cleaning in Progress` sensor that combines these fields. The text sensors are published independently, so Home Assistant automations currently have to interpret them.
- **Next proof required:** Capture state transitions for start, mapping, active cleaning, finish, stop, and disconnected cases. Then define a derived binary sensor with explicit semantics rather than making users infer state from text.
