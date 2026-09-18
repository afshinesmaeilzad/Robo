// ESP32-CAM WiFi robot: own access point, low-res MJPEG video, arrow-key control.
// Join WiFi "Robo-CAM" (password robo12345), open http://192.168.4.1

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

const char *AP_SSID = "Robo-CAM";
const char *AP_PASS = "robo12345";

// L293D inputs (same wiring as motor_test)
const int LEFT_IN1  = 14;
const int LEFT_IN2  = 15;
const int RIGHT_IN1 = 13;
const int RIGHT_IN2 = 12;
const int FLASH_LED = 4;

// LEDC channels 2-5 (timers 1-2); the camera uses channel 0 / timer 0 for XCLK
const int PWM_FREQ = 1000;
const int PWM_BITS = 8;

// Stop the motors if the browser stops sending commands (lost WiFi, closed tab)
const uint32_t CMD_TIMEOUT_MS = 600;

// AI-Thinker ESP32-CAM camera pins
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

volatile int speed = 200;
volatile uint32_t lastCmdMs = 0;
volatile bool moving = false;

httpd_handle_t ctrlServer = NULL;
httpd_handle_t streamServer = NULL;

// ---------- motors ----------

void setMotor(int in1, int in2, int value) {
  value = constrain(value, -255, 255);
  ledcWrite(in1, value > 0 ? value : 0);
  ledcWrite(in2, value < 0 ? -value : 0);
}

void drive(int left, int right) {
  setMotor(LEFT_IN1, LEFT_IN2, left);
  setMotor(RIGHT_IN1, RIGHT_IN2, right);
  moving = (left != 0 || right != 0);
}

void applyDirection(const char *d) {
  int s = speed;
  int h = s / 3;  // inner wheel speed on curves
  if      (!strcmp(d, "f"))  drive(s, s);
  else if (!strcmp(d, "b"))  drive(-s, -s);
  else if (!strcmp(d, "l"))  drive(-s, s);
  else if (!strcmp(d, "r"))  drive(s, -s);
  else if (!strcmp(d, "fl")) drive(h, s);
  else if (!strcmp(d, "fr")) drive(s, h);
  else if (!strcmp(d, "bl")) drive(-h, -s);
  else if (!strcmp(d, "br")) drive(-s, -h);
  else                       drive(0, 0);
}

// ---------- web page ----------

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Robo</title>
<style>
body{margin:0;background:#111;color:#eee;font-family:system-ui,sans-serif;display:flex;flex-direction:column;align-items:center;gap:12px;padding:12px}
#cam{width:100%;max-width:640px;aspect-ratio:4/3;background:#000;border-radius:8px;object-fit:contain;image-rendering:pixelated}
.pad{display:grid;grid-template-columns:repeat(3,64px);grid-template-rows:repeat(2,64px);gap:6px}
.pad button{font-size:24px;border:0;border-radius:8px;background:#333;color:#eee;touch-action:none}
.pad button.on{background:#2a7}
.row{display:flex;gap:16px;align-items:center;flex-wrap:wrap;justify-content:center}
select,button.b{background:#333;color:#eee;border:0;border-radius:6px;padding:6px 10px}
#st{font-size:13px;color:#8a8}
</style></head><body>
<img id="cam" alt="video">
<div class="pad">
  <span></span><button data-k="ArrowUp">▲</button><span></span>
  <button data-k="ArrowLeft">◀</button><button data-k="ArrowDown">▼</button><button data-k="ArrowRight">▶</button>
</div>
<div class="row">
  <label>Speed <input id="spd" type="range" min="80" max="255" value="200"></label>
  <label>Video <select id="res">
    <option value="qqvga">160x120</option><option value="qvga" selected>320x240</option><option value="vga">640x480</option>
  </select></label>
  <button class="b" id="led">Light</button>
</div>
<div id="st">Arrow keys to drive · Space = stop</div>
<script>
const host = location.hostname;
const cam = document.getElementById('cam');
const st = document.getElementById('st');
function startStream(){ cam.src = 'http://' + host + ':81/stream?t=' + Date.now(); }
cam.onerror = () => setTimeout(startStream, 1000);
startStream();

const held = new Set();
let lastDir = 's';
function dir(){
  const u = held.has('ArrowUp'), d = held.has('ArrowDown'), l = held.has('ArrowLeft'), r = held.has('ArrowRight');
  if (u && l) return 'fl'; if (u && r) return 'fr';
  if (d && l) return 'bl'; if (d && r) return 'br';
  if (u) return 'f'; if (d) return 'b'; if (l) return 'l'; if (r) return 'r';
  return 's';
}
function send(d){
  fetch('/go?d=' + d).then(() => st.textContent = 'Connected · ' + d).catch(() => st.textContent = 'No connection');
}
function update(){
  const d = dir();
  if (d !== lastDir) { lastDir = d; send(d); }
  document.querySelectorAll('.pad button').forEach(b => b.classList.toggle('on', held.has(b.dataset.k)));
}
// keep-alive while moving so the robot's safety timeout doesn't stop it
setInterval(() => { if (lastDir !== 's') send(lastDir); }, 250);

addEventListener('keydown', e => {
  if (e.key === ' ') { held.clear(); update(); e.preventDefault(); return; }
  if (e.key.startsWith('Arrow')) { held.add(e.key); update(); e.preventDefault(); }
});
addEventListener('keyup', e => { if (held.delete(e.key)) update(); });
addEventListener('blur', () => { held.clear(); update(); });

document.querySelectorAll('.pad button').forEach(b => {
  b.addEventListener('pointerdown', () => { held.add(b.dataset.k); update(); });
  ['pointerup','pointerleave','pointercancel'].forEach(ev => b.addEventListener(ev, () => { held.delete(b.dataset.k); update(); }));
});

document.getElementById('spd').onchange = e => fetch('/set?speed=' + e.target.value);
document.getElementById('res').onchange = e => fetch('/set?size=' + e.target.value).then(startStream);
let led = 0;
document.getElementById('led').onclick = () => { led ^= 1; fetch('/set?led=' + led); };
</script></body></html>)HTML";

// ---------- HTTP handlers ----------

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static bool queryParam(httpd_req_t *req, const char *key, char *out, size_t len) {
  char q[64];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  return httpd_query_key_value(q, key, out, len) == ESP_OK;
}

static esp_err_t goHandler(httpd_req_t *req) {
  char d[4] = "s";
  queryParam(req, "d", d, sizeof(d));
  applyDirection(d);
  lastCmdMs = millis();
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "ok");
}

static esp_err_t setHandler(httpd_req_t *req) {
  char v[12];
  if (queryParam(req, "speed", v, sizeof(v))) speed = constrain(atoi(v), 0, 255);
  if (queryParam(req, "led", v, sizeof(v)))   digitalWrite(FLASH_LED, atoi(v) ? HIGH : LOW);
  if (queryParam(req, "size", v, sizeof(v))) {
    sensor_t *s = esp_camera_sensor_get();
    framesize_t fs = FRAMESIZE_QVGA;
    if (!strcmp(v, "qqvga")) fs = FRAMESIZE_QQVGA;
    else if (!strcmp(v, "vga")) fs = FRAMESIZE_VGA;
    s->set_framesize(s, fs);
  }
  return httpd_resp_sendstr(req, "ok");
}

#define PART_BOUNDARY "robofrm"
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_SEP = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, STREAM_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char hdr[64];
  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { delay(10); continue; }
    size_t n = snprintf(hdr, sizeof(hdr), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, n);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
  }
  return res;
}

void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  httpd_uri_t idx = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t go  = {"/go", HTTP_GET, goHandler, NULL};
  httpd_uri_t set = {"/set", HTTP_GET, setHandler, NULL};
  if (httpd_start(&ctrlServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(ctrlServer, &idx);
    httpd_register_uri_handler(ctrlServer, &go);
    httpd_register_uri_handler(ctrlServer, &set);
  }

  cfg.server_port = 81;
  cfg.ctrl_port = 32769;
  httpd_uri_t stream = {"/stream", HTTP_GET, streamHandler, NULL};
  if (httpd_start(&streamServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream);
  }
}

// ---------- camera ----------

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QVGA;
  c.jpeg_quality = 14;  // higher = smaller/lower quality
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (psramFound()) {
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

// ---------- main ----------

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // battery dips when WiFi starts
  Serial.begin(115200);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  initCamera();

  ledcAttachChannel(LEFT_IN1,  PWM_FREQ, PWM_BITS, 2);
  ledcAttachChannel(LEFT_IN2,  PWM_FREQ, PWM_BITS, 3);
  ledcAttachChannel(RIGHT_IN1, PWM_FREQ, PWM_BITS, 4);
  ledcAttachChannel(RIGHT_IN2, PWM_FREQ, PWM_BITS, 5);
  drive(0, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  Serial.printf("WiFi AP \"%s\" / \"%s\"  ->  http://%s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());

  startServers();
}

void loop() {
  if (moving && millis() - lastCmdMs > CMD_TIMEOUT_MS) {
    drive(0, 0);
  }
  delay(20);
}
