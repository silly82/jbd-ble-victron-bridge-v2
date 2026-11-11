#!/usr/bin/env python3
"""
VenusOS DBus Battery Driver — JBD BMS via MQTT

Liest BMS-Daten von MQTT (ESP32 BLE-Bridge) und published sie
an Victron Energy's D-Bus, damit der Akku im VenusOS/Cerbo GX
angezeigt wird.

Basierend auf dbus-serialbattery / velib_python.

Installation auf Cerbo GX:
  cp dbus_mqtt_battery.py /data/dbus-mqtt-battery/
  cp dbus_mqtt_battery.service /data/etc/systemd/system/
  systemctl enable dbus-mqtt-battery && systemctl start dbus-mqtt-battery

SmartShunt-Modus (SMARTSHUNT_MODE=1):
  Wenn ein SmartShunt vorhanden ist, übernimmt dieser SoC, Kapazität
  und Energiebilanz. Dieser Daemon liefert dann nur noch die JBD-spezifischen
  Daten: Einzelzellspannungen, BMS-Schutzstatus und Temperaturen.
  → VenusOS Einstellungen → System-Setup → Batteriemonitor: SmartShunt wählen.
"""

import json
import os
import sys
import signal
import logging
import time
import threading
from typing import Optional

# --- VenusOS velib_python (Standard-Pfad) ---
sys.path.insert(1, "/opt/victronenergy/dbus-systemcalc-py/ext/velib_python")
from vedbus import VeBusService, VeBusItem
from ve_utils import exit_on_error

# --- Konfiguration ---
MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_TOPIC = os.environ.get("MQTT_TOPIC", "bms/jbd/data")
DBUS_SERVICE = os.environ.get("DBUS_SERVICE", "com.victronenergy.battery")
DBUS_INSTANCE = int(os.environ.get("DBUS_INSTANCE", "256"))
POLL_TIMEOUT = int(os.environ.get("POLL_TIMEOUT", "60"))  # Sekunden bis "offline"

# SmartShunt-Modus: SoC/Kapazität/Energiebilanz dem SmartShunt überlassen.
# Dieser Daemon liefert dann nur Zelldaten, BMS-Status und Temperaturen.
SMARTSHUNT_MODE = os.environ.get("SMARTSHUNT_MODE", "0").strip() in ("1", "true", "yes")

# --- Logging ---
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("/data/dbus-mqtt-battery/dbus_mqtt_battery.log", mode="a")
    ]
)
log = logging.getLogger("dbus-mqtt-battery")


class DBusMqttBattery:
    """Hauptklasse: MQTT-Subscriber + D-Bus Publisher."""

    def __init__(self):
        self._running = False
        self._last_data = {}
        self._last_update = 0
        self._lock = threading.Lock()
        self._dbus_service: Optional[VeBusService] = None
        self._mqtt_client = None

    def _setup_dbus(self):
        """D-Bus Service und Items registrieren (velib_python)."""
        service_name = f"{DBUS_SERVICE}.http_{DBUS_INSTANCE}"
        self._dbus_service = VeBusService(
            service_name,
            dbus_name=DBUS_SERVICE,
            device_instance=DBUS_INSTANCE
        )

        # --- Pfade, die Victron erwartet ---
        paths = {
            # Dc/0 — Hauptbatteriepfad
            "/Dc/0/Voltage": {"initial": 0.0},
            "/Dc/0/Current": {"initial": 0.0},
            "/Dc/0/Power": {"initial": 0.0},
            "/Dc/0/Temperature": {"initial": None},

            # SoC + Capacity (im SmartShunt-Modus nur als Spiegel, nicht primär)
            "/Soc": {"initial": 0.0},
            "/Capacity": {"initial": 0.0},
            "/ConsumedAmphours": {"initial": 0.0},

            # History / Info
            "/History/DischargeCycles": {"initial": 0},
            "/History/TotalDischarge": {"initial": 0.0},
            "/Info/ChargeRequest": {"initial": 1},
            "/Info/MaxChargeCurrent": {"initial": 0.0},
            "/Info/MaxDischargeCurrent": {"initial": 0.0},

            # BMS-Schutzstatus (JBD-spezifisch)
            "/Io/AllowToCharge": {"initial": 1},
            "/Io/AllowToDischarge": {"initial": 1},
            "/Balancing": {"initial": 0},

            # Zellspannungen (werden dynamisch ergänzt, max. 32 Zellen)
            "/Voltages/Sum": {"initial": 0.0},
            "/Voltages/Diff": {"initial": 0.0},
            "/Voltages/Min": {"initial": 0.0},
            "/Voltages/Max": {"initial": 0.0},

            # Device info
            "/Mgmt/ProductName": {"initial": "JBD smart BMS (BLE Bridge)"},
            "/Mgmt/Connection": {"initial": "MQTT"},
            "/Mgmt/DeviceInstance": {"initial": DBUS_INSTANCE},
            "/ProductId": {"initial": 0xB000},
            "/DeviceType": {"initial": 0},  # Generic
            "/Connected": {"initial": 1},
            "/AutoSelected": {"initial": 1},
        }

        # Zellspannungspfade vorregistrieren (bis zu 32 Zellen)
        for i in range(1, 33):
            paths[f"/Voltages/Cell{i}"] = {"initial": None}

        for path, config in paths.items():
            VeBusItem(
                self._dbus_service,
                path,
                config.get("initial", None)
            )

        mode_info = " [SmartShunt-Modus: SoC/Kap. vom SmartShunt]" if SMARTSHUNT_MODE else ""
        log.info(f"D-Bus Service '{service_name}' gestartet{mode_info}")

    def _setup_mqtt(self):
        """MQTT-Client initialisieren."""
        import paho.mqtt.client as mqtt

        def on_connect(client, userdata, flags, rc):
            if rc == 0:
                log.info(f"MQTT verbunden ({MQTT_HOST}:{MQTT_PORT})")
                client.subscribe(MQTT_TOPIC, qos=0)
                log.info(f"Subscribed: {MQTT_TOPIC}")
            else:
                log.error(f"MQTT Connect-Fehler: rc={rc}")

        def on_message(client, userdata, msg):
            try:
                data = json.loads(msg.payload.decode("utf-8"))
                with self._lock:
                    self._last_data = data
                    self._last_update = time.time()
                    self._publish_to_dbus(data)
            except json.JSONDecodeError as e:
                log.error(f"Ungültiges JSON von MQTT: {e}")
            except Exception as e:
                log.error(f"Fehler bei MQTT-Nachricht: {e}")

        def on_disconnect(client, userdata, rc):
            if rc != 0:
                log.warning(f"MQTT getrennt (rc={rc}), reconnecte...")

        self._mqtt_client = mqtt.Client(client_id="dbus-mqtt-battery")
        self._mqtt_client.on_connect = on_connect
        self._mqtt_client.on_message = on_message
        self._mqtt_client.on_disconnect = on_disconnect
        self._mqtt_client.connect_async(MQTT_HOST, MQTT_PORT, 60)

    def _publish_to_dbus(self, data: dict):
        """BMS-JSON-Daten an D-Bus schreiben."""
        if not self._dbus_service:
            return

        voltage = float(data.get("voltage", 0))
        current = float(data.get("current", 0))
        soc = float(data.get("soc", data.get("battery_level", 0)))
        capacity = float(data.get("capacity", data.get("design_capacity", 0)))
        charge = float(data.get("charge", data.get("cycle_charge", 0)))
        power = float(data.get("power", voltage * current))

        # Dc/0
        self._set_value("/Dc/0/Voltage", voltage)
        self._set_value("/Dc/0/Current", current)
        self._set_value("/Dc/0/Power", power)

        # Temperatur (ersten Sensor nutzen)
        temps = data.get("temperatures", [])
        if temps:
            self._set_value("/Dc/0/Temperature", float(temps[0]))

        # SoC / Capacity — im SmartShunt-Modus übernimmt der SmartShunt diese Werte.
        # Der Daemon schreibt sie trotzdem (für Geräteübersicht), aber VenusOS nutzt
        # den als Hauptmonitor konfigurierten SmartShunt für die Systemanzeige.
        if not SMARTSHUNT_MODE:
            self._set_value("/Soc", min(soc, 100.0))
            if capacity > 0:
                self._set_value("/Capacity", float(capacity))
                consumed = max(0, capacity * (100 - soc) / 100)
                self._set_value("/ConsumedAmphours", consumed)
                self._set_value("/Info/MaxChargeCurrent", capacity * 0.5)
                self._set_value("/Info/MaxDischargeCurrent", capacity * 0.5)

        # Zyklen (immer schreiben — SmartShunt zählt diese nicht)
        cycles = data.get("cycles", 0)
        if cycles:
            self._set_value("/History/DischargeCycles", int(cycles))

        # BMS-Schutzstatus (JBD-spezifisch — SmartShunt kennt diese nicht)
        chrg_mosfet = data.get("chrg_mosfet", True)
        dischrg_mosfet = data.get("dischrg_mosfet", True)
        self._set_value("/Io/AllowToCharge", 1 if chrg_mosfet else 0)
        self._set_value("/Io/AllowToDischarge", 1 if dischrg_mosfet else 0)

        # Einzelzellspannungen (JBD-spezifisch — SmartShunt kennt diese nicht)
        cell_voltages = data.get("cell_voltages", [])
        if cell_voltages:
            cv_floats = [float(v) for v in cell_voltages[:32]]
            for i, cv in enumerate(cv_floats, start=1):
                self._set_value(f"/Voltages/Cell{i}", cv)

            min_v = min(cv_floats)
            max_v = max(cv_floats)
            self._set_value("/Voltages/Sum", sum(cv_floats))
            self._set_value("/Voltages/Min", min_v)
            self._set_value("/Voltages/Max", max_v)
            self._set_value("/Voltages/Diff", max_v - min_v)

        # Verbindung aktiv halten
        self._set_value("/Connected", 1)

        log.debug(f"DBus Update: {voltage:.2f}V, {current:.2f}A, {soc:.0f}%"
                  + (f", {len(cell_voltages)} Zellen" if cell_voltages else ""))

    def _set_value(self, path: str, value):
        """Setzt einen D-Bus-Item-Wert."""
        try:
            item = getattr(self._dbus_service, path, None)
            if item is not None:
                item.set_value(value)
        except Exception as e:
            log.error(f"Setze {path}={value} fehlgeschlagen: {e}")

    def _watchdog(self):
        """Watchdog: setzt Werte zurück wenn MQTT ausfällt."""
        while self._running:
            time.sleep(10)
            elapsed = time.time() - self._last_update
            if elapsed > POLL_TIMEOUT and self._last_update > 0:
                log.warning(f"Keine MQTT-Daten seit {elapsed:.0f}s — setze offline")
                with self._lock:
                    self._set_value("/Connected", 0)

    def run(self):
        """Hauptschleife starten."""
        self._running = True

        # D-Bus einrichten
        self._setup_dbus()
        # MQTT verbinden
        self._setup_mqtt()
        # Watchdog-Thread
        watch = threading.Thread(target=self._watchdog, daemon=True)
        watch.start()

        log.info("dbus-mqtt-battery läuft. Drücke Ctrl+C zum Beenden.")

        # MQTT-Netzwerkschleife
        try:
            while self._running:
                self._mqtt_client.loop(timeout=1.0)
        except KeyboardInterrupt:
            self.stop()

    def stop(self):
        """Aufräumen."""
        self._running = False
        if self._mqtt_client:
            self._mqtt_client.disconnect()
        log.info("dbus-mqtt-battery gestoppt")


# ============ START ============
def main():
    log.info("=" * 50)
    log.info("VenusOS DBus MQTT Battery Bridge")
    log.info(f"  MQTT: {MQTT_HOST}:{MQTT_PORT} → {MQTT_TOPIC}")
    log.info(f"  DBus: {DBUS_SERVICE} / instance {DBUS_INSTANCE}")
    log.info(f"  Modus: {'SmartShunt (SoC/Kap. vom SmartShunt)' if SMARTSHUNT_MODE else 'Standalone (SoC/Kap. vom JBD BMS)'}")
    log.info("=" * 50)

    bridge = DBusMqttBattery()

    # Signal-Handler für sauberes Beenden
    def handle_signal(sig, frame):
        log.info(f"Signal {sig} empfangen, beende...")
        bridge.stop()
        sys.exit(0)

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    bridge.run()


if __name__ == "__main__":
    main()