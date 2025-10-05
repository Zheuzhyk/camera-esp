#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "bt_provision.h"

// weak-стаб enable_led (реальная в app_httpd.cpp)
extern "C" void enable_led(bool on) __attribute__((weak));
void enable_led(bool on) { (void)on; }

// ================== BLE (NimBLE) ==================
#include <NimBLEDevice.h>

// UART-like сервис (NUS-подобный)
static const char* kServiceUUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* kRX_UUID     = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // Write
static const char* kTX_UUID     = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // Notify

static const char* kBleName = "ESP32-CAM-Setup";
static const uint32_t kWifiConnectTimeoutMs = 12000;

static NimBLEServer*         s_server  = nullptr;
static NimBLECharacteristic* s_txChr   = nullptr; // notify to client
static NimBLECharacteristic* s_rxChr   = nullptr; // client writes here

static bool     s_bleActive  = false;
static uint32_t s_deadlineMs = 0;

// ---- RX assemble buffer for long writes ----
static String   s_rxBuf;
static uint32_t s_rxLastAt = 0;
static const uint32_t kRxAssembleQuietMs = 120;   // таймаут «тишины» между чанками
static const size_t   kRxMaxLen           = 2048;  // защитный лимит буфера

// ===== утилиты =====
static String trimCRLF(String s){ s.replace("\r",""); s.replace("\n",""); s.trim(); return s; }

static bool parseCreds(const String& line, String& outSsid, String& outPass) {
  int ssidPos = line.indexOf("ssid=");
  int passPos = line.indexOf("pass=");
  if (ssidPos < 0 || passPos < 0) return false;
  int ssidEnd = line.indexOf(';', ssidPos);
  if (ssidEnd < 0) ssidEnd = line.length();
  outSsid = line.substring(ssidPos + 5, ssidEnd); outSsid.trim();
  outPass = line.substring(passPos + 5);          outPass.trim();
  return outSsid.length() > 0;
}

bool WiFi_trySaved(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

static void bleNotify(const String& s) {
  if (s_txChr) { s_txChr->setValue((uint8_t*)s.c_str(), s.length()); s_txChr->notify(); }
  Serial.println(String("[BLE] ") + s);
}

static void handleCmd(const String& raw) {
  String line = trimCRLF(raw);
  if (!line.length()) return;

  if (line.equalsIgnoreCase("STATUS")) {
    if (WiFi.status() == WL_CONNECTED) bleNotify(WiFi.localIP().toString());
    else bleNotify("WIFI:DISCONNECTED");
    return;
  }
  if (line.equalsIgnoreCase("CLEAR")) {
    WiFi.disconnect(true, true);
    Preferences p; p.begin("wifi", false); p.clear(); p.end();
    bleNotify("CLEARED");
    return;
  }

  String ssid, pass;
  if (parseCreds(line, ssid, pass)) {
    bleNotify("TRY:" + ssid);
    WiFi.mode(WIFI_STA);
    enable_led(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t t0 = millis(); bool ok = false;
    while (millis() - t0 < kWifiConnectTimeoutMs) {
      if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
      delay(150);
    }
    enable_led(false);

    if (ok) {
      Preferences p; p.begin("wifi", false);
      p.putString("ssid", ssid); p.putString("pass", pass); p.end();
      bleNotify("OK " + WiFi.localIP().toString());
      s_deadlineMs = 1; // закроем окно в следующем loop
    } else {
      bleNotify("FAIL");
    }
    return;
  }

  bleNotify("ERR:UNKNOWN_CMD");
}

// ===== callbacks с поддержкой разных версий NimBLE =====
struct NimBLEConnInfo;               // forward-декларация на случай новых версий
struct ble_gap_conn_desc;            // forward для очень старых сигнатур

class RxCallbacks : public NimBLECharacteristicCallbacks {
public:
  // старые версии
  void onWrite(NimBLECharacteristic* chr) {
    std::string v = chr->getValue();
    if (v.empty()) return;

    // --- Сборка длинных сообщений из нескольких BLE-чанков ---
    // Защита от переполнения: если не влазит — сбрасываем буфер.
    if (s_rxBuf.length() + v.size() > kRxMaxLen) {
      s_rxBuf = "";
    }
    for (char c: v) s_rxBuf += c;
    s_rxLastAt = millis();

    // Если пришёл разделитель — разберём все готовые строки (CR/LF)
    int pos;
    while ( (pos = s_rxBuf.indexOf('\n')) >= 0 || (pos = s_rxBuf.indexOf('\r')) >= 0 ) {
      String line = s_rxBuf.substring(0, pos);
      // пропустим подряд идущие \r\n
      int drop = 1;
      while (pos+drop < (int)s_rxBuf.length() && (s_rxBuf[pos+drop]=='\r' || s_rxBuf[pos+drop]=='\n')) drop++;
      s_rxBuf.remove(0, pos + drop);
      line.trim();
      if (line.length()) handleCmd(line);
    }
  }
  // новые версии (с ConnInfo) — просто делегируем
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) { onWrite(chr); }
};

class ServerCallbacks : public NimBLEServerCallbacks {
public:
  void onConnect(NimBLEServer*) { Serial.println("[BLE] Central connected"); }
  void onDisconnect(NimBLEServer*) {
    Serial.println("[BLE] Central disconnected");
    NimBLEDevice::startAdvertising();
  }
  // на случай иных сигнатур — дубли
  void onConnect(NimBLEServer*, NimBLEConnInfo&) { onConnect(nullptr); }
  void onConnect(NimBLEServer*, ble_gap_conn_desc*) { onConnect(nullptr); }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&) { onDisconnect(nullptr); }
};

// ===== запуск / остановка BLE =====
static void startBLE() {
  if (s_bleActive) return;

  Serial.println("[BLE] Starting provisioning (NimBLE)...");
  NimBLEDevice::init(kBleName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  // (опционально) Попросим большую MTU — реальное значение согласуется клиентом
  NimBLEDevice::setMTU(247);

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = s_server->createService(kServiceUUID);
  s_txChr = svc->createCharacteristic(kTX_UUID, NIMBLE_PROPERTY::NOTIFY);
  s_rxChr = svc->createCharacteristic(kRX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  s_rxChr->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kServiceUUID);
  // adv->setScanResponse(true); // иногда недоступно — пропускаем
  adv->start();

  s_bleActive = true;

  Serial.printf("[BLE] READY. Name: %s\n", kBleName);
  Serial.println("[BLE] Use nRF Connect / LightBlue:");
  Serial.println("[BLE]  - Write to RX: STATUS | CLEAR | ssid=MyWiFi;pass=Secret123");
}

static void stopBLE() {
  if (!s_bleActive) return;
  Serial.println("[BLE] Stopping...");
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);
  s_bleActive = false;
}

// ===== публичные API =====
void BTProv_begin(uint32_t ttlMs) {
  startBLE();
  s_deadlineMs = ttlMs ? (millis() + ttlMs) : 0;
}

void BTProv_loop() {
  if (s_bleActive && s_deadlineMs != 0 && millis() > s_deadlineMs) {
    Serial.println("[BLE] TTL expired, closing");
    stopBLE();
    s_deadlineMs = 0;
  }

  // --- RX assemble: флаш по «тишине», даже если нет \n ---
  if (s_rxBuf.length() && (uint32_t)(millis() - s_rxLastAt) > kRxAssembleQuietMs) {
    String line = s_rxBuf; s_rxBuf = "";
    line.trim();
    if (line.length()) handleCmd(line);
  }
}
