// Automatic movement test: forward, backward, left, right, round.
// No WiFi, no camera, no commands. Power it up and it runs the five moves in a
// loop, with a pause between each one, and says over serial what it is doing.
//
// Wiring:
//   GPIO14 -> L293D pin 2  (IN1)   left motor
//   GPIO15 -> L293D pin 7  (IN2)   left motor
//   GPIO13 -> L293D pin 10 (IN3)   right motor
//   GPIO12 -> L293D pin 15 (IN4)   right motor
//
// Serial 115200 baud: s = stop and pause, g = go again, + / - = speed

#include <Arduino.h>

const int LEFT_IN1  = 14;
const int LEFT_IN2  = 15;
const int RIGHT_IN1 = 13;
const int RIGHT_IN2 = 12;
const int FLASH_LED = 4;

const int PWM_FREQ = 1000;
const int PWM_BITS = 8;

int speed = 200;        // 0-255
bool running = true;

// One wheel: positive turns it forward, negative backward, 0 stops it
void wheel(int in1, int in2, int value) {
  value = constrain(value, -255, 255);
  ledcWrite(in1, value > 0 ? value : 0);
  ledcWrite(in2, value < 0 ? -value : 0);
}

void drive(int left, int right) {
  wheel(LEFT_IN1, LEFT_IN2, left);
  wheel(RIGHT_IN1, RIGHT_IN2, right);
}

void stopNow() { drive(0, 0); }

// Do one move, then stop and wait, so each move is easy to tell apart
void move(const char *name, int left, int right, int ms) {
  if (!running) return;
  Serial.printf("%-10s  left %+4d   right %+4d\n", name, left, right);
  drive(left, right);
  delay(ms);
  stopNow();
  delay(900);
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  ledcAttach(LEFT_IN1,  PWM_FREQ, PWM_BITS);
  ledcAttach(LEFT_IN2,  PWM_FREQ, PWM_BITS);
  ledcAttach(RIGHT_IN1, PWM_FREQ, PWM_BITS);
  ledcAttach(RIGHT_IN2, PWM_FREQ, PWM_BITS);
  stopNow();

  Serial.println("\n\nMovement test: forward, backward, left, right, round");
  Serial.println("Put the robot on the floor with space around it.");
  Serial.println("Serial: s = stop, g = go, + / - = speed");
  Serial.println("Starting in 3 seconds...");
  delay(3000);
}

void checkSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 's')      { running = false; stopNow(); Serial.println("\n** stopped, send 'g' to go **"); }
  else if (c == 'g') { running = true;  Serial.println("\n** running **"); }
  else if (c == '+') { speed = min(speed + 20, 255); Serial.printf("speed %d\n", speed); }
  else if (c == '-') { speed = max(speed - 20, 80);  Serial.printf("speed %d\n", speed); }
}

void loop() {
  checkSerial();
  if (!running) { delay(100); return; }

  int s = speed;
  Serial.printf("\n--- round of moves at speed %d ---\n", s);

  move("FORWARD",  s,  s, 1500);   // both wheels forward
  move("BACKWARD", -s, -s, 1500);  // both wheels backward
  move("LEFT",     0,  s, 1000);   // right wheel only: swings to the left
  move("RIGHT",    s,  0, 1000);   // left wheel only: swings to the right
  move("ROUND",   -s,  s, 2500);   // wheels opposite: spins on the spot

  Serial.println("--- pause ---");
  for (int i = 0; i < 20 && running; i++) { checkSerial(); delay(100); }
}
