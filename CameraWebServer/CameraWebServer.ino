#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"

#include "board_config.h"
#include "bt_provision.h"

// Из app_httpd.cpp
extern void startCameraServer();
extern void setupLedFlash();

static const uint32_t kBTProvisionTTLms = 180000; // 3 мин
static bool serverStarted = false;

static bool init_camera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) { config.frame_size = FRAMESIZE_SVGA; config.jpeg_quality = 10; config.fb_count = 2; }
  else { config.frame_size = FRAMESIZE_VGA; config.jpeg_quality = 12; config.fb_count = 1; }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[BOOT] Camera init failed 0x%x\n", err);
    return false;
  }
  return true;
}

static void maybeStartServerOnce() {
  if (!serverStarted && WiFi.status() == WL_CONNECTED) {
    startCameraServer();
    serverStarted = true;
    Serial.printf("[BOOT] Camera server: http://%s/\n", WiFi.localIP().toString().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-CAM BOOT ===");

  // Камера (если не взлетит — продолжаем ради BT провижининга)
  if (!init_camera()) {
    Serial.println("[BOOT] Camera not available. Web stream will be disabled.");
  }

  setupLedFlash();

  // Wi-Fi из NVS -> если нет, открываем окно BT
  if (!WiFi_trySaved(8000)) {
    Serial.println("[BOOT] No saved Wi-Fi -> opening BT provisioning...");
    BTProv_begin(kBTProvisionTTLms);
  } else {
    Serial.printf("[BOOT] Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  }

  maybeStartServerOnce();
}

void loop() {
  BTProv_loop();            // SPP команды: STATUS / CLEAR / ssid=...;pass=...

  maybeStartServerOnce();   // поднимет сервер, когда появится Wi-Fi
  delay(10);
}
