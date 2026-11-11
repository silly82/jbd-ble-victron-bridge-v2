/*
 * ESP32 BLE → MQTT Bridge für JBD/Jiabaida/Xiaoxiang BMS
 * 
 * Liest JBD BMS per BLE aus (Service 0xFF00, Char ff01/ff02)
 * und published Daten per MQTT an VenusOS/Cerbo GX.
 * 
 * Basierend auf aiobmsble (patman15) Protokollanalyse.
 * 
 * Benötigte Libraries (PlatformIO / Arduino):
 *   - PubSubClient (MQTT)
 *   - ArduinoJson
 *   - NimBLE-Arduino (oder standard BLE)
 */

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

// JBD BMS Bluetooth-Name (z.B. "JBD-..." oder "JBD-12345678")
// Leer lassen = scannt nach erstem JBD-Gerät
const char* JBD_DEVICE_NAME = "";

// BLE Service/Charakteristik UUIDs (JBD)
const char* JBD_SERVICE_UUID = "0000ff00-0000-1000-8000-00805f9b34fb";
const char* JBD_CHAR_RX      = "0000ff01-0000-1000-8000-00805f9b34fb";  // notify
const char* JBD_CHAR_TX      = "0000ff02-0000-1000-8000-00805f9b34fb";  // write

// Polling-Intervall (ms)
const unsigned long POLL_INTERVAL = 30000;  // 30 Sekunden

// ============ PROTOTYPEN ============
void connectWiFi();
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
bool connectToBMS();
void queryBMS();
void requestBasicInfo();
void requestCellVoltages();
void publishData();
void publishStatus(const char* state);

// ============ GLOBALS ============
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

NimBLEClient* pBLEClient = nullptr;
NimBLERemoteService* pBmsService = nullptr;
NimBLERemoteCharacteristic* pCharRx = nullptr;  // notify
NimBLERemoteCharacteristic* pCharTx = nullptr;  // write

// BLE-Notification-Flag
bool notificationReceived = false;
uint8_t notifyBuffer[256];
size_t notifyLength = 0;
uint8_t expectedReplyType = 0;

// BMS-Daten
float voltage = 0;
float current = 0;
float cycleCharge = 0;
int designCapacity = 0;
int cycles = 0;
int batteryLevel = 0;
bool chrgMosfet = false;
bool dischrgMosfet = false;
int tempSensors = 0;
float tempValues[8] = {0};
int tempCount = 0;
int cellCount = 0;
float cellVoltages[32] = {0};
float power = 0;
unsigned long lastPoll = 0;
bool bmsConnected = false;
int reconnectAttempts = 0;

// ============ BLE CALLBACKS ============
class ClientCallback : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pClient) {
    Serial.println("[BLE] Verbindung getrennt");
    bmsConnected = false;
  }
};

static ClientCallback clientCB;

void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* data, size_t len, bool isNotify) {
  // Kopieren für Verarbeitung
  size_t copyLen = min(len, sizeof(notifyBuffer));
  memcpy(notifyBuffer, data, copyLen);
  notifyLength = copyLen;
  notificationReceived = true;
}

// ============ JBD PROTOKOLL ============
// CRC: 0x10000 - sum(frame)
uint16_t jbdCrc(const uint8_t* data, size_t len) {
  long sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return (uint16_t)(0x10000 - sum);
}

void sendInit() {
  uint8_t initCmd[] = {0xFF, 0xAA, 0x15, 0x01, 0x00};
  initCmd[4] = (0x15 + 0x01) & 0xFF;  // checksum
  Serial.println("[JBD] Sende Init...");
  pCharTx->writeValue(initCmd, 5, false);
}

void sendCommand(uint8_t cmd) {
  uint8_t frame[7];
  frame[0] = 0xDD;
  frame[1] = 0xA5;
  frame[2] = cmd;       // command
  frame[3] = 0x00;      // len

  uint16_t crc = jbdCrc(&frame[2], 2);  // CRC über cmd + len
  frame[4] = (crc >> 8) & 0xFF;
  frame[5] = crc & 0xFF;
  frame[6] = 0x77;      // tail

  expectedReplyType = cmd;
  notificationReceived = false;
  notifyLength = 0;

  Serial.printf("[JBD] Sende Command 0x%02X\n", cmd);
  pCharTx->writeValue(frame, 7, false);
}

bool waitForNotification(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (!notificationReceived) {
    if (millis() - start > timeoutMs) {
      Serial.println("[JBD] Timeout auf Notification");
      return false;
    }
    delay(10);
  }
  return true;
}

// Prüfe Frame-Integrität (CRC)
bool checkFrame(const uint8_t* data, size_t len) {
  if (len < 7) return false;  // min frame: header(2) + cmd(1) + len(1) + data + crc(2) + tail(1)
  
  int dataLen = data[3];       // Datenlänge
  int totalLen = 4 + dataLen + 3;  // header(4) + data + crc(2) + tail(1)
  
  if (len < (size_t)totalLen) return false;
  if (data[totalLen - 1] != 0x77) {
    Serial.println("[JBD] Falsches Frame-Ende (0x77 fehlt)");
    return false;
  }
  
  // CRC prüfen (über cmd + len + data)
  uint16_t expectedCrc = (uint16_t)((data[totalLen - 3] << 8) | data[totalLen - 2]);
  uint16_t calcCrc = jbdCrc(&data[2], dataLen + 2);  // cmd(1) + len(1) + data
  if (calcCrc != expectedCrc) {
    Serial.printf("[JBD] CRC Fehler: erwartet 0x%04X, berechnet 0x%04X\n", expectedCrc, calcCrc);
    return false;
  }
  
  return true;
}

void parseBasicInfo(const uint8_t* data, size_t len) {
  int dataLen = data[3];
  if (dataLen < 23) {
    Serial.printf("[JBD] BasicInfo zu kurz: %d bytes\n", dataLen);
    return;
  }
  
  // Voltage (pos 4, 2 bytes, /100)
  uint16_t vRaw = (data[4] << 8) | data[5];
  voltage = vRaw / 100.0;
  
  // Current (pos 6, 2 bytes, signed, /100)
  int16_t cRaw = (int16_t)((data[6] << 8) | data[7]);
  current = cRaw / 100.0;
  
  // Cycle charge (pos 8, 2 bytes, /100)
  uint16_t ccRaw = (data[8] << 8) | data[9];
  cycleCharge = ccRaw / 100.0;
  
  // Design capacity (pos 10, 2 bytes, //100)
  uint16_t dcRaw = (data[10] << 8) | data[11];
  designCapacity = dcRaw / 100;
  
  // Cycles (pos 12, 2 bytes)
  cycles = (data[12] << 8) | data[13];
  
  // Problem code (pos 20, 2 bytes)
  // Übersprungen
  
  // Battery level (pos 23, 1 byte)
  batteryLevel = data[23];
  
  // MOSFET status (pos 24, 1 byte)
  chrgMosfet = data[24] & 0x01;
  dischrgMosfet = data[24] & 0x02;
  
  // Temp sensors count (pos 26, 1 byte)
  tempSensors = data[26];
  if (tempSensors > 8) tempSensors = 8;
  
  // Temp values (start 27, each 2 bytes, unsigned, offset 2731, /10)
  tempCount = 0;
  for (int i = 0; i < tempSensors && (27 + i * 2 + 1) < len; i++) {
    uint16_t tRaw = (data[27 + i * 2] << 8) | data[27 + i * 2 + 1];
    tempValues[i] = (tRaw - 2731) / 10.0;
    tempCount++;
  }
  
  // Power berechnen
  power = voltage * current;
  
  Serial.printf("[JBD] Voltage: %.2fV, Current: %.2fA, SoC: %d%%, Temp: %.1f°C\n",
                voltage, current, batteryLevel, tempCount > 0 ? tempValues[0] : 0);
}

void parseCellVoltages(const uint8_t* data, size_t len) {
  int dataLen = data[3];
  cellCount = dataLen / 2;
  if (cellCount > 32) cellCount = 32;
  
  for (int i = 0; i < cellCount; i++) {
    uint16_t cvRaw = (data[4 + i * 2] << 8) | data[4 + i * 2 + 1];
    cellVoltages[i] = cvRaw / 1000.0;
  }
  
  Serial.printf("[JBD] %d Zellen: ", cellCount);
  for (int i = 0; i < min(cellCount, 4); i++) {
    Serial.printf("%.3fV ", cellVoltages[i]);
  }
  Serial.println(cellCount > 4 ? "..." : "");
}

// ============ BLE VERBINDUNG ============
bool connectToBMS() {
  if (bmsConnected && pBLEClient && pBLEClient->isConnected()) return true;
  
  Serial.println("[BLE] Scan starte...");
  
  // Suche nach JBD-Gerät
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);
  pScan->setInterval(97);
  pScan->setWindow(37);
  
  std::vector<NimBLEAdvertisedDevice*> devices = pScan->getResults(10);  // 10 Sekunden scan
  
  NimBLEAdvertisedDevice* target = nullptr;
  
  for (auto& device : devices) {
    String name = device->getName().c_str();
    Serial.printf("[BLE] Gefunden: %s (%s)\n", name.c_str(), device->getAddress().toString().c_str());
    
    if (JBD_DEVICE_NAME[0] != '\0') {
      // Exakter Name
      if (name == String(JBD_DEVICE_NAME)) {
        target = device;
        break;
      }
    } else {
      // Pattern-Matching (JBD-*, DWF*, LSG-*, SX1*, SBL-*, OGR-*)
      if (name.startsWith("JBD-") || name.startsWith("DWF-") || 
          name.startsWith("LSG-") || name.startsWith("SX1") ||
          name.startsWith("SBL-") || name.startsWith("OGR-") ||
          name.startsWith("TZ-H")) {
        target = device;
        break;
      }
    }
  }
  
  if (!target) {
    Serial.println("[BLE] Kein JBD-Gerät gefunden");
    return false;
  }
  
  Serial.printf("[BLE] Verbinde zu %s (%s)...\n", 
                target->getName().c_str(), target->getAddress().toString().c_str());
  
  pBLEClient = NimBLEDevice::createClient();
  pBLEClient->setClientCallbacks(&clientCB, false);
  pBLEClient->setConnectionParams(12, 24, 0, 100);
  pBLEClient->setConnectTimeout(10);
  
  if (!pBLEClient->connect(target)) {
    Serial.println("[BLE] Verbindung fehlgeschlagen");
    return false;
  }
  
  Serial.println("[BLE] Verbunden, suche Service...");
  
  pBmsService = pBLEClient->getService(JBD_SERVICE_UUID);
  if (!pBmsService) {
    Serial.println("[BLE] Service 0xFF00 nicht gefunden");
    pBLEClient->disconnect();
    return false;
  }
  
  pCharRx = pBmsService->getCharacteristic(JBD_CHAR_RX);
  pCharTx = pBmsService->getCharacteristic(JBD_CHAR_TX);
  
  if (!pCharRx || !pCharTx) {
    Serial.println("[BLE] Charakteristiken ff01/ff02 nicht gefunden");
    pBLEClient->disconnect();
    return false;
  }
  
  // Notification registrieren
  if (!pCharRx->subscribe(true, notifyCallback)) {
    Serial.println("[BLE] Subscribe auf ff01 fehlgeschlagen");
    pBLEClient->disconnect();
    return false;
  }
  
  delay(500);
  
  // Init-Sekvenz senden
  notificationReceived = false;
  sendInit();
  
  if (!waitForNotification(3000)) {
    Serial.println("[BLE] Init fehlgeschlagen (keine Antwort)");
    pBLEClient->disconnect();
    return false;
  }
  
  Serial.println("[BLE] JBD BMS erfolgreich verbunden!");
  bmsConnected = true;
  reconnectAttempts = 0;
  publishStatus("online");
  return true;
}

// ============ MQTT ============
void connectWiFi() {
  Serial.printf("[WiFi] Verbinde zu %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Verbunden, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Fehlgeschlagen!");
  }
}

void connectMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  
  char clientId[32];
  snprintf(clientId, 32, "esp32_jbd_bms_%06x", (uint32_t)ESP.getEfuseMac());
  
  Serial.printf("[MQTT] Verbinde zu %s:%d als %s...\n", MQTT_HOST, MQTT_PORT, clientId);
  
  if (mqttClient.connect(clientId, MQTT_USER, MQTT_PASS, MQTT_TOPIC_ST, 1, true, "offline")) {
    Serial.println("[MQTT] Verbunden");
    mqttClient.publish(MQTT_TOPIC_ST, "online", true);
  } else {
    Serial.printf("[MQTT] Fehler %d\n", mqttClient.state());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Für spätere Steuerbefehle
}

void publishStatus(const char* state) {
  if (mqttClient.connected()) {
    mqttClient.publish(MQTT_TOPIC_ST, state, true);
  }
}

void publishData() {
  if (!mqttClient.connected()) return;
  
  StaticJsonDocument<1024> doc;
  
  doc["voltage"] = serialized(String(voltage, 2));
  doc["current"] = serialized(String(current, 2));
  doc["power"] = serialized(String(power, 2));
  doc["soc"] = batteryLevel;
  doc["cycles"] = cycles;
  doc["charge"] = serialized(String(cycleCharge, 1));
  doc["capacity"] = designCapacity;
  doc["temp_count"] = tempCount;
  
  if (tempCount > 0) {
    JsonArray temps = doc.createNestedArray("temperatures");
    for (int i = 0; i < tempCount; i++) {
      temps.add(serialized(String(tempValues[i], 1)));
    }
  }
  
  doc["cells"] = cellCount;
  if (cellCount > 0) {
    JsonArray cells = doc.createNestedArray("cell_voltages");
    for (int i = 0; i < cellCount; i++) {
      cells.add(serialized(String(cellVoltages[i], 3)));
    }
  }
  
  doc["charging"] = current > 0;
  doc["chrg_mosfet"] = chrgMosfet;
  doc["dischrg_mosfet"] = dischrgMosfet;
  doc["battery_level"] = batteryLevel;
  
  char buffer[1024];
  size_t n = serializeJson(doc, buffer);
  
  if (mqttClient.publish(MQTT_TOPIC, buffer, false)) {
    Serial.printf("[MQTT] Daten published (%d bytes)\n", n);
  } else {
    Serial.println("[MQTT] Publish fehlgeschlagen");
  }
}

// ============ BMS QUERY ============
void queryBMS() {
  if (!pBLEClient || !pBLEClient->isConnected()) {
    bmsConnected = false;
    return;
  }
  
  // 1. Basic Info (0x03)
  sendCommand(0x03);
  if (waitForNotification(5000)) {
    if (checkFrame(notifyBuffer, notifyLength)) {
      parseBasicInfo(notifyBuffer, notifyLength);
    } else {
      Serial.println("[JBD] BasicInfo CRC-Fehler");
    }
  } else {
    Serial.println("[JBD] Keine Antwort auf BasicInfo");
    return;
  }
  
  // 2. Cell Voltages (0x04)
  delay(200);  // kurze Pause zwischen Requests
  sendCommand(0x04);
  if (waitForNotification(5000)) {
    if (checkFrame(notifyBuffer, notifyLength)) {
      parseCellVoltages(notifyBuffer, notifyLength);
    } else {
      Serial.println("[JBD] CellVoltages CRC-Fehler");
    }
  }
  
  // 3. Daten per MQTT publizieren
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) connectMQTT();
    if (mqttClient.connected()) publishData();
    mqttClient.loop();
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 JBD BLE → MQTT Bridge ===");
  Serial.printf("Firmware v1.0\n\n");
  
  NimBLEDevice::init("ESP32-JBD-Bridge");
  
  connectWiFi();
  connectMQTT();
  
  lastPoll = millis();
}

// ============ LOOP ============
void loop() {
  // MQTT verbunden halten
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      connectMQTT();
    }
    mqttClient.loop();
  } else {
    connectWiFi();
  }
  
  // BLE verbinden falls nötig
  if (!bmsConnected) {
    if (connectToBMS()) {
      lastPoll = millis();
    } else {
      reconnectAttempts++;
      int delaySec = min(reconnectAttempts * 10, 120);  // max 2 min
      Serial.printf("[BLE] Reconnect in %ds (Versuch %d)\n", delaySec, reconnectAttempts);
      delay(delaySec * 1000);
    }
    return;
  }
  
  // Polling-Intervall
  if (millis() - lastPoll >= POLL_INTERVAL) {
    queryBMS();
    lastPoll = millis();
  }
}
