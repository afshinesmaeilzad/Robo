// ESP32 (plain dev board, NOT the ESP32-CAM) + Nokia 5110 LCD
// An animated robot face: eyes that blink, look around and wink, over a
// mouth that smiles, grins and goes "oh". Runs forever, no input needed.
//
// Same wiring and same bit-banged PCD8544 driver as lcd_test/:
//
//   LCD pin   ESP32          Notes
//   -------   ------------   ----------------------------------------
//   CLK       GPIO 32        clock
//   DIN       GPIO 33        data in
//   LIGHT     GPIO 25        backlight (see BACKLIGHT_ON below)
//   DC        GPIO 26        0 = command, 1 = data
//   RST       GPIO 27        reset
//   CE        GPIO 14        chip enable (active LOW)
//   VCC       3V3            3.3V ONLY - 5V kills the panel
//   GND       GND            common ground
//
// Serial commands (115200 baud), to force one expression on demand:
//   k = blink   w = wink   l = look around   g = grin   o = surprised
//   s = sleepy  + / - = contrast             b = backlight on/off

#include <Arduino.h>

enum Mouth { SMILE, BIG_SMILE, GRIN, FLAT, OH };  // mouth shapes, see drawMouth()

const int LCD_RST   = 27;
const int LCD_CE    = 14;
const int LCD_DC    = 26;
const int LCD_DIN   = 33;
const int LCD_CLK   = 32;
const int LCD_LIGHT = 25;

const int BACKLIGHT_ON = LOW;   // LOW for red modules, HIGH for most blue ones

const int LCD_W = 84;
const int LCD_H = 48;
const int LCD_ROWS = LCD_H / 8;  // 6 rows of 8 vertical pixels

uint8_t buffer[LCD_W * LCD_ROWS];  // 504 bytes, one bit per pixel
int contrast = 0x3A;               // Vop, usable range is roughly 0x30-0x50
bool inverted = false;
bool backlight = true;

// 5x7 font, ASCII 0x20 (space) to 0x7E (~)
const uint8_t font[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5f,0x00,0x00},
  {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7f,0x14,0x7f,0x14},
  {0x24,0x2a,0x7f,0x2a,0x12}, {0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
  {0x00,0x1c,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1c,0x00},
  {0x14,0x08,0x3e,0x08,0x14}, {0x08,0x08,0x3e,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
  {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
  {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
  {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
  {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
  {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
  {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
  {0x32,0x49,0x79,0x41,0x3e}, {0x7e,0x11,0x11,0x11,0x7e},
  {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
  {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41},
  {0x7f,0x09,0x09,0x09,0x01}, {0x3e,0x41,0x49,0x49,0x7a},
  {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00},
  {0x20,0x40,0x41,0x3f,0x01}, {0x7f,0x08,0x14,0x22,0x41},
  {0x7f,0x40,0x40,0x40,0x40}, {0x7f,0x02,0x0c,0x02,0x7f},
  {0x7f,0x04,0x08,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
  {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e},
  {0x7f,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f},
  {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f},
  {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
  {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7f,0x41,0x41,0x00},
  {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7f,0x00},
  {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
  {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
  {0x7f,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
  {0x38,0x44,0x44,0x48,0x7f}, {0x38,0x54,0x54,0x54,0x18},
  {0x08,0x7e,0x09,0x01,0x02}, {0x0c,0x52,0x52,0x52,0x3e},
  {0x7f,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00},
  {0x20,0x40,0x44,0x3d,0x00}, {0x7f,0x10,0x28,0x44,0x00},
  {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x18,0x04,0x78},
  {0x7c,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
  {0x7c,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7c},
  {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3f,0x44,0x40,0x20}, {0x3c,0x40,0x40,0x20,0x7c},
  {0x1c,0x20,0x40,0x20,0x1c}, {0x3c,0x40,0x30,0x40,0x3c},
  {0x44,0x28,0x10,0x28,0x44}, {0x0c,0x50,0x50,0x50,0x3c},
  {0x44,0x64,0x54,0x4c,0x44}, {0x00,0x08,0x36,0x41,0x00},
  {0x00,0x00,0x7f,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00},
  {0x10,0x08,0x08,0x10,0x08},
};

// --- low level: bit-banged SPI, MSB first, data sampled on the rising edge ---

void lcdWrite(bool isData, uint8_t value) {
  digitalWrite(LCD_DC, isData ? HIGH : LOW);
  digitalWrite(LCD_CE, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(LCD_DIN, (value >> i) & 1);
    digitalWrite(LCD_CLK, HIGH);
    digitalWrite(LCD_CLK, LOW);
  }
  digitalWrite(LCD_CE, HIGH);
}

void lcdCommand(uint8_t c) { lcdWrite(false, c); }
void lcdData(uint8_t d)    { lcdWrite(true, d); }

void setContrast(int vop) {
  contrast = constrain(vop, 0x00, 0x7F);
  lcdCommand(0x21);               // extended instruction set
  lcdCommand(0x80 | contrast);    // set Vop (contrast)
  lcdCommand(0x20);               // back to basic instruction set
}

void setInvert(bool on) {
  inverted = on;
  lcdCommand(0x20);
  lcdCommand(on ? 0x0D : 0x0C);   // inverse video / normal
}

void setBacklight(bool on) {
  backlight = on;
  digitalWrite(LCD_LIGHT, on ? BACKLIGHT_ON : !BACKLIGHT_ON);
}

void lcdInit() {
  pinMode(LCD_RST, OUTPUT);
  pinMode(LCD_CE, OUTPUT);
  pinMode(LCD_DC, OUTPUT);
  pinMode(LCD_DIN, OUTPUT);
  pinMode(LCD_CLK, OUTPUT);
  pinMode(LCD_LIGHT, OUTPUT);

  digitalWrite(LCD_CE, HIGH);
  digitalWrite(LCD_CLK, LOW);

  digitalWrite(LCD_RST, LOW);     // reset pulse, must happen after power-up
  delay(10);
  digitalWrite(LCD_RST, HIGH);
  delay(10);

  lcdCommand(0x21);               // extended instruction set
  lcdCommand(0x80 | contrast);    // Vop / contrast
  lcdCommand(0x04);               // temperature coefficient 0
  lcdCommand(0x14);               // bias 1:48
  lcdCommand(0x20);               // basic instruction set, horizontal addressing
  lcdCommand(0x0C);               // normal display
}

// --- frame buffer drawing ---

void clear() { memset(buffer, 0x00, sizeof(buffer)); }
void fill()  { memset(buffer, 0xFF, sizeof(buffer)); }

void display() {
  lcdCommand(0x20);
  lcdCommand(0x80);               // x = 0
  lcdCommand(0x40);               // y = 0
  for (size_t i = 0; i < sizeof(buffer); i++) lcdData(buffer[i]);
}

void drawPixel(int x, int y, bool on = true) {
  if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
  uint8_t mask = 1 << (y % 8);
  if (on) buffer[x + (y / 8) * LCD_W] |= mask;
  else    buffer[x + (y / 8) * LCD_W] &= ~mask;
}

void drawLine(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    drawPixel(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void drawRect(int x, int y, int w, int h) {
  drawLine(x, y, x + w - 1, y);
  drawLine(x, y + h - 1, x + w - 1, y + h - 1);
  drawLine(x, y, x, y + h - 1);
  drawLine(x + w - 1, y, x + w - 1, y + h - 1);
}

// Text is drawn on the 8-pixel row grid: col 0-13, row 0-5.
void drawChar(int col, int row, char c) {
  if (c < 0x20 || c > 0x7E) c = ' ';
  int x = col * 6;
  if (x + 5 >= LCD_W || row < 0 || row >= LCD_ROWS) return;
  for (int i = 0; i < 5; i++)
    buffer[x + i + row * LCD_W] = pgm_read_byte(&font[c - 0x20][i]);
  buffer[x + 5 + row * LCD_W] = 0x00;   // 1px gap between characters
}

void drawText(int col, int row, const char *s) {
  while (*s) drawChar(col++, row, *s++);
}

// --- extra shapes for the face ---

void fillRect(int x, int y, int w, int h) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) drawPixel(i, j);
}

// Rounded rectangle, filled row by row: rows inside the corner radius get
// inset by the circle equation, the rest run full width.
void fillRoundRect(int x, int y, int w, int h, int r) {
  if (h < 2 * r) r = h / 2;
  if (w < 2 * r) r = w / 2;
  for (int i = 0; i < h; i++) {
    int dy = -1;
    if (i < r) dy = r - 1 - i;
    else if (i >= h - r) dy = i - (h - r);
    int inset = 0;
    if (dy >= 0) {
      int v = r * r - dy * dy;
      inset = r - (int)sqrtf(v > 0 ? v : 0);
    }
    fillRect(x + inset, y + i, w - 2 * inset, 1);
  }
}

// Filled ellipse. lowerHalf draws only the bottom half, which is what an
// open smiling mouth looks like.
void fillEllipse(int cx, int cy, int rx, int ry, bool lowerHalf = false) {
  for (int dy = lowerHalf ? 0 : -ry; dy <= ry; dy++) {
    float f = 1.0f - (float)(dy * dy) / (float)(ry * ry);
    int half = (int)(rx * sqrtf(f > 0.0f ? f : 0.0f));
    fillRect(cx - half, cy + dy, 2 * half + 1, 1);
  }
}

// --- the face ---

const int EYE_W  = 20;   // eye width
const int EYE_H  = 20;   // eye height, wide open
const int EYE_Y  = 16;   // vertical centre of both eyes
const int EYE_LX = 22;   // horizontal centre, left eye
const int EYE_RX = 62;   // horizontal centre, right eye

const int MOUTH_CX = 42;
const int MOUTH_Y  = 34;

// One eye. h shrinks towards the centre line to blink; lookX slides it
// sideways so the face can glance left and right.
void drawEye(int cx, int h, int lookX) {
  if (h < 2) h = 2;
  int x = cx - EYE_W / 2 + lookX;
  int y = EYE_Y - h / 2;
  fillRoundRect(x, y, EYE_W, h, 6);

  if (h > 12)  // glossy highlight, cleared back out of the filled eye
    for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++) drawPixel(x + 4 + i, y + 4 + j, false);
}

// A smile is a parabola: ends high, middle dropped by depth. Drawn 2px thick
// so it stays readable on a 48-pixel-tall panel. Near the ends the curve drops
// faster than one pixel per column, so each column also fills the gap back to
// the previous one - otherwise the steep parts come out as dashes.
void drawSmileCurve(int w, int depth) {
  int half = w / 2;
  int prev = MOUTH_Y;
  for (int dx = -half; dx <= half; dx++) {
    float t = (float)dx / (float)half;
    int y = MOUTH_Y + (int)(depth * (1.0f - t * t));
    int from = y < prev ? y : prev;
    int to   = y < prev ? prev : y;
    for (int yy = from; yy <= to + 1; yy++) drawPixel(MOUTH_CX + dx, yy);
    prev = y;
  }
}

void drawMouth(Mouth m) {
  switch (m) {
    case SMILE:     drawSmileCurve(30, 6); break;
    case BIG_SMILE: drawSmileCurve(38, 9); break;
    case GRIN:      fillEllipse(MOUTH_CX, MOUTH_Y + 1, 16, 10, true); break;
    case FLAT:      fillRect(MOUTH_CX - 10, MOUTH_Y + 5, 21, 2); break;
    case OH:        fillEllipse(MOUTH_CX, MOUTH_Y + 6, 5, 7); break;
  }
}

void render(int leftH, int rightH, int lookX, Mouth m) {
  clear();
  drawEye(EYE_LX, leftH, lookX);
  drawEye(EYE_RX, rightH, lookX);
  drawMouth(m);
  display();
}

// Draw a frame and hold it. Every animation below is a series of these.
void frame(int leftH, int rightH, int lookX, Mouth m, int ms) {
  render(leftH, rightH, lookX, m);
  delay(ms);
}

// --- animations ---

const int lidSeq[] = {20, 15, 9, 4, 2, 2, 4, 9, 15, 20};
const int lidSteps = sizeof(lidSeq) / sizeof(lidSeq[0]);

void blink(Mouth m = SMILE, int lookX = 0) {
  for (int i = 0; i < lidSteps; i++)
    frame(lidSeq[i], lidSeq[i], lookX, m, 26);
}

void doubleBlink() {
  Serial.println("double blink");
  blink();
  delay(120);
  blink();
}

// Only the right eye closes, and the smile widens on the way.
void wink() {
  Serial.println("wink");
  for (int i = 0; i < lidSteps; i++)
    frame(EYE_H, lidSeq[i], 0, BIG_SMILE, 40);
  frame(EYE_H, EYE_H, 0, BIG_SMILE, 500);
}

void lookAround() {
  Serial.println("look around");
  for (int x = 0; x >= -7; x--) frame(EYE_H, EYE_H, x, SMILE, 30);
  delay(600);
  blink(SMILE, -7);
  for (int x = -7; x <= 7; x++)  frame(EYE_H, EYE_H, x, SMILE, 25);
  delay(600);
  blink(SMILE, 7);
  for (int x = 7; x >= 0; x--)   frame(EYE_H, EYE_H, x, SMILE, 30);
}

// Squint the eyes down a little while the mouth opens: a laugh.
void grin() {
  Serial.println("grin");
  frame(EYE_H, EYE_H, 0, BIG_SMILE, 250);
  for (int n = 0; n < 4; n++) {
    frame(12, 12, 0, GRIN, 170);
    frame(16, 16, 0, BIG_SMILE, 130);
  }
  frame(EYE_H, EYE_H, 0, SMILE, 300);
}

// Eyes pop wide, mouth goes round, then settles back.
void surprised() {
  Serial.println("surprised");
  frame(6, 6, 0, FLAT, 140);
  frame(EYE_H, EYE_H, 0, OH, 700);
  frame(EYE_H, EYE_H, 0, OH, 200);
  frame(EYE_H, EYE_H, 0, BIG_SMILE, 600);
}

// Lids sag shut, hold, then wake up again.
void sleepy() {
  Serial.println("sleepy");
  for (int h = EYE_H; h >= 3; h--) frame(h, h, 0, FLAT, 45);
  delay(1200);
  for (int h = 3; h <= EYE_H; h += 2) frame(h, h, 0, SMILE, 35);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32 + Nokia 5110 animated face");

  lcdInit();
  setBacklight(true);
  randomSeed(esp_random());

  // wake up: eyes open from shut, then a first smile
  clear();
  display();
  for (int h = 2; h <= EYE_H; h += 2) frame(h, h, 0, FLAT, 40);
  frame(EYE_H, EYE_H, 0, SMILE, 400);
  Serial.println("running - send k/w/l/g/o/s to force an expression");
}

void loop() {
  // idle: hold the smile, blink, hold again
  frame(EYE_H, EYE_H, 0, SMILE, 1100);
  blink();
  frame(EYE_H, EYE_H, 0, SMILE, 800);

  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'k': doubleBlink(); return;
      case 'w': wink(); return;
      case 'l': lookAround(); return;
      case 'g': grin(); return;
      case 'o': surprised(); return;
      case 's': sleepy(); return;
      case 'b': setBacklight(!backlight); return;
      case '+': setContrast(contrast + 2); Serial.printf("VOP 0x%02X\n", contrast); return;
      case '-': setContrast(contrast - 2); Serial.printf("VOP 0x%02X\n", contrast); return;
    }
  }

  // then something random, so it never loops identically
  switch (random(6)) {
    case 0: doubleBlink(); break;
    case 1: wink(); break;
    case 2: lookAround(); break;
    case 3: grin(); break;
    case 4: surprised(); break;
    case 5: sleepy(); break;
  }
}
