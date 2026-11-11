/*
 * ESP32 JBD BLE -> MQTT Bridge — v2 (NimBLE-Arduino 2.x)
 *
 * !!! STATUS: UNGETESTET / UNTESTED !!!
 * Diese Version wurde nie an einem echten JBD-BMS betrieben.
 * Sie behebt die bekannten Fehler der v1 (siehe README.md in diesem Ordner),
 * ist aber bis zu einer Rueckmeldung aus der Praxis als experimentell zu betrachten.
 * This version has never run against real JBD hardware. Feedback welcome via GitHub Issues.
 *
 * Hardware: ESP32 (WROOM/S3/C3) mit WLAN + BLE
 * Bibliotheken:
 *   - h2zero/NimBLE-Arduino  >= 2.3  (1.x wird NICHT unterstuetzt)
 *   - knolleary/PubSubClient >= 2.8
 *   - bblanchon/ArduinoJson  >= 7.0
 *
 * JBD-Protokoll (BLE Service 0xFF00, Notify 0xFF01, Write 0xFF02):
 *   Request : DD A5 <cmd> 00 <crcH> <crcL> 77      crc = 0x10000 - (cmd + 0)
 *   Response: DD <cmd> <status> <len> <data...> <crcH> <crcL> 77
 *             crc = 0x10000 - sum(status, len, data)
 *   Antworten kommen per BLE in ~20-Byte-Fragmenten und werden hier zusammengesetzt.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// ============ KONFIGURATION ============
const char* WIFI_SSID     = "DEIN_WLAN";
const char* WIFI_PASS     = "DEIN_WLAN_PASS";

const char* MQTT_HOST     = "192.168.1.100";  // Cerbo GX oder MQTT-Broker
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";
const char* MQTT_PASS     = "";
const char* MQTT_TOPIC    = "bms/jbd/data";
const char* MQTT_TOPIC_ST = "bms/jbd/status";

// Exakter BLE-Name des BMS, z.B. "JBD-SP04S034-L4S". Leer = erstes passendes Geraet.
const char* JBD_DEVICE_NAME = "";

// Polling-Intervall
const unsigned long POLL_INTERVAL_MS = 30000;

// ============ JBD KONSTANTEN ============
const char* JBD_SERVICE_UUID = "0000ff00-0000-1000-8000-00805f9b34fb";
const char* JBD_CHAR_RX      = "0000ff01-0000-1000-8000-00805f9b34fb";  // notify
const char* JBD_CHAR_TX      = "0000ff02-0000-1000-8000-00805f9b34fb";  // write

const uint8_t JBD_CMD_BASIC = 0x03;
const uint8_t JBD_CMD_CELLS = 0x04;

const uint32_t SCAN_TIME_MS        = 10000;
const uint32_t CONNECT_TIMEOUT_MS  = 10000;
const uint32_t REPLY_TIMEOUT_MS    = 3000;
const uint32_t MQTT_RETRY_MS       = 5000;

// ============ GLOBALS ============
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

NimBLEClient* pBLEClient = nullptr;
NimBLERemoteCharacteristic* pCharRx = nullptr;
NimBLERemoteCharacteristic* pCharTx = nullptr;

volatile bool bmsConnected = false;

// Frame-Zusammensetzung (wird im NimBLE-Task befuellt)
uint8_t           rxBuf[256];
volatile size_t   rxLen = 0;
volatile bool     rxReady = false;
volatile uint8_t  rxExpectedCmd = 0;
portMUX_TYPE      rxMux = portMUX_INITIALIZER_UNLOCKED;

// Kopie des letzten vollstaendigen Frames (Loop-Task)
uint8_t frame[256];
size_t  frameLen = 0;

// BMS-Daten
float voltage = 0, current = 0, power = 0, cycleCharge = 0;
int   designCapacity = 0, cycles = 0, batteryLevel = 0;
bool  chrgMosfet = false, dischrgMosfet = false;
int   tempCount = 0;
float tempValues[8] = {0};
int   cellCount = 0;
float cellVoltages[32] = {0};

unsigned long lastPoll = 0;
unsigned long nextBleAttempt = 0;
unsigned long lastMqttAttempt = 0;
int reconnectAttempts = 0;

// ============ BLE CALLBACKS ============
class ClientCallback : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    Serial.printf("[BLE] Verbindung getrennt (reason %d)\n", reason);
    bmsConnected = false;
  }
};
static ClientCallback clientCB;

void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* data, size_t len, bool isNotify) {
  portENTER_CRITICAL(&rxMux);
  if (!rxReady) {
    bool isStart = len >= 2 && data[0] == 0xDD && data[1] == rxExpectedCmd;
    if (isStart) rxLen = 0;
    if ((isStart || rxLen > 0) && rxLen + len <= sizeof(rxBuf)) {
      memcpy(rxBuf + rxLen, data, len);
      rxLen += len;
      if (rxLen >= 4 && rxLen >= (size_t)(4 + rxBuf[3] + 3)) rxReady = true;
    } else if (rxLen + len > sizeof(rxBuf)) {
      rxLen = 0;  // Ueberlauf -> verwerfen
    }
  }
  portEXIT_CRITICAL(&rxMux);
}

// ============ JBD PROTOKOLL ============
uint16_t jbdCrc(const uint8_t* data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return (uint16_t)(0x10000 - sum);
}

bool sendCommand(uint8_t cmd) {
  uint8_t req[7] = {0xDD, 0xA5, cmd, 0x00, 0, 0, 0x77};
  uint16_t crc = jbdCrc(&req[2], 2);
  req[4] = crc >> 8;
  req[5] = crc & 0xFF;

  portENTER_CRITICAL(&rxMux);
  rxExpectedCmd = cmd;
  rxLen = 0;
  rxReady = false;
  portEXIT_CRITICAL(&rxMux);

  return pCharTx->writeValue(req, sizeof(req), false);
}

// Wartet auf vollstaendigen Frame und kopiert ihn nach frame[]
bool waitForFrame(uint32_t timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (rxReady) {
      portENTER_CRITICAL(&rxMux);
      frameLen = rxLen;
      memcpy(frame, rxBuf, frameLen);
      rxReady = false;
      rxLen = 0;
      portEXIT_CRITICAL(&rxMux);
      return true;
    }
    delay(10);
  }
  return false;
}

bool checkFrame(uint8_t cmd) {
  if (frameLen < 7 || frame[0] != 0xDD || frame[1] != cmd) return false;
  if (frame[2] != 0x00) {
    Serial.printf("[JBD] BMS meldet Fehlerstatus 0x%02X\n", frame[2]);
    return false;
  }
  size_t dataLen = frame[3];
  size_t total = 4 + dataLen + 3;
  if (frameLen < total || frame[total - 1] != 0x77) return false;
  uint16_t expected = (uint16_t)((frame[total - 3] << 8) | frame[total - 2]);
  uint16_t calc = jbdCrc(&frame[2], dataLen + 2);  // status + len + data
  if (expected != calc) {
    Serial.printf("[JBD] CRC-Fehler: erwartet 0x%04X, berechnet 0x%04X\n", expected, calc);
    return false;
  }
  return true;
}

bool request(uint8_t cmd) {
  if (!sendCommand(cmd)) {
    Serial.printf("[JBD] Schreiben von 0x%02X fehlgeschlagen\n", cmd);
    return false;
  }
  if (!waitForFrame(REPLY_TIMEOUT_MS)) {
    Serial.printf("[JBD] Keine Antwort auf 0x%02X\n", cmd);
    return false;
  }
  return checkFrame(cmd);
}

void parseBasicInfo() {
  const uint8_t* d = &frame[4];
  size_t n = frame[3];
  if (n < 23) {
    Serial.printf("[JBD] BasicInfo zu kurz: %u Bytes\n", (unsigned)n);
    return;
  }
  voltage        = ((d[0] << 8) | d[1]) / 100.0f;
  current        = (int16_t)((d[2] << 8) | d[3]) / 100.0f;
  cycleCharge    = ((d[4] << 8) | d[5]) / 100.0f;
  designCapacity = ((d[6] << 8) | d[7]) / 100;
  cycles         = (d[8] << 8) | d[9];
  batteryLevel   = d[19];
  chrgMosfet     = d[20] & 0x01;
  dischrgMosfet  = d[20] & 0x02;
  int ntc        = d[22];
  if (ntc > 8) ntc = 8;
  tempCount = 0;
  for (int i = 0; i < ntc && (size_t)(23 + i * 2 + 1) < n; i++) {
    uint16_t raw = (d[23 + i * 2] << 8) | d[24 + i * 2];
    tempValues[tempCount++] = (raw - 2731) / 10.0f;
  }
  power = voltage * current;
  Serial.printf("[JBD] %.2fV %.2fA SoC %d%%\n", voltage, current, batteryLevel);
}

void parseCellVoltages() {
  const uint8_t* d = &frame[4];
  cellCount = frame[3] / 2;
  if (cellCount > 32) cellCount = 32;
  for (int i = 0; i < cellCount; i++) {
    cellVoltages[i] = ((d[i * 2] << 8) | d[i * 2 + 1]) / 1000.0f;
  }
  Serial.printf("[JBD] %d Zellen gelesen\n", cellCount);
}

// ============ BLE VERBINDUNG ============
bool nameMatches(const std::string& n) {
  if (JBD_DEVICE_NAME[0] != '\0') return n == JBD_DEVICE_NAME;
  const char* prefixes[] = {"JBD-", "DWF-", "LSG-", "SX1", "SBL-", "OGR-", "TZ-H", "xiaoxiang"};
  for (const char* p : prefixes) {
    if (n.rfind(p, 0) == 0) return true;
  }
  return false;
}

bool connectToBMS() {
  Serial.println("[BLE] Scan startet...");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);
  pScan->setInterval(97);
  pScan->setWindow(37);
  pScan->setDuplicateFilter(true);

  NimBLEScanResults results = pScan->getResults(SCAN_TIME_MS, false);  // blockierend

  const NimBLEAdvertisedDevice* target = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    std::string name = dev->getName();
    if (name.empty()) continue;
    Serial.printf("[BLE] Gefunden: %s (%s)\n", name.c_str(), dev->getAddress().toString().c_str());
    if (nameMatches(name)) {
      target = dev;
      break;
    }
  }

  if (!target) {
    Serial.println("[BLE] Kein JBD-Geraet gefunden");
    pScan->clearResults();
    return false;
  }

  if (!pBLEClient) {
    pBLEClient = NimBLEDevice::createClient();
    pBLEClient->setClientCallbacks(&clientCB, false);
    pBLEClient->setConnectionParams(12, 24, 0, 200);
    pBLEClient->setConnectTimeout(CONNECT_TIMEOUT_MS);
  }

  Serial.printf("[BLE] Verbinde zu %s...\n", target->getAddress().toString().c_str());
  bool ok = pBLEClient->connect(target);
  pScan->clearResults();  // target ist ab hier ungueltig
  if (!ok) {
    Serial.println("[BLE] Verbindung fehlgeschlagen");
    return false;
  }

  NimBLERemoteService* svc = pBLEClient->getService(JBD_SERVICE_UUID);
  if (!svc) {
    Serial.println("[BLE] Service 0xFF00 nicht gefunden");
    pBLEClient->disconnect();
    return false;
  }
  pCharRx = svc->getCharacteristic(JBD_CHAR_RX);
  pCharTx = svc->getCharacteristic(JBD_CHAR_TX);
  if (!pCharRx || !pCharTx) {
    Serial.println("[BLE] Charakteristik 0xFF01/0xFF02 nicht gefunden");
    pBLEClient->disconnect();
    return false;
  }
  if (!pCharRx->subscribe(true, notifyCallback)) {
    Serial.println("[BLE] Subscribe auf 0xFF01 fehlgeschlagen");
    pBLEClient->disconnect();
    return false;
  }

  // Protokoll-Check: JBD braucht keine Init-Sequenz, BasicInfo muss direkt antworten
  delay(300);
  if (!request(JBD_CMD_BASIC)) {
    Serial.println("[BLE] Geraet antwortet nicht im JBD-Protokoll");
    pBLEClient->disconnect();
    return false;
  }
  parseBasicInfo();

  Serial.println("[BLE] JBD BMS verbunden");
  bmsConnected = true;
  return true;
}

// ============ MQTT ============
void publishStatus(const char* state) {
  if (mqttClient.connected()) mqttClient.publish(MQTT_TOPIC_ST, state, true);
}

void connectMQTT() {
  if (millis() - lastMqttAttempt < MQTT_RETRY_MS && lastMqttAttempt != 0) return;
  lastMqttAttempt = millis();

  char clientId[32];
  snprintf(clientId, sizeof(clientId), "esp32_jbd_bms_%06lx", (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));
  Serial.printf("[MQTT] Verbinde zu %s:%d...\n", MQTT_HOST, MQTT_PORT);

  bool ok = (MQTT_USER[0] != '\0')
              ? mqttClient.connect(clientId, MQTT_USER, MQTT_PASS, MQTT_TOPIC_ST, 1, true, "offline")
              : mqttClient.connect(clientId, nullptr, nullptr, MQTT_TOPIC_ST, 1, true, "offline");
  if (ok) {
    Serial.println("[MQTT] Verbunden");
    publishStatus(bmsConnected ? "online" : "bms_offline");
  } else {
    Serial.printf("[MQTT] Fehler %d\n", mqttClient.state());
  }
}

void publishData() {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["voltage"]  = serialized(String(voltage, 2));
  doc["current"]  = serialized(String(current, 2));
  doc["power"]    = serialized(String(power, 2));
  doc["soc"]      = batteryLevel;
  doc["battery_level"] = batteryLevel;
  doc["cycles"]   = cycles;
  doc["charge"]   = serialized(String(cycleCharge, 2));
  doc["capacity"] = designCapacity;
  doc["temp_count"] = tempCount;
  JsonArray temps = doc["temperatures"].to<JsonArray>();
  for (int i = 0; i < tempCount; i++) temps.add(serialized(String(tempValues[i], 1)));
  doc["cells"] = cellCount;
  JsonArray cells = doc["cell_voltages"].to<JsonArray>();
  for (int i = 0; i < cellCount; i++) cells.add(serialized(String(cellVoltages[i], 3)));
  doc["charging"]       = current > 0;
  doc["chrg_mosfet"]    = chrgMosfet;
  doc["dischrg_mosfet"] = dischrgMosfet;

  char buffer[1024];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  if (mqttClient.publish(MQTT_TOPIC, (const uint8_t*)buffer, n, false)) {
    Serial.printf("[MQTT] %u Bytes publiziert\n", (unsigned)n);
  } else {
    Serial.println("[MQTT] Publish fehlgeschlagen");
  }
}

// ============ BMS ABFRAGE ============
void queryBMS() {
  if (!pBLEClient || !pBLEClient->isConnected()) {
    bmsConnected = false;
    return;
  }
  if (!request(JBD_CMD_BASIC)) return;
  parseBasicInfo();
  delay(200);
  if (request(JBD_CMD_CELLS)) parseCellVoltages();
  publishData();
}

// ============ SETUP / LOOP ============
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 JBD BLE -> MQTT Bridge v2 (UNGETESTET) ===");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] Verbinde zu %s...\n", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] noch nicht verbunden, versuche im Hintergrund weiter");
  }

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(1024);   // Default 256 Bytes ist zu klein fuer die Zellspannungen
  mqttClient.setKeepAlive(60);

  NimBLEDevice::init("ESP32-JBD-Bridge");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();
  }

  if (!bmsConnected) {
    if (millis() >= nextBleAttempt) {
      if (connectToBMS()) {
        reconnectAttempts = 0;
        publishStatus("online");
        publishData();
        lastPoll = millis();
      } else {
        reconnectAttempts++;
        unsigned long waitS = min(reconnectAttempts * 10, 120);
        Serial.printf("[BLE] Neuer Versuch in %lus (Versuch %d)\n", waitS, reconnectAttempts);
        publishStatus("bms_offline");
        nextBleAttempt = millis() + waitS * 1000;
      }
    }
    delay(10);
    return;
  }

  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    queryBMS();
  }
  delay(10);
}
