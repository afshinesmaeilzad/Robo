# Robo

A small two-wheel robot built on an **ESP32-CAM** and an **L293D** motor driver.

The robot creates its own WiFi network. You connect your laptop straight to it, open a web page and drive with the arrow keys. To see where the robot is, you either ask for a single picture or switch on video. You don't need a router, internet or an app. The web page is stored on the ESP32-CAM itself.

Commands come first: they travel over one always-open WebSocket as tiny messages, and pictures are sent one at a time on the same connection, so they can never pile up in front of the controls.

```
 ┌──────────────┐     WiFi "Robo-CAM"      ┌────────────────────────────┐
 │    Laptop    │ ───────────────────────▶ │         ESP32-CAM          │
 │   browser    │  http://192.168.4.1      │  • WiFi access point       │
 │              │   commands (WebSocket)   │  • web page + API (:80)    │
 │              │ ◀─────────────────────── │  • WebSocket /ws           │
 │ arrow keys ↑↓←→   pictures (JPEG)       │  • camera, idle until used │
 │ P = picture  │                          └─────────────┬──────────────┘
 └──────────────┘                                        │ GPIO 12,13,14,15
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
7. [Step 2: WiFi control and pictures](#7-step-2-wifi-control-and-pictures-robo_wifi)
8. [How the code works](#8-how-the-code-works)
9. [API](#9-api)
10. [Customizing](#10-customizing)
11. [Troubleshooting](#11-troubleshooting)
12. [AI explorer](#12-ai-explorer-server)
13. [Project structure](#13-project-structure)

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
| AA battery holder, 4× (6V), or a USB power bank | 1 | ESP32-CAM power |
| Jumper wires / breadboard | | |

---

## 2. Power

The robot uses **two separate battery packs**:

| Pack | Voltage | Powers | Why separate |
|---|---|---|---|
| 4×AA | ~6V | L293D motor supply (VCC2) and L293D logic (VCC1) | Motors cause big current spikes and electrical noise |
| 4×AA (or a USB power bank) | ~6V | ESP32-CAM **5V** pin | Keeps motor noise away from the ESP32 and camera, so it doesn't reset |

**The negative (−) of both packs must be connected together** and to the ESP32-CAM GND and the L293D GND. Without a shared ground, the ESP32's control signals have no reference and the motors won't respond.

About the voltages:
- The ESP32-CAM has an on-board regulator that turns the 5V pin into 3.3V. It needs about **1.1V more than it gives out**, so at least **4.4V** at the 5V pin.
- **3×AA (4.5V) is not enough in practice.** Fresh cells are just above the limit, but the voltage sags during WiFi bursts and falls as the cells drain, which makes the board freeze, reset or drop WiFi. Use **4×AA (about 6V)**, a **USB power bank**, or an 18650 cell with a 5V step-up converter.
- **6V on the 5V pin is safe.** That pin only feeds the regulator, which accepts far more than 6V. It simply runs a little warm. A series diode (1N5819 or 1N4007, stripe towards the board) lowers it slightly and protects against reversed batteries.
- **Never** connect a battery to the ESP32-CAM **3.3V** pin. That bypasses the regulator and damages the board.
- A **470–1000 µF capacitor** across the ESP32-CAM's 5V and GND (+ to 5V) covers the short current bursts and helps a lot on batteries.
- The L293D loses about 1.5–2V internally, so from 6V the TT motors get roughly 4V. That's fine for a small robot.
- WiFi draws current in bursts, up to about 300 mA. The sketch keeps this down with a soft motor start and by sending no video unless you ask for it. If the **restarts** counter on the web page goes up, the supply is still too weak (see [Troubleshooting](#11-troubleshooting)).

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
| ESP32 pack **+** (4×AA or power bank) | ESP32-CAM **5V** | ESP32 supply |
| Motor pack **−**, ESP32 pack **−**, ESP32-CAM **GND** | Common GND | **Required** |

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

## 7. Step 2: WiFi control and pictures (`robo_wifi/`)

The main sketch. It uses the same wiring as the motor test.

### Connecting

1. Power the robot.
2. On your laptop, join the WiFi network:
   - **Network:** `Robo-CAM`
   - **Password:** `robo12345`
3. Open **http://192.168.4.1** in a browser. Type `http://`, not `https://`.

The laptop gets an address like `192.168.4.2`. The network has **no internet**. That's normal, because the robot *is* the network.

### The web page

- **Picture area:** empty until the first picture arrives.
- **Arrow pad:** on-screen buttons that work with mouse or touch and light up while pressed.
- **View:** `Photo (on request)` or `Video (uses more power)` — see below.
- **📷 Take picture:** in photo mode, asks for one picture. The **P** key does the same.
- **Size:** 160×120, 320×240 (default) or 640×480.
- **Speed slider:** motor power from 80 to 255.
- **Light:** turns the flash LED on or off. It's very bright, so use it in the dark.
- **Status line:** connection state, the direction being sent, the command delay in ms, and picture or video information.
- **Health line:** uptime, restarts and why, WiFi channel, signal strength and picture count. It turns red when the robot has restarted, which almost always means the ESP32's battery is too weak.

### Photo mode and video mode

**Photo mode (default)** sends nothing over WiFi except tiny drive commands. When you press 📷 the robot takes one fresh picture (about 4 KB at 320×240) and sends it. This keeps commands fast and saves power.

**Video mode** sends pictures continuously. The robot captures the next frame only after the previous one has been handed to the network, so video can never build up a backlog in front of the drive commands. It uses much more WiFi and power than photo mode.

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
| P | take a picture |
| (release all keys) | stop |

### Safety stop

The robot only moves while it keeps receiving commands. While a key is held, the browser resends the command every 200 ms; while stopped it checks in every second. If the robot hears nothing for **500 ms**, it stops the motors. It also stops the moment the connection closes. So if you close the tab, switch windows, walk out of WiFi range or the laptop disconnects, the robot stops instead of driving away.

---

## 8. How the code works

### One WebSocket for everything

The page opens a single WebSocket to `ws://192.168.4.1/ws` and keeps it open:

- **Browser → robot:** a direction as a short text message (`f`, `fl`, `s`, …), or `img` (take a picture), `v1` / `v0` (video on / off).
- **Robot → browser:** the same text echoed back, so the page can show the round-trip delay, and pictures as binary JPEG messages.

One open connection avoids the cost of a new HTTP request per command. Earlier versions used a request per command plus a separate MJPEG video stream on port 81, which filled the WiFi with video and left commands waiting seconds behind it.

### Startup (`setup()`)

1. **Brown-out detector off.** When WiFi starts, the current spike can make battery voltage dip, and the ESP32's brown-out detector would reset the board. It's turned off so the robot keeps running on AA cells.
2. **Restart counter.** The reason for the last restart and a counter kept in RTC memory are printed and shown on the page, which makes brown-outs easy to spot.
3. **Camera init.** The sensor is power-cycled with its PWDN pin first, because it keeps power across an ESP32 reset and can be left in a state where it won't answer. The driver starts at 640×480 so its JPEG buffer is big enough for any picture size, then switches to 320×240. It uses `CAMERA_GRAB_WHEN_EMPTY` with one buffer, so nothing is captured until a frame is taken.
4. **Motor PWM.** Each of the 4 motor pins gets its own PWM channel (1 kHz, 8-bit, so 0–255). Channels **2–5** are used on purpose, because the camera needs channel 0 and timer 0 for its 20 MHz clock (XCLK). Sharing them would break the camera.
5. **WiFi access point.** The board scans first and starts `Robo-CAM` on the least crowded of channels 1, 6 and 11, with WiFi sleep off and transmit power at the maximum.
6. **One web server on port 80**, with a connection budget (see below), and the picture task.

> **Tried and reverted:** an 80 MHz CPU (raised to 240 MHz only while streaming) and reduced transmit power, to save current on batteries. The link became slow and the board hung and reset. A dependable link matters more than battery life here, so the CPU stays at full speed and the radio at full power; give the ESP32 a supply that can feed it (4×AA or a power bank) instead of starving the radio. Changing the CPU clock while WiFi is running is best avoided.

### Main loop (`loop()`)

Every 5 ms it applies the motor ramp and checks the safety timeout. If the motors are running and no command has arrived for 500 ms, it stops them.

### Soft start

`drive()` only sets a target speed. `loop()` moves the actual PWM towards it by at most `RAMP_STEP` every 5 ms, so full speed is reached in about 50 ms. Slowing down and stopping are immediate. This limits the current spike when the motors start, which is a common cause of brown-outs on batteries.

### Pictures

`pictureTask` sleeps until a picture is asked for. It then captures a fresh frame (throwing away the buffered one), copies the JPEG, and hands it to `httpd_queue_work`, so the send happens inside the web server task and never collides with a command reply. In video mode it keeps doing this in a loop, but only once the previous frame has gone out.

The camera cannot be put to sleep between pictures on this board: the PWDN pin, the sensor's own standby mode and restarting the driver each all leave the driver hung or the sensor unresponsive. It therefore stays powered but idle, which costs some current but no CPU, and sends nothing until asked.

### Connection budget

lwIP allows **16 sockets in total**, and each HTTP server also uses 2 internally. An earlier version ran two servers of 7 clients each (18), so the robot ran out of sockets, `accept()` failed with `ENFILE` and everything hung. Now a single server allows 6 clients, purges the oldest connection when full, and uses TCP keep-alive to drop dead ones within about 4 seconds.

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

---

## 9. API

You can control the robot from any program (Python, curl, and so on) while connected to `Robo-CAM`.

| Request | Effect |
|---|---|
| `GET http://192.168.4.1/` | Control web page |
| `GET /go?d=f` | Drive. `d` = `f` `b` `l` `r` `fl` `fr` `bl` `br` `s` |
| `GET /set?speed=200` | Motor speed, 0–255 |
| `GET /set?size=qvga` | Picture size: `qqvga` (160×120), `qvga` (320×240), `vga` (640×480) |
| `GET /set?led=1` | Flash LED on (`1`) / off (`0`) |
| `GET /jpg` | One JPEG picture |
| `GET /info` | JSON health: uptime, restarts and reason, WiFi channel, signal, free memory, picture stats |
| `ws://192.168.4.1/ws` | Drive commands and pictures (see section 8) |

Remember the 500 ms safety stop: to keep moving, repeat `/go` more often than that.

```bash
curl "http://192.168.4.1/go?d=f"
```

```bash
curl -o shot.jpg "http://192.168.4.1/jpg"
```

### Link test

`tools/robo_test.js` measures command delay, lost messages, disconnects and picture delivery, and writes `robo_test_report.txt`. It only sends "stop" commands, so the robot doesn't move. Close the robot's browser tab first, because the script takes over the drive link.

```bash
node tools/robo_test.js
```

### Serial self-test

With the board on USB, send `p` in the serial monitor at 115200 baud. It takes 3 pictures and prints their size, brightness and timing, which checks the camera without any WiFi involved.

---

## 10. Customizing

Everything is at the top of `robo_wifi/robo_wifi.ino`:

| Setting | Default | What it does |
|---|---|---|
| `AP_SSID` | `Robo-CAM` | WiFi network name |
| `AP_PASS` | `robo12345` | WiFi password, at least 8 characters |
| `WIFI_TX_POWER` | `WIFI_POWER_19_5dBm` | WiFi transmit power, at the maximum. Lower values shrink current spikes but weaken the link. |
| `LEFT_IN1` … `RIGHT_IN2` | 14, 15, 13, 12 | Motor pins |
| `CMD_TIMEOUT_MS` | 500 | Safety stop delay |
| `RAMP_STEP` | 20 | Soft start rate (PWM steps per 5 ms) |
| `CAM_WARMUP_FRAMES` | 2 | Frames thrown away before a picture |
| `speed` | 200 | Starting motor speed |
| `jpeg_quality` (in `initCamera`) | 12 | 10 = better image and bigger files, 30 = worse image and smaller |

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
| Page shows "Reconnecting…" often | The link is dropping. Check the health line: if **restarts** goes up, the ESP32's battery is too weak (see [Power](#2-power)). Otherwise move closer, or raise `WIFI_TX_POWER` to `WIFI_POWER_19_5dBm`. On a Mac, AirDrop briefly moves the WiFi radio to other channels; `sudo ifconfig awdl0 down` turns that off until you restart. |
| Restart count goes up | Brown-out: the ESP32 isn't getting enough power. Use 4×AA or a USB power bank for it, and add a 470–1000 µF capacitor across its 5V and GND. |
| No picture, or "camera not found" | Reseat the camera ribbon cable. Over USB, send `p` in the serial monitor: it prints what the camera does. A full power cycle (not just reset) clears a sensor stuck in a bad state. |
| Commands feel slow | Switch **View** to photo mode, which leaves the WiFi almost free. The status line shows the delay in ms: under about 50 ms is good. |
| Robot resets when motors start | Power problem. Make sure the motors are on the 4×AA pack, not the ESP32 pack. Use fresh batteries or a power bank for the ESP32-CAM. |
| A wheel goes the wrong way | Swap that motor's two wires on the L293D. |
| Left/right swapped | Swap the motor connections between pins 3/6 and 11/14, or swap the pin numbers in the code. |
| Motors hum but don't turn | Speed too low for the battery voltage. Raise the speed slider, or use fresh batteries. |
| Flashing fails: `Failed to connect` / `Timed out` | Disconnect the GPIO12 wire while flashing. With an FTDI adapter, connect IO0 to GND and press RESET. |
| L293D gets hot | Normal under load. Don't stall the motors for long; the chip limit is 600 mA per channel. |

---

## 12. AI explorer (`server/`)

An optional server that lets an OpenAI vision model drive the robot: it looks
through the camera, explores the room, avoids obstacles and remembers what it
finds. The server keeps the robot's estimated position (dead reckoning from the
commands it sent) and its memory of earlier runs, so the model can plan several
moves from one picture instead of looking after every step.

Before a picture is sent to OpenAI the server fingerprints it locally (a
difference hash plus a colour histogram) and compares it with everything seen
before. An unchanged view is never sent twice — the model is told in one line of
text — and a familiar place arrives with the notes made there. That keeps both
the token bill and the WiFi traffic down.

To put the robot on your home network instead of it making its own, fill in
`HOME_SSID` and `HOME_PASS` at the top of `robo_wifi/robo_wifi.ino`. The server
then reaches the robot and the internet at once. If the robot cannot join, it
falls back to its own `Robo-CAM` network.

Your API key lives in `server/.env`, which git ignores, and is passed to the
container at run time — it is never committed or baked into the image.

```bash
cp server/.env.example server/.env   # paste your key
cd server && docker compose up --build
```

Then open http://localhost:8000. Set `ROBOT_HOST=fake` to try it with no
hardware. Full details, including calibration and cost, are in
[server/README.md](server/README.md).

---

## 13. Project structure

```
Robo/
├── README.md               this file
├── motor_test/
│   └── motor_test.ino      step 1: wiring and motor test (serial control)
├── robo_wifi/
│   └── robo_wifi.ino       step 2: WiFi access point + control + pictures
├── tools/
│   └── robo_test.js        link test: command delay, drops, picture delivery
└── server/                 optional: GPT-4.1 explores the room
    ├── app.py              FastAPI server and dashboard
    ├── agent.py            the model's tools and the mission loop
    ├── robot.py            drive commands (ws) and pictures (/jpg), dead reckoning
    ├── memory.py           notes and snapshots that survive between runs
    ├── vision_index.py     local picture fingerprints: is this view new?
    ├── test_agent.py       offline check: stub model, fake robot
    └── docker-compose.yml  runs it, reading the key from .env
```
