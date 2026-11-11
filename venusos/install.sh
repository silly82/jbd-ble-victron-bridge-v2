#!/bin/bash
# Installationsscript für Cerbo GX (VenusOS)
# Kopiert Dateien und richtet den Service ein.
# 
# Annahme: Dieses Skript wird im selben Verzeichnis wie 
#   dbus_mqtt_battery.py und dbus_mqtt_battery.service ausgeführt.

set -e

TARGET_DIR="/data/dbus-mqtt-battery"
SERVICE_FILE="/data/etc/systemd/system/dbus-mqtt-battery.service"
LOG_FILE="${TARGET_DIR}/dbus_mqtt_battery.log"

echo "=== VenusOS DBus MQTT Battery Bridge Installation ==="
echo ""

# 1. Zielverzeichnis erstellen
echo "[1/4] Erstelle ${TARGET_DIR}..."
mkdir -p "${TARGET_DIR}"

# 2. Python-Script kopieren
echo "[2/4] Kopiere dbus_mqtt_battery.py..."
cp "$(dirname "$0")/dbus_mqtt_battery.py" "${TARGET_DIR}/"
chmod +x "${TARGET_DIR}/dbus_mqtt_battery.py"

# 3. Service-Datei installieren
echo "[3/4] Installiere systemd Service..."
cp "$(dirname "$0")/dbus_mqtt_battery.service" "${SERVICE_FILE}"
ln -sf "${SERVICE_FILE}" "/etc/systemd/system/dbus-mqtt-battery.service"

# 4. Service aktivieren und starten
echo "[4/4] Starte Service..."
systemctl daemon-reload
systemctl enable dbus-mqtt-battery
systemctl restart dbus-mqtt-battery

# Status prüfen
echo ""
echo "=== Service Status ==="
systemctl status dbus-mqtt-battery --no-pager || true

echo ""
echo "=== Log (letzte 20 Zeilen) ==="
tail -20 "${LOG_FILE}" 2>/dev/null || echo "(Log noch leer)"

echo ""
echo "=== Fertig! ==="
echo "Nach Änderungen an der Konfiguration direkt in ${TARGET_DIR}/dbus_mqtt_battery.py"
echo "danach: systemctl restart dbus-mqtt-battery"