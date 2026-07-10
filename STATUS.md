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
  - The warning `Invalid option none` in ESPHome logs has been resolved. The C++ code now suppresses publishing `"None"` (mode `0x00`) to the Home Assistant select entity when the robot is idle, while still reporting `"None"` to the text sensor.
- **PWS Features**:
  - Successfully decodes capabilities: `in_water=true`, `cellular=false`.
- **Cycle Start Time & Configured Cycle Duration**:
  - Correctly parsed as big-endian values from the system status frame (fixed u32 offset 10 for cycle start time and u16 offset 31 for mode duration).
- **LED Light Control (Opcode 0x10)**:
  - Implemented as a custom ESPHome `light` entity which allows toggle, brightness control (0-100% mapped to byte 1), and mode effects (`Blinking` mapped to mode 1, `Constant/None` mapped to mode 2, and `Disco` mapped to mode 3).

---

## 2. Next Steps / Future Work
- **Importable Template Split**:
  - Split `dolphin_ble.yaml` into a public importable ESPHome template (e.g. `dolphin_ble_core.yaml`) and a private local development overlay once the configuration is thoroughly validated.
