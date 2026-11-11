# JBD BLE → Victron Bridge 🚲⚡

[![CI](https://github.com/silly82/jbd-ble-victron-bridge-v2/actions/workflows/ci.yml/badge.svg)](https://github.com/silly82/jbd-ble-victron-bridge-v2/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-FF6600)](https://platformio.org/)
[![VenusOS](https://img.shields.io/badge/VenusOS-Cerbo%20GX-0047AB)](https://www.victronenergy.com/)
[![JBD](https://img.shields.io/badge/BMS-JBD%20%7C%20Jiabaida-008000)](#)

**[Deutsch (Schweizer Hochdeutsch)](README.de.md)**

> ℹ️ Successor of the archived repository [silly82/jbd-ble-victron-bridge](https://github.com/silly82/jbd-ble-victron-bridge).
>
> ⚠️ **Status (2026-11): firmware v1 does not compile.**
> `esp32_jbd_ble_mqtt/` (v1) fails with current NimBLE-Arduino and also had protocol bugs that would have prevented it from working.
> A corrected **v2** is in [`esp32_jbd_ble_mqtt_v2_untested/`](esp32_jbd_ble_mqtt_v2_untested/) — it compiles in CI but is **untested on real JBD hardware**.
> Test reports are very welcome as [issues](https://github.com/silly82/jbd-ble-victron-bridge-v2/issues). The VenusOS daemon and Node-RED flow are unchanged.

Display your **JBD/Jiabaida/Xiaoxiang BMS** battery data in **Victron VenusOS** via Bluetooth Low Energy — no expensive SmartShunt required.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  ┌──────────────┐   BLE    ┌──────────────────────┐   MQTT   ┌────────────┐│
│  │  JBD BMS     │◄────────►│  ESP32                │─────────►│ Cerbo GX   ││
│  │  (battery)   │          │  BLE → MQTT Bridge    │          │ (VenusOS)  ││
│  │  ~JBD-xxxx   │          │                      │          │            ││
│  └──────────────┘          └──────────────────────┘          │ ┌────────┐ ││
│                                                              │ │ DBus   │─►│─► D-Bus
│                                                              │ │ Driver │  ││
│                                                              │ └────────┘ ││
│                                                              │ ┌────────┐ ││
│                                                              │ │Node-RED│─►│─► Dashboard
│                                                              │ │(Debug)  │ ││
│                                                              │ └────────┘ ││
│                                                              └────────────┘│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Data flow:**
1. **ESP32** reads the JBD BMS via BLE (service 0xFF00, protocol reverse-engineered from [aiobmsble](https://github.com/patman15/aiobmsble))
2. **ESP32** publishes the values as JSON via **MQTT**
3. **Cerbo GX** runs a Python daemon that subscribes to MQTT and publishes to Victron's **D-Bus**
4. **Victron VenusOS** shows the battery as a native device — voltage, SoC, current, temperature included
5. **Node-RED** (optional) for dashboards, debugging, InfluxDB logging

## Features

- ✅ **JBD BLE protocol** — voltage, current, SoC, temperature, cell voltages
- ✅ **ESP32 with NimBLE** — stable and memory-efficient, better than stock BLE
- ✅ **VenusOS D-Bus** — battery appears like any Victron battery
- ✅ **MQTT** — flexible integration, no proprietary protocols
- ✅ **Node-RED flow** — debug and dashboard included
- ✅ **Watchdog** — detects BLE connection loss, sets "offline" state
- ✅ **CI** — GitHub Actions builds the v2 firmware for ESP32 / S3 / C3

## Quick Start

### 1. Flash the ESP32

Use the v2 firmware (untested, see status above):

```bash
cd esp32_jbd_ble_mqtt_v2_untested/
# edit WIFI_SSID, WIFI_PASS, MQTT_HOST at the top of the .ino
pio run -e esp32dev -t upload     # or esp32s3 / esp32c3
pio device monitor
```

> **No PlatformIO?** Arduino IDE → open `esp32_jbd_ble_mqtt_v2_untested/esp32_jbd_ble_mqtt_v2_untested.ino` → install **NimBLE-Arduino 2.x**, PubSubClient, ArduinoJson 7.x → Upload.
> Details and list of v1 bugs: [`esp32_jbd_ble_mqtt_v2_untested/README.md`](esp32_jbd_ble_mqtt_v2_untested/README.md)

### 2. Install the VenusOS Daemon

```bash
ssh root@<cerbo-ip>
cd /tmp
curl -sL https://github.com/silly82/jbd-ble-victron-bridge-v2/archive/main.tar.gz | tar xz
cd jbd-ble-victron-bridge-v2-main/venusos
./install.sh
```

**Or manually:**
```bash
scp -r venusos/* root@<cerbo-ip>:/data/dbus-mqtt-battery/
ssh root@<cerbo-ip>
/data/dbus-mqtt-battery/install.sh
```

### 3. Import Node-RED Flow (optional)

1. On Cerbo GX: VenusOS App Store → install **Node-RED**
2. Import `flows.json` → Menu → Import
3. Adjust MQTT broker config
4. Deploy

## Configuration

### ESP32 (`esp32_jbd_ble_mqtt_v2_untested.ino`)

```cpp
const char* WIFI_SSID     = "FRITZ!Box 7530";
const char* WIFI_PASS     = "secret";
const char* MQTT_HOST     = "192.168.1.100";     // Cerbo GX or broker
const int   MQTT_PORT     = 1883;
const char* JBD_DEVICE_NAME = "";                 // empty = auto-scan
```

> Leave `JBD_DEVICE_NAME` empty for auto-scan. The ESP will find devices named `JBD-*`, `DWF*`, `SX1*`, `SBL*`, etc.

### VenusOS Daemon (`venusos/dbus_mqtt_battery.service`)

```ini
Environment=MQTT_HOST=192.168.1.100
Environment=MQTT_TOPIC=bms/jbd/data
Environment=DBUS_INSTANCE=256
Environment=POLL_TIMEOUT=60
# Set to 1 if a Victron SmartShunt is present (see below)
Environment=SMARTSHUNT_MODE=0
```

## SmartShunt Coexistence

If a **Victron SmartShunt** is already installed, it should remain the primary battery monitor — its Coulomb-counting SoC is far more accurate than the JBD BMS voltage-based estimate.

Set `SMARTSHUNT_MODE=1` in the service unit. In this mode the daemon skips SoC, capacity and energy-accounting paths (those stay with the SmartShunt) and only publishes JBD-specific data that the SmartShunt cannot provide:

| What | SmartShunt | JBD BMS daemon (`SMARTSHUNT_MODE=1`) |
|------|-----------|--------------------------------------|
| SoC (Coulomb-counting) | ✅ primary | skipped |
| Voltage / Current / Power | ✅ | ✅ (cross-check) |
| Individual cell voltages | ❌ | ✅ `/Voltages/Cell1..N` |
| Cell voltage spread (Diff) | ❌ | ✅ `/Voltages/Diff` |
| Allow-to-charge/discharge | ❌ | ✅ `/Io/AllowToCharge` |
| BMS temperatures | ✅ (1 sensor) | ✅ (all BMS sensors) |
| Charge cycles | ✅ | ✅ |

**After enabling SmartShunt mode:**
1. Set `SMARTSHUNT_MODE=1` in `/data/dbus-mqtt-battery/dbus_mqtt_battery.service`
2. `systemctl restart dbus-mqtt-battery`
3. In VenusOS: **Settings → System Setup → Battery Monitor → SmartShunt**

## MQTT Data Format

Topic: `bms/jbd/data`

```json
{
  "voltage": 13.25,
  "current": 5.02,
  "power": 66.5,
  "soc": 78,
  "charge": 98.5,
  "capacity": 120,
  "cycles": 42,
  "temp_count": 2,
  "temperatures": [22.5, 23.1],
  "cells": 4,
  "cell_voltages": [3.312, 3.315, 3.308, 3.310],
  "charging": true,
  "chrg_mosfet": true,
  "dischrg_mosfet": true
}
```

## VenusOS / D-Bus Paths

| D-Bus Path | Description | Unit | SmartShunt mode |
|-----------|-------------|------|----------------|
| `/Dc/0/Voltage` | Battery voltage | V | ✅ always |
| `/Dc/0/Current` | Current (positive = charging) | A | ✅ always |
| `/Dc/0/Power` | Power | W | ✅ always |
| `/Dc/0/Temperature` | Temperature | °C | ✅ always |
| `/Soc` | State of charge | % | ⏭ skipped |
| `/Capacity` | Rated capacity | Ah | ⏭ skipped |
| `/ConsumedAmphours` | Consumed amphours | Ah | ⏭ skipped |
| `/History/DischargeCycles` | Charge cycles | # | ✅ always |
| `/Info/MaxChargeCurrent` | Max charge current | A | ⏭ skipped |
| `/Info/MaxDischargeCurrent` | Max discharge current | A | ⏭ skipped |
| `/Io/AllowToCharge` | BMS allows charging | 0/1 | ✅ always |
| `/Io/AllowToDischarge` | BMS allows discharging | 0/1 | ✅ always |
| `/Voltages/Cell1..N` | Individual cell voltages | V | ✅ always |
| `/Voltages/Sum` | Sum of all cell voltages | V | ✅ always |
| `/Voltages/Min` | Minimum cell voltage | V | ✅ always |
| `/Voltages/Max` | Maximum cell voltage | V | ✅ always |
| `/Voltages/Diff` | Cell voltage spread (max−min) | V | ✅ always |
| `/Connected` | Connection state | 0/1 | ✅ always |

## JBD BLE Protocol (Reverse Engineered)

Based on [aiobmsble](https://github.com/patman15/aiobmsble) by patman15.

| Parameter | Value |
|-----------|-------|
| Service UUID | `0000ff00-0000-1000-8000-00805f9b34fb` |
| RX Char (Notify) | `ff01` |
| TX Char (Write) | `ff02` |
| Init packet | none — not required (the v1 init sequence was wrong) |
| Command BasicInfo | `DD A5 03 00 <CRC16> 77` |
| Command Cells | `DD A5 04 00 <CRC16> 77` |
| Response | `DD <cmd> <status> <len> <data…> <CRC16> 77`, arrives in ~20-byte BLE fragments |
| CRC | `0x10000 − sum(status, len, data)` |
| Frame tail | `0x77` |

## Directory Structure

```
jbd-ble-victron-bridge-v2/
├── esp32_jbd_ble_mqtt_v2_untested/ ← ESP32 firmware v2 (NimBLE 2.x) — UNTESTED
│   ├── esp32_jbd_ble_mqtt_v2_untested.ino
│   ├── platformio.ini
│   └── README.md                  ← v1 bug list, test checklist
├── esp32_jbd_ble_mqtt/       ← ESP32 firmware v1 — DOES NOT COMPILE, kept for reference
├── venusos/                   ← Cerbo GX / VenusOS
│   ├── dbus_mqtt_battery.py      ← D-Bus daemon (Python)
│   ├── dbus_mqtt_battery.service ← systemd service unit
│   └── install.sh                ← Installation script
├── flows.json                 ← Node-RED flow (importable)
├── .github/workflows/         ← GitHub Actions CI
├── LICENSE                    ← MIT
├── README.md                  ← This file (English)
└── README.de.md               ← Deutsch (Schweizer Hochdeutsch)
```

## Troubleshooting

**ESP32 can't find the JBD BMS:**
- Close the BMS app on your phone (only one connection at a time)
- Leave `JBD_DEVICE_NAME` empty (auto-scan)
- Check RSSI: place ESP32 close to the battery
- Some clones use different service UUIDs — check if `ff00` is right

**No data on Cerbo:**
```bash
# Check MQTT
mosquitto_sub -h localhost -t bms/jbd/data

# Service status
systemctl status dbus-mqtt-battery

# Log
journalctl -u dbus-mqtt-battery -n 50 --no-pager
```

**Battery not showing in VenusOS:**
- Restart service: `systemctl restart dbus-mqtt-battery`
- Change instance ID if taken: `DBUS_INSTANCE=257`
- VenusOS Remote Console → Devices → Scan for new devices

## Why Not a Pure Node-RED Solution?

[`@victronenergy/node-red-contrib-victron`](https://flows.nodered.org/node/@victronenergy/node-red-contrib-victron) provides official Victron nodes for Node-RED — but they can only **read from or write to existing D-Bus services**. There is no node that can **register a new virtual battery device** on D-Bus.

| Task | Node-RED alone |
|------|---------------|
| Subscribe to MQTT from ESP32 | ✅ Yes |
| Dashboard / visualisation | ✅ Yes |
| InfluxDB / Grafana logging | ✅ Yes |
| Alerts and automations | ✅ Yes |
| Write to *existing* VenusOS services | ✅ Yes (via `victron-output` nodes) |
| **Create a new virtual battery in VenusOS** | ❌ No — requires Python + D-Bus |

The Python daemon (`dbus_mqtt_battery.py`) is the only way to register a new `com.victronenergy.battery.*` service so VenusOS recognises the JBD BMS as a native battery. Node-RED remains an optional addition for dashboards and logging, but it cannot replace the daemon.

## Related Projects

- [aiobmsble](https://github.com/patman15/aiobmsble) — Python BLE BMS library (our foundation)
- [BMS_BLE-HA](https://github.com/patman15/BMS_BLE-HA) — Home Assistant integration
- [dbus-serialbattery](https://github.com/mr-manuel/venus-os_dbus-serialbattery) — VenusOS serial battery driver
- [velib_python](https://github.com/victronenergy/velib_python) — Victron D-Bus Python bindings

## License

MIT — do what you want, no warranty. See [LICENSE](LICENSE).