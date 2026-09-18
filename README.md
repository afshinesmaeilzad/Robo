# Robo

ESP32-CAM two-wheel robot with an L293D motor driver. The robot runs its own WiFi access point, streams low-resolution video and is driven from a laptop browser with the arrow keys.

## Hardware
- ESP32-CAM (AI-Thinker)
- L293D motor driver
- 2× TT motors (left / right wheels)
- 4×AA (6V) for the motors, 3×AA (4.5V) for the ESP32-CAM

## Wiring

| ESP32-CAM | L293D | Function |
|---|---|---|
| GPIO14 | pin 2 (IN1) | left motor |
| GPIO15 | pin 7 (IN2) | left motor |
| GPIO13 | pin 10 (IN3) | right motor |
| GPIO12 | pin 15 (IN4) | right motor |

- Left motor → L293D pins 3 and 6; right motor → pins 11 and 14
- L293D pins 1, 9, 16 (EN1, EN2, VCC1) and pin 8 (VCC2) → 4×AA +
- L293D pins 4, 5, 12, 13 → GND
- 3×AA + → ESP32-CAM 5V
- All grounds (both packs, L293D, ESP32-CAM) joined

GPIO12 is a boot strapping pin: if flashing fails while wired, disconnect it during upload.

## Sketches

### `motor_test/`
Runs forward, backward, turn and spin once at boot, then accepts serial commands at 115200 baud: `f` `b` `l` `r` `q` `e` `s` `t`, `0`–`9` for speed.

### `robo_wifi/`
- WiFi AP `Robo-CAM`, password `robo12345`
- Open http://192.168.4.1
- Arrow keys drive (combine for curves), Space stops
- Speed slider, video size (160×120 / 320×240 / 640×480), flash LED toggle
- Motors stop automatically 0.6 s after the last command

## Build and flash

```bash
arduino-cli compile --fqbn esp32:esp32:esp32cam robo_wifi
arduino-cli upload --fqbn esp32:esp32:esp32cam -p /dev/cu.usbserial-10 robo_wifi
```
