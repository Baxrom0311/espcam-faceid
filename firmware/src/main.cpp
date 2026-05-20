#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ===== SOZLAMALAR =====
const char* ssid = "Incubatsiya_2.4G";
const char* password = "123456789";
const char* verifyUrl = "http://192.168.0.165:8000/verify";

#define VERIFY_INTERVAL_MS 4000
#define FLASH_GPIO_NUM 4

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

static httpd_handle_t stream_httpd = NULL;
static volatile bool faceRecognized = false;

// ===== MJPEG boundary =====
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ===== KAMERA INIT =====
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
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.fb_count = 2;
    Serial.printf("PSRAM: bor\n");
  } else {
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    Serial.printf("PSRAM: yo'q\n");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Kamera xato: 0x%x — qayta urinish\n", err);
    delay(1000);
    esp_camera_deinit();
    delay(500);
    err = esp_camera_init(&config);
    if (err != ESP_OK) {
      Serial.printf("Kamera 2-urinish xato: 0x%x — restart\n", err);
      delay(2000);
      ESP.restart();
    }
  }
  Serial.println("Kamera tayyor!");
}

// ===== WIFI =====
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
    Serial.println("\nWiFi ulanmadi!");
    delay(3000);
    ESP.restart();
  }
}

// ===== /stream — MJPEG via httpd_resp_send_chunk =====
static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = ESP_OK;
  char partBuf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { delay(50); continue; }

    size_t hlen = snprintf(partBuf, 64, STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, partBuf, hlen);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
    delay(50);
  }
  return res;
}

// ===== /capture — bitta rasm =====
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ===== /status — JSON =====
static esp_err_t status_handler(httpd_req_t *req) {
  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"status\":\"online\",\"ip\":\"%s\",\"heap\":%lu,\"rssi\":%d,\"face\":%s}",
    WiFi.localIP().toString().c_str(),
    (unsigned long)ESP.getFreeHeap(),
    WiFi.RSSI(),
    faceRecognized ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, buf, strlen(buf));
}

// ===== HTTP SERVER INIT =====
void startHttpServer() {
  // Stream server on port 80
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 2;
  config.stack_size = 8192;
  config.core_id = 1;

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &capture_uri);
  }

  // Status server on port 81
  httpd_handle_t status_httpd = NULL;
  httpd_config_t config2 = HTTPD_DEFAULT_CONFIG();
  config2.server_port = 81;
  config2.ctrl_port = 32769;
  config2.max_uri_handlers = 2;
  config2.core_id = 0;

  if (httpd_start(&status_httpd, &config2) == ESP_OK) {
    httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    httpd_register_uri_handler(status_httpd, &status_uri);
  }

  Serial.println("HTTP servers ishga tushdi (80+81)");
}

// ===== VERIFY — alohida task =====
void verifyTask(void* param) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(VERIFY_INTERVAL_MS));
    if (WiFi.status() != WL_CONNECTED) continue;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;
    size_t len = fb->len;
    uint8_t *imgBuf = (uint8_t*)ps_malloc(len);
    if (!imgBuf) imgBuf = (uint8_t*)malloc(len);
    if (!imgBuf) { esp_camera_fb_return(fb); continue; }
    memcpy(imgBuf, fb->buf, len);
    esp_camera_fb_return(fb);

    // Now send to server (camera is free for stream)
    WiFiClient client;
    HTTPClient http;
    http.begin(client, verifyUrl);
    http.setTimeout(5000);

    String boundary = "----ESP32Boundary";
    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"file\"; filename=\"img.jpg\"\r\n"
                  "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    uint32_t totalLen = head.length() + len + tail.length();
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    uint8_t *body = (uint8_t*)ps_malloc(totalLen);
    if (!body) body = (uint8_t*)malloc(totalLen);
    if (!body) { free(imgBuf); http.end(); continue; }

    memcpy(body, head.c_str(), head.length());
    memcpy(body + head.length(), imgBuf, len);
    memcpy(body + head.length() + len, tail.c_str(), tail.length());
    free(imgBuf);

    int httpCode = http.POST(body, totalLen);
    free(body);

    if (httpCode == 200) {
      String response = http.getString();
      Serial.printf("V: %s\n", response.c_str());
      if (response.indexOf("\"status\":\"ok\"") > -1) {
        faceRecognized = true;
        digitalWrite(FLASH_GPIO_NUM, HIGH);
        vTaskDelay(pdMS_TO_TICKS(300));
        digitalWrite(FLASH_GPIO_NUM, LOW);
      } else {
        faceRecognized = false;
      }
    } else {
      faceRecognized = false;
    }
    http.end();
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);

  Serial.println("\n\nESP32-CAM Face ID (esp_http_server)");

  initCamera();
  connectWiFi();
  startHttpServer();

  xTaskCreatePinnedToCore(verifyTask, "verify", 12288, NULL, 1, NULL, 0);

  Serial.printf("Stream: http://%s/stream\n", WiFi.localIP().toString().c_str());
}

// ===== LOOP =====
void loop() {
  delay(1000);
}
