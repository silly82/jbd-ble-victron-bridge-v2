# Changelog

Nachfolger von `silly82/jbd-ble-victron-bridge` (archiviert). Die Historie bis 2026-07 liegt dort.

## 2026-11-11 — Firmware v2 (untested)

### Hinzugefügt
- `esp32_jbd_ble_mqtt_v2_untested/`: neue ESP32-Firmware für NimBLE-Arduino 2.x, eigene README mit Test-Checkliste.
- CI baut die v2-Firmware mit PlatformIO für ESP32, ESP32-S3 und ESP32-C3.
- `CHANGELOG.md`.

### Behoben (gegenüber v1)
- Kompilierfehler `NimBLEScanResults` → `std::vector` (gemeldet von Jean-Marc Ternisien).
- Scan-Dauer/Connect-Timeout in ms statt s (NimBLE 2.x).
- `onDisconnect`-Signatur für NimBLE 2.x, Verbindungsverlust wird jetzt erkannt.
- Falsche Init-Sequenz entfernt; JBD braucht keine.
- Zusammensetzen fragmentierter BLE-Antworten.
- MQTT-Puffer auf 1024 Bytes vergrössert.
- Kein Speicherleck mehr durch `createClient()` bei jedem Reconnect.
- Nicht-blockierender Reconnect, MQTT bleibt verbunden.

### Geändert
- `esp32_jbd_ble_mqtt/` (v1) als nicht kompilierbar markiert, bleibt als Referenz.
- README (DE/EN): Status-Hinweis, Schnellstart und Protokolltabelle auf v2 angepasst.

### Unverändert
- `venusos/` (D-Bus-Daemon), `flows.json`. Das MQTT-JSON der v2 ist kompatibel.
