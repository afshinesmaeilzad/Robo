# Robo brain — GPT explores the room

A small server that sits between the robot and an OpenAI vision model. The model
gets a camera picture and a handful of tools; the server keeps everything a
language model is bad at: where the robot is, what it has already seen, and the
limits that stop it driving into things.

```
 ┌───────────┐   pictures + tool calls   ┌──────────────┐   ws /ws : commands   ┌──────────┐
 │  OpenAI   │ ◀───────────────────────▶ │    server    │ ────────────────────▶ │ ESP32-CAM│
 │  GPT-4.1  │                           │  FastAPI     │ ◀──────────────────── │  robot   │
 └───────────┘                           │  pose, memory│    GET /jpg : picture └──────────┘
                                         └──────┬───────┘
                                    http://localhost:8000 (watch, start, stop)
```

## Setup

1. **Key.** Copy the example and paste your key. `.env` is git-ignored, so the
   key stays on this machine and never reaches GitHub or the Docker image.

   ```bash
   cp server/.env.example server/.env
   ```

2. **Flash the robot** with `robo_wifi` (or `robo_lite`) and power it up.

3. **Join the robot's WiFi** (`Robo-CAM`). The server has to be on the same
   network as the robot. Your computer then has no internet over WiFi, so the
   OpenAI calls need another route: an ethernet adapter, a phone over USB, or a
   second WiFi adapter. Without one, run with `ROBOT_HOST=fake` to try the
   server, or put the robot on your home WiFi instead (see "Robot on your own
   WiFi" below).

## Run

With Docker:

```bash
cd server && docker compose up --build
```

Without Docker:

```bash
cd server && pip install -r requirements.txt && uvicorn app:app --port 8000
```

Then open **http://localhost:8000**: the camera view, the estimated path, the
live log and the memory, with Start and Stop buttons.

To try the whole thing without hardware, set `ROBOT_HOST=fake` in `.env`.

## How the agent works

The model is given these tools:

| Tool | What it does |
|---|---|
| `look(reason)` | takes a picture and shows it to the model |
| `move(direction, ms)` | one move, then stop — for small corrections |
| `follow_path(steps, purpose)` | up to 6 moves in a row, then **one** picture |
| `remember(label, description, tags)` | saves a note with the current position |
| `recall(query)` | searches earlier notes |
| `finish(summary)` | ends the mission |

**Fewer pictures.** Pictures cost tokens and WiFi, and the robot's link is the
weak point. The server therefore tracks the robot's position by dead reckoning
from the commands it sent (`cm_per_sec`, `deg_per_sec` in `.env`), and tells the
model where it is after every move. The model is pushed to plan several moves
from one picture with `follow_path` instead of looking after every step. Older
pictures are dropped from the conversation, so cost per step stays flat.

**Memory.** Notes go to `data/memories.json` with the position they were made
from, and every picture is kept in `data/snapshots/`. Notes are loaded at
startup, so later missions can recall what earlier ones found.

**Safety.** One move is capped at 2 s, a mission at `max_steps` (40 by default),
and the robot stops by itself 0.5 s after the last command, so a crashed or
disconnected server leaves the robot standing still rather than driving away.
Stop ends the mission at the next step.

## Calibration

Dead reckoning is only as good as two numbers in `.env`. Measure them once:

1. Open the dashboard and press **▲ forward** (400 ms) a few times, measure the
   distance travelled, and work out cm per second at your `DRIVE_SPEED`.
2. Press **spin** several times, count the degrees turned, and do the same.

The estimate drifts, especially on carpet. It is a hint for the model, not a map.

## Robot on your own WiFi

Simplest if you only have one WiFi adapter: change the sketch from access-point
mode to joining your home network (`WiFi.mode(WIFI_STA)` and `WiFi.begin(ssid,
pass)`), then set `ROBOT_HOST` to the address it gets. Your computer keeps its
internet, and the server reaches both the robot and OpenAI.

## Cost

Each step is one model call with at most a few small pictures. At 320×240 a
picture is a few hundred tokens. `PICTURE_SIZE=qqvga` and a smaller `max_steps`
keep a mission cheap while you experiment.

## Testing

```bash
python test_agent.py
```

Runs a scripted mission with a stub model and a fake robot: no key, no hardware.
It checks the tool handling, the image pruning, the dead reckoning and the
memory file.
