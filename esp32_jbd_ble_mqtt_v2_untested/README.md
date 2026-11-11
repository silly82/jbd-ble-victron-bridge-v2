# ESP32 JBD BLE → MQTT Bridge — v2

> ⚠️ **UNGETESTET / UNTESTED** — Diese Version wurde noch nie an einem echten JBD-BMS betrieben.
> Sie kompiliert und die Protokoll-Logik ist mit synthetischen Frames geprüft, mehr nicht.
> Rückmeldungen bitte als [GitHub Issue](https://github.com/silly82/jbd-ble-victron-bridge-v2/issues).
>
> *This version has never run against real JBD hardware. It compiles and the protocol logic was
> checked with synthetic frames only. Please report results via GitHub Issues.*

Ersetzt `../esp32_jbd_ble_mqtt/` (v1). Die v1 kompiliert nicht und hätte auch kompiliert nicht funktioniert.

## Was in v1 kaputt war

| # | Problem v1 | Auswirkung | Fix v2 |
|---|------------|-----------|--------|
| 1 | `std::vector<…> = pScan->getResults(10)` | Kompilierfehler `conversion from 'NimBLEScanResults' to non-scalar type…` | `NimBLEScanResults` + `getCount()`/`getDevice()` |
| 2 | Code gemischt aus NimBLE 1.x/2.x, `platformio.ini` pinnt 1.4 | Arduino IDE installiert 2.x → weitere Fehler | Konsequent NimBLE-Arduino **2.x** |
| 3 | Scan-Dauer und Connect-Timeout in Sekunden statt ms | 10 ms Scan → findet nichts | Werte in ms |
| 4 | `onDisconnect(NimBLEClient*)` alte Signatur | Callback nie aufgerufen, Verbindungsverlust unbemerkt | `onDisconnect(NimBLEClient*, int) override` |
| 5 | Erfundene Init-Sequenz `FF AA 15 01 …` mit Pflicht-Antwort | JBD antwortet nicht → Verbindung wird immer abgebrochen | Entfernt, stattdessen direkt BasicInfo (0x03) als Protokoll-Check |
| 6 | BLE-Fragmente (~20 Bytes) nicht zusammengesetzt | BasicInfo (~34 Bytes) immer CRC-/Längenfehler | Frame-Reassembly im Notify-Callback |
| 7 | PubSubClient-Puffer 256 Bytes | Payload mit Zellspannungen wird nicht publiziert | `setBufferSize(1024)` |
| 8 | `createClient()` bei jedem Reconnect | Speicherleck | Client wird wiederverwendet |
| 9 | Blockierendes `delay()` bis 120 s beim Reconnect | MQTT-Keepalive läuft ab | Nicht-blockierender Timer |
| 10 | Projektstruktur ohne `src_dir` | `pio run` findet die `.ino` nicht | `src_dir = .` |

## Benötigt

- ESP32 mit BLE (die CI baut für ESP32, ESP32-S3 und ESP32-C3)
- **NimBLE-Arduino ≥ 2.3** (1.x funktioniert nicht), PubSubClient ≥ 2.8, ArduinoJson ≥ 7

## Build

**PlatformIO**
```bash
cd esp32_jbd_ble_mqtt_v2_untested
pio run -e esp32dev -t upload
pio device monitor
```

**Arduino IDE**
1. Ordner `esp32_jbd_ble_mqtt_v2_untested` öffnen (Ordnername = Sketchname).
2. Bibliotheksverwalter: *NimBLE-Arduino* (2.x), *PubSubClient*, *ArduinoJson* (7.x).
3. Werkzeuge → Partition Scheme → **«Huge APP (3MB No OTA/1MB SPIFFS)»** wählen. Mit dem Standard-Schema (1.2 MB) passt die Firmware auf 4-MB-Boards nicht.
4. Konfiguration oben im Sketch anpassen, hochladen.

## Konfiguration

```cpp
const char* WIFI_SSID       = "DEIN_WLAN";
const char* WIFI_PASS       = "DEIN_WLAN_PASS";
const char* MQTT_HOST       = "192.168.1.100";   // Cerbo GX oder Broker
const char* JBD_DEVICE_NAME = "";                // leer = Auto-Scan (JBD-, DWF-, LSG-, SX1, SBL-, OGR-, TZ-H, xiaoxiang)
```

Steht der BMS-Name nicht in der Liste: Seriellen Monitor öffnen, unter `[BLE] Gefunden: …` den Namen ablesen und in `JBD_DEVICE_NAME` eintragen.

## MQTT

| Topic | Inhalt |
|-------|--------|
| `bms/jbd/data` | JSON, Format identisch zu v1 (kompatibel mit `venusos/dbus_mqtt_battery.py`) |
| `bms/jbd/status` | `online` / `bms_offline` / `offline` (LWT), retained |

## Was beim Testen zu prüfen ist

Bitte den seriellen Log (115200 Baud) beilegen, wenn etwas nicht klappt:

1. `[BLE] Gefunden: …` — taucht das BMS auf?
2. `[BLE] JBD BMS verbunden` — antwortet es auf BasicInfo?
3. `[JBD] xx.xxV x.xxA SoC xx%` — stimmen die Werte mit der Xiaoxiang-App überein?
4. `[MQTT] … Bytes publiziert` — kommt das JSON auf `bms/jbd/data` an?

**Wichtig:** Die Xiaoxiang-App am Handy schliessen. Das BMS akzeptiert nur eine BLE-Verbindung gleichzeitig.
