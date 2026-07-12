# ESPHome Dolphin Plus BLE

ESPHome external component and package for controlling a Maytronics Dolphin Plus
power supply over BLE.

This project runs on an ESP32 near the pool power supply. The ESP32 connects to
the power supply over BLE and exposes robot state, cycle timing, cleaning
controls, scheduling controls, manual drive, filter status, runtime counters,
and LED controls to Home Assistant through ESPHome.

This is not an official Maytronics integration.

## Current Status

The integration is built around one known Dolphin Plus BLE protocol family. It
currently supports:

- connection to the power supply BLE service;
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
- `dolphin_ble.import.yaml`: public dashboard-import entrypoint intended to be
  hosted on GitHub.
- `dolphin_ble.yaml`: local development overlay. It uses local `components/`
  and private `secrets.yaml`.
- `PROTOCOL.md`: protocol notes for the implemented packet behavior.
- `secrets.example.yaml`: example local secrets file.

## Public GitHub URLs

The public import files currently assume this repository will be published at:

```text
https://github.com/jimsh/esphome-dolphin-plus
```

If the repository is published somewhere else, update these references before
publishing:

- `dashboard_import.package_import_url` in `dolphin_ble.import.yaml`
- `dashboard_import.package_import_url` in `dolphin_ble.yaml`
- `external_components.source` in `dolphin_ble.import.yaml`
- `packages.dolphin_ble.url` in `dolphin_ble.import.yaml`
- the README examples below

## Quick Start for Users

You need:

- an ESP32 board supported by ESPHome;
- ESPHome Dashboard or ESPHome CLI;
- the BLE MAC address of your Dolphin power supply;
- Wi-Fi credentials for the ESP32.

The BLE MAC can be found with a BLE scanner, ESPHome BLE tracker logs, or Home
Assistant's Bluetooth diagnostics. The optional `dolphin_name_filter` can be
left blank unless you want an extra advertised-name check.

### Option 1: Compile a Local YAML

Create a new ESPHome YAML and adjust the substitutions and network settings:

```yaml
substitutions:
  device_name: dolphin-ble
  friendly_name: Dolphin BLE
  dolphin_mac: "AA:BB:CC:DD:EE:FF"
  dolphin_name_filter: ""
  dolphin_temperature_supported: "false"

esphome:
  name: ${device_name}
  friendly_name: ${friendly_name}
  project:
    name: jimsh.dolphin_ble
    version: "0.1.0"

dashboard_import:
  package_import_url: github://jimsh/esphome-dolphin-plus/dolphin_ble.import.yaml@main
  import_full_config: false

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

external_components:
  - source: github://jimsh/esphome-dolphin-plus@main
    components:
      - dolphin_ble

packages:
  dolphin_ble:
    url: https://github.com/jimsh/esphome-dolphin-plus
    files:
      - packages/dolphin_ble.yaml
    ref: main
    refresh: 1d
```

Then compile and flash it from ESPHome Dashboard or the CLI.

After the device boots on Wi-Fi, ESPHome Dashboard should offer to adopt/take
control of it. Adoption works because the firmware includes `project` metadata
and `dashboard_import`.

### Option 2: Flash the Public Import YAML

`dolphin_ble.import.yaml` is the public dashboard-import entrypoint. It contains
fallback provisioning:

- Wi-Fi fallback AP with captive portal;
- Improv over serial;
- ESP32 Improv over BLE;
- API and OTA enabled without private credentials.

Before compiling it yourself, set:

```yaml
substitutions:
  dolphin_mac: "AA:BB:CC:DD:EE:FF"
  dolphin_name_filter: ""
```

If you flash it without Wi-Fi credentials, provision Wi-Fi through the fallback
AP or Improv, then adopt the device in ESPHome Dashboard and add your permanent
Wi-Fi/API/OTA settings there.

## Local Development

For development in this repository:

1. Copy the example secrets file:

   ```sh
   cp secrets.example.yaml secrets.yaml
   ```

2. Edit `secrets.yaml` with your Wi-Fi, API, OTA, and Dolphin BLE details.

3. Build:

   ```sh
   make build
   ```

4. Flash over USB:

   ```sh
   make flash
   ```

   Set another serial port with `make flash PORT=/dev/ttyACM0`.

5. Or run OTA after the first flash:

   ```sh
   uv run esphome upload dolphin_ble.yaml
   ```

The local `dolphin_ble.yaml` imports `packages/dolphin_ble.yaml` but uses
`external_components` from the checked-out `components/` directory, so local
source edits are compiled immediately.

## Configuration Knobs

Use substitutions in the top-level YAML:

| Name | Required | Default | Description |
| --- | --- | --- | --- |
| `device_name` | No | `dolphin-ble` | ESPHome node name. |
| `friendly_name` | No | `Dolphin BLE` | Home Assistant display name. |
| `dolphin_mac` | Yes | none | BLE MAC address of the power supply. |
| `dolphin_name_filter` | No | empty | Optional advertised-name filter. |
| `dolphin_temperature_supported` | No | `false` | Enable only after validating temperature support for your model. |

## Operational Notes

- Keep the ESP32 close enough to the power supply for a stable BLE connection.
- `Protocol Debug Logging` is exposed as a switch. Leave it off for normal use;
  enable it only when collecting packet-level diagnostics.
- Passive BLE advertisement relay is enabled. Home Assistant active GATT proxy
  connections through this ESP32 are disabled so they do not compete with the
  Dolphin BLE session.
- The component synchronizes the power supply RTC on connection and hourly
  thereafter using ESPHome time.

## References

This repository uses ESPHome's public sharing flow:

- `dashboard_import.package_import_url` points ESPHome Dashboard at a public
  package so it can create a minimal adopted config.
- `packages` pulls the reusable YAML from GitHub.
- `project` metadata is required for dashboard adoption.

See ESPHome's documentation for sharing devices and packages:

- https://esphome.io/guides/creators/
- https://esphome.io/components/packages/
- https://esphome.io/components/external_components/
