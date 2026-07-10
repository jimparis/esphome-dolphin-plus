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
  - protocol analysis now confirms status payload offsets 4–5 for `cycleTime`, 6–9 for device uptime, and 10–13 for UTC Unix time, all big-endian. The protocol adds no epoch offset. The duration estimate matrix begins at payload offset 30, but is separate from `cycleTime`.
- **LED Light Control (Opcode 0x10)**:
  - Implemented as a custom ESPHome `light` entity which allows toggle, brightness control (0-100% mapped to byte 1), and mode effects (`Blinking` mapped to mode 1, `Constant/None` mapped to mode 2, and `Disco` mapped to mode 3).

---

## 2. Next Steps / Future Work
- **Importable Template Split**:
  - Split `dolphin_ble.yaml` into a public importable ESPHome template (e.g. `dolphin_ble_core.yaml`) and a private local development overlay once the configuration is thoroughly validated.

## 3. protocol-Verified Offset Findings

The checked-in `protocol documentation` is the authority for these offsets. Its `protocol field definitions` uses half-open ranges (`start` inclusive, `end` exclusive), and the parser implementation confirms the integer conversion behavior.

- `system_status`: payload bytes 0–3 are states/mode; bytes 4–5 are the protocol-named big-endian `cycleTime`/cycle-type field; bytes 6–9 are big-endian device uptime; bytes 10–13 are big-endian `cycleStartTimeUTC`; bytes 30–51 are 11 big-endian minute estimates.
- `get_sm_data`: timezone is payload bytes 63–64; Quick Features is byte 65; weekly data is 72–107; delay is 108–113; SSID is 118–150; cycle-time settings are 217–236.
- `get_mu_data`: the authoritative fields are robot type 132–133, flash counter 134–137, cycle time 138–139, PCB hours 140–141, PCB minutes 142, impeller hours 143–144, impeller minutes 145, turn-on count 146–147, incomplete cycles 148–149, LEDs 157, clean mode 167, and climb period 170. Multi-byte values are little-endian in the protocol parser.
- The previous implementation was wrong at SM offsets 63/65 and at several MU offsets after byte 140. Those corrections are now applied.

## 4. Active Issues — Do Not Treat These as Resolved

These are explicit investigation items. Each one needs packet evidence and a reproducible before/after observation before it is moved to resolved. In particular, do not change the epoch interpretation again without recording the raw bytes and comparing all candidate fields.

### ISSUE-1: Configured cycle duration is invalid

- **Observed:** Home Assistant reports `3916800 s` (65,280 minutes / raw `0xff00`), which is not a plausible cleaning duration.
- **Previous code path:** The implementation incorrectly selected a matrix entry using status byte 3 and a fallback mode. The protocol instead parses `cycle_info` bytes 4–5 directly as `cycleTime`; the matrix at byte 30 is a separate set of estimates.
- **Evidence:** The captured idle frame contains a duration table with repeated `0x0078` (120-minute) values and `0xff` bytes before it. The `0xff00` result proves that an invalid/sentinel pair or an incorrectly aligned field is being treated as minutes; it is not a real duration.
- **History / finding:** The table start oscillation was an indexing mistake; protocol evidence fixes it at 30. The invalid values `0x0100`, `0x0200`, and `0x0300` track the selected mode, proving the protocol-named `cycleTime` field is not a minute duration on this PWS.
- **Current correction:** The protocol’s actual configured values are SM payload bytes 217–236 (`cycle_times`), not the status estimates table. The implementation now reads that table after SM metadata arrives and selects the entry matching the chosen mode.
- **Remaining proof:** Verify the ten SM values against the app’s displayed mode durations, especially Spot.

### ISSUE-2: Cleaning Mode reports `None`

- **Observed:** The Cleaning Mode text sensor shows `None`; the captured idle status frame has payload byte 3 equal to `0x00`.
- **Current behavior:** `0x00` is intentionally mapped to `None`, because it appears to mean “no active cycle,” and the select entity suppresses that value. That only hides the invalid select update; it does not answer which program is configured for the next run.
- **Likely distinction:** The status frame’s `cleaning_mode` is an **active-cycle** field. When idle it may legitimately be zero. The configured/last-selected mode may instead come from the MU block (documented byte 167), the SM schedule/start-delay block, or a previously issued mode command. The current implementation does not expose those as separate entities.
- **Next proof required:** Compare status byte 3, MU byte 167, SM schedule mode bytes, and the mode command response while idle and while a cycle is running. Keep “active cleaning mode” and “configured/next cleaning mode” as separate concepts; do not simply map zero to mode 1.

### ISSUE-3: Cycle start time is about six months in the future / sometimes `NA`

- **Observed:** The displayed timestamp is about six months ahead. Before a cycle starts it has also appeared as `NA`.
- **What has actually been tried:** The parser was changed repeatedly between raw values, little-endian, big-endian, and a hard-coded `1559347200` epoch offset. protocol parser implementation now conclusively fixes bytes 6–9 as device uptime and bytes 10–13 as `cycleStartTimeUTC`, both big-endian, with no added epoch.
- **Important conclusion:** The six-month error was caused by the unverified epoch addition. The protocol does not add one. If the live value is still wrong after this correction, the remaining question is device clock synchronization—not adjacent status-byte selection.
- **Current correction:** Selecting a mode does not start a cycle; opcode `0x03` only sets the mode. The implementation now suppresses start time unless the status reports an active cycle and rejects timestamps outside a sane Unix range or far in the future.
- **`NA` interpretation:** A zero/unset start field before a cycle can be legitimate. Publishing `NA` while idle is preferable to inventing a timestamp, but we need to determine whether Home Assistant should retain the last completed start time or clear it.
- **Next proof required:** Record raw bytes 4–13, the ESPHome SNTP Unix timestamp, and the device RTC write value at the same moment immediately after starting. Confirm bytes 10–13 match UTC wall time. Do not revisit adjacent offsets or endian order without contradicting protocol parser implementation evidence.

### ISSUE-4: No schedule viewing or control is exposed

- **Current state:** `dolphin_ble.yaml` exposes start/stop/pickup buttons and a cleaning-mode select, but no weekly schedule entities, start-delay entities, or schedule readback.
- **What the protocol supports:** The SM block contains a weekly schedule and start-delay settings. The protocol notes also describe write commands `0x45` (weekly schedule), `0x47` (single-day schedule), and `0x46` (start delay).
- **Gap:** The command formats are documented only; there is no parser or ESPHome entity model for reading or writing them. This is a real feature gap, not a Home Assistant display problem.
- **Next proof required:** First publish the raw/read-only schedule (repeat flag, seven day entries, start delay) from the SM response. Then add validated controls and writes, gated by the advertised quick-feature capability bits.

### ISSUE-5: “Quick button features” are being misunderstood

- **What it means:** The current `Quick Features` text sensor is a capability bitmask, not the power-supply button configuration and not a control. It reports which features the PWS claims to support: weekly timer intervals, start delay, filter LED, Floor Only, Fast, and Pickup.
- **Resolved offset correction:** The protocol fixes Quick Features at payload byte 65 and timezone at bytes 63–64; the implementation now uses those offsets. Physical button behavior remains a separate unresolved feature question.
- **What it does not do:** It does not configure what pressing the physical power-supply button does, and the YAML has no entity for that. The current code only formats the bitmask into text.
- **Physical button control status:** No command or response mapping for “physical button action” has been established in this repository. We must not claim that the button can be configured until an app trace or protocol mapping identifies that setting.
- **Next proof required:** Identify the app’s setting/API and corresponding packet, if one exists. Separately expose capability flags and actual schedule/button configuration so they cannot be conflated.

### ISSUE-6: Which state indicates that cleaning is in progress?

- **Best current answer:** The protocol defines PWS state byte 1 value `0x05` (`onCleanMode`) as the power supply’s direct “cleaning” state. Robot/MU state byte 0 value `0x02` (`scanning`) also indicates the robot is cleaning, while `0x01` (`mapping`) may be part of startup/cleaning behavior.
- **Therefore:** `PWS State == Cleaning` is a strong primary indicator for the power supply cycle, but it should be corroborated with `Robot State` and, when available, In-Water status. `PWS State` alone can be transitional or stale if the MU is disconnected.
- **Current implementation gap:** There is no dedicated binary `Cleaning in Progress` sensor that combines these fields. The text sensors are published independently, so Home Assistant automations currently have to interpret them.
- **Next proof required:** Capture state transitions for start, mapping, active cleaning, finish, stop, and disconnected cases. Then define a derived binary sensor with explicit semantics rather than making users infer state from text.
