#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// ===== SOZLAMALAR =====
const char* ssid = "Incubatsiya_2.4G";
const char* password = "123456789";
const char* serverUrl = "http://192.168.0.165:8000/verify";

#define FRAME_SIZE FRAMESIZE_QVGA   // 320x240
#define JPEG_QUALITY 12
#define SEND_INTERVAL_MS 3000

// ===== AI-Thinker ESP32-CAM pinlar =====
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define FLASH_GPIO_NUM     4

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAME_SIZE;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    Serial.println("PSRAM topildi — double buffer");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Kamera xato: 0x%x\n", err);
    delay(3000);
    ESP.restart();
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("WiFi ulanmoqda");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nUlandi! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi ulanmadi! Qayta urinish...");
    delay(3000);
    ESP.restart();
  }
}

bool sendPhoto(camera_fb_t* fb) {
  WiFiClient client;
  HTTPClient http;

  http.begin(client, serverUrl);
  http.setTimeout(10000);

  String boundary = "----ESP32Boundary";
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"img.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  uint32_t totalLen = head.length() + fb->len + tail.length();
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // Oddiy POST — kichik rasm uchun malloc xavfsiz
  if (fb->len > 50000) {
    Serial.println("Rasm juda katta, o'tkazib yuborildi");
    http.end();
    return false;
  }

  uint8_t* body = (uint8_t*)ps_malloc(totalLen);
  if (!body) {
    body = (uint8_t*)malloc(totalLen);
  }
  if (!body) {
    Serial.println("Xotira yetmadi!");
    http.end();
    return false;
  }

  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), fb->buf, fb->len);
  memcpy(body + head.length() + fb->len, tail.c_str(), tail.length());

  int httpCode = http.POST(body, totalLen);
  free(body);

  bool success = false;
  if (httpCode == 200) {
    String response = http.getString();
    Serial.printf("Javob: %s\n", response.c_str());

    if (response.indexOf("\"status\":\"ok\"") > -1) {
      // Tanildi — LED yoqish
      digitalWrite(FLASH_GPIO_NUM, HIGH);
      delay(300);
      digitalWrite(FLASH_GPIO_NUM, LOW);
      success = true;
    }
  } else {
    Serial.printf("HTTP xato: %d\n", httpCode);
  }

  http.end();
  return success;
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);

  Serial.println("\n\nESP32-CAM Face ID");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  initCamera();
  connectWiFi();

  Serial.println("Tayyor!");
}

void loop() {
  static unsigned long lastSend = 0;
  unsigned long now = millis();

  if (now - lastSend < SEND_INTERVAL_MS) {
    delay(100);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi uzildi, qayta ulanish...");
    connectWiFi();
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Kamera xato");
    delay(1000);
    return;
  }

  Serial.printf("Rasm: %d bytes, heap: %d\n", fb->len, ESP.getFreeHeap());
  sendPhoto(fb);
  esp_camera_fb_return(fb);

  lastSend = now;
}
