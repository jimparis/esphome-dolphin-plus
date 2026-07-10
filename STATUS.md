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
- **Cleaning Mode Select Component**:
  - The warning `Invalid option none` in ESPHome logs has been resolved. The C++ code now suppresses publishing `"None"` (mode `0x00`) to the Home Assistant select entity when the robot is idle, while still reporting `"None"` to the text sensor.
- **PWS Features**:
  - Successfully decodes capabilities: `in_water=true`, `cellular=false`.

---

## 2. Active Debugging / Parsing Discrepancies

### A. Cycle Start Time (showing `3964693248`)
- **Discovery**: The PWS encodes the cycle start time as a **Big-Endian** 32-bit integer. 
- **Bug**: Our current C++ code reads it at offset 10 using `read_u32_le_` (little-endian). This turns the raw bytes (`00 17 4F EC` when not synced) into `3964693248`. 
- **Fix**: Update the parsing in `DolphinBle::publish_status_from_frame_` in [dolphin_ble.cpp](file:///home/jim/git/ha-maytronics-plus-bluetooth/components/dolphin_ble/dolphin_ble.cpp):
  ```cpp
  // Change:
  uint32_t start_time = read_u32_le_(payload + 10);
  // To:
  uint32_t start_time = read_u32_be_(payload + 10);
  ```
  *(Note: If the robot RTC has not been synced at cycle start, it defaults to a small number of seconds like `1527788` which represents time since default boot epoch).*

### B. Configured Cycle Duration (showing `0s` or incorrect values)
- **Discovery**: In the system status frame, the durations table is **Big-Endian** u16 values in minutes starting at offset **31** (e.g. `00 78` = 120 minutes, `00 3c` = 60 minutes).
- **Bug**: In a previous change, we changed the read to little-endian (`read_u16_le_`). This needs to be reverted to `read_u16_be_` at offset 31.
- **Fix**:
  ```cpp
  // Revert back to:
  duration_mins = read_u16_be_(payload + 31 + (active_mode - 1) * 2);
  ```

---

## 3. Missing Features to Implement

### LED Light Control
- **Protocol Findings**:
  - Command is sent to destination `FFF7` with **opcode `0x10`**.
  - Payload is **3 bytes**:
    1. **Enabled** (bool: `0` or `1`)
    2. **Intensity** (byte: `0` to `100`)
    3. **Mode** (byte: `1` = Blinking, `2` = Constant, `3` = Disco)
- **Telemetry**:
  - The current LED status is stored in the MU parameters response (opcode `0x01`) at offset `157` (field `leds` in the new JSON protocol mapping).
- **Next Steps**:
  1. Add a `light` component or custom selects/numbers in `dolphin_ble.yaml` and bound python schemas to expose LED controls.
  2. Implement the command construction and transmission in `dolphin_ble.cpp`.
