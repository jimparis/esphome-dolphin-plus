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

See https://github.com/jimparis/esphome-dolphin-plus/ for more details.

# Installation

You can use the button below to install the pre-built firmware directly to your device via USB from the browser.

<esp-web-install-button manifest="firmware/dolphin_ble.manifest.json"></esp-web-install-button>

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
