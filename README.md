# ESPHome Dolphin Plus BLE

ESPHome external component and package for controlling a Maytronics Dolphin Plus
power supply over BLE.

This project runs on an ESP32 near the pool power supply. The ESP32 connects to
the power supply over BLE and exposes robot state, cycle timing, cleaning
controls, scheduling controls, manual drive, filter status, runtime counters,
LED controls, and the resolved power-supply MAC address to Home Assistant
through ESPHome.

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
- `dolphin_ble.yaml`: ready-to-customize YAML for flashing.
- `PROTOCOL.md`: protocol notes for the implemented packet behavior.
- `secrets.example.yaml`: example local secrets file.

## Quick Start

You need:

- an ESP32 board supported by ESPHome;
- ESPHome Device Builder running
- Wi-Fi credentials for the ESP32.

The BLE MAC is optional. By default, the component auto-discovers a nearby
Maytronics power supply that advertises the Dolphin BLE service and uses a known
Maytronics address range. If multiple matching power supplies are nearby, set
`dolphin_mac` explicitly. The BLE MAC can often be found on a sticker on the
power supply. It can also be found with a BLE scanner or Home Assistant's
Bluetooth diagnostics.

### Option 1: Web installer

TODO

### Option 2: ESPHome Device Builder or CLI

Use the `dolphin_ble.yaml` in this repo to either flash via CLI (`esphome run dolphin_ble.yaml`), or import the YAML into the "Create Device" dialog of ESPHome Device Builder.  You probably will need to manually add Wifi credentials to the file at minimum.

## Configuration Knobs

Use substitutions in `dolphin_ble.yaml`:

| Name | Required | Default | Description |
| --- | --- | --- | --- |
| `device_name` | No | `dolphin-ble` | ESPHome node name. |
| `friendly_name` | No | `Dolphin BLE` | Home Assistant display name. |
| `dolphin_mac` | No | empty | Optional BLE MAC address of the power supply. Leave empty for auto-discovery by Dolphin service UUID and Maytronics address range. |

## Operational Notes

- Keep the ESP32 close enough to the power supply for a stable BLE connection.
- Auto-discovery matches the advertised Dolphin service UUID plus these
  Maytronics address ranges: `30:09:F9:70:00:00/28`,
  `70:B3:D5:90:E0:00/36`, and `AC:74:C4/24`.
- `Protocol Debug Logging` is exposed as a switch. Leave it off for normal use;
  enable it only when collecting packet-level diagnostics.
- Passive BLE advertisement relay is enabled. Home Assistant active GATT proxy
  connections through this ESP32 are disabled so they do not compete with the
  Dolphin BLE session.
- The component synchronizes the power supply RTC on connection and hourly
  thereafter using the first available time source: Home Assistant time or SNTP.
