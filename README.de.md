# JBD BLE → Victron Bridge 🚲⚡

[![CI](https://github.com/silly82/jbd-ble-victron-bridge-v2/actions/workflows/ci.yml/badge.svg)](https://github.com/silly82/jbd-ble-victron-bridge-v2/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/Licence-MIT-yellow.svg)](LICENSE)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-FF6600)](https://platformio.org/)
[![VenusOS](https://img.shields.io/badge/VenusOS-Cerbo%20GX-0047AB)](https://www.victronenergy.com/)
[![JBD](https://img.shields.io/badge/BMS-JBD%20%7C%20Jiabaida-008000)](#)

**[English version](README.md)**

> ℹ️ Nachfolger des archivierten Repositorys [silly82/jbd-ble-victron-bridge](https://github.com/silly82/jbd-ble-victron-bridge).
>
> ⚠️ **Status (2026-11): Firmware v1 kompiliert nicht.**
> `esp32_jbd_ble_mqtt/` (v1) scheitert mit aktuellem NimBLE-Arduino und hatte zusätzlich Protokollfehler, mit denen sie auch nicht funktioniert hätte.
> Eine korrigierte **v2** liegt in [`esp32_jbd_ble_mqtt_v2_untested/`](esp32_jbd_ble_mqtt_v2_untested/) — sie kompiliert in der CI, ist aber **an echter JBD-Hardware ungetestet**.
> Testberichte bitte als [Issue](https://github.com/silly82/jbd-ble-victron-bridge-v2/issues). VenusOS-Daemon und Node-RED-Flow sind unverändert.


Zeigen Sie Ihren **JBD/Jiabaida/Xiaoxiang BMS**-Akku im **Victron VenusOS** an — ohne teuren SmartShunt. Der ESP32 liest den Akku via Bluetooth aus und sendet die Werte per MQTT an den Cerbo GX.

## Architektur

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  ┌──────────────┐   BLE    ┌──────────────────────┐   MQTT   ┌────────────┐│
│  │  JBD BMS     │◄────────►│  ESP32                │─────────►│ Cerbo GX   ││
│  │  (Akku)      │          │  BLE → MQTT Bridge    │          │ (VenusOS)  ││
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

**Der Datenweg:**
1. **ESP32** liest den JBD BMS via BLE aus (Service 0xFF00, Protokoll aus [aiobmsble](https://github.com/patman15/aiobmsble) reverse-engineered)
2. **ESP32** publiziert die Werte als JSON via **MQTT**
3. **Cerbo GX** startet einen Python-Daemon, der MQTT abonniert und an Victrons **D-Bus** weiterleitet
4. **Victron VenusOS** zeigt den Akku als native Batterie an — Spannung, SoC, Strom, Temperatur inklusive
5. **Node-RED** (optional) für Dashboards, Debugging, InfluxDB

## Features

- ✅ **JBD BLE-Protokoll** — Spannung, Strom, SoC, Temperatur, Zellspannungen
- ✅ **ESP32 mit NimBLE** — stabil und speicherschonend, besser als Standard-BLE
- ✅ **VenusOS D-Bus** — Akku erscheint wie jede Victron-Batterie
- ✅ **MQTT** — flexible Anbindung, keine proprietären Protokolle
- ✅ **Node-RED Flow** — Debug und Dashboard inklusive
- ✅ **Watchdog** — erkennt BLE-Verbindungsabbruch, setzt "offline"
- ✅ **CI** — GitHub Actions baut die v2-Firmware für ESP32 / S3 / C3

## Schnellstart

### 1. ESP32 flashen

Die v2-Firmware verwenden (ungetestet, siehe Status oben):

```bash
cd esp32_jbd_ble_mqtt_v2_untested/
# WIFI_SSID, WIFI_PASS, MQTT_HOST oben in der .ino anpassen
pio run -e esp32dev -t upload     # oder esp32s3 / esp32c3
pio device monitor
```

> **Kein PlatformIO?** Arduino IDE → `esp32_jbd_ble_mqtt_v2_untested/esp32_jbd_ble_mqtt_v2_untested.ino` öffnen → **NimBLE-Arduino 2.x**, PubSubClient, ArduinoJson 7.x installieren → Upload.
> Details und Fehlerliste der v1: [`esp32_jbd_ble_mqtt_v2_untested/README.md`](esp32_jbd_ble_mqtt_v2_untested/README.md)

### 2. VenusOS Daemon installieren

```bash
ssh root@<cerbo-ip>
cd /tmp
curl -sL https://github.com/silly82/jbd-ble-victron-bridge-v2/archive/main.tar.gz | tar xz
cd jbd-ble-victron-bridge-v2-main/venusos
./install.sh
```

**Oder manuell:**
```bash
scp -r venusos/* root@<cerbo-ip>:/data/dbus-mqtt-battery/
ssh root@<cerbo-ip>
/data/dbus-mqtt-battery/install.sh
```

### 3. Node-RED Flow importieren (optional)

1. Auf dem Cerbo GX: VenusOS App Store → **Node-RED** installieren
2. `flows.json` importieren → Menü → Import
3. MQTT-Broker-Konfiguration anpassen
4. Deploy

## Konfiguration

### ESP32 (`esp32_jbd_ble_mqtt_v2_untested.ino`)

```cpp
const char* WIFI_SSID     = "FRITZ!Box 7530";
const char* WIFI_PASS     = "geheim";
const char* MQTT_HOST     = "192.168.1.100";     // Cerbo GX oder Broker
const int   MQTT_PORT     = 1883;
const char* JBD_DEVICE_NAME = "";                 // leer = Auto-Scan
```

> `JBD_DEVICE_NAME` leer lassen = ESP scannt automatisch nach Geräten mit Namen wie `JBD-*`, `DWF*`, `SX1*`, `SBL*`.

### VenusOS Daemon (`venusos/dbus_mqtt_battery.service`)

```ini
Environment=MQTT_HOST=192.168.1.100
Environment=MQTT_TOPIC=bms/jbd/data
Environment=DBUS_INSTANCE=256
Environment=POLL_TIMEOUT=60
# Auf 1 setzen wenn ein Victron SmartShunt vorhanden ist (siehe unten)
Environment=SMARTSHUNT_MODE=0
```

## SmartShunt-Koexistenz

Wenn ein **Victron SmartShunt** vorhanden ist, bleibt dieser der primäre Batteriemonitor — sein Coulomb-Counting-SoC ist deutlich präziser als die spannungsbasierte Schätzung des JBD BMS.

`SMARTSHUNT_MODE=1` in der Service-Unit setzen. In diesem Modus überspringt der Daemon SoC, Kapazität und Energiebilanz (diese verbleiben beim SmartShunt) und veröffentlicht nur JBD-spezifische Daten, die der SmartShunt nicht kennt:

| Funktion | SmartShunt | JBD-Daemon (`SMARTSHUNT_MODE=1`) |
|----------|-----------|----------------------------------|
| SoC (Coulomb-Counting) | ✅ primär | übersprungen |
| Spannung / Strom / Leistung | ✅ | ✅ (Quervergleich) |
| Einzelzellspannungen | ❌ | ✅ `/Voltages/Cell1..N` |
| Zellspannungsdifferenz | ❌ | ✅ `/Voltages/Diff` |
| Lade-/Entladefreigabe BMS | ❌ | ✅ `/Io/AllowToCharge` |
| BMS-Temperaturen | ✅ (1 Sensor) | ✅ (alle BMS-Sensoren) |
| Ladezyklen | ✅ | ✅ |

**Nach Aktivierung des SmartShunt-Modus:**
1. `SMARTSHUNT_MODE=1` in `/data/dbus-mqtt-battery/dbus_mqtt_battery.service` setzen
2. `systemctl restart dbus-mqtt-battery`
3. In VenusOS: **Einstellungen → System-Setup → Batteriemonitor → SmartShunt**

## MQTT-Datenformat

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

## VenusOS / D-Bus Pfade

| D-Bus Pfad | Beschreibung | Einheit | SmartShunt-Modus |
|-----------|-------------|--------|-----------------|
| `/Dc/0/Voltage` | Batteriespannung | V | ✅ immer |
| `/Dc/0/Current` | Strom (positiv = laden) | A | ✅ immer |
| `/Dc/0/Power` | Leistung | W | ✅ immer |
| `/Dc/0/Temperature` | Temperatur | °C | ✅ immer |
| `/Soc` | Ladezustand | % | ⏭ übersprungen |
| `/Capacity` | Nennkapazität | Ah | ⏭ übersprungen |
| `/ConsumedAmphours` | Verbrauchte Amperestunden | Ah | ⏭ übersprungen |
| `/History/DischargeCycles` | Ladezyklen | # | ✅ immer |
| `/Info/MaxChargeCurrent` | Max. Ladestrom | A | ⏭ übersprungen |
| `/Info/MaxDischargeCurrent` | Max. Entladestrom | A | ⏭ übersprungen |
| `/Io/AllowToCharge` | BMS erlaubt Laden | 0/1 | ✅ immer |
| `/Io/AllowToDischarge` | BMS erlaubt Entladen | 0/1 | ✅ immer |
| `/Voltages/Cell1..N` | Einzelzellspannungen | V | ✅ immer |
| `/Voltages/Sum` | Summe aller Zellspannungen | V | ✅ immer |
| `/Voltages/Min` | Minimale Zellspannung | V | ✅ immer |
| `/Voltages/Max` | Maximale Zellspannung | V | ✅ immer |
| `/Voltages/Diff` | Zellspannungsdifferenz (max−min) | V | ✅ immer |
| `/Connected` | Verbindungsstatus | 0/1 | ✅ immer |

## JBD BLE-Protokoll (Reverse Engineering)

Basiert auf [aiobmsble](https://github.com/patman15/aiobmsble) von patman15.

| Parameter | Wert |
|-----------|------|
| Service UUID | `0000ff00-0000-1000-8000-00805f9b34fb` |
| RX Char (Notify) | `ff01` |
| TX Char (Write) | `ff02` |
| Init-Paket | keines — nicht nötig (die Init-Sequenz der v1 war falsch) |
| Command BasicInfo | `DD A5 03 00 <CRC16> 77` |
| Command Cells | `DD A5 04 00 <CRC16> 77` |
| Antwort | `DD <cmd> <status> <len> <daten…> <CRC16> 77`, kommt in ~20-Byte-BLE-Fragmenten |
| CRC | `0x10000 − sum(status, len, daten)` |
| Frame Tail | `0x77` |

## Verzeichnisstruktur

```
jbd-ble-victron-bridge-v2/
├── esp32_jbd_ble_mqtt_v2_untested/ ← ESP32 Firmware v2 (NimBLE 2.x) — UNGETESTET
│   ├── esp32_jbd_ble_mqtt_v2_untested.ino
│   ├── platformio.ini
│   └── README.md                  ← Fehlerliste v1, Test-Checkliste
├── esp32_jbd_ble_mqtt/       ← ESP32 Firmware v1 — KOMPILIERT NICHT, nur als Referenz
├── venusos/                   ← Cerbo GX / VenusOS
│   ├── dbus_mqtt_battery.py      ← D-Bus Daemon (Python)
│   ├── dbus_mqtt_battery.service ← systemd Service Unit
│   └── install.sh                ← Installationsskript
├── flows.json                 ← Node-RED Flow (importierbar)
├── .github/workflows/         ← GitHub Actions CI
├── LICENSE                    ← MIT
├── README.md                  ← English
└── README.de.md               ← Deutsch (Schweizer Standardsprache)
```

## Fehlersuche

**ESP32 findet kein JBD BMS:**
- BMS-App auf dem Handy schliessen (nur eine Verbindung gleichzeitig)
- `JBD_DEVICE_NAME` leer lassen (Auto-Scan)
- RSSI prüfen — ESP32 nahe am Akku platzieren
- Service UUID `ff00` prüfen (manchen Klone nutzen abweichende UUIDs)

**Keine Daten auf dem Cerbo:**
```bash
# MQTT prüfen
mosquitto_sub -h localhost -t bms/jbd/data

# Service-Status
systemctl status dbus-mqtt-battery

# Log
journalctl -u dbus-mqtt-battery -n 50 --no-pager
```

**Akku erscheint nicht in VenusOS:**
- Service neu starten: `systemctl restart dbus-mqtt-battery`
- Instance-ID ändern falls belegt: `DBUS_INSTANCE=257`
- VenusOS Remote Console → Geräte → Nach neuen Geräten suchen

## Warum keine reine Node-RED-Lösung?

[`@victronenergy/node-red-contrib-victron`](https://flows.nodered.org/node/@victronenergy/node-red-contrib-victron) stellt offizielle Victron-Nodes für Node-RED bereit — diese können jedoch nur **bestehende D-Bus-Dienste lesen oder beschreiben**. Es gibt keinen Node, der ein **neues virtuelles Batteriegerät auf dem D-Bus registrieren** kann.

| Aufgabe | Node-RED allein |
|---------|----------------|
| MQTT vom ESP32 abonnieren | ✅ Ja |
| Dashboard / Visualisierung | ✅ Ja |
| InfluxDB / Grafana-Logging | ✅ Ja |
| Alarme und Automatisierungen | ✅ Ja |
| Bestehende VenusOS-Dienste beschreiben | ✅ Ja (via `victron-output`-Nodes) |
| **Neues virtuelles Gerät in VenusOS anlegen** | ❌ Nein — erfordert Python + D-Bus |

Der Python-Daemon (`dbus_mqtt_battery.py`) ist der einzige Weg, einen neuen `com.victronenergy.battery.*`-Dienst zu registrieren, damit VenusOS den JBD-BMS als native Batterie erkennt. Node-RED bleibt eine optionale Ergänzung für Dashboards und Logging, kann den Daemon aber nicht ersetzen.

## Verwandte Projekte

- [aiobmsble](https://github.com/patman15/aiobmsble) — Python BLE BMS Library (unsere Basis)
- [BMS_BLE-HA](https://github.com/patman15/BMS_BLE-HA) — Home Assistant Integration
- [dbus-serialbattery](https://github.com/mr-manuel/venus-os_dbus-serialbattery) — VenusOS Serial Battery Driver
- [velib_python](https://github.com/victronenergy/velib_python) — Victron D-Bus Python Bindings

## Lizenz

MIT — machen Sie damit, was Sie wollen, aber ohne Garantie. Siehe [LICENSE](LICENSE).