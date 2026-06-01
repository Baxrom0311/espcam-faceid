#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFiClientSecure.h>
#include <Preferences.h>

// ===== SOZLAMALAR =====
const char* DEFAULT_SSID = "Incubatsiya_2.4G";
const char* DEFAULT_PASSWORD = "123456789";
const char* DEFAULT_VERIFY_URL = "https://faceid.boos.uz/verify";
const char* SETUP_AP_SSID = "ESP32-CAM-Setup";
const char* SETUP_AP_PASSWORD = "12345678";

String wifiSsid;
String wifiPassword;
String verifyUrl;

#define VERIFY_INTERVAL_MS 5000
#define HEARTBEAT_INTERVAL_MS 5000
#define FLASH_GPIO_NUM 4
#define RELAY_GPIO_NUM 13
#define RELAY_ON_TIME_MS 20000
#define WIFI_CONNECT_TIMEOUT_MS 20000

static unsigned long relayOffTime = 0;
static unsigned long lastHeartbeat = 0;
static bool setupMode = false;
static Preferences prefs;

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
static httpd_handle_t status_httpd = NULL;
static httpd_handle_t setup_httpd = NULL;
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
  config.jpeg_quality = 15;
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

// ===== CONFIG =====
String htmlEscape(const String& value) {
  String out = value;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

String urlDecode(String input) {
  String decoded;
  char temp[] = "0x00";
  for (unsigned int i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < input.length()) {
      temp[2] = input[i + 1];
      temp[3] = input[i + 2];
      decoded += (char)strtol(temp, NULL, 16);
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

String formValue(const String& body, const String& key) {
  String token = key + "=";
  int start = body.indexOf(token);
  if (start < 0) return "";
  start += token.length();
  int end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  return urlDecode(body.substring(start, end));
}

void loadConfig() {
  prefs.begin("faceid", false);
  wifiSsid = prefs.getString("ssid", DEFAULT_SSID);
  wifiPassword = prefs.getString("password", DEFAULT_PASSWORD);
  verifyUrl = prefs.getString("verify_url", DEFAULT_VERIFY_URL);
}

void saveConfig(const String& ssid, const String& password, const String& url) {
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.putString("verify_url", url.length() ? url : DEFAULT_VERIFY_URL);
}

// ===== WIFI =====
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.print("WiFi ulanmoqda");
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nUlandi! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.println("\nWiFi ulanmadi!");
    return false;
  }
}

// ===== SETUP PORTAL =====
static esp_err_t setup_index_handler(httpd_req_t *req) {
  String page =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM WiFi Setup</title>"
    "<style>body{font-family:Arial,sans-serif;background:#0a0e1a;color:#e2e8f0;margin:0;padding:24px}"
    "main{max-width:420px;margin:auto}.card{background:#131a2e;border:1px solid #1e2a45;border-radius:10px;padding:20px}"
    "label{display:block;margin-top:14px;color:#94a3b8}input{width:100%;box-sizing:border-box;margin-top:6px;padding:12px;border-radius:8px;border:1px solid #334155;background:#020617;color:#e2e8f0}"
    "button{width:100%;margin-top:18px;padding:12px;border:0;border-radius:8px;background:#3b82f6;color:white;font-weight:700}"
    ".muted{color:#94a3b8;font-size:14px}</style></head><body><main><h2>ESP32-CAM WiFi Setup</h2>"
    "<div class='card'><p class='muted'>ESP32 serverga ulana olishi uchun WiFi nomi va parolini kiriting.</p>"
    "<form method='post' action='/wifi'>"
    "<label>WiFi nomi</label><input name='ssid' value='" + htmlEscape(wifiSsid) + "' required>"
    "<label>WiFi paroli</label><input name='password' type='password' value='" + htmlEscape(wifiPassword) + "'>"
    "<label>Server verify URL</label><input name='verify_url' value='" + htmlEscape(verifyUrl) + "' required>"
    "<button type='submit'>Saqlash va qayta ishga tushirish</button></form>"
    "<p class='muted'>Setup AP: " + String(SETUP_AP_SSID) + " / " + String(SETUP_AP_PASSWORD) + "</p>"
    "</div></main></body></html>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, page.c_str(), page.length());
}

static esp_err_t setup_wifi_handler(httpd_req_t *req) {
  int len = req->content_len;
  String body;
  body.reserve(len + 1);
  char buf[129];
  while (len > 0) {
    int chunk = httpd_req_recv(req, buf, min(len, 128));
    if (chunk <= 0) return ESP_FAIL;
    buf[chunk] = '\0';
    body += buf;
    len -= chunk;
  }

  String ssid = formValue(body, "ssid");
  String password = formValue(body, "password");
  String url = formValue(body, "verify_url");
  if (!ssid.length()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    return ESP_FAIL;
  }

  saveConfig(ssid, password, url);
  const char* response = "<!doctype html><html><body><h3>Saqlangan. ESP32 qayta ishga tushmoqda...</h3></body></html>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, response, strlen(response));
  delay(1000);
  ESP.restart();
  return ESP_OK;
}

void startSetupPortal() {
  setupMode = true;
  if (stream_httpd) {
    httpd_stop(stream_httpd);
    stream_httpd = NULL;
  }
  if (status_httpd) {
    httpd_stop(status_httpd);
    status_httpd = NULL;
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
  Serial.printf("Setup AP: %s, IP: %s\n", SETUP_AP_SSID, WiFi.softAPIP().toString().c_str());

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32770;
  config.max_uri_handlers = 4;
  config.stack_size = 8192;
  if (httpd_start(&setup_httpd, &config) == ESP_OK) {
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = setup_index_handler, .user_ctx = NULL };
    httpd_uri_t wifi_uri = { .uri = "/wifi", .method = HTTP_POST, .handler = setup_wifi_handler, .user_ctx = NULL };
    httpd_register_uri_handler(setup_httpd, &index_uri);
    httpd_register_uri_handler(setup_httpd, &wifi_uri);
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
    delay(120);
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

void markHeartbeatTime() {
  lastHeartbeat = millis();
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastHeartbeat < HEARTBEAT_INTERVAL_MS) return;

  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  String heartbeatUrl = verifyUrl;
  int pathStart = heartbeatUrl.indexOf("/verify");
  if (pathStart >= 0) {
    heartbeatUrl = heartbeatUrl.substring(0, pathStart) + "/esp32/heartbeat";
  } else {
    heartbeatUrl += "/esp32/heartbeat";
  }

  if (heartbeatUrl.startsWith("https://")) {
    secureClient.setInsecure();
    http.begin(secureClient, heartbeatUrl);
  } else {
    http.begin(plainClient, heartbeatUrl);
  }
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"rssi\":" + String(WiFi.RSSI()) +
                ",\"heap\":" + String((unsigned long)ESP.getFreeHeap()) +
                ",\"stream_url\":\"http://" + WiFi.localIP().toString() + "/stream\"}";
  int code = http.POST(body);
  if (code > 0) {
    markHeartbeatTime();
  }
  http.end();
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
    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    if (verifyUrl.startsWith("https://")) {
      secureClient.setInsecure(); // Skip cert verification
      http.begin(secureClient, verifyUrl);
    } else {
      http.begin(plainClient, verifyUrl);
    }
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
      markHeartbeatTime();
      String response = http.getString();
      Serial.printf("V: %s\n", response.c_str());
      if (response.indexOf("\"status\":\"ok\"") > -1) {
        faceRecognized = true;
        // Relay ON + LED flash
        digitalWrite(RELAY_GPIO_NUM, HIGH);
        relayOffTime = millis() + RELAY_ON_TIME_MS;
        digitalWrite(FLASH_GPIO_NUM, HIGH);
        vTaskDelay(pdMS_TO_TICKS(300));
        digitalWrite(FLASH_GPIO_NUM, LOW);
        Serial.println("RELAY ON — 20s");
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
  pinMode(RELAY_GPIO_NUM, OUTPUT);
  digitalWrite(RELAY_GPIO_NUM, LOW);

  Serial.println("\n\nESP32-CAM Face ID (esp_http_server)");

  loadConfig();
  initCamera();
  if (!connectWiFi()) {
    startSetupPortal();
    return;
  }
  startHttpServer();

  xTaskCreatePinnedToCore(verifyTask, "verify", 12288, NULL, 1, NULL, 0);

  Serial.printf("Stream: http://%s/stream\n", WiFi.localIP().toString().c_str());
}

// ===== LOOP =====
void loop() {
  if (setupMode) {
    delay(500);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi uzildi, qayta ulanmoqda...");
    if (!connectWiFi()) {
      startSetupPortal();
      return;
    }
  }

  sendHeartbeat();

  // Relay auto-off after 20s
  if (relayOffTime > 0 && millis() >= relayOffTime) {
    digitalWrite(RELAY_GPIO_NUM, LOW);
    relayOffTime = 0;
    Serial.println("RELAY OFF");
  }
  delay(100);
}
