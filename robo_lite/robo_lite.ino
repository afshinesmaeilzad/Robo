// ESP32-CAM WiFi robot, light version: arrow-key control, one picture on request.
// Join WiFi "Robo-CAM" (password robo12345), open http://192.168.4.1
//
// Everything runs over one WebSocket (/ws):
//   browser -> robot : drive commands ("f", "fl", "s", ...), echoed back,
//                      or "img" to ask for a picture
//   robot -> browser : that picture, as one binary JPEG frame
//
// No live video: the WiFi carries nothing but tiny command frames until you
// press the button, so driving stays responsive on a weak link.
//
// Kept deliberately small and fast:
//   - GRAB_LATEST with 2 buffers, so a fresh frame is always ready: taking a
//     picture is one capture, with no warm-up frames to throw away
//   - small page, no health polling, no diagnostics

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
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

const int PWM_FREQ = 1000;   // LEDC channels 2-5; the camera owns channel 0
const int PWM_BITS = 8;

const uint32_t CMD_TIMEOUT_MS = 500;  // stop if the browser goes quiet
const int RAMP_STEP = 20;             // soft start, limits the current spike

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
volatile int targetLeft = 0, targetRight = 0;
int curLeft = 0, curRight = 0;
volatile int wsFd = -1;

bool camReady = false;
TaskHandle_t picTask = NULL;
volatile bool sendBusy = false;
volatile framesize_t wantSize = FRAMESIZE_QVGA;  // 320x240
framesize_t curSize = FRAMESIZE_QVGA;

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

int rampStep(int cur, int tgt) {
  if ((cur > 0 && tgt < 0) || (cur < 0 && tgt > 0)) cur = 0;  // reversing: cut first
  if (abs(tgt) <= abs(cur)) return tgt;                        // slowing: immediate
  return tgt > cur ? min(cur + RAMP_STEP, tgt) : max(cur - RAMP_STEP, tgt);
}

void updateMotors() {
  int l = rampStep(curLeft, targetLeft);
  int r = rampStep(curRight, targetRight);
  if (l != curLeft)  { curLeft = l;  setMotor(LEFT_IN1, LEFT_IN2, l); }
  if (r != curRight) { curRight = r; setMotor(RIGHT_IN1, RIGHT_IN2, r); }
}

void applyDirection(const char *d) {
  lastCmdMs = millis();
  int s = speed, h = s / 3;
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
<title>Robo</title><style>
body{margin:0;background:#111;color:#eee;font:15px system-ui,sans-serif;display:flex;flex-direction:column;align-items:center;gap:10px;padding:10px}
#view{width:100%;max-width:480px;aspect-ratio:4/3;background:#000;border-radius:8px;display:flex;align-items:center;justify-content:center;color:#666;overflow:hidden}
#cam{width:100%;height:100%;object-fit:contain;display:none}
.pad{display:grid;grid-template-columns:repeat(3,64px);grid-template-rows:repeat(2,64px);gap:6px}
.pad button{font-size:24px;border:0;border-radius:8px;background:#333;color:#eee;touch-action:none}
.pad button.on{background:#2a7}
.row{display:flex;gap:14px;align-items:center;flex-wrap:wrap;justify-content:center}
select,button.b{background:#333;color:#eee;border:0;border-radius:6px;padding:6px 10px}
#snap{background:#2a7;font-size:16px;padding:10px 18px}#snap:disabled{background:#555}
#st{font-size:13px;color:#8a8}#st.bad{color:#e66}
</style></head><body>
<div id="view"><span id="hint">Press Take picture (or P)</span><img id="cam" alt=""></div>
<div class="pad">
  <span></span><button data-k="ArrowUp">&#9650;</button><span></span>
  <button data-k="ArrowLeft">&#9664;</button><button data-k="ArrowDown">&#9660;</button><button data-k="ArrowRight">&#9654;</button>
</div>
<div class="row">
  <button class="b" id="snap">Take picture</button>
  <label>Size <select id="res"><option value="qqvga">160x120</option><option value="qvga" selected>320x240</option><option value="vga">640x480</option></select></label>
  <label>Speed <input id="spd" type="range" min="80" max="255" value="200"></label>
  <button class="b" id="led">Light</button>
</div>
<div id="st">Connecting...</div>
<script>
const cam=document.getElementById('cam'),st=document.getElementById('st'),snap=document.getElementById('snap');
let ws=null,lastDir='s',sentAt=0,lastSend=0,lastEcho=0,rtt=0,askedAt=0,picInfo='';
function connect(){
  ws=new WebSocket('ws://'+location.hostname+'/ws');
  ws.binaryType='blob';
  ws.onopen=()=>{lastEcho=performance.now();send(lastDir);st.classList.remove('bad');};
  ws.onmessage=e=>{
    lastEcho=performance.now();
    if(typeof e.data!=='string'){show(e.data);return;}
    if(e.data!=='img')rtt=Math.round(lastEcho-sentAt);
  };
  ws.onclose=()=>{ws=null;snap.disabled=false;st.textContent='Reconnecting...';st.classList.add('bad');setTimeout(connect,300);};
  ws.onerror=()=>{try{ws.close();}catch(e){}};
}
function show(blob){
  const url=URL.createObjectURL(blob),old=cam.src;
  cam.onload=()=>{if(old.startsWith('blob:'))URL.revokeObjectURL(old);};
  cam.src=url;cam.style.display='block';
  document.getElementById('hint').style.display='none';
  picInfo=(blob.size/1024).toFixed(1)+' KB in '+Math.round(performance.now()-askedAt)+' ms';
  snap.disabled=false;
}
function takePicture(){
  if(snap.disabled)return;
  askedAt=performance.now();
  if(!send('img'))return;
  snap.disabled=true;
  setTimeout(()=>{snap.disabled=false;},5000);  // give up waiting after 5 s
}
snap.onclick=takePicture;
function send(d){if(ws&&ws.readyState===1){sentAt=lastSend=performance.now();ws.send(d);return true;}return false;}
setInterval(()=>{
  if(!ws||ws.readyState!==1)return;
  st.textContent='Connected - '+lastDir+' - '+rtt+' ms'+(snap.disabled?' - taking picture...':(picInfo?' - '+picInfo:''));
},1000);
setInterval(()=>{
  if(!ws||ws.readyState!==1)return;
  const now=performance.now();
  if(now-lastEcho>3000){ws.close();return;}
  if(lastDir!=='s'||now-lastSend>=1000)send(lastDir);
},200);
const held=new Set();
function dir(){
  const u=held.has('ArrowUp'),d=held.has('ArrowDown'),l=held.has('ArrowLeft'),r=held.has('ArrowRight');
  if(u&&l)return'fl';if(u&&r)return'fr';if(d&&l)return'bl';if(d&&r)return'br';
  if(u)return'f';if(d)return'b';if(l)return'l';if(r)return'r';return's';
}
function update(){
  const d=dir();
  if(d!==lastDir){lastDir=d;send(d);}
  document.querySelectorAll('.pad button').forEach(b=>b.classList.toggle('on',held.has(b.dataset.k)));
}
addEventListener('keydown',e=>{
  if(e.key===' '){held.clear();update();e.preventDefault();return;}
  if(e.key==='p'||e.key==='P'){takePicture();return;}
  if(e.key.startsWith('Arrow')){held.add(e.key);update();e.preventDefault();}
});
addEventListener('keyup',e=>{if(held.delete(e.key))update();});
addEventListener('blur',()=>{held.clear();update();});
document.querySelectorAll('.pad button').forEach(b=>{
  b.addEventListener('pointerdown',()=>{held.add(b.dataset.k);update();});
  ['pointerup','pointerleave','pointercancel'].forEach(ev=>b.addEventListener(ev,()=>{held.delete(b.dataset.k);update();}));
});
document.getElementById('spd').onchange=e=>fetch('/set?speed='+e.target.value);
document.getElementById('res').onchange=e=>fetch('/set?size='+e.target.value);
let led=0;
document.getElementById('led').onclick=()=>{led^=1;fetch('/set?led='+led);};
connect();
</script></body></html>)HTML";

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
  // The driver sizes its JPEG buffer from the init frame size, so init at the
  // largest size the page offers
  c.frame_size = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  c.jpeg_quality = 12;  // pictures are rare, so they can be good
  c.grab_mode = CAMERA_GRAB_LATEST;  // always a fresh frame ready, no warm-up needed
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  // The sensor keeps power across a reset: power-cycle it before probing
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
  s->set_framesize(s, curSize);
  esp_log_level_set("cam_hal", ESP_LOG_ERROR);
  return true;
}

// ---------- sending frames ----------

// One reusable buffer: only ever one frame in flight, and allocating per frame
// fragments the heap and eventually crashes the board.
static uint8_t *picBuf = NULL;
static size_t picBufCap = 0, picLen = 0;
static int picFd = -1;

static void sendWork(void *arg) {
  if (httpd_ws_get_fd_info(server, picFd) == HTTPD_WS_CLIENT_WEBSOCKET) {
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_BINARY;
    f.payload = picBuf;
    f.len = picLen;
    httpd_ws_send_frame_async(server, picFd, &f);
  }
  sendBusy = false;
}

static bool queueFrame(camera_fb_t *fb, int fd) {
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
  if (httpd_queue_work(server, sendWork, NULL) != ESP_OK) {
    sendBusy = false;
    return false;
  }
  return true;
}

// Sleeps until the page asks for a picture, so nothing but command frames use
// the WiFi while driving.
static void pictureTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int fd = wsFd;
    if (!camReady || fd < 0 || sendBusy) continue;

    if (wantSize != curSize) {
      sensor_t *s = esp_camera_sensor_get();
      s->set_framesize(s, (framesize_t)wantSize);
      curSize = wantSize;
      esp_camera_fb_return(esp_camera_fb_get());  // first frame after a resize is stale
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;
    queueFrame(fb, fd);
    esp_camera_fb_return(fb);
  }
}

static void requestPicture() {
  if (picTask) xTaskNotifyGive(picTask);
}

// ---------- HTTP ----------

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static bool queryParam(httpd_req_t *req, const char *key, char *out, size_t len) {
  char q[64];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  return httpd_query_key_value(q, key, out, len) == ESP_OK;
}

static esp_err_t setHandler(httpd_req_t *req) {
  char v[12];
  if (queryParam(req, "speed", v, sizeof(v))) speed = constrain(atoi(v), 0, 255);
  if (queryParam(req, "led", v, sizeof(v)))   digitalWrite(FLASH_LED, atoi(v) ? HIGH : LOW);
  if (queryParam(req, "size", v, sizeof(v))) {
    if      (!strcmp(v, "qqvga")) wantSize = FRAMESIZE_QQVGA;
    else if (!strcmp(v, "vga"))   wantSize = FRAMESIZE_VGA;
    else                          wantSize = FRAMESIZE_QVGA;
  }
  return httpd_resp_sendstr(req, "ok");
}

// One still picture, handy for testing with curl
static esp_err_t jpgHandler(httpd_req_t *req) {
  if (!camReady) return httpd_resp_send_500(req);
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t r = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return r;
}

// Text frames are drive commands, echoed back so the page can show the delay
static esp_err_t wsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {  // handshake
    int old = wsFd, fd = httpd_req_to_sockfd(req);
    if (old >= 0 && old != fd) httpd_sess_trigger_close(req->handle, old);
    wsFd = fd;
    int one = 1;  // send small packets right away, no Nagle delay
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    Serial.printf("[%lu ms] drive link open\n", millis());
    return ESP_OK;
  }
  uint8_t buf[8] = {0};
  httpd_ws_frame_t f = {};
  f.payload = buf;
  esp_err_t r = httpd_ws_recv_frame(req, &f, sizeof(buf) - 1);
  if (r != ESP_OK) return r;
  if (f.type == HTTPD_WS_TYPE_TEXT) {
    buf[f.len] = 0;
    if (!strcmp((const char *)buf, "img")) requestPicture();
    else applyDirection((const char *)buf);
    httpd_ws_frame_t echo = {};   // echo back: the page measures the round trip
    echo.type = HTTPD_WS_TYPE_TEXT;
    echo.payload = buf;
    echo.len = f.len;
    httpd_ws_send_frame(req, &echo);
  }
  return ESP_OK;
}

void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.max_uri_handlers = 8;
  cfg.lru_purge_enable = true;
  if (httpd_start(&server, &cfg) != ESP_OK) return;

  httpd_uri_t idx = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t set = {"/set", HTTP_GET, setHandler, NULL};
  httpd_uri_t jpg = {"/jpg", HTTP_GET, jpgHandler, NULL};
  httpd_uri_t ws = {};
  ws.uri = "/ws";
  ws.method = HTTP_GET;
  ws.handler = wsHandler;
  ws.is_websocket = true;
  httpd_register_uri_handler(server, &idx);
  httpd_register_uri_handler(server, &set);
  httpd_register_uri_handler(server, &jpg);
  httpd_register_uri_handler(server, &ws);
}

// ---------- main ----------

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // battery dips when WiFi starts
  Serial.begin(115200);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  camReady = initCamera();

  ledcAttachChannel(LEFT_IN1,  PWM_FREQ, PWM_BITS, 2);
  ledcAttachChannel(LEFT_IN2,  PWM_FREQ, PWM_BITS, 3);
  ledcAttachChannel(RIGHT_IN1, PWM_FREQ, PWM_BITS, 4);
  ledcAttachChannel(RIGHT_IN2, PWM_FREQ, PWM_BITS, 5);
  setMotor(LEFT_IN1, LEFT_IN2, 0);
  setMotor(RIGHT_IN1, RIGHT_IN2, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  esp_wifi_set_max_tx_power(78);  // 19.5 dBm: a solid link matters more than current
  Serial.printf("WiFi AP \"%s\" / \"%s\"  ->  http://%s  camera %s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str(), camReady ? "ok" : "FAILED");

  startServer();
  xTaskCreatePinnedToCore(pictureTask, "pic", 4096, NULL, 4, &picTask, 0);
}

void loop() {
  if ((targetLeft || targetRight) && millis() - lastCmdMs > CMD_TIMEOUT_MS) drive(0, 0);
  updateMotors();
  delay(5);
}
