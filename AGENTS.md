# Agent Context

This repository is a local integration development and ESPHome prototype for connecting to a Maytronics Dolphin Plus power supply over BLE.

Working assumptions:

- Target BLE peripheral:
  - MAC: `AC:74:C4:0E:D8:6E`
  - Name: `Z4868YMR`
  - Advertised service UUID: `fd5abba0-3935-11e5-85a6-0002a5d5c51b`
  - Manufacturer data: company/id `0x0600`, payload `d6b2f005f0f8`
- The ESP32 is expected on `/dev/ttyUSB0` unless overridden with `make PORT=/dev/...`.
- Use `uv` for ESPHome Python tooling.

Important repo files:

- `dolphin_ble.yaml`: ESPHome config.
- `components/dolphin_ble/`: local ESPHome external component.
- `PROTOCOL.md`: documented BLE sequence and UUIDs.
- `Makefile`: common build, flash, and log commands.

Current implementation notes:

- `dolphin_ble.yaml` exposes read state, cleaning controls, and manual drive controls through ESPHome.
- The bridge sends robot commands as text notifications containing framed hex packets.
- Manual drive is implemented as a direct `FFF7` command with direction and speed bytes.

Commit hygiene:

- Commit source/docs/config changes as work progresses.
- Do not commit third-party binaries, extracted third-party output, ESPHome build output, virtual environments, or generated Python cache files.
- Keep committed protocol notes to resulting packet constants, observed BLE behavior, and project-owned implementation details.
