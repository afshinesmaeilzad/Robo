// Automatic L293D check: the robot tests its own motors using the camera.
//
// No WiFi, no web page, nobody watching. For each L293D input the board takes a
// grayscale picture, drives that input alone for a moment, takes another picture,
// and measures how far the view slid sideways. Driving one wheel pivots the
// robot, so the scene sweeps across the camera: that sideways slide is the proof
// the motor really turned.
//
// PUT THE ROBOT ON THE FLOOR with room to pivot, pointing at something with
// visible detail (furniture, a patterned wall). Wheels spinning in the air move
// nothing, so the camera would see no change and every test would fail.
//
// The verdicts do not depend on which way the camera is mounted. They come from
// comparing the tests against each other:
//   - the two inputs of one wheel must slide the view opposite ways
//   - the same direction on the other wheel must also slide it the opposite way
//
// Serial, 115200 baud:  a = run the check again,  s = stop

#include <Arduino.h>
#include "esp_camera.h"

const int LEFT_IN1  = 14;   // L293D pin 2
const int LEFT_IN2  = 15;   // L293D pin 7
const int RIGHT_IN1 = 13;   // L293D pin 10
const int RIGHT_IN2 = 12;   // L293D pin 15
const int FLASH_LED = 4;

const int PWM_FREQ = 1000;
const int PWM_BITS = 8;

const int TEST_SPEED = 230;   // strong enough to move a loaded robot
const int DRIVE_MS   = 600;   // how long each input runs
const int SETTLE_MS  = 450;   // let the robot stop rocking before looking

// A slide of this many pixels or more counts as real movement
const int MOVE_PIXELS = 3;
// Largest slide we look for (also the correlation search range)
const int MAX_SHIFT = 28;
// Below this, the view has too little detail to measure anything
const int MIN_TEXTURE = 12;

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

const int IMG_W = 160;   // QQVGA grayscale
const int IMG_H = 120;

bool camReady = false;

struct Result {
  const char *name;
  int pin;
  int shift;       // sideways slide in pixels, signed
  int diff;        // average pixel change, a second opinion
  bool moved;
};

Result results[4];

// ---------- motors ----------

void allOff() {
  ledcWrite(LEFT_IN1, 0);
  ledcWrite(LEFT_IN2, 0);
  ledcWrite(RIGHT_IN1, 0);
  ledcWrite(RIGHT_IN2, 0);
}

// ---------- seeing ----------

// One column profile: the brightness of each column, averaged down the image.
// Collapsing to 160 numbers makes the comparison cheap and ignores small
// vertical wobble, which is exactly what a pivoting robot produces.
static bool grabProfile(int16_t *profile) {
  if (!camReady) return false;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb || fb->len < (size_t)(IMG_W * IMG_H)) {
    if (fb) esp_camera_fb_return(fb);
    return false;
  }
  int32_t sums[IMG_W] = {0};
  for (int y = 0; y < IMG_H; y++) {
    const uint8_t *row = fb->buf + y * IMG_W;
    for (int x = 0; x < IMG_W; x++) sums[x] += row[x];
  }
  esp_camera_fb_return(fb);

  int32_t mean = 0;
  for (int x = 0; x < IMG_W; x++) mean += sums[x] / IMG_H;
  mean /= IMG_W;
  for (int x = 0; x < IMG_W; x++) profile[x] = (int16_t)(sums[x] / IMG_H - mean);
  return true;
}

// How much detail the view has. A blank wall gives almost none, and then no
// measurement means anything.
static int texture(const int16_t *profile) {
  int32_t total = 0;
  for (int x = 1; x < IMG_W; x++) total += abs(profile[x] - profile[x - 1]);
  return total / (IMG_W - 1);
}

// Slide "after" across "before" and find the offset where they line up best
// (smallest average difference). That offset is how far the scene travelled.
static int bestShift(const int16_t *before, const int16_t *after, int *quality) {
  int32_t bestScore = INT32_MAX, worstScore = 0;
  int best = 0;
  for (int s = -MAX_SHIFT; s <= MAX_SHIFT; s++) {
    int32_t total = 0;
    int count = 0;
    for (int x = 0; x < IMG_W; x++) {
      int j = x + s;
      if (j < 0 || j >= IMG_W) continue;
      total += abs(before[x] - after[j]);
      count++;
    }
    if (count < IMG_W / 2) continue;
    int32_t score = total / count;
    if (score < bestScore) { bestScore = score; best = s; }
    if (score > worstScore) worstScore = score;
  }
  // How much better the winner is than the worst alignment: a real slide has a
  // clear winner, noise does not.
  *quality = worstScore > 0 ? (int)(100 - (bestScore * 100) / worstScore) : 0;
  return best;
}

// Average pixel change between two profiles, used only as a sanity check
static int profileDiff(const int16_t *a, const int16_t *b) {
  int32_t total = 0;
  for (int x = 0; x < IMG_W; x++) total += abs(a[x] - b[x]);
  return total / IMG_W;
}

// ---------- one test ----------

static int16_t before[IMG_W], after[IMG_W];

static Result testInput(const char *name, int pin) {
  Result r = {name, pin, 0, 0, false};
  Serial.printf("\n%-26s GPIO%-3d ", name, pin);

  allOff();
  delay(250);
  if (!grabProfile(before)) { Serial.print("camera failed"); return r; }

  ledcWrite(pin, TEST_SPEED);
  delay(DRIVE_MS);
  allOff();
  delay(SETTLE_MS);

  if (!grabProfile(after)) { Serial.print("camera failed"); return r; }

  int quality = 0;
  r.shift = bestShift(before, after, &quality);
  r.diff = profileDiff(before, after);
  r.moved = abs(r.shift) >= MOVE_PIXELS && quality >= 25;

  Serial.printf("slide %+3d px  change %3d  %s",
                r.shift, r.diff, r.moved ? "MOVED" : "no movement");
  return r;
}

// ---------- the check ----------

void runCheck() {
  if (!camReady) {
    Serial.println("\nCamera not working, so the robot cannot watch itself.");
    Serial.println("Use motor_check instead, which needs you to watch.");
    return;
  }

  Serial.println("\n\n================ automatic L293D check ================");
  Serial.println("Robot on the floor, facing something with visible detail.");
  Serial.println("Starting in 3 seconds...");
  delay(3000);

  if (!grabProfile(before)) {
    Serial.println("Cannot read the camera.");
    return;
  }
  int detail = texture(before);
  Serial.printf("View detail: %d ", detail);
  if (detail < MIN_TEXTURE) {
    Serial.println("- TOO PLAIN");
    Serial.println("The camera sees a blank surface, so movement cannot be measured.");
    Serial.println("Point the robot at furniture or a patterned wall and send 'a'.");
    return;
  }
  Serial.println("- good");

  results[0] = testInput("LEFT  IN1 (pin 2)",  LEFT_IN1);
  results[1] = testInput("LEFT  IN2 (pin 7)",  LEFT_IN2);
  results[2] = testInput("RIGHT IN3 (pin 10)", RIGHT_IN1);
  results[3] = testInput("RIGHT IN4 (pin 15)", RIGHT_IN2);
  allOff();

  Serial.println("\n\n---------------------- verdict ----------------------");

  int movedCount = 0;
  for (int i = 0; i < 4; i++) if (results[i].moved) movedCount++;

  if (movedCount == 0) {
    Serial.println("FAIL: nothing moved on any input.");
    Serial.println("  1. Are the grounds joined? Battery -, L293D pins 4/5/12/13, ESP32-CAM GND.");
    Serial.println("  2. Is L293D pin 16 (VCC1) and pin 8 (VCC2) at +6V?");
    Serial.println("  3. Are pins 1 and 9 (EN1/EN2) tied to +6V rather than left floating?");
    Serial.println("  4. Is the robot on the floor? Wheels in the air move nothing.");
    return;
  }

  for (int i = 0; i < 4; i++) {
    if (!results[i].moved)
      Serial.printf("FAIL: %s did nothing. Check that jumper wire and its enable pin.\n",
                    results[i].name);
  }

  // Relative checks: these hold whichever way the camera happens to face.
  bool leftPair  = results[0].moved && results[1].moved;
  bool rightPair = results[2].moved && results[3].moved;

  if (leftPair) {
    if ((results[0].shift > 0) == (results[1].shift > 0))
      Serial.println("FAIL: both left inputs pivot the robot the same way. "
                     "IN1 and IN2 should be opposites - check pins 2 and 7.");
    else
      Serial.println("OK: the left wheel turns both ways.");
  }
  if (rightPair) {
    if ((results[2].shift > 0) == (results[3].shift > 0))
      Serial.println("FAIL: both right inputs pivot the robot the same way. "
                     "IN3 and IN4 should be opposites - check pins 10 and 15.");
    else
      Serial.println("OK: the right wheel turns both ways.");
  }
  if (leftPair && rightPair) {
    // Left forward pivots one way, right forward must pivot the other way
    if ((results[0].shift > 0) == (results[2].shift > 0))
      Serial.println("FAIL: the left and right wheels pivot the robot the same way. "
                     "One motor is wired backwards: swap that motor's two wires.");
    else
      Serial.println("OK: the two wheels turn opposite ways, as a robot base should.");
  }

  if (movedCount == 4)
    Serial.println("\nAll four inputs drive their motor. The L293D wiring works.");

  Serial.println("\nSend 'a' to run it again.");
}

// ---------- setup ----------

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
  c.pixel_format = PIXFORMAT_GRAYSCALE;  // raw pixels: nothing to decode
  c.frame_size = FRAMESIZE_QQVGA;        // 160x120 is plenty to spot a pivot
  c.grab_mode = CAMERA_GRAB_LATEST;      // always the newest frame
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

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
  esp_log_level_set("cam_hal", ESP_LOG_ERROR);
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  camReady = initCamera();

  ledcAttachChannel(LEFT_IN1,  PWM_FREQ, PWM_BITS, 2);  // camera owns channel 0
  ledcAttachChannel(LEFT_IN2,  PWM_FREQ, PWM_BITS, 3);
  ledcAttachChannel(RIGHT_IN1, PWM_FREQ, PWM_BITS, 4);
  ledcAttachChannel(RIGHT_IN2, PWM_FREQ, PWM_BITS, 5);
  allOff();

  Serial.printf("\n\nAutomatic motor check. Camera %s.\n", camReady ? "ready" : "NOT WORKING");
  runCheck();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'a') runCheck();
  else if (c == 's') { allOff(); Serial.println("stopped"); }
}
