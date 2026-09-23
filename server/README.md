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

## Where each setting goes

| What | Where | Example |
|---|---|---|
| OpenAI key | `server/.env` | `OPENAI_API_KEY=sk-...` |
| WiFi name and password | top of `robo_wifi/robo_wifi.ino` | `#define HOME_SSID "my-wifi"` |
| Robot's address | `server/.env` | `ROBOT_HOST=auto` |

**The robot does not need this machine's address.** The server is the client: it
opens the connection to the robot, sends the commands and asks for the pictures.
So your laptop's IP can change, and nothing on the robot has to be updated.

**Finding the robot** works three ways, set by `ROBOT_HOST`:

- `auto` — at startup the server asks every address on the local network whether
  it answers `/info` like a Robo. Takes a second or two, and works inside Docker.
  The **🔎 find robot** button on the dashboard does the same at any time.
- `robo.local` — the name the robot advertises. Works on the host, but usually
  not inside a container.
- `192.168.1.42` — the address the robot prints on the serial monitor at boot.

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

## Two kinds of mission

The dashboard has a mode next to the goal, and `auto` reads it from the wording:

| Mode | For | Pictures |
|---|---|---|
| **explore & map** | learning the layout of a space | unchanged views are **not** sent again; familiar places arrive as notes |
| **find a target** | finding, approaching or following something | **every** picture is sent in full |

The difference matters. When mapping, a view that has not changed tells the
model nothing new, so sending it again is wasted money and WiFi. When looking
for something, the opposite is true: the target moves, and the difference between
"a chair" and "a chair with the ball behind it" is exactly what a similarity
score throws away.

The two modes also get different instructions. Exploring is told to cover ground
in long runs and not to stop at the first glimpse of the target. A target mission
is told to sweep the room in small turns, look after most moves, centre the
target before approaching, close in slowly, and never chase a person or an animal
or drive at anything breakable.

`auto` picks *find a target* when the goal says find, look for, search, locate,
follow, track, approach or fetch; otherwise it explores.

## Not sending the same picture twice (exploring)

Every snapshot is fingerprinted **locally** (`vision_index.py`): a 64-bit
difference hash of its structure, plus a colour histogram. Comparing those takes
microseconds and needs no model, so before anything goes to OpenAI the server
can answer two questions:

- **"Is this the same view I already sent?"** At 95% similar or above, the
  picture is *not* sent again. The model gets a line of text instead: the view
  has not changed, and the note made there. A robot that is stuck against a
  chair therefore costs a few tokens per step instead of a picture per step.
- **"Have I been here before?"** At 90% or above, the notes made at those
  earlier views are added as text, so the model is told it is going in circles
  and can use what it learned last time.

`remember()` ties the note to the picture it was made from, so the index grows
into a small searchable memory of places: fingerprint, position and description.
It is kept in `data/vision_index.json` and loaded at startup.

The dashboard shows the count: **pictures taken, sent, skipped**.

## When the robot cannot move

A wheel catches on a rug, the robot noses into a chair leg, the battery sags
under load: the command is sent, the position estimate moves, and the robot
stays exactly where it was.

The server catches this with the same fingerprints: after a move of 300 ms or
more, if the new picture is **95% or more** the same as the one before it, the
robot did not actually move. The server then drives an escape by itself —
**back 600 ms, then turn 500 ms**, turning the other way each time so it doesn't
repeat the same escape — takes a fresh picture, and tells the model what
happened, so it picks a different direction instead of pushing the same way.

Short nudges under 300 ms are exempt: they may genuinely change nothing.

The dashboard counts these as **stuck Nx**.

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

**Old notes are hints, not facts.** Notes from earlier runs are handed over with
their age ("14 min ago") and a warning that the room may have changed since.
Without that, a target mission reads "bear lamp reached and viewed" and finishes
on the spot — even after the lamp has been moved. A target mission may only end
at once if `finish(seen_now=true)` says the target is in the picture just taken;
otherwise, having taken fewer than 6 pictures or driven less than a metre, the
server answers: that is one spot, not a search — go and look somewhere else.

**Keep going (exploring only).** A model told "stop when you get there" tends to stop at the
first glimpse of the target. If `finish()` is called having driven less than
150 cm, and less than a third of the steps are used, the server questions it once
("you have driven only N cm; unless you are blocked, keep going") and obeys a
second, insistent call.

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

Pictures are sent at **low detail** by default (`IMAGE_DETAIL=low`): a fixed,
small token cost, and enough to see a floor, a doorway or a chair leg. `high`
costs several times more for detail this robot does not need.

On a model that thinks before answering, `REASONING_EFFORT` (none | minimal |
low | medium | high) trades thinking for speed and cost. `none` suits driving:
the pictures do the work. A model that does not take the setting is detected and
the setting dropped, rather than the mission failing.

The dashboard shows tokens in (and how many of those were cached), tokens out,
seconds per step and an estimated price. **Set the prices in `.env` to match the
model you are running**; the defaults are `gpt-6-luna`'s — $0.10 per million in,
$0.01 cached, $0.50 out. Cache *writes* ($0.125/M) are not reported by the API,
so they are missing from the estimate and the real bill runs slightly higher.

Measured on real missions with `gpt-4.1-mini` at 320×240: about 8 seconds per
step, nearly all of it model latency rather than the robot. With low detail a
picture costs roughly 85 tokens instead of several hundred.

## Testing

```bash
python test_agent.py
```

Runs three scripted missions against a stub model and a fake robot whose view
changes as it drives: no key, no hardware. It checks the tool handling, the
image pruning, the dead reckoning, the memory file, the "same view" skip and the
stuck escape (28 checks).
