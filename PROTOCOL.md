# Maytronics Dolphin Plus BLE Protocol

This document describes the Bluetooth Low Energy (BLE) protocol used by the Maytronics Dolphin Plus robot power supply, including packet formats, commands, responses, and observed device behavior.

There are two protocol families:
1. **IOT Protocol Family**: Used by modern Dolphin Plus power supplies (including the unit advertiser name `Z4868YMR`). This is the primary protocol.
2. **UART/POP Protocol Family**: A secondary protocol used by older or alternative controller variants.

---

## 1. Connection Lifecycles & State Tracking

Connecting to a Maytronics Dolphin Plus power supply over BLE requires a specific dual-role GATT setup. A generic GATT client connection will connect at the BLE layer but fails to communicate because the robot expects to write notifications back to the client.

### Connection Sequence

To establish a functional session with the IOT protocol:
1. **GATT Server Setup (Host side)**:
   - Start a local GATT server on the connecting device (e.g. ESP32).
   - Advertise/expose the service: `fd5abba0-3935-11e5-85a6-0002a5d5c51b`.
   - Add a local notify characteristic: `fd5abba1-3935-11e5-85a6-0002a5d5c51b` (with Notify properties).
   - Add a Client Characteristic Configuration Descriptor (CCCD) `00002902-0000-1000-8000-00805f9b34fb`.
2. **Connect to Robot (Client side)**:
   - Scan for the peripheral advertising `fd5abba0-3935-11e5-85a6-0002a5d5c51b` (or matching MAC `AC:74:C4:0E:D8:6E`).
   - Connect as a GATT Client to the robot.
   - Negotiate a high MTU (typically `512` bytes).
3. **Handshake & Subscription Exchange**:
   - Discover services on the robot.
   - Locate the remote service `fd5abba0-3935-11e5-85a6-0002a5d5c51b` and characteristic `fd5abba1-3935-11e5-85a6-0002a5d5c51b`.
   - Enable notifications on the robot's remote characteristic by writing `01 00` to its remote CCCD.
   - The robot will simultaneously connect back to the host's local GATT Server and write `01 00` to the host's local CCCD to subscribe to outbound commands.
4. **Session Readiness**:
   - The connection is fully active once both notify subscriptions are acknowledged and MTU negotiation is successful.

### Keep-Alive & Status Polling
To track status optimally and ensure the connection doesn't time out, the host should:
- Periodically poll the `system_status` command.
- If features indicate support, optionally poll the `temperature` command.
- Handle notification fragment reassembly. Responses are sent as ASCII hex strings prefixed with `:` and terminated by a newline/null byte. Because long payloads (like 256-byte MU/SM dumps) exceed single packet sizes, they are sent as fragmented notifications. The host must buffer and concatenate these fragments until the total payload matches the parsed `data_length` field + envelope overhead, then validate the checksum.

---

## 2. IOT Packet Framing (Outbound & Inbound)

The IOT protocol wraps command frames in an ASCII text-based envelope. 

### ASCII Envelope
- **Outbound (Host to Robot)**: Formatted as ASCII string `03:<lowercase_frame_hex>` (e.g. `03:ab03fff806000002ab`).
- **Inbound (Robot to Host)**: Received as notifications formatted as `:<lowercase_frame_hex>`. Long packets are split across multiple notifications and must be reassembled.

### Low-Level Frame Layout
The hexadecimal payload inside the envelope has the following layout:

| Byte Offset | Field Name | Size (Bytes) | Description / Value |
| :--- | :--- | :---: | :--- |
| **0** | SOP Preamble | 1 | Start of Packet. Always `ab`. |
| **1** | Source (SRC) | 1 | Source identifier. Always `03` for the host. |
| **2 - 3** | Destination | 2 | Target component (Big-Endian). E.g. `FFF8` (System), `FFF7` (Drive/LEDs), `FFFD` (Memory blocks). |
| **4** | Opcode | 1 | Command identifier. |
| **5 - 6** | Data Length | 2 | Payload size in bytes (Big-Endian). |
| **7 ... (N-3)** | Payload Data | Variable | Command parameters or payload bytes. |
| **(N-2) - (N-1)** | Checksum | 2 | Big-Endian sum of every preceding byte in the frame (excluding the checksum field itself). |

---

## 3. Protocol State Definitions (Enums)

### Power Supply State (`sm_state` / `PwsState`)
Represents the state of the PWS (Power Supply).

| Value | State Name | Description |
| :---: | :--- | :--- |
| **0** | `on` | Power supply is on / active. |
| **1** | `on` | Power supply is turned on. |
| **2** | `holdWeekly` | Weekly program is set and waiting. |
| **3** | `holdDelay` | Start delay timer is active. |
| **4** | `programming` | Controller is in programming mode. |
| **5** | `onCleanMode` | Currently executing a cleaning cycle. |
| **6** | `sleep` | Power supply is in sleep / standby mode. |
| **7** | `off` | Power supply is off / standby. |
| **-1 / Other**| `Unknown` | Unknown state. |

### Robot State (`mu_state` / `RobotState`)
Represents the state of the MU (Motor Unit / Robot).

| Value | State Name | Description |
| :---: | :--- | :--- |
| **0** | `init` | Initializing. |
| **1** | `mapping` | Mapping pool layout. |
| **2** | `scanning` | Scanning / Cleaning. |
| **3** | `recovery` | Moving to pool side for retrieval / pickup. |
| **4** | `finished` | Cleaning cycle completed successfully. |
| **5** | `programming` | Programming mode. |
| **6** | `fault` | Fault detected. Error code is active. |
| **7** | `notConnected` | Communication between PWS and Robot is lost. |
| **-1 / Other**| `Unknown` | Unknown state. |

### Cleaning Mode (`CleanMode`)
Represents the chosen cleaning program.

| Value (Byte) | Program Name | Name in Shadow / UI | Description |
| :---: | :--- | :---: | :--- |
| **1** | `AllSurfaces` / `Regular` | all / regular | Regular pool cleaning (floor, walls, waterline). |
| **2** | `Fast` | short | Shortened duration, floor only. |
| **3** | `Cove` | cove | Cove cleaning. |
| **4** | `FloorOnly` | floor | Floor only cleaning. |
| **5** | `WaterLine` | water | Waterline scrubbing focus. |
| **6** | `UltraClean` | ultra | Extra thorough cleaning at slower speeds. |
| **7** | `Spot` | spot | Spot clean targeted area. |
| **8** | `WallOnly` | wall | Wall scrubbing focus. |
| **9** | `TicTac` | tictac | Specialized movement program. |
| **10** | `Custom` | custom | User-customized parameters. |
| **11** | `PickUp` | pickup | Navigates to pool side and climbs wall for retrieval. |
| **-2** | `Empty` | empty | No program set. |
| **-1 / Other**| `Unknown` | unknown | Unrecognized mode. |

### Filter Canister Blockage Status (`filter_state` / `FilterStatus`)
Represents the level of blockage or debris in the filter canister. Devices use one of three resolution models depending on available features (Advanced, Basic, or Full/Not Full).

#### Advanced FBI Mapping (Variable Byte Value `0` - `102`):
- **`0`**: `Empty` ("empty") - Filter is completely clean.
- **`1 - 25`**: `PartiallyFull` ("partially_full") - Low debris.
- **`26 - 74`**: `GettingFull` ("getting_full") - Medium debris.
- **`75 - 99`**: `AlmostFull` ("almost_full") - High debris, recommend cleaning soon.
- **`100`**: `Full` ("full") - Filter is full, airflow restricted.
- **`101`**: `Fault` ("fault") - Filter block sensor fault.
- **`102`**: `NotAvailable` ("not_available") - Filter status sensor offline.
- **Other**: `Unknown` ("unknown").

#### Basic FBI Mapping:
- **`0`**: `Empty`
- **`100`**: `Full`
- **`101`**: `Fault`
- **`102`**: `NotAvailable`

#### Full / Not Full FBI Mapping:
- **`100`**: `Full`
- **`101`**: `Fault`
- **`102`**: `NotAvailable`
- **Other**: `Not Full` (visually displayed as clean).

---

## 4. Primary Control Commands

These commands are sent via GATT server notification using the SOP framing.

### Start Cleaning Cycle
- **Opcode**: `06`
- **Destination**: `FFF8`
- **Payload**: None
- **Raw Hex**: `ab 03 ff f8 06 00 00 02 ab`
- **Envelope Command**: `03:ab03fff806000002ab`

### Stop Cleaning Cycle / Shutdown
- **Opcode**: `05`
- **Destination**: `FFF8`
- **Payload**: None
- **Raw Hex**: `ab 03 ff f8 05 00 00 02 aa`
- **Envelope Command**: `03:ab03fff805000002aa`

### Select Cleaning Mode
- **Opcode**: `03`
- **Destination**: `FFE9`
- **Payload**: 1 byte (corresponding to `CleanMode` value, e.g. `02` for Fast Mode)
- **Envelope Command**: `03:ab03ffe9030001[clean_mode_byte][checksum]`

### Remote Control / Manual Drive
Direct steering controls. Active RC mode is initiated by driving and exited when quitting.
- **Manual Drive Steering**:
  - **Opcode**: `03`
  - **Destination**: `FFF7`
  - **Payload**: 2 bytes:
    - Byte 0: Direction (`01` = Stop, `02` = Forward, `03` = Backward, `04` = Right, `05` = Left)
    - Byte 1: Speed percentage (`0` - `100` / `00` - `64` hex)
  - **Envelope Command**: `03:ab03fff7030002[direction_byte][speed_byte][checksum]`
- **Quit RC Mode**:
  - **Opcode**: `04`
  - **Destination**: `FFF7`
  - **Payload**: None
  - **Envelope Command**: `03:ab03fff704000002a9`

### LED Controls
Controls power supply status light patterns and intensity. Note that RGB color changes are NOT supported; the payload controls preset patterns only.
- **Opcode**: `10`
- **Destination**: `FFF7`
- **Payload**: 3 bytes:
  - Byte 0: Enabled (`01` = Enabled, `00` = Disabled)
  - Byte 1: Intensity percentage (`0` - `100`)
  - Byte 2: Pattern Mode (`01` = Blinking, `02` = Constant, `03` = Disco)
- **Envelope Command**: `03:ab03fff7100003[enabled_byte][intensity_byte][mode_byte][checksum]`

### Reset Filter Indicator
Clears the "filter bag full" alert light.
- **Opcode**: `0a`
- **Destination**: `FFF7`
- **Payload**: None
- **Envelope Command**: `03:ab03fff70a000002af`

### Set Start Delay Timer
Configures a delay before the next cleaning run begins.
- **Opcode**: `46`
- **Destination**: `FFF9`
- **Payload**: 6 bytes:
  - Byte 0: Delay Trigger Type (integer)
  - Byte 1: Fixed padding byte (`FF`)
  - Byte 2: Enabled (`01` = Enabled, `00` = Disabled)
  - Byte 3: Run Hour (0-23)
  - Byte 4: Run Minute (0-59)
  - Byte 5: Cleaning Mode (`CleanMode` value)
- **Envelope Command**: `03:ab03fff9460006[trigger][ff][enabled][hour][minute][mode][checksum]`

### Set Weekly Schedule
Configures the schedule for Monday through Sunday.
- **Opcode**: `45`
- **Destination**: `FFF9`
- **Payload**: 37 bytes:
  - Byte 0: Repeat Schedule (`00` = Repeat weekly, `01` = Run once and disable)
  - Byte 1: Trigger Source (integer)
  - Bytes 2 - 36: 7 blocks of 5 bytes (one for each day Monday to Sunday). Each block is structured as:
    - Byte 0: Day Index / ID (`1` to `7`)
    - Byte 1: Enabled (`01` = Active, `00` = Off)
    - Byte 2: Hour (0-23)
    - Byte 3: Minute (0-59)
    - Byte 4: Cleaning Mode (`CleanMode` value)
- **Envelope Command**: `03:ab03fff9450025[repeat][trigger][7x5_daily_blocks][checksum]`

### Set Single Day Weekly Program
Configures the schedule for a single day of the week.
- **Opcode**: `47`
- **Destination**: `FFF9`
- **Payload**: 7 bytes:
  - Byte 0: Repeat Schedule (`00` = Repeat weekly, `01` = Run once and disable)
  - Byte 1: Trigger Source (integer)
  - Byte 2: Day Index / ID (`1` to `7`, representing Monday to Sunday)
  - Byte 3: Enabled (`01` = Active, `00` = Off)
  - Byte 4: Hour (0-23, or -1/`FF` if disabled)
  - Byte 5: Minute (0-59, or -1/`FF` if disabled)
  - Byte 6: Cleaning Mode (`CleanMode` value)
- **Envelope Command**: `03:ab03fff9470007[repeat][trigger][day][enabled][hour][minute][mode][checksum]`

---

## 5. Wi-Fi Provisioning & Connection Diagnostics

The Dolphin Plus controller exposes local network management and setup APIs over BLE.

### WiFi Security Protocols (`SecurityType`)
Used to specify the network security type during Wi-Fi setup.

- **`0`**: `INVALID` / `NOT_USED`
- **`1`**: `OPEN` (No password required)
- **`2`**: `WPA_WPA2_Personal` (Standard WPA/WPA2 password-protected home network)
- **`3`**: `WEP`
- **`4`**: `WPA_WPA2_Enterprise`

### Request Scan Results (Get WiFi List)
Triggers a local AP scan by the robot and returns discovered networks.
- **Opcode**: `c6`
- **Destination**: `FFFE`
- **Response Stride Format**: contiguous sequence of variable-length network blocks:
  - Byte 0: `N` (SSID string length)
  - Bytes 1 to N: `SSID` (ASCII string, e.g. empty for hidden networks)
  - Byte N+1: `RSSI` (1-byte signed integer signal strength)
  - Byte N+2: `Security Type` (1-byte ordinal corresponding to `SecurityType`)
  - Byte N+3: `Is Connected` (1-byte boolean flag; non-zero if the PWS is currently connected to this network)
  - *Next block starts immediately at offset `N+4`*

### Connect to Wi-Fi Network
Configures SSID credentials and password.
- **Opcode**: `cd`
- **Destination**: `FFFE`
- **Payload**: variable length depending on SSID and password:
  - Byte 0: SSID length `S` (1 byte)
  - Bytes 1 to S: SSID string (ASCII)
  - Byte S+1: Password length `P` (1 byte)
  - Bytes S+2 to S+1+P: Password string (ASCII)
  - Byte S+2+P: Security Type (1-byte corresponding to `SecurityType`)
- **Envelope Command**: `03:ab03fffe[payload_length_hex][payload_hex][checksum]`

### Query Connection Status Diagnostics
Checks status and reports troubleshooting errors during Wi-Fi/Cloud association.
- **Opcode**: `de`
- **Destination**: `FFFE`
- **Response**: The PWS responds with an acknowledgment text frame. If connection fails, the string payload indicates the failure category:
  - `"rc_crc_error"` or `"cant_connect_to_network"`: Incorrect password (`INCORRECT_WIFI_PASSWORD`).
  - `"no_wifi_signal"`: Router out of range or turned off (`NO_WIFI_SIGNAL`).
  - `"pws_ip_address"`: IP addressing conflict or DHCP failure (`WRONG_IP_ADDRESS`).
  - `"cant_connect_to_cloud"`: Associated with Wi-Fi router but cannot establish a TLS connection to Maytronics AWS servers (`WIFI_NOT_CONNECTED` / `CANT_CONNECT_TO_CLOUD`).
  - No error string / generic ACK: Connected successfully (`CONNECTED`).

---

## 6. Telemetry & Sensor Parameters Data Blocks

Device parameters, diagnostics, and sensors are requested as structured blocks.

### Power Supply Features (`pws_features`)
- **Opcode**: `1a`, **Destination**: `FFFA`, **Request Payload**: None
- **Envelope Request**: `03:ab03fffa1a000002c1`
- **Response Layout (3 Bytes payload data)**:
  Parsed as a reversed bit string from the payload hex array. For a standard 3-byte payload, the feature bits are mapped on the last byte `payload[2]`:
  - **Bit 0**: `networkSensing` (Wi-Fi connectivity support).
  - **Bit 1**: `inWat` (In-Water sensor / capability).
  - **Bit 2**: `cellular` (Cellular modem support).
  - **Bit 3**: `OTA` (Over-the-Air update support).
  - **Bit 4**: `PSC` (PCS support).

### PWS Configuration Block (`get_sm_data`)
- **Opcode**: `02`, **Destination**: `FFFD`, **Request Payload**: `02 00 ff`
- **Envelope Request**: `03:ab03fffd0200030200ff03b0`
- **Response Layout (256 Bytes parsed dynamically)**:
  - The first raw response-payload byte is an ACK; the offsets below refer to the remaining data bytes. In raw ESPHome frames, add 1 to every offset below.
  - **Bytes 38 - 41**: PWS Software Version.
    - Byte 38: Major version (as unsigned int).
    - Byte 39: Minor version (as unsigned int).
    - Bytes 40-41: Patch version (2 bytes, Short).
  - **Bytes 63 - 64**: Timezone offset (2 bytes, Short).
  - **Byte 65**: Quick Features (1-byte Bit-mask).
    - Bit 0: Weekly Timer Every Day supported.
    - Bit 1: Weekly Timer Every 2 Days supported.
    - Bit 2: Weekly Timer Every 3 Days supported.
    - Bit 3: Start Delay Timer supported.
    - Bit 4: Filter Status Indication LED supported.
    - Bit 5: Floor Only cleaning mode supported.
    - Bit 6: Short/Fast cleaning mode supported.
    - Bit 7: Pickup Mode supported.
  - **Byte 72**: Weekly Schedule Repeat Flag (`0` = Repeat weekly, non-zero = Run once).
  - **Bytes 73 - 107**: Weekly Schedule program (7 days × 5 bytes each; matches `set_weekly_schedule` format).
  - **Bytes 108 - 113**: Start Delay Settings.
    - Byte 108: Trigger type.
    - Byte 109: Padding.
    - Byte 110: Enabled flag (`1` = Active, `0` = Off).
    - Byte 111: Hour.
    - Byte 112: Minute.
    - Byte 113: Cleaning mode.
  - **Bytes 118 - 150**: Wi-Fi SSID (33 bytes US-ASCII, null-trimmed).
  - **Bytes 213**: Timer Trigger Source.
  - **Bytes 217 - 236**: Configured Cleaning Mode Cycle Times. 10 modes, 2 bytes per mode (Big-Endian minutes):
    - 217-218: All Surfaces (Regular)
    - 219-220: Fast
    - 221-222: Cove
    - 223-224: Floor Only
    - 225-226: Waterline
    - 227-228: Ultra Clean
    - 229-230: Spot Clean
    - 231-232: Wall Only
    - 233-234: TicTac
    - 235-236: Custom

### Motor Unit Status Block (`get_mu_data`)
- **Opcode**: `01`, **Destination**: `FFFD`, **Request Payload**: `01 00 ff`
- **Envelope Request**: `03:ab03fffd0100030100ff03ae`
- **Response Layout (256 Bytes parsed dynamically)**:
  - The first raw response-payload byte is an ACK; the offsets below refer to the remaining data bytes. In raw ESPHome frames, add 1 to every offset below.
  - **Bytes 0 - 119**: Fault History Records. Contains 10 historical fault blocks of 12 bytes each, ordered chronologically (newest first if flipped):
    - Byte 0: Fault Error Code.
    - Bytes 1-2: PCB Operating Hours at fault (2 bytes, Little-Endian Short).
    - Byte 3: PCB Operating Minutes at fault.
    - Bytes 4-5: Turn-On Count at fault (2-byte Little-Endian Short).
    - Bytes 6-7: Sensor/Diagnostic Value 1.
    - Bytes 8-9: Sensor/Diagnostic Value 2.
    - Bytes 10-11: Sensor/Diagnostic Value 3.
  - **Bytes 132 - 133**: Robot Type ID (2 bytes, Short).
  - **Bytes 134 - 137**: Flash Write Counter (4 bytes, Int).
  - **Bytes 138 - 139**: Configured Cycle Time (2 bytes, Short).
  - **Bytes 140 - 141**: PCB Runtime Hours (2-byte Little-Endian Short).
  - **Byte 142**: PCB Runtime Minutes (1 byte).
  - **Bytes 143 - 144**: Impeller Runtime Hours (2-byte Little-Endian Short).
  - **Byte 145**: Impeller Runtime Minutes (1 byte).
  - **Bytes 146 - 147**: Turn-On Counter (2-byte Little-Endian Short).
  - **Bytes 148 - 149**: Not Completed Cycle Counter (2-byte Little-Endian Short).
  - **Byte 152**: Robot Software Version Major.
  - **Bytes 153 - 154**: Robot Software Version Minor.
  - **Byte 155**: Packed active LED configuration: bits 3-7 are intensity in 5% increments; bit 0 is Disco; bit 1 is Constant; neither mode bit is Blinking. The LED is enabled when decoded intensity is nonzero.
  - **Byte 167**: Current Clean Mode.
  - **Byte 170**: Wall climbing period config.

### Periodic Status Update (`system_status`)
- **Opcode**: `07`, **Destination**: `FFF8`, **Request Payload**: None
- **Envelope Request**: `03:ab03fff807000002ac`
- **Response Layout (53 Bytes)**:
  - Raw payload byte 0 is the response ACK. The following offsets begin at data byte 0 after that ACK.
  - **Raw byte 1** (data byte 0): `mu_state` (`RobotState` value).
  - **Raw byte 2** (data byte 1): `sm_state` (`PwsState` value).
  - **Raw byte 3** (data byte 2): `filter_state` (Filter bag clog byte).
  - **Raw byte 4** (data byte 3): `cleaning_mode` (Active `CleanMode` value).
  - **Raw bytes 5 - 14** (data bytes 4 - 13): Active Cleaning Cycle Progress.
    - Data bytes 4-5: `cycleTime`/cycle-type field, 16-bit big-endian. On the observed PWS this changes as `0x0100`, `0x0200`, etc. with the selected cleaning mode and must not be treated as minutes.
    - Data bytes 6-9: Monotonic PWS start uptime in seconds (`cycleStartTime`), 32-bit big-endian.
    - Data bytes 10-13: UTC cycle start time Unix timestamp in seconds (`cycleStartTimeUTC`), 32-bit big-endian.
  - **Raw byte 15** (data byte 14): `is_smart` feature flag (Boolean, `00` or `01`).
  - **Raw bytes 16 - 18** (data bytes 15 - 17): Next scheduled cleaning cycle.
    - Byte 15: Cleaning Mode.
    - Bytes 16-17: Delay/Time to next run in minutes (2-byte Short).
  - **Raw bytes 19 - 30** (data bytes 18 - 29): Currently active fault/error blocks (12 bytes).
  - **Raw bytes 31 - 52** (data bytes 30 - 51): Cleaning modes estimate matrix table.
    - Contains 11 entries of 16-bit big-endian minute values, one per cleaning mode. This is the source for the configured duration estimate: entry `cleaning_mode - 1`.

### Data offset conventions

The response layouts use zero-based, half-open ranges where applicable. A range from 63 through 65 therefore contains bytes 63 and 64. For the MU response, multi-byte runtime and counter fields are little-endian.

### Temperature & In-Water Sensor (`temperature`)
The compact PWS feature reply may advertise in-water support, but that alone is not sufficient to enable this request: on the reference unit it has not produced a valid response and coincides with malformed short-ACL traffic. Keep polling disabled until a valid response is captured without disrupting status traffic.
- **Opcode**: `09`, **Destination**: `FFF8`, **Request Payload**: None
- **Response Layout (10 Bytes)**:
  - The first raw response-payload byte is an ACK; the data offsets below begin at raw payload byte 1.
  - **Byte 0**: In-Water Status (`00` = Out of water, `01` = In water, `02` = Unknown, `03` = Error, `04` = No Baro/Calibrate, `0f` = Loading).
  - **Bytes 1 - 2**: Water Temperature (16-bit signed Big-Endian Int in Celsius, scaled by 10, e.g. `00 f5` = 24.5°C). Special values `0xFFFF` (`65535`), `0x03E9` (`1001`), and `0x03EA` (`1002`) indicate unavailable or reading-failed states.
  - **Byte 3**: Measuring During Cycle active flag (`01` = Active).
  - **Byte 4**: Measurement Health/Status code.
  - **Bytes 5 - 9**: Epoch Timestamp of the last measurement (5 bytes).

### PWS Features (`pws_features`)

- **Opcode**: `1A`, **Destination**: `FFFA`, **Request Payload**: None
- **Response Layout (3 Bytes)**: This compact reply does not follow the structured-response ACK/data convention. The feature bitfield is response payload byte 2.
  - Bit 0: Network sensing
  - Bit 1: In-water sensing
  - Bit 2: Cellular
  - Bit 3: OTA
  - Bit 4: PCS

---

## 7. UART / POP Protocol Family (Alternative)

This protocol uses two separate BLE characteristics (a write-only and a notify-only) resembling a standard serial bridge.

### Service UUIDs
- **Service**: `fd5abca0-3935-11e5-85a6-0002a5d5c51b`
- **Write Characteristic**: `fd5abca1-3935-11e5-85a6-0002a5d5c51b`
- **Notify Characteristic**: `fd5abca2-3935-11e5-85a6-0002a5d5c51b`

### Frame Format
Flipped byte fields are transported as Little-Endian.

| Byte Offset | Field Name | Size (Bytes) | Description / Value |
| :--- | :--- | :---: | :--- |
| **0 - 3** | SOP Preamble | 4 | Start of Packet. Always `4A 4B 4C 4D` (hex for "JKLM"). |
| **4** | Destination | 1 | Destination identifier (usually `6C` for SM). |
| **5** | Source (SRC) | 1 | Source identifier. Always `67` for the host. |
| **6 - 7** | Reserve Field | 2 | Fixed padding. Always `0A 0A`. |
| **8 - 9** | Opcode | 2 | Flipped command code (e.g. `0013` = `13 00` on wire). |
| **10 - 11** | Data Length | 2 | Flipped payload size in bytes. |
| **12 ... (N-5)** | Payload Data | Variable | Command parameters. |
| **(N-4) - (N-1)** | Checksum | 4 | Flipped 32-bit CRC checksum of frame bytes. |

### Known POP Commands
- `get_sm_data` (Opcode `0013`): Returns a 36-byte device info packet containing serial number (bytes 1-20), software version major/minor (bytes 21-24), PCB operating runtime (bytes 30-33), and cycle counter (bytes 34-35).
- `get_mu_data` (Opcode `001A`): Returns 256-byte motor unit calibration, hours, gyroscope counters, and runtime states.
