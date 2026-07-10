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
  - The protocol parser strips the first response-payload byte as an ACK before applying offsets. In the raw ESPHome frame, status bytes 5–6 are `cycleTime`, bytes 7–10 are device uptime, and bytes 11–14 are UTC Unix time, all big-endian. The protocol adds no epoch offset. The duration estimate matrix begins at raw payload offset 31, but is separate from `cycleTime`.
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

## 5. Phone-App Ground Truth (2026-07-10)

The official phone app provided useful behavioral anchors before the ESP32 was reconnected:

- Device presentation: `Crazy Climber`; description `DOLPHIN NAUTILUS CC PRO IOT`; PKF `PKF0111`; serial `Z4868YMRBM`; MU serial `Z4868YMR`.
- Configured schedule: every day at 09:00, for 2 hours.
- LED: enabled, Disco.
- Physical power-supply quick button: Weekly timer, Every 2 days. This is the frequency of the cleaning operation launched by holding the physical button for five seconds; it is not the normal weekly schedule itself.
- Live cycle: at 14:46 the app showed 1% and an estimated finish at 16:44; at 15:19 it showed 29%. The elapsed-time ratio (33 minutes / 118 minutes) is approximately 28%, strongly confirming a two-hour configured duration and an elapsed-time-based progress indicator.

When the ESP32 is reconnected, preserve complete raw MU, SM, and status frames. The first comparisons should be SM cycle-time bytes 217–236 against the app's 2-hour setting, SM weekly bytes 72–107 against 09:00 daily, the quick-button trigger/configuration fields against Every 2 days, and status transitions during an actual start/stop. Do not infer MU units from the old sample alone.

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
- **What has actually been tried:** The parser was changed repeatedly between raw values, little-endian, big-endian, and a hard-coded `1559347200` epoch offset. protocol parser implementation fixes the mData offsets at 6–9 for device uptime and 10–13 for `cycleStartTimeUTC`, both big-endian, with no added epoch. The raw-frame ACK was the remaining off-by-one.
- **Important conclusion:** The six-month error was caused by the unverified epoch addition. The protocol does not add one. If the live value is still wrong after this correction, the remaining question is device clock synchronization—not adjacent status-byte selection.
- **Current correction:** The implementation now removes the raw ACK conceptually and reads `cycleStartTimeUTC` from raw payload bytes 11–14. The live cleaning capture decoded `0x6a513d7f` (2026-07-10 18:44 UTC), matching the phone's 2:44 PM local cycle start. Selecting a mode does not start a cycle; opcode `0x03` only sets the mode.
- **`NA` interpretation:** A zero/unset start field before a cycle can be legitimate. Publishing `NA` while idle is preferable to inventing a timestamp, but we need to determine whether Home Assistant should retain the last completed start time or clear it.
- **Next proof required:** Verify the flashed entity value against this same active-cycle capture. Do not revisit adjacent offsets or endian order without contradicting the protocol response slicing and raw frame evidence.

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

- **Current finding:** The live capture was definitely cleaning (phone showed 29%), but the state bytes did not map to the expected cleaning enum. The protocol-aligned cycle info did: it contained a 120-minute cycle and a start time matching the phone. This disproves using PWS State alone as the primary truth source for this device.
- **Therefore:** A derived cleaning indicator must combine valid cycle timing with state transitions and, when available, robot/in-water status. PWS State remains useful corroboration, not a sufficient answer by itself.
- **Current implementation gap:** There is no dedicated binary `Cleaning in Progress` sensor that combines these fields. The text sensors are published independently, so Home Assistant automations currently have to interpret them.
- **Next proof required:** Capture state transitions for start, mapping, active cleaning, finish, stop, and disconnected cases. Then define a derived binary sensor with explicit semantics rather than making users infer state from text.

### ISSUE-7: MU PCB/impeller runtime units or layout are not physically credible

- **Observed:** The current combined runtime sensors report implausibly large hour totals.
- **protocol evidence:** `protocol field definitions` names separate fields `pcb_hours` (bytes 140–141), `pcb_minutes` (byte 142), `impeller_hours` (bytes 143–144), and `impeller_minutes` (byte 145). The protocol parser implementation parses the two-hour fields as 16-bit integers and does not convert them from minutes.
- **Captured bytes:** The prior MU log for payload offsets 130–159 was `000000a00f4f4300007800390325390322c50141000216020000a2ffffff`. At the protocol-defined ranges this is PCB `00 39`, PCB minutes `03`, impeller `25 39`, impeller minutes `03`.
- **Additional observation:** `MU Not Completed Cycles` has also appeared as `16897` (`0x4201`). For bytes `01 42`, big-endian is `322`, while little-endian is `16897`; the latter is not credible. This demonstrates that the protocol's generic MU endian behavior cannot be blindly applied to this device.
- **Conflict:** Little-endian at the protocol-defined runtime ranges gives PCB `14592 h` and impeller `14629 h`; big-endian gives PCB `57 h` and impeller `9529 h`. The previous adjacent-byte interpretation yielded roughly `825 h`, which is numerically more plausible but is not supported by the protocol ranges. This is therefore not safe to “fix” by another endian or ±1 change.
- **Next proof required:** Obtain a fresh full MU response from this exact PWS and compare it with the official app’s displayed diagnostics or a known runtime change. Until that comparison exists, the runtime sensors must be treated as unverified and should not be presented as accurate hours.
