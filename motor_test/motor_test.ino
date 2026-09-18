// ESP32-CAM + L293D motor test
// Runs a movement test sequence at boot, then accepts serial commands (115200 baud):
//   f = forward, b = backward, l = turn left, r = turn right,
//   q = spin left, e = spin right, s = stop, t = repeat test, 0-9 = speed

#include <Arduino.h>

// L293D inputs (EN1/EN2 tied to 5V, speed via PWM on inputs)
const int LEFT_IN1  = 14;  // L293D pin 2  -> left motor
const int LEFT_IN2  = 15;  // L293D pin 7
const int RIGHT_IN1 = 13;  // L293D pin 10 -> right motor
const int RIGHT_IN2 = 12;  // L293D pin 15

const int FLASH_LED = 4;   // on-board flash LED, keep off

const int PWM_FREQ = 1000;
const int PWM_BITS = 8;

int speed = 200;  // 0-255

void setMotor(int in1, int in2, int value) {
  if (value > 0) {
    ledcWrite(in1, value);
    ledcWrite(in2, 0);
  } else if (value < 0) {
    ledcWrite(in1, 0);
    ledcWrite(in2, -value);
  } else {
    ledcWrite(in1, 0);
    ledcWrite(in2, 0);
  }
}

void drive(int left, int right) {
  setMotor(LEFT_IN1, LEFT_IN2, left);
  setMotor(RIGHT_IN1, RIGHT_IN2, right);
}

void forward()   { drive(speed, speed);        Serial.println("FORWARD"); }
void backward()  { drive(-speed, -speed);      Serial.println("BACKWARD"); }
void turnLeft()  { drive(0, speed);            Serial.println("TURN LEFT"); }
void turnRight() { drive(speed, 0);            Serial.println("TURN RIGHT"); }
void spinLeft()  { drive(-speed, speed);       Serial.println("SPIN LEFT"); }
void spinRight() { drive(speed, -speed);       Serial.println("SPIN RIGHT"); }
void stopAll()   { drive(0, 0);                Serial.println("STOP"); }

void step(void (*move)(), int ms) {
  move();
  delay(ms);
  stopAll();
  delay(700);
}

void runTest() {
  Serial.println("--- Motor test start ---");
  step(forward, 1500);
  step(backward, 1500);
  step(turnLeft, 1000);
  step(turnRight, 1000);
  step(spinLeft, 1000);
  step(spinRight, 1000);
  Serial.println("--- Motor test done. Send f/b/l/r/q/e/s/t or 0-9 ---");
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  ledcAttach(LEFT_IN1,  PWM_FREQ, PWM_BITS);
  ledcAttach(LEFT_IN2,  PWM_FREQ, PWM_BITS);
  ledcAttach(RIGHT_IN1, PWM_FREQ, PWM_BITS);
  ledcAttach(RIGHT_IN2, PWM_FREQ, PWM_BITS);
  stopAll();

  delay(3000);  // time to put the robot down
  runTest();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case 'f': forward(); break;
    case 'b': backward(); break;
    case 'l': turnLeft(); break;
    case 'r': turnRight(); break;
    case 'q': spinLeft(); break;
    case 'e': spinRight(); break;
    case 's': stopAll(); break;
    case 't': runTest(); break;
    default:
      if (c >= '0' && c <= '9') {
        speed = map(c - '0', 0, 9, 80, 255);
        Serial.printf("SPEED %d\n", speed);
      }
  }
}
