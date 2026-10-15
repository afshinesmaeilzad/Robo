// L293D wiring check. No WiFi, no camera, no web page: just the motors.
//
// Drives each L293D input on its own, so you can see exactly which wire moves
// which wheel and in which direction. Connect USB and open the serial monitor
// at 115200 baud, or run tools/motor_check.py for a guided test.
//
// Wiring (same as the rest of the project):
//   GPIO14 -> L293D pin 2  (IN1)   left motor
//   GPIO15 -> L293D pin 7  (IN2)   left motor
//   GPIO13 -> L293D pin 10 (IN3)   right motor
//   GPIO12 -> L293D pin 15 (IN4)   right motor
//
// Commands (115200 baud):
//   1 = left IN1    2 = left IN2    3 = right IN3   4 = right IN4
//   a = run all four in order       t = both motors forward
//   s = stop        0-9 = speed     ? = show this list

#include <Arduino.h>

const int LEFT_IN1  = 14;   // L293D pin 2
const int LEFT_IN2  = 15;   // L293D pin 7
const int RIGHT_IN1 = 13;   // L293D pin 10
const int RIGHT_IN2 = 12;   // L293D pin 15
const int FLASH_LED = 4;

const int PWM_FREQ = 1000;
const int PWM_BITS = 8;
const int RUN_MS = 1500;    // how long each step drives

int speed = 200;            // 0-255

void allOff() {
  ledcWrite(LEFT_IN1, 0);
  ledcWrite(LEFT_IN2, 0);
  ledcWrite(RIGHT_IN1, 0);
  ledcWrite(RIGHT_IN2, 0);
}

// Drive one input by itself. Its partner stays at 0, so the motor turns in the
// direction that input alone selects.
void pulse(int pin, const char *label, const char *expect) {
  Serial.printf("\n%s (GPIO%d) at speed %d for %d ms\n", label, pin, speed, RUN_MS);
  Serial.printf("  expect: %s\n", expect);
  allOff();
  ledcWrite(pin, speed);
  delay(RUN_MS);
  allOff();
  Serial.println("  stopped");
  delay(800);
}

void stepLeft1()  { pulse(LEFT_IN1,  "LEFT  IN1 -> L293D pin 2",  "LEFT wheel turns FORWARD, right wheel still"); }
void stepLeft2()  { pulse(LEFT_IN2,  "LEFT  IN2 -> L293D pin 7",  "LEFT wheel turns BACKWARD, right wheel still"); }
void stepRight1() { pulse(RIGHT_IN1, "RIGHT IN3 -> L293D pin 10", "RIGHT wheel turns FORWARD, left wheel still"); }
void stepRight2() { pulse(RIGHT_IN2, "RIGHT IN4 -> L293D pin 15", "RIGHT wheel turns BACKWARD, left wheel still"); }

void bothForward() {
  Serial.printf("\nBOTH motors forward at speed %d\n", speed);
  Serial.println("  expect: robot drives straight forward");
  allOff();
  ledcWrite(LEFT_IN1, speed);
  ledcWrite(RIGHT_IN1, speed);
  delay(RUN_MS);
  allOff();
  Serial.println("  stopped");
}

void runAll() {
  Serial.println("\n===== L293D check: 4 inputs, one at a time =====");
  stepLeft1();
  stepLeft2();
  stepRight1();
  stepRight2();
  Serial.println("\n===== done =====");
  Serial.println("Nothing moved at all?  Check the common ground, L293D pin 16 and pin 8 at +6V,");
  Serial.println("  and pins 1 and 9 (EN1/EN2) tied to +6V.");
  Serial.println("A wheel turned the wrong way?  Swap that motor's two wires on the L293D.");
  Serial.println("The wrong wheel moved?  Swap the motor pairs: pins 3/6 with pins 11/14.");
  Serial.println("\nCommands: 1 2 3 4 = one input, a = all, t = both forward, s = stop, 0-9 = speed");
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);   // keep the bright LED off

  ledcAttach(LEFT_IN1,  PWM_FREQ, PWM_BITS);
  ledcAttach(LEFT_IN2,  PWM_FREQ, PWM_BITS);
  ledcAttach(RIGHT_IN1, PWM_FREQ, PWM_BITS);
  ledcAttach(RIGHT_IN2, PWM_FREQ, PWM_BITS);
  allOff();

  Serial.println("\n\nL293D motor check ready");
  Serial.println("Put the robot on a box so the wheels spin free.");
  Serial.println("Send 'a' to run the full check, or 1/2/3/4 for one input.");
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case '1': stepLeft1(); break;
    case '2': stepLeft2(); break;
    case '3': stepRight1(); break;
    case '4': stepRight2(); break;
    case 'a': runAll(); break;
    case 't': bothForward(); break;
    case 's': allOff(); Serial.println("STOP"); break;
    case '?': Serial.println("1 2 3 4 = one input, a = all, t = both forward, s = stop, 0-9 = speed"); break;
    default:
      if (c >= '0' && c <= '9') {
        speed = map(c - '0', 0, 9, 80, 255);
        Serial.printf("speed %d\n", speed);
      }
  }
}
