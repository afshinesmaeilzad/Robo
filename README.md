# Robo

A small two-wheel robot built on an **ESP32-CAM** and an **L293D** motor driver.

The robot creates its own WiFi network. You connect your laptop straight to it, open a web page, see live low-resolution video from the camera and drive the robot with the arrow keys. You don't need a router, internet or an app. The web page is stored on the ESP32-CAM itself.

```
 ┌──────────────┐     WiFi "Robo-CAM"      ┌────────────────────────────┐
 │    Laptop    │ ───────────────────────▶ │         ESP32-CAM          │
 │   browser    │  http://192.168.4.1      │  • WiFi access point       │
 │              │ ◀─────────────────────── │  • web page + control (:80)│
 │ arrow keys ↑↓←→   video stream (:81)    │  • MJPEG video (:81)       │
 └──────────────┘                          └─────────────┬──────────────┘
                                                         │ GPIO 12,13,14,15
                                                   ┌─────▼─────┐
                                                   │   L293D   │
                                                   └──┬─────┬──┘
                                               left motor  right motor
```

---

## Contents

1. [Parts](#1-parts)
2. [Power](#2-power)
3. [Wiring](#3-wiring)
4. [Software setup](#4-software-setup)
5. [Flashing the ESP32-CAM](#5-flashing-the-esp32-cam)
6. [Step 1: motor test](#6-step-1--motor-test-motor_test)
7. [Step 2: WiFi control and video](#7-step-2--wifi-control--video-robo_wifi)
8. [How the code works](#8-how-the-code-works)
9. [HTTP API](#9-http-api)
10. [Customizing](#10-customizing)
11. [Troubleshooting](#11-troubleshooting)
12. [Project structure](#12-project-structure)

---

## 1. Parts

| Part | Qty | Notes |
|---|---|---|
| ESP32-CAM (AI-Thinker) with OV2640 camera | 1 | The main board: WiFi, camera and control |
| ESP32-CAM-MB USB programmer board (or a USB-serial adapter) | 1 | Only needed for flashing |
| L293D motor driver (16-pin DIP chip) | 1 | Drives 2 DC motors forward and backward |
| TT gear motor (3–6V) with wheel | 2 | Left and right wheels |
| Robot base with caster wheel | 1 | |
| AA battery holder, 4× (6V) | 1 | Motor power |
| AA battery holder, 3× (4.5V) | 1 | ESP32-CAM power |
| Jumper wires / breadboard | | |

---

## 2. Power

The robot uses **two separate battery packs**:

| Pack | Voltage | Powers | Why separate |
|---|---|---|---|
| 4×AA | ~6V | L293D motor supply (VCC2) and L293D logic (VCC1) | Motors cause big current spikes and electrical noise |
| 3×AA | ~4.5V | ESP32-CAM **5V** pin | Keeps motor noise away from the ESP32 and camera, so it doesn't reset |

**The negative (−) of both packs must be connected together** and to the ESP32-CAM GND and the L293D GND. Without a shared ground, the ESP32's control signals have no reference and the motors won't respond.

About the voltages:
- The ESP32-CAM has an on-board regulator that turns the 5V pin into 3.3V. 4.5V from 3 AA cells is enough, but only just.
- **Never** connect a battery to the ESP32-CAM **3.3V** pin.
- The L293D loses about 1.5–2V internally, so from 6V the TT motors get roughly 4V. That's fine for a small robot.
- WiFi and the camera draw current in bursts, up to about 300 mA. Weak AA cells can drop the voltage and reset the board (see [Troubleshooting](#11-troubleshooting)). For more reliable power, use a USB power bank or an 18650 cell with a 5V step-up converter for the ESP32-CAM.

---

## 3. Wiring

### L293D pinout

```
             L293D  (notch / dot at the top)
         ┌──────────────┐
 EN1   1 ┤●             ├ 16  VCC1  (logic supply)
 IN1   2 ┤              ├ 15  IN4
 OUT1  3 ┤              ├ 14  OUT4
 GND   4 ┤              ├ 13  GND
 GND   5 ┤              ├ 12  GND
 OUT2  6 ┤              ├ 11  OUT3
 IN2   7 ┤              ├ 10  IN3
 VCC2  8 ┤              ├  9  EN2
         └──────────────┘
```

- **IN1/IN2** control motor A (the left motor), whose outputs are **OUT1/OUT2**.
- **IN3/IN4** control motor B (the right motor), whose outputs are **OUT3/OUT4**.
- **EN1/EN2** enable each side. They are tied HIGH, so both sides are always on and the speed is set with PWM on the IN pins.
- **VCC1** is the chip's logic supply (4.5–7V). **VCC2** is the motor supply.
- The 4 **GND** pins in the middle also act as the heat sink.

### Connections

| From | To | Purpose |
|---|---|---|
| ESP32-CAM **GPIO14** | L293D pin **2** (IN1) | Left motor, direction A |
| ESP32-CAM **GPIO15** | L293D pin **7** (IN2) | Left motor, direction B |
| ESP32-CAM **GPIO13** | L293D pin **10** (IN3) | Right motor, direction A |
| ESP32-CAM **GPIO12** | L293D pin **15** (IN4) | Right motor, direction B |
| Left motor (2 wires) | L293D pins **3** and **6** | |
| Right motor (2 wires) | L293D pins **11** and **14** | |
| L293D pins **1, 9, 16** | 4×AA **+** | Enables always ON, logic supply |
| L293D pin **8** | 4×AA **+** | Motor supply |
| L293D pins **4, 5, 12, 13** | GND | |
| 3×AA **+** | ESP32-CAM **5V** | ESP32 supply |
| 4×AA **−**, 3×AA **−**, ESP32-CAM **GND** | Common GND | **Required** |

### How a motor is driven

Each motor has two inputs. Which input gets the PWM signal sets the direction, and the PWM duty (0–255) sets the speed:

| IN1 | IN2 | Motor |
|---|---|---|
| PWM | 0 | Forward, at the PWM speed |
| 0 | PWM | Backward, at the PWM speed |
| 0 | 0 | Stop (coast) |

The ESP32 outputs 3.3V logic, which the L293D reads as HIGH (its threshold is 2.3V), so no level shifter is needed.

### Why these GPIO pins

The camera uses most of the ESP32-CAM's pins. The ones left free are **GPIO 2, 4, 12, 13, 14, 15, 16**:
- GPIO 4 is the bright flash LED, and the sketches use it as a light.
- GPIO 16 is used by the PSRAM on most boards.
- GPIO 2 is connected to the SD card.

That leaves **12, 13, 14, 15** for the motors.

> **GPIO12 warning:** GPIO12 is a *strapping pin*. If it is pulled HIGH while the board powers up, the ESP32 can fail to boot or to flash. The L293D input doesn't normally pull it high, but if you ever get flashing or boot errors while the robot is wired, unplug the GPIO12 wire, flash, then plug it back in.

---

## 4. Software setup

Everything is built from the command line with **arduino-cli**. The Arduino IDE isn't needed.

```bash
brew install arduino-cli
```

```bash
arduino-cli config init
```

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

```bash
arduino-cli core update-index
```

```bash
arduino-cli core install esp32:esp32
```

These sketches were built with **esp32:esp32 core 3.3.x**. The board type is `esp32:esp32:esp32cam` (AI Thinker ESP32-CAM).

---

## 5. Flashing the ESP32-CAM

The ESP32-CAM has no USB port of its own.

- **ESP32-CAM-MB board (easiest):** plug the ESP32-CAM onto it and connect USB. It resets into flashing mode automatically.
- **USB-serial adapter (FTDI, CP2102, and similar):** connect TX→U0R, RX→U0T, 5V, GND. Connect **IO0 to GND** before powering on so it enters flashing mode. After flashing, remove the IO0–GND wire and press RESET.

Find the serial port:

```bash
arduino-cli board list
```

On a Mac it looks like `/dev/cu.usbserial-10`. Compile and upload:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32cam robo_wifi
```

```bash
arduino-cli upload --fqbn esp32:esp32:esp32cam -p /dev/cu.usbserial-10 robo_wifi
```

To read the board's log output:

```bash
arduino-cli monitor -p /dev/cu.usbserial-10 -c baudrate=115200
```

---

## 6. Step 1: motor test (`motor_test/`)

This first sketch checks that the wiring is right before WiFi and the camera are added.

**What it does:**
1. At power-up it waits 3 seconds, giving you time to put the robot down.
2. It runs one test sequence, stopping briefly between each move:

   | Move | Duration | Left wheel | Right wheel |
   |---|---|---|---|
   | Forward | 1.5 s | forward | forward |
   | Backward | 1.5 s | backward | backward |
   | Turn left | 1 s | stop | forward |
   | Turn right | 1 s | forward | stop |
   | Spin left | 1 s | backward | forward |
   | Spin right | 1 s | forward | backward |

3. Then it waits for commands from the serial monitor at 115200 baud:

   | Key | Action |
   |---|---|
   | `f` | forward |
   | `b` | backward |
   | `l` | turn left |
   | `r` | turn right |
   | `q` | spin left |
   | `e` | spin right |
   | `s` | stop |
   | `t` | run the test sequence again |
   | `0`–`9` | speed from slow (0) to full (9) |

**Checking the result:**
- If a wheel turns the **wrong way** during "Forward", swap that motor's two wires on the L293D.
- If **left and right are swapped**, swap the motor wires between pins 3/6 and 11/14.
- If **nothing moves**, check the common ground, that the EN pins (1, 9) are at +6V, and that pin 8 is at +6V.

---

## 7. Step 2: WiFi control and video (`robo_wifi/`)

The main sketch. It uses the same wiring as the motor test.

### Connecting

1. Power the robot.
2. On your laptop, join the WiFi network:
   - **Network:** `Robo-CAM`
   - **Password:** `robo12345`
3. Open **http://192.168.4.1** in a browser. Type `http://`, not `https://`.

The laptop gets an address like `192.168.4.2`. The network has **no internet**. That's normal, because the robot *is* the network.

### The web page

- **Video:** live camera image at the top. It reconnects automatically if the stream drops.
- **Arrow pad:** on-screen buttons that work with mouse or touch and light up while pressed.
- **Speed slider:** motor power from 80 to 255.
- **Video size:** 160×120 (smoothest), 320×240 (default) or 640×480 (sharpest, slowest).
- **Light:** turns the flash LED on or off. It's very bright, so use it in the dark.

### Keyboard

| Keys | Robot |
|---|---|
| ↑ | forward |
| ↓ | backward |
| ← | spin left in place |
| → | spin right in place |
| ↑ + ← | curve forward-left |
| ↑ + → | curve forward-right |
| ↓ + ← | curve backward-left |
| ↓ + → | curve backward-right |
| Space | stop |
| (release all keys) | stop |

### Safety stop

The robot only moves while it keeps receiving commands. While a key is held, the browser resends the command every 250 ms. If the robot hears nothing for **600 ms**, it stops the motors. So if you close the tab, switch windows, walk out of WiFi range or the laptop disconnects, the robot stops instead of driving away.

---

## 8. How the code works

### Startup (`setup()`)

1. **Brown-out detector off.** When WiFi starts, the current spike can make battery voltage dip, and the ESP32's brown-out detector would reset the board. It's turned off so the robot keeps running on AA cells.
2. **Camera init.** The OV2640 is set up for JPEG output at 320×240, JPEG quality 14 (smaller files, faster stream). With PSRAM it uses 2 frame buffers and always grabs the **latest** frame, so the video doesn't lag behind.
3. **Motor PWM.** Each of the 4 motor pins gets its own PWM channel (1 kHz, 8-bit, so 0–255). Channels **2–5** are used on purpose, because the camera needs channel 0 and timer 0 for its 20 MHz clock (XCLK). Sharing them would break the camera.
4. **WiFi access point.** The board starts the `Robo-CAM` network at `192.168.4.1` with WiFi power saving off, to reduce lag.
5. **Two web servers:**
   - **Port 80:** the web page, `/go` (drive) and `/set` (settings).
   - **Port 81:** `/stream` (video).

   Video has its own server so that the endless video stream never blocks the drive commands.

### Main loop (`loop()`)

It checks the safety timeout every 20 ms. If the motors are running and no command has arrived for 600 ms, it stops them.

### Video stream

The stream is **MJPEG**: an endless HTTP response where each part is one JPEG image, separated by a boundary marker (`--robofrm`). Browsers show this natively in an `<img>` tag, so no video player or JavaScript decoding is needed. Each frame is about 5–10 KB at 320×240.

### Driving logic

`drive(left, right)` takes a speed for each wheel from −255 (full backward) to +255 (full forward). Each direction command maps to a pair of wheel speeds (`s` = the current speed setting):

| Command | Left | Right |
|---|---|---|
| `f` forward | +s | +s |
| `b` backward | −s | −s |
| `l` spin left | −s | +s |
| `r` spin right | +s | −s |
| `fl` forward-left | +s/3 | +s |
| `fr` forward-right | +s | +s/3 |
| `bl` backward-left | −s/3 | −s |
| `br` backward-right | −s | −s/3 |
| `s` stop | 0 | 0 |

### Browser side

The page tracks which arrow keys are held, works out the direction (such as `fl`), and sends `/go?d=...` only when the direction changes. It also resends every 250 ms while moving, to keep the safety timer alive.

---

## 9. HTTP API

You can control the robot from any program (Python, curl, and so on) while connected to `Robo-CAM`.

| Request | Effect |
|---|---|
| `GET http://192.168.4.1/` | Control web page |
| `GET /go?d=f` | Drive. `d` = `f` `b` `l` `r` `fl` `fr` `bl` `br` `s` |
| `GET /set?speed=200` | Motor speed, 0–255 |
| `GET /set?size=qvga` | Video size: `qqvga` (160×120), `qvga` (320×240), `vga` (640×480) |
| `GET /set?led=1` | Flash LED on (`1`) / off (`0`) |
| `GET http://192.168.4.1:81/stream` | MJPEG video stream |

Remember the 600 ms safety stop: to keep moving, repeat `/go` more often than that.

```bash
curl "http://192.168.4.1/go?d=f"
```

---

## 10. Customizing

Everything is at the top of `robo_wifi/robo_wifi.ino`:

| Setting | Default | What it does |
|---|---|---|
| `AP_SSID` | `Robo-CAM` | WiFi network name |
| `AP_PASS` | `robo12345` | WiFi password, at least 8 characters |
| `LEFT_IN1` … `RIGHT_IN2` | 14, 15, 13, 12 | Motor pins |
| `CMD_TIMEOUT_MS` | 600 | Safety stop delay |
| `speed` | 200 | Starting motor speed |
| `jpeg_quality` (in `initCamera`) | 14 | 10 = better image and bigger files, 30 = worse image and faster |
| `frame_size` (in `initCamera`) | `FRAMESIZE_QVGA` | Starting video size |

For gentler curves, change `h = s / 3` in `applyDirection()`, for example to `s / 2`.

If the image is upside down, add these lines after `esp_camera_init` in `initCamera()`:

```cpp
sensor_t *s = esp_camera_sensor_get();
s->set_vflip(s, 1);
s->set_hmirror(s, 1);
```

---

## 11. Troubleshooting

| Problem | Cause and fix |
|---|---|
| Can't see `Robo-CAM` in the WiFi list | The board isn't running or keeps rebooting. Check the 5V power; try USB power. Watch the serial monitor: it should print `WiFi AP "Robo-CAM" ... -> http://192.168.4.1`. |
| Laptop keeps leaving `Robo-CAM` | Your computer prefers a network with internet and switches back automatically. Turn off **Auto-Join** for your other WiFi networks while driving. |
| `192.168.4.1` shows a different website or asks for https | You're not actually on `Robo-CAM`. Some other networks use the same address. Check that your laptop's IP starts with `192.168.4.` |
| Page loads but no video | Reload the page. Try a smaller video size. Check the camera ribbon cable is fully inserted and latched. If the serial log shows `Camera init failed`, reseat the camera. |
| Video very slow or freezing | Choose 160×120, stay closer to the robot, or check whether the ESP32-CAM's antenna jumper is set for an external antenna that isn't connected. |
| Robot resets when motors start | Power problem. Make sure the motors are on the 4×AA pack, not the ESP32 pack. Use fresh batteries or a power bank for the ESP32-CAM. |
| A wheel goes the wrong way | Swap that motor's two wires on the L293D. |
| Left/right swapped | Swap the motor connections between pins 3/6 and 11/14, or swap the pin numbers in the code. |
| Motors hum but don't turn | Speed too low for the battery voltage. Raise the speed slider, or use fresh batteries. |
| Flashing fails: `Failed to connect` / `Timed out` | Disconnect the GPIO12 wire while flashing. With an FTDI adapter, connect IO0 to GND and press RESET. |
| L293D gets hot | Normal under load. Don't stall the motors for long; the chip limit is 600 mA per channel. |

---

## 12. Project structure

```
Robo/
├── README.md               this file
├── motor_test/
│   └── motor_test.ino      step 1: wiring and motor test (serial control)
└── robo_wifi/
    └── robo_wifi.ino       step 2: WiFi access point + video + arrow-key control
```
