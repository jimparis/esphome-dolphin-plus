# ESPHome Dolphin Plus BLE

ESPHome external component and package for controlling a Maytronics Dolphin Plus
power supply over BLE.

This project runs on an ESP32 near the pool power supply. The ESP32 connects to
the power supply over BLE and exposes robot state, cycle timing, cleaning
controls, scheduling controls, manual drive, filter status, runtime counters,
and LED controls to Home Assistant through ESPHome.

This is not an official Maytronics integration. It has been tested with a
Dolphin Nautilus CC Pro.

## Current Status

The integration is built around one known Dolphin Plus BLE protocol family. It
currently supports:

- connection to the power supply BLE service by MAC address;
- status polling every 2 seconds;
- motor-unit telemetry polling every 30 seconds;
- start, stop, pickup, cleaning-mode selection, and filter reset;
- manual drive direction and speed;
- weekly schedule controls;
- configured cycle duration and derived remaining time;
- LED on/off, brightness, and effects;
- passive BLE advertisement relay to Home Assistant.

Temperature polling is disabled by default because this robot profile has not
returned a reliable temperature response.

## Repository Layout

- `components/dolphin_ble/`: the ESPHome external component.
- `packages/dolphin_ble.yaml`: reusable public ESPHome package. This contains
  the ESP32/BLE setup and all Dolphin entities, but no Wi-Fi, API, OTA, or
  external-component source.
- `dolphin_ble.import.yaml`: public dashboard-import entrypoint.
- `dolphin_ble.yaml`: ready-to-customize local YAML for flashing.
- `PROTOCOL.md`: protocol notes for the implemented packet behavior.
- `secrets.example.yaml`: example local secrets file.

## Quick Start

You need:

- an ESP32 board supported by ESPHome;
- ESPHome Dashboard or ESPHome CLI;
- the BLE MAC address of your Dolphin power supply;
- Wi-Fi credentials for the ESP32.

The BLE MAC can often be found on a sticker on the power supply. It can also be
found with a BLE scanner or Home Assistant's Bluetooth diagnostics.

1. Clone this repository:

   ```sh
   git clone https://github.com/jimparis/esphome-dolphin-plus.git
   cd esphome-dolphin-plus
   ```

2. Copy and edit the example secrets file:

   ```sh
   cp secrets.example.yaml secrets.yaml
   ```

   Set your Wi-Fi, API, OTA, and `dolphin_mac` values.

3. Flash `dolphin_ble.yaml` with ESPHome Dashboard or the ESPHome CLI:

   ```sh
   make flash
   ```

   The default serial port is `/dev/ttyUSB0`. To use another port:

   ```sh
   make flash PORT=/dev/ttyACM0
   ```

After the device boots on Wi-Fi, ESPHome Dashboard should offer to adopt/take
control of it. Adoption works because the firmware includes ESPHome `project`
metadata and `dashboard_import`.

For later updates over the network:

```sh
make ota
```

## Configuration Knobs

Use substitutions in `dolphin_ble.yaml`:

| Name | Required | Default | Description |
| --- | --- | --- | --- |
| `device_name` | No | `dolphin-ble` | ESPHome node name. |
| `friendly_name` | No | `Dolphin BLE` | Home Assistant display name. |
| `dolphin_mac` | Yes | none | BLE MAC address of the power supply. |
| `dolphin_temperature_supported` | No | `false` | Enable only after validating temperature support for your model. |

## Operational Notes

- Keep the ESP32 close enough to the power supply for a stable BLE connection.
- `Protocol Debug Logging` is exposed as a switch. Leave it off for normal use;
  enable it only when collecting packet-level diagnostics.
- Passive BLE advertisement relay is enabled. Home Assistant active GATT proxy
  connections through this ESP32 are disabled so they do not compete with the
  Dolphin BLE session.
- The component synchronizes the power supply RTC on connection and hourly
  thereafter using Home Assistant time.

## ESPHome Sharing Flow

This repository uses ESPHome's public sharing flow:

- `dashboard_import.package_import_url` points ESPHome Dashboard at this public
  package so it can create a minimal adopted config.
- `packages` pulls the reusable YAML from GitHub.
- `project` metadata is included for dashboard adoption.

References:

- https://esphome.io/guides/creators/
- https://esphome.io/components/packages/
- https://esphome.io/components/external_components/
