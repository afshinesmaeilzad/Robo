// ESP32 (plain dev board, NOT the ESP32-CAM) + Nokia 5110 LCD test
// 84x48 PCD8544 module, the 8-pin one labelled RST CE DC DIN CLK VCC LIGHT GND.
//
// No library needed: the PCD8544 is driven by bit-banged SPI below.
//
// Wiring (ESP32 DevKit v1 / WROOM-32):
//
// All six signals sit on the LEFT header of the DevKit v1, top to bottom,
// so the module wires up from one rail with no jumpers crossing the board:
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
// Any GPIO works here because the SPI is bit-banged, not the hardware VSPI
// peripheral. Avoid 34-39 (input only) and 12 / 15 (strapping pins that
// change how the chip boots).
//
// The ESP32 is a 3.3V chip, so all 5 signal lines connect straight to the
// module. The series resistors you see in Arduino Uno guides are only there
// to drop 5V logic and are not needed here.
//
// Most red modules have the backlight LEDs wired to VCC, so LIGHT must be
// pulled LOW to light them. Some blue modules are the other way round.
// If the backlight stays dark (or is always on), flip BACKLIGHT_ON.
//
// Serial commands (115200 baud):
//   t = run the test again    i = invert on/off      b = backlight on/off
//   + / - = contrast          c = clear              a = all pixels on

#include <Arduino.h>

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

// --- test sequence ---

void screen(const char *l0, const char *l1, const char *l2,
            const char *l3, const char *l4, const char *l5) {
  clear();
  drawText(0, 0, l0); drawText(0, 1, l1); drawText(0, 2, l2);
  drawText(0, 3, l3); drawText(0, 4, l4); drawText(0, 5, l5);
  display();
}

void runTest() {
  Serial.println("--- LCD test start ---");

  Serial.println("1/6 backlight + text");
  setBacklight(true);
  screen("  ROBO  ", " LCD TEST", "", "PCD8544", "84 x 48", "ESP32 3V3");
  delay(2000);

  Serial.println("2/6 all pixels on (look for dead rows/columns)");
  fill();
  display();
  delay(1500);

  Serial.println("3/6 checkerboard");
  clear();
  for (int y = 0; y < LCD_H; y++)
    for (int x = 0; x < LCD_W; x++)
      if ((x + y) % 2 == 0) drawPixel(x, y);
  display();
  delay(1500);

  Serial.println("4/6 lines and border");
  clear();
  drawRect(0, 0, LCD_W, LCD_H);
  drawLine(0, 0, LCD_W - 1, LCD_H - 1);
  drawLine(LCD_W - 1, 0, 0, LCD_H - 1);
  drawRect(30, 16, 24, 16);
  display();
  delay(1500);

  Serial.println("5/6 contrast sweep");
  screen("CONTRAST", "SWEEP", "", "0x30 -> 0x50", "", "");
  for (int v = 0x30; v <= 0x50; v += 2) { setContrast(v); delay(120); }
  setContrast(0x3A);
  delay(500);

  Serial.println("6/6 invert");
  screen(" INVERT ", "", "black on", "white", "", "");
  setInvert(true);
  delay(1000);
  setInvert(false);
  delay(500);

  Serial.println("--- LCD test done. Send t/i/b/+/-/c/a ---");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32 + Nokia 5110 LCD test");
  Serial.printf("RST=%d CE=%d DC=%d DIN=%d CLK=%d LIGHT=%d\n",
                LCD_RST, LCD_CE, LCD_DC, LCD_DIN, LCD_CLK, LCD_LIGHT);

  lcdInit();
  setBacklight(true);
  clear();
  display();

  runTest();
}

void loop() {
  // live screen: uptime counter, so a frozen display is obvious
  static uint32_t last = 0;
  if (millis() - last > 500) {
    last = millis();
    char line[16];
    clear();
    drawText(0, 0, "ROBO LCD OK");
    drawLine(0, 9, LCD_W - 1, 9);
    snprintf(line, sizeof(line), "up %lus", millis() / 1000);
    drawText(0, 2, line);
    snprintf(line, sizeof(line), "vop 0x%02X", contrast);
    drawText(0, 3, line);
    snprintf(line, sizeof(line), "light %s", backlight ? "on" : "off");
    drawText(0, 4, line);
    drawText(0, 5, "t=test i=inv");
    display();
  }

  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case 't': runTest(); break;
    case 'i': setInvert(!inverted); Serial.printf("INVERT %d\n", inverted); break;
    case 'b': setBacklight(!backlight); Serial.printf("LIGHT %d\n", backlight); break;
    case '+': setContrast(contrast + 2); Serial.printf("VOP 0x%02X\n", contrast); break;
    case '-': setContrast(contrast - 2); Serial.printf("VOP 0x%02X\n", contrast); break;
    case 'c': clear(); display(); delay(1500); break;
    case 'a': fill();  display(); delay(1500); break;
  }
}
