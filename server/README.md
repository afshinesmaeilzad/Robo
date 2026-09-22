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

2. **Put the robot on your home WiFi.** In `robo_wifi/robo_wifi.ino`, fill in

   ```cpp
   #define HOME_SSID "your-network"
   #define HOME_PASS "your-password"
   ```

   then flash it. The robot joins that network and prints its address; it also
   answers to `robo.local`. If it cannot join in 15 s it falls back to making
   its own `Robo-CAM` network, so a typo never locks you out.

   This matters because the server needs the robot **and** the internet at the
   same time. On the robot's own network your computer has no internet.

3. **Set `ROBOT_HOST`** in `.env` to the address the robot printed (or
   `robo.local`). Use `fake` to try the server with no hardware at all.

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

## Not sending the same picture twice

Every snapshot is fingerprinted **locally** (`vision_index.py`): a 64-bit
difference hash of its structure, plus a colour histogram. Comparing those takes
microseconds and needs no model, so before anything goes to OpenAI the server
can answer two questions:

- **"Is this the same view I already sent?"** At 97% similar or above, the
  picture is *not* sent again. The model gets a line of text instead: the view
  has not changed, and the note made there. A robot that is stuck against a
  chair therefore costs a few tokens per step instead of a picture per step.
- **"Have I been here before?"** At 80% or above, the notes made at those
  earlier views are added as text, so the model is told it is going in circles
  and can use what it learned last time.

`remember()` ties the note to the picture it was made from, so the index grows
into a small searchable memory of places: fingerprint, position and description.
It is kept in `data/vision_index.json` and loaded at startup.

The dashboard shows the count: **pictures taken, sent, skipped**.

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

## Cost

Each step is one model call with at most a few small pictures. At 320×240 a
picture is a few hundred tokens. `PICTURE_SIZE=qqvga` and a smaller `max_steps`
keep a mission cheap while you experiment, and unchanged views cost text rather
than an image.

## Testing

```bash
python test_agent.py
```

Runs a scripted mission with a stub model and a fake robot: no key, no hardware.
It checks the tool handling, the image pruning, the dead reckoning and the
memory file.
