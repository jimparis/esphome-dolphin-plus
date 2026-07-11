# Agent Context

This repository contains an ESPHome integration for connecting to a Maytronics Dolphin Plus power supply over BLE.

Working assumptions:

- Target BLE peripheral:
  - Advertised service UUID: `fd5abba0-3935-11e5-85a6-0002a5d5c51b`
  - Manufacturer data: company/id `0x0600`, payload `d6b2f005f0f8`
- The ESP32 is expected on `/dev/ttyUSB0`, or flashed OTA
- Use `uv` for ESPHome Python tooling.

Important repo files:

- `dolphin_ble.yaml`: ESPHome config.
- `components/dolphin_ble/`: local ESPHome external component.
- `PROTOCOL.md`: BLE sequence, packet formats, and UUIDs.
- `Makefile`: common build, flash, and log commands.

Current implementation notes:

- `dolphin_ble.yaml` exposes structured read states, cleaning controls, and other controls through ESPHome.
- All polling coordination is handled natively in the C++ component: it sequentially requests static metadata on connection (PWS features, MU stats, SM config) and then transitions to periodic polling of status (2s) and MU telemetry (30s).
- Periodically synchronizes the power supply's real-time clock (RTC) using ESPHome's internal time component (SNTP) immediately upon connection and hourly thereafter.
- Manual drive is implemented as a direct `FFF7` command with direction and speed bytes. Steering commands are sent automatically whenever the direction select or speed number entities change in Home Assistant.
- TODO: split this into a public importable ESPHome template and a private local development overlay once the config stabilizes.

Commit hygiene:

- Commit source/docs/config changes as work progresses.
- Do not commit third-party binaries, extracted third-party output, ESPHome build output, virtual environments, or generated Python cache files.
- Keep committed protocol notes to resulting packet constants, observed BLE behavior, and project-owned implementation details.
