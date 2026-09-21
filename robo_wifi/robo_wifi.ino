// ESP32-CAM WiFi robot: own access point, arrow-key control, pictures on request.
// Join WiFi "Robo-CAM" (password robo12345), open http://192.168.4.1
//
// Everything goes over one WebSocket (/ws):
//  - browser -> robot: drive commands ("f", "fl", "s", ...) as tiny text frames,
//    echoed back so the page can show the delay. "img" asks for a picture.
//  - robot -> browser: the picture as one binary JPEG frame.
// The camera idles until a picture is asked for, so normally only tiny command
// frames use the WiFi.

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "lwip/sockets.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

const char *AP_SSID = "Robo-CAM";
const char *AP_PASS = "robo12345";

// Full transmit power: a solid link matters more than battery life. Lower
// values (8.5 and 13 dBm) were tried to save current and made the link slow
// and unreliable. Feed the ESP32 from 4xAA or a power bank instead.
const wifi_power_t WIFI_TX_POWER = WIFI_POWER_19_5dBm;

// L293D inputs (same wiring as motor_test)
const int LEFT_IN1  = 14;
const int LEFT_IN2  = 15;
const int RIGHT_IN1 = 13;
const int RIGHT_IN2 = 12;
const int FLASH_LED = 4;

// LEDC channels 2-5 (timers 1-2); the camera uses channel 0 / timer 0 for XCLK
const int PWM_FREQ = 1000;
const int PWM_BITS = 8;

// Stop the motors if the browser stops sending commands (lost WiFi, closed tab).
// While moving, the page resends the current command every 200 ms.
const uint32_t CMD_TIMEOUT_MS = 500;

// Soft start: max PWM change per 5 ms tick when speeding up. Limits the current
// spike when the motors start, which can brown out the ESP32 on AA batteries.
// Slowing down and stopping are immediate.
const int RAMP_STEP = 20;

// Frames thrown away before a picture (the buffered one is stale)
const int CAM_WARMUP_FRAMES = 2;

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
volatile int targetLeft = 0, targetRight = 0;  // set by commands
int curLeft = 0, curRight = 0;                 // applied by loop() with ramp
volatile int wsFd = -1;                        // socket of the driving WebSocket

// Pictures
bool camReady = false;
SemaphoreHandle_t camLock;                     // one capture at a time
TaskHandle_t picTask = NULL;
volatile bool sendBusy = false;                // a picture is queued / being sent
volatile bool streaming = false;               // video mode: keep sending frames
volatile int streamFps = 5;                    // video frame-rate cap (1-25)
volatile framesize_t wantSize = FRAMESIZE_QVGA;
volatile uint32_t picSent = 0, picFailed = 0, lastPicBytes = 0, lastPicMs = 0;

// Diagnostics shown on the page: restarts since power-on survive a reset in RTC RAM
RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t bootCount;
int apChannel = 1;

httpd_handle_t server = NULL;

// ---------- motors ----------

void setMotor(int in1, int in2, int value) {
  value = constrain(value, -255, 255);
  ledcWrite(in1, value > 0 ? value : 0);
  ledcWrite(in2, value < 0 ? -value : 0);
}

void drive(int left, int right) {
  targetLeft = left;
  targetRight = right;
}

// One ramp step from cur toward tgt
int rampStep(int cur, int tgt) {
  if ((cur > 0 && tgt < 0) || (cur < 0 && tgt > 0)) cur = 0;  // reversing: cut first
  if (abs(tgt) <= abs(cur)) return tgt;                         // slowing: immediate
  return tgt > cur ? min(cur + RAMP_STEP, tgt) : max(cur - RAMP_STEP, tgt);
}

void updateMotors() {
  int l = rampStep(curLeft, targetLeft);
  int r = rampStep(curRight, targetRight);
  if (l != curLeft)  { curLeft = l;  setMotor(LEFT_IN1, LEFT_IN2, l); }
  if (r != curRight) { curRight = r; setMotor(RIGHT_IN1, RIGHT_IN2, r); }
}

void applyDirection(const char *d) {
  lastCmdMs = millis();  // before the targets, so loop() never sees a new target with an old time
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
#view{width:100%;max-width:640px;aspect-ratio:4/3;background:#000;border-radius:8px;display:flex;align-items:center;justify-content:center;color:#666;overflow:hidden}
#cam{width:100%;height:100%;object-fit:contain;display:none}
.pad{display:grid;grid-template-columns:repeat(3,64px);grid-template-rows:repeat(2,64px);gap:6px}
.pad button{font-size:24px;border:0;border-radius:8px;background:#333;color:#eee;touch-action:none}
.pad button.on{background:#2a7}
.row{display:flex;gap:16px;align-items:center;flex-wrap:wrap;justify-content:center}
select,button.b{background:#333;color:#eee;border:0;border-radius:6px;padding:6px 10px}
#snap{background:#2a7;font-size:16px;padding:10px 18px}
#snap:disabled{background:#555}
#st{font-size:13px;color:#8a8}
#st.bad{color:#e66}
#diag{font-size:12px;color:#888;text-align:center}
</style></head><body>
<div id="view"><span id="hint">Press "Take picture" (or P) to see what the robot sees</span><img id="cam" alt="picture"></div>
<div class="pad">
  <span></span><button data-k="ArrowUp">▲</button><span></span>
  <button data-k="ArrowLeft">◀</button><button data-k="ArrowDown">▼</button><button data-k="ArrowRight">▶</button>
</div>
<div class="row">
  <label>View <select id="mode">
    <option value="photo" selected>Photo (on request)</option><option value="video">Video (uses more power)</option>
  </select></label>
  <label id="fpslabel" style="display:none">FPS <select id="fps">
    <option value="1">1</option><option value="2">2</option><option value="5" selected>5</option><option value="10">10</option><option value="15">15</option><option value="25">max</option>
  </select></label>
  <button class="b" id="snap">📷 Take picture</button>
  <label>Size <select id="res">
    <option value="qqvga">160x120</option><option value="qvga" selected>320x240</option><option value="vga">640x480</option>
  </select></label>
  <label>Speed <input id="spd" type="range" min="80" max="255" value="200"></label>
  <button class="b" id="led">Light</button>
</div>
<div id="st">Connecting…</div>
<div id="diag"></div>
<script>
const host = location.hostname;
const cam = document.getElementById('cam');
const snap = document.getElementById('snap');
const st = document.getElementById('st');
function status(t, bad){ st.textContent = t; st.classList.toggle('bad', !!bad); }

// ---- one WebSocket: commands out (echoed back), pictures in ----
let ws = null, lastDir = 's', sentAt = 0, lastSend = 0, lastEcho = 0, rtt = 0;
let askedAt = 0, picAt = 0, picInfo = '', video = false;
function connect(){
  ws = new WebSocket('ws://' + host + '/ws');
  ws.binaryType = 'blob';
  ws.onopen = () => { lastEcho = performance.now(); send(lastDir); if (video) send('v1'); };
  ws.onmessage = e => {
    lastEcho = performance.now();
    if (typeof e.data !== 'string') { showPicture(e.data); return; }
    if (e.data !== 'img') rtt = Math.round(lastEcho - sentAt);
    showStatus();
  };
  ws.onclose = () => { ws = null; snap.disabled = false; status('Reconnecting…', true); setTimeout(connect, 300); };
  ws.onerror = () => { try { ws.close(); } catch(e){} };
}
function showPicture(blob){
  const url = URL.createObjectURL(blob);
  const old = cam.src;
  cam.onload = () => { if (old.startsWith('blob:')) URL.revokeObjectURL(old); };
  cam.src = url;
  cam.style.display = 'block';
  document.getElementById('hint').style.display = 'none';
  picAt = Date.now();
  frames++;
  picInfo = (blob.size / 1024).toFixed(1) + ' KB in ' + Math.round(performance.now() - askedAt) + ' ms';
  snap.disabled = false;
  showStatus();
}
let frames = 0, fps = 0;
setInterval(() => { fps = frames; frames = 0; }, 1000);
function showStatus(){
  let t = 'Connected · ' + lastDir + ' · ' + rtt + ' ms';
  if (video) t += ' · video ' + fps + ' fps (' + picInfo + ')';
  else if (snap.disabled) t += ' · taking picture…';
  else if (picAt) t += ' · picture ' + Math.round((Date.now() - picAt) / 1000) + ' s ago (' + picInfo + ')';
  status(t);
}
function send(d){
  if (ws && ws.readyState === 1) { sentAt = lastSend = performance.now(); ws.send(d); return true; }
  return false;
}
function takePicture(){
  if (snap.disabled) return;
  askedAt = performance.now();
  if (!send('img')) return;
  snap.disabled = true;
  setTimeout(() => { snap.disabled = false; }, 5000);  // give up waiting after 5 s
  showStatus();
}
snap.onclick = takePicture;
const mode = document.getElementById('mode');
mode.onchange = () => {
  video = mode.value === 'video';
  snap.style.display = video ? 'none' : '';
  document.getElementById('fpslabel').style.display = video ? '' : 'none';
  snap.disabled = false;
  askedAt = performance.now();
  send(video ? 'v1' : 'v0');
  showStatus();
};
connect();
// Keep-alive: every 200 ms while moving (robot stops after 500 ms of silence),
// every second while stopped, so a dead link is noticed quickly.
setInterval(() => {
  if (!ws || ws.readyState !== 1) return;
  const now = performance.now();
  if (now - lastEcho > 3000) { ws.close(); return; }
  if (lastDir !== 's' || now - lastSend >= 1000) send(lastDir);
}, 200);
setInterval(() => { if (ws && ws.readyState === 1) showStatus(); }, 1000);

const held = new Set();
function dir(){
  const u = held.has('ArrowUp'), d = held.has('ArrowDown'), l = held.has('ArrowLeft'), r = held.has('ArrowRight');
  if (u && l) return 'fl'; if (u && r) return 'fr';
  if (d && l) return 'bl'; if (d && r) return 'br';
  if (u) return 'f'; if (d) return 'b'; if (l) return 'l'; if (r) return 'r';
  return 's';
}
function update(){
  const d = dir();
  if (d !== lastDir) { lastDir = d; send(d); }
  document.querySelectorAll('.pad button').forEach(b => b.classList.toggle('on', held.has(b.dataset.k)));
}
addEventListener('keydown', e => {
  if (e.key === ' ') { held.clear(); update(); e.preventDefault(); return; }
  if (e.key === 'p' || e.key === 'P') { takePicture(); return; }
  if (e.key.startsWith('Arrow')) { held.add(e.key); update(); e.preventDefault(); }
});
addEventListener('keyup', e => { if (held.delete(e.key)) update(); });
addEventListener('blur', () => { held.clear(); update(); });
document.querySelectorAll('.pad button').forEach(b => {
  b.addEventListener('pointerdown', () => { held.add(b.dataset.k); update(); });
  ['pointerup','pointerleave','pointercancel'].forEach(ev => b.addEventListener(ev, () => { held.delete(b.dataset.k); update(); }));
});

// ---- robot health: a restart count going up means power problems ----
const diag = document.getElementById('diag');
let lastUp = -1;
async function health(){
  try {
    const i = await (await fetch('/info', {cache: 'no-store'})).json();
    const restarted = lastUp >= 0 && i.up < lastUp;
    lastUp = i.up;
    diag.textContent = 'Robot up ' + i.up + ' s · restarts ' + i.boots + ' (last: ' + i.reset + ')' +
      ' · WiFi ch ' + i.ch + ' · signal ' + i.rssi + ' dBm · ' + (i.video ? 'video on' : 'photo mode') +
      ' · pictures ' + i.pics +
      (i.picfail ? ' (' + i.picfail + ' failed)' : '') + (i.cam ? '' : ' · camera not found');
    diag.style.color = (restarted || i.boots > 0) ? '#e66' : '#888';
  } catch (e) { diag.textContent = 'Robot not answering'; diag.style.color = '#e66'; }
}
health();
setInterval(health, 10000);

document.getElementById('spd').onchange = e => fetch('/set?speed=' + e.target.value);
document.getElementById('res').onchange = e => fetch('/set?size=' + e.target.value);
document.getElementById('fps').onchange = e => fetch('/set?fps=' + e.target.value);
let led = 0;
document.getElementById('led').onclick = () => { led ^= 1; fetch('/set?led=' + led); };
</script></body></html>)HTML";

// ---------- camera ----------

// The driver is started once and idles between pictures (CAMERA_GRAB_WHEN_EMPTY:
// no capture/DMA work while nobody takes a frame). Putting the sensor to sleep
// (PWDN pin, COM2 standby, or restarting the driver) proved unreliable on this
// board: the driver hangs or the sensor stops answering.
static int jpegBrightness(camera_fb_t *fb);
static bool warmupLog = false;  // print warm-up frames (serial self-test)
static framesize_t curSize = FRAMESIZE_QVGA;

// Wake the camera, let it settle, and return one fresh frame (or NULL).
// Caller must esp_camera_fb_return() the frame, then call camRelease().
static camera_fb_t *camCapture(int warmup) {
  if (!camReady) return NULL;
  xSemaphoreTake(camLock, portMAX_DELAY);
  if (wantSize != curSize) {
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, (framesize_t)wantSize);
    curSize = wantSize;
  }
  // The buffered frame was captured right after the last picture: throw it away
  for (int i = 0; i < warmup; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      if (warmupLog) Serial.printf("  warm-up %d: %u bytes, brightness %d\n", i, fb->len, jpegBrightness(fb));
      esp_camera_fb_return(fb);
    }
  }
  return esp_camera_fb_get();
}

static void camRelease() {
  xSemaphoreGive(camLock);
}

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
  // The driver sizes its JPEG buffer from the init frame size, so init at the
  // largest size the page offers; frames that don't fit are dropped (FB-OVF).
  c.frame_size = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  c.jpeg_quality = 12;  // lower = sharper; pictures are rare so they can be good
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;  // capture only when a frame is taken
  c.fb_count = 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  // The sensor keeps power across an ESP32 reset and can be left in a bad state:
  // power-cycle it with PWDN before probing, and retry a few times
  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
    if (attempt) esp_camera_deinit();
    pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, HIGH);
    delay(100);
    digitalWrite(PWDN_GPIO_NUM, LOW);
    delay(100);
    err = esp_camera_init(&c);
  }
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);
  esp_log_level_set("cam_hal", ESP_LOG_ERROR);
  return true;
}

// ---------- sending pictures ----------

// One reusable buffer: only one picture is ever in flight (sendBusy), and
// allocating per frame in video mode fragments the heap and can crash the board.
static uint8_t *picBuf = NULL;
static size_t picBufCap = 0;
static int picFd = -1;
static size_t picLen = 0;

// Runs inside the HTTP server task, so it never collides with command replies
static void sendPicWork(void *arg) {
  if (httpd_ws_get_fd_info(server, picFd) == HTTPD_WS_CLIENT_WEBSOCKET) {
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_BINARY;
    f.payload = picBuf;
    f.len = picLen;
    httpd_ws_send_frame_async(server, picFd, &f);
  }
  sendBusy = false;
}

static bool queuePicture(camera_fb_t *fb, int fd) {
  if (fb->len > picBufCap) {
    uint8_t *grown = (uint8_t *)heap_caps_realloc(picBuf, fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!grown) grown = (uint8_t *)realloc(picBuf, fb->len);
    if (!grown) return false;
    picBuf = grown;
    picBufCap = fb->len;
  }
  memcpy(picBuf, fb->buf, fb->len);
  picFd = fd;
  picLen = fb->len;
  sendBusy = true;
  if (httpd_queue_work(server, sendPicWork, NULL) != ESP_OK) {
    sendBusy = false;
    return false;
  }
  return true;
}

// Sends one picture (on "img") or a stream of them (video mode). In video mode
// the next frame is only captured once the previous one has been handed to the
// network, so video can never queue up in front of drive commands.
static void sendOnePicture(int fd, int warmup) {
  uint32_t t0 = millis();
  camera_fb_t *fb = camCapture(warmup);
  if (fb) {
    lastPicBytes = fb->len;
    if (queuePicture(fb, fd)) picSent++; else picFailed++;
    esp_camera_fb_return(fb);
  } else {
    picFailed++;
  }
  camRelease();
  lastPicMs = millis() - t0;
}

void setStreaming(bool on);

static void pictureTask(void *) {
  uint32_t lastFrameMs = 0;
  for (;;) {
    if (!streaming) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // sleeps until a picture is asked for
      int fd = wsFd;
      if (fd >= 0 && !sendBusy) sendOnePicture(fd, CAM_WARMUP_FRAMES);
      continue;
    }
    int fd = wsFd;
    if (fd < 0) { setStreaming(false); continue; }
    vTaskDelay(pdMS_TO_TICKS(5));  // always yield: a tight loop trips the watchdog
    if (sendBusy) continue;        // wait for the last frame to go out
    uint32_t frameMs = 1000 / constrain(streamFps, 1, 25);
    if (millis() - lastFrameMs < frameMs) continue;
    lastFrameMs = millis();
    sendOnePicture(fd, 0);  // frames are already flowing: no warm-up needed
  }
}

static void requestPicture() {
  if (picTask) xTaskNotifyGive(picTask);
}

// Changing the CPU clock at runtime hung the board, so it stays at full speed
void setStreaming(bool on) {
  if (streaming == on) return;
  streaming = on;
  Serial.printf("[%lu ms] video %s\n", millis(), on ? "on" : "off");
  if (on) requestPicture();
}

// ---------- HTTP handlers ----------

static void noDelay(int fd) {
  int one = 1;  // send small packets right away (no Nagle delay)
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static bool queryParam(httpd_req_t *req, const char *key, char *out, size_t len) {
  char q[64];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  return httpd_query_key_value(q, key, out, len) == ESP_OK;
}

// WebSocket /ws: text frames are drive commands (echoed back) or "img" (take a picture)
static esp_err_t wsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {  // handshake
    // Only one driver: close the previous drive link so dead links don't pile up
    int old = wsFd, fd = httpd_req_to_sockfd(req);
    if (old >= 0 && old != fd) httpd_sess_trigger_close(req->handle, old);
    wsFd = fd;
    noDelay(fd);
    Serial.printf("[%lu ms] drive link open\n", millis());
    return ESP_OK;
  }
  uint8_t buf[8];
  httpd_ws_frame_t f = {};
  f.payload = buf;
  esp_err_t r = httpd_ws_recv_frame(req, &f, sizeof(buf) - 1);
  if (r != ESP_OK) return r;
  if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
  buf[f.len] = 0;
  wsFd = httpd_req_to_sockfd(req);
  if (!strcmp((const char *)buf, "img")) requestPicture();
  else if (!strcmp((const char *)buf, "v1")) setStreaming(true);
  else if (!strcmp((const char *)buf, "v0")) setStreaming(false);
  else applyDirection((const char *)buf);
  return httpd_ws_send_frame(req, &f);
}

// Stop immediately when the driving WebSocket closes
static void onClose(httpd_handle_t hd, int fd) {
  if (fd == wsFd) {
    wsFd = -1;
    drive(0, 0);
    setStreaming(false);
    Serial.printf("[%lu ms] drive link closed\n", millis());
  }
  close(fd);
}

// Plain HTTP drive command, kept for curl / scripts
static esp_err_t goHandler(httpd_req_t *req) {
  char d[4] = "s";
  queryParam(req, "d", d, sizeof(d));
  applyDirection(d);
  noDelay(httpd_req_to_sockfd(req));
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "ok");
}

static esp_err_t setHandler(httpd_req_t *req) {
  char v[12];
  if (queryParam(req, "speed", v, sizeof(v))) speed = constrain(atoi(v), 0, 255);
  if (queryParam(req, "led", v, sizeof(v)))   digitalWrite(FLASH_LED, atoi(v) ? HIGH : LOW);
  if (queryParam(req, "fps", v, sizeof(v)))   streamFps = constrain(atoi(v), 1, 25);
  if (queryParam(req, "size", v, sizeof(v))) {
    // Applied at the next picture, while the camera is awake
    if (!strcmp(v, "qqvga")) wantSize = FRAMESIZE_QQVGA;
    else if (!strcmp(v, "vga")) wantSize = FRAMESIZE_VGA;
    else wantSize = FRAMESIZE_QVGA;
  }
  return httpd_resp_sendstr(req, "ok");
}

static const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_BROWNOUT: return "brownout (low battery)";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog (froze)";
    case ESP_RST_SW:       return "software";
    case ESP_RST_EXT:      return "reset button";
    default:               return "other";
  }
}

// Robot health for the page: uptime, restarts, signal strength, picture stats
static esp_err_t infoHandler(httpd_req_t *req) {
  int rssi = 0;
  wifi_sta_list_t sta;
  if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num > 0) rssi = sta.sta[0].rssi;
  char out[256];
  snprintf(out, sizeof(out),
           "{\"up\":%lu,\"boots\":%lu,\"reset\":\"%s\",\"ch\":%d,\"rssi\":%d,\"clients\":%d,\"heap\":%lu,"
           "\"cam\":%d,\"video\":%d,\"fps\":%d,\"pics\":%lu,\"picfail\":%lu,\"picbytes\":%lu,\"picms\":%lu}",
           (unsigned long)(millis() / 1000), (unsigned long)bootCount, resetReason(), apChannel,
           rssi, WiFi.softAPgetStationNum(), (unsigned long)ESP.getMinFreeHeap(),
           camReady ? 1 : 0, streaming ? 1 : 0, streamFps, (unsigned long)picSent, (unsigned long)picFailed,
           (unsigned long)lastPicBytes, (unsigned long)lastPicMs);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, out);
}

// Single JPEG snapshot, for curl / scripts
static esp_err_t jpgHandler(httpd_req_t *req) {
  if (!camReady) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "camera not found");
  camera_fb_t *fb = camCapture(CAM_WARMUP_FRAMES);
  esp_err_t r;
  if (fb) {
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    r = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
  } else {
    r = httpd_resp_send_500(req);
  }
  camRelease();
  return r;
}

void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.task_priority = tskIDLE_PRIORITY + 8;  // above the picture task
  cfg.max_uri_handlers = 8;
  // lwIP has only 16 sockets in total (the server also uses 2 internally).
  // Stay under that so the server's own limit is hit first and the oldest
  // connection is purged, instead of accept() failing (ENFILE) and hanging.
  cfg.max_open_sockets = 6;
  cfg.lru_purge_enable = true;
  cfg.keep_alive_enable = true;  // detect dead clients (laptop left WiFi) within ~4 s
  cfg.keep_alive_idle = 2;
  cfg.keep_alive_interval = 1;
  cfg.keep_alive_count = 2;
  cfg.send_wait_timeout = 2;
  cfg.recv_wait_timeout = 2;
  cfg.close_fn = onClose;
  httpd_uri_t idx  = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t go   = {"/go", HTTP_GET, goHandler, NULL};
  httpd_uri_t set  = {"/set", HTTP_GET, setHandler, NULL};
  httpd_uri_t info = {"/info", HTTP_GET, infoHandler, NULL};
  httpd_uri_t jpg  = {"/jpg", HTTP_GET, jpgHandler, NULL};
  httpd_uri_t ws   = {};
  ws.uri = "/ws";
  ws.method = HTTP_GET;
  ws.handler = wsHandler;
  ws.is_websocket = true;
  if (httpd_start(&server, &cfg) == ESP_OK) {
    httpd_register_uri_handler(server, &idx);
    httpd_register_uri_handler(server, &go);
    httpd_register_uri_handler(server, &set);
    httpd_register_uri_handler(server, &info);
    httpd_register_uri_handler(server, &jpg);
    httpd_register_uri_handler(server, &ws);
  }
}

// ---------- WiFi ----------

// Scan once at boot and use the least crowded of channels 1, 6, 11. A busy
// channel (e.g. the home router) causes lag spikes and dropped connections.
int pickChannel() {
  const int cand[3] = {1, 6, 11};
  long load[3] = {0, 0, 0};
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    long w = constrain(100 + WiFi.RSSI(i), 1, 100);  // stronger neighbour = more interference
    for (int c = 0; c < 3; c++)
      if (abs(ch - cand[c]) < 5) load[c] += w;
  }
  WiFi.scanDelete();
  int best = 0;
  for (int c = 1; c < 3; c++)
    if (load[c] < load[best]) best = c;
  Serial.printf("WiFi scan: %d networks, load ch1=%ld ch6=%ld ch11=%ld\n", n, load[0], load[1], load[2]);
  return cand[best];
}

// ---------- main ----------

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // battery dips when WiFi starts
  Serial.begin(115200);
  if (bootMagic != 0x520B0 || esp_reset_reason() == ESP_RST_POWERON) { bootMagic = 0x520B0; bootCount = 0; }
  else bootCount++;
  Serial.printf("Boot: reset reason %s, restarts since power-on %lu\n", resetReason(), (unsigned long)bootCount);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  camLock = xSemaphoreCreateMutex();
  camReady = initCamera();

  ledcAttachChannel(LEFT_IN1,  PWM_FREQ, PWM_BITS, 2);
  ledcAttachChannel(LEFT_IN2,  PWM_FREQ, PWM_BITS, 3);
  ledcAttachChannel(RIGHT_IN1, PWM_FREQ, PWM_BITS, 4);
  ledcAttachChannel(RIGHT_IN2, PWM_FREQ, PWM_BITS, 5);
  setMotor(LEFT_IN1, LEFT_IN2, 0);
  setMotor(RIGHT_IN1, RIGHT_IN2, 0);

  apChannel = pickChannel();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, apChannel);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_TX_POWER);
  Serial.printf("WiFi AP \"%s\" / \"%s\" on channel %d  ->  http://%s\n", AP_SSID, AP_PASS, apChannel,
                WiFi.softAPIP().toString().c_str());

  startServer();
  if (camReady)
    xTaskCreatePinnedToCore(pictureTask, "pictures", 4096, NULL, tskIDLE_PRIORITY + 2, &picTask, 1);
}

// Average brightness (0-255) of a JPEG, from a 1/8 scale decode
static int jpegBrightness(camera_fb_t *fb) {
  size_t n = (fb->width / 8) * (fb->height / 8);
  uint8_t *rgb = (uint8_t *)malloc(n * 2);
  if (!rgb) return -1;
  long sum = -1;
  if (jpg2rgb565(fb->buf, fb->len, rgb, JPG_SCALE_8X)) {
    sum = 0;
    for (size_t i = 0; i < n; i++) {
      uint16_t v = (rgb[2 * i + 1] << 8) | rgb[2 * i];
      sum += (((v >> 11) << 3) * 77 + (((v >> 5) & 63) << 2) * 150 + ((v & 31) << 3) * 29) >> 8;
    }
    sum /= n;
  }
  free(rgb);
  return sum;
}

// Serial "p": take 3 pictures the same way the button does (camera check without WiFi)
void cameraSelfTest() {
  warmupLog = true;
  for (int i = 0; i < 3; i++) {
    uint32_t t = millis();
    camera_fb_t *fb = camCapture(CAM_WARMUP_FRAMES);
    if (fb) {
      Serial.printf("picture %d: %ux%u %u bytes, brightness %d, in %lu ms\n", i, fb->width, fb->height, fb->len,
                    jpegBrightness(fb), millis() - t);
      esp_camera_fb_return(fb);
    } else {
      Serial.printf("picture %d: FAILED\n", i);
    }
    camRelease();
    delay(1000);
  }
  warmupLog = false;
}

void loop() {
  if (Serial.available() && Serial.read() == 'p') cameraSelfTest();
  // Read in reverse of the write order (time, then targets) since commands arrive on another core
  bool active = targetLeft || targetRight;
  uint32_t last = lastCmdMs;
  if (active && millis() - last > CMD_TIMEOUT_MS) drive(0, 0);
  updateMotors();
  delay(5);
}
