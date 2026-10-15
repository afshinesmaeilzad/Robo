"""Robo brain server.

Runs next to the robot (on the same WiFi), gives a GPT model with vision a few
tools, and lets it explore. Open http://localhost:8000 to watch, start and stop.

The OpenAI key is read from the environment (.env locally, env_file in Docker)
and never leaves this machine.
"""

from __future__ import annotations

import asyncio
import os
import time
from collections import deque
from pathlib import Path

from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse, JSONResponse, Response
from openai import AsyncOpenAI
from pydantic import BaseModel

from agent import Agent, mode_for
from discover import find_robot
from memory import Memory
from robot import Calibration, FakeRobot, Robot, RobotError
from vision_index import VisionIndex, fingerprint, similarity

load_dotenv()

ROBOT_HOST = os.getenv("ROBOT_HOST", "192.168.4.1")
MODEL = os.getenv("OPENAI_MODEL", "gpt-4.1")
PICTURE_SIZE = os.getenv("PICTURE_SIZE", "qvga")
IMAGE_DETAIL = os.getenv("IMAGE_DETAIL", "low")        # low | high | auto
# Finding something needs to actually see it, so target missions get bigger,
# sharper pictures. At gpt-6-luna's prices that is fractions of a cent.
TARGET_PICTURE_SIZE = os.getenv("TARGET_PICTURE_SIZE", "vga")
TARGET_IMAGE_DETAIL = os.getenv("TARGET_IMAGE_DETAIL", "high")
REASONING = os.getenv("REASONING_EFFORT", "") or None  # none | minimal | low | ...
DATA_DIR = Path(os.getenv("DATA_DIR", Path(__file__).parent / "data"))
# Rough prices per million tokens, for the estimate on the dashboard only.
# Check what your account is actually charged; set these in .env.
PRICE_IN = float(os.getenv("PRICE_IN_PER_M", "0.10"))
PRICE_CACHED = float(os.getenv("PRICE_CACHED_PER_M", "0.01"))
PRICE_OUT = float(os.getenv("PRICE_OUT_PER_M", "0.50"))
CAL = Calibration(
    cm_per_sec=float(os.getenv("CM_PER_SEC", "20")),
    deg_per_sec=float(os.getenv("DEG_PER_SEC", "180")),
    speed=int(os.getenv("DRIVE_SPEED", "200")),
)

app = FastAPI(title="Robo brain")
events: deque[dict] = deque(maxlen=400)
memory = Memory(DATA_DIR)
index = VisionIndex(DATA_DIR)
robot: Robot = FakeRobot(CAL) if ROBOT_HOST == "fake" else Robot(ROBOT_HOST, CAL)
client = AsyncOpenAI() if os.getenv("OPENAI_API_KEY") else None
agent = Agent(
    robot=robot,
    memory=memory,
    client=client,
    model=MODEL,
    on_event=lambda kind, text: events.append({"t": time.time(), "kind": kind, "text": text}),
    picture_size=PICTURE_SIZE,
    keep_images=int(os.getenv("KEEP_IMAGES", "6")),
    index=index,
    image_detail=IMAGE_DETAIL,
    reasoning_effort=REASONING,
    target_picture_size=TARGET_PICTURE_SIZE,
    target_image_detail=TARGET_IMAGE_DETAIL,
)
task: asyncio.Task | None = None


class StartRequest(BaseModel):
    goal: str = "Explore the room, map it and avoid obstacles."
    max_steps: int = 60
    # explore = map it, skipping unchanged pictures; target = send every picture;
    # auto = decide from the wording of the goal
    mode: str = "auto"


@app.post("/api/start")
async def start(req: StartRequest):
    global task
    if client is None:
        raise HTTPException(400, "No OPENAI_API_KEY. Put it in server/.env and restart.")
    if task and not task.done():
        raise HTTPException(409, "A mission is already running.")
    mode = req.mode if req.mode in ("explore", "target") else mode_for(req.goal)
    task = asyncio.create_task(agent.run(req.goal, req.max_steps, mode))
    return {"started": True, "goal": req.goal, "mode": mode}


@app.post("/api/stop")
async def stop():
    if agent.mission:
        agent.mission.stop_requested = True
    try:
        await robot.stop()
    except RobotError as exc:
        return {"stopped": False, "error": str(exc)}
    return {"stopped": True}


@app.post("/api/drive")
async def drive(direction: str, ms: int = 400):
    """Manual control, for testing the link and calibrating."""
    if agent.mission and agent.mission.running:
        raise HTTPException(409, "A mission is running; stop it first.")
    try:
        pose = await robot.move(direction, ms)
        cm, blocked, paused = await robot.sonar()
        agent.last_sonar = {"cm": cm, "blocked": blocked, "paused": paused}
    except RobotError as exc:
        raise HTTPException(502, str(exc)) from exc
    return {"pose": vars(pose), "sonar": agent.last_sonar}


@app.post("/api/calibrate")
async def calibrate(ms: int = 1000, runs: int = 3):
    """Measure how far the robot really travels, using the range finder.

    Point it at a wall 60-200 cm away with clear floor between, and it drives
    forward in short bursts, watching the distance close. That is a measurement
    rather than the guess in .env, and everything the model is told about
    distances depends on it.
    """
    if agent.mission and agent.mission.running:
        raise HTTPException(409, "A mission is running; stop it first.")

    async def fresh() -> tuple[int, bool]:
        """A reading taken after we asked, not before.

        Standing still the robot pings once a second, so the first answer can
        describe where it was a moment ago. Waiting past one ping and reading
        again gives a current one.
        """
        await robot.sonar()
        await asyncio.sleep(1.2)
        cm, blocked, _ = await robot.sonar()
        return cm, blocked

    cm_before, blocked = await fresh()
    if cm_before < 0:
        raise HTTPException(400, "No range reading. Is the sensor fitted and facing a wall?")
    if blocked:
        raise HTTPException(400, f"Only {cm_before} cm ahead: back away from the wall first.")

    measured = []
    for _ in range(max(1, min(runs, 6))):
        start, _ = await fresh()
        if start < 35:
            break  # close enough to the wall; stop before nosing into it
        await robot.move("f", ms)
        end, _ = await fresh()
        if start > 0 and end > 0 and start > end:
            measured.append((start - end) / (ms / 1000))
    if not measured:
        raise HTTPException(400, "Could not measure: give it a clear metre or two of wall.")

    speed_now = robot.cal.speed
    # The median, not the mean: one bad echo off an angled surface should not
    # decide how far the robot thinks it travels.
    ordered = sorted(measured)
    per_sec = ordered[len(ordered) // 2]
    spread = (max(measured) - min(measured)) / per_sec if per_sec and len(measured) > 1 else 0
    full = per_sec * 255 / speed_now  # .env holds the figure at full power
    robot.cal.cm_per_sec = full
    return {
        "runs": [round(m, 1) for m in measured],
        "cm_per_sec_at_speed": round(per_sec, 1),
        "speed": speed_now,
        "cm_per_sec_full_power": round(full, 1),
        "spread": round(spread, 2),
        "trust": "good" if len(measured) >= 3 and spread < 0.4 else
                 "rough - run it again facing a flat wall",
        "put_in_env": f"CM_PER_SEC={full:.0f}",
    }


@app.post("/api/calibrate_turn")
async def calibrate_turn(step_ms: int = 400, max_steps: int = 40):
    """Measure how fast the robot really turns, by spinning until the view returns.

    A full turn brings the camera back to where it started, and the picture index
    already knows how to tell one view from another. Needs room to spin and a
    scene with some detail in it - a blank wall all round will not do.
    """
    if agent.mission and agent.mission.running:
        raise HTTPException(409, "A mission is running; stop it first.")
    first = await robot.picture(PICTURE_SIZE)
    ref = fingerprint(first)
    scores, elapsed = [], 0.0
    left_home = False
    for _ in range(max_steps):
        await robot.move("r", step_ms)
        elapsed += step_ms / 1000
        await asyncio.sleep(0.25)
        score = similarity(*ref, *fingerprint(await robot.picture(PICTURE_SIZE)))
        scores.append(round(score, 3))
        if score < 0.75:
            left_home = True          # we are looking at something else now
        elif left_home and score > 0.88:
            deg_per_sec = 360 / elapsed
            full = deg_per_sec * 255 / robot.cal.speed
            robot.cal.deg_per_sec = full
            return {
                "turned_full_circle_in": round(elapsed, 1),
                "deg_per_sec_at_speed": round(deg_per_sec),
                "speed": robot.cal.speed,
                "deg_per_sec_full_power": round(full),
                "scores": scores,
                "put_in_env": f"DEG_PER_SEC={full:.0f}",
            }
    raise HTTPException(
        400,
        "Never came back to the starting view. Give it room to spin, point it at "
        f"something with detail, and try again. Similarities seen: {scores}",
    )


@app.get("/api/state")
async def state():
    m = agent.mission
    return {
        "robot_host": ROBOT_HOST,
        "model": MODEL,
        "key_loaded": client is not None,
        "detail": agent.detail_now(),
        "reasoning": REASONING or "default",
        "pose": vars(robot.pose),
        "sonar": agent.last_sonar,
        "speed": robot.cal.speed,
        "trail": [vars(p) for p in robot.trail[-200:]],
        "notes": [n.as_text() for n in memory.notes[-30:]],
        "index_size": len(index.shots),
        "mission": None
        if not m
        else {
            "goal": m.goal,
            "mode": m.mode,
            "running": m.running,
            "steps": m.steps,
            "max_steps": m.max_steps,
            "pictures": m.pictures,
            "images_sent": m.images_sent,
            "images_skipped": m.images_skipped,
            "stuck_events": m.stuck_events,
            "tokens_in": m.tokens_in,
            "tokens_out": m.tokens_out,
            "tokens_cached": m.tokens_cached,
            "cost": round(
                (m.tokens_in - m.tokens_cached) / 1e6 * PRICE_IN
                + m.tokens_cached / 1e6 * PRICE_CACHED
                + m.tokens_out / 1e6 * PRICE_OUT,
                4,
            ),
            "seconds": round(time.time() - m.started),
            "summary": m.finished_summary,
            "error": m.error,
            "ended": m.ended,
        },
        "events": list(events)[-80:],
    }


@app.get("/api/picture")
async def picture(fresh: bool = False):
    """The last picture the agent saw, or a new one."""
    jpeg = agent.last_jpeg
    if fresh or jpeg is None:
        try:
            jpeg = await robot.picture(PICTURE_SIZE)
            agent.last_jpeg = jpeg
        except RobotError as exc:
            raise HTTPException(502, str(exc)) from exc
    return Response(jpeg, media_type="image/jpeg", headers={"Cache-Control": "no-store"})


@app.get("/api/robot")
async def robot_info():
    return JSONResponse(await robot.info())


@app.post("/api/find")
async def find():
    """Look for the robot on the local network and use what is found."""
    global ROBOT_HOST
    if isinstance(robot, FakeRobot):
        return {"found": "fake"}
    host = await find_robot(None if ROBOT_HOST in ("auto", "") else ROBOT_HOST)
    if not host:
        raise HTTPException(
            404,
            "No robot found. Is it powered on and on this network? "
            "Check the serial monitor for the address it printed.",
        )
    ROBOT_HOST = robot.host = host
    return {"found": host}


async def watchdog() -> None:
    """Keep the robot findable without anyone restarting anything.

    A robot that is switched off, runs flat, or comes back on a different address
    used to mean a dead server until someone noticed. Now the failures are
    counted, the connections are rebuilt, and the network is searched again.
    """
    while True:
        await asyncio.sleep(20)
        if isinstance(robot, FakeRobot):
            continue
        if agent.mission and agent.mission.running:
            continue  # a mission does its own retrying; do not fight it
        if robot.failures < 2:
            continue
        events.append({"t": time.time(), "kind": "find",
                       "text": f"{robot.failures} failed requests: looking for the robot"})
        await robot._fresh_http()
        found = await find_robot(robot.host)
        if found:
            moved = found != robot.host
            globals()["ROBOT_HOST"] = robot.host = found
            robot.failures = 0
            events.append({"t": time.time(), "kind": "find",
                           "text": f"robot at {found}" + (" (it had moved)" if moved else "")})
        else:
            events.append({"t": time.time(), "kind": "find",
                           "text": "still cannot find the robot; is it powered on?"})


@app.on_event("startup")
async def startup():
    asyncio.create_task(watchdog())
    """With ROBOT_HOST=auto, go looking for the robot before the first mission."""
    global ROBOT_HOST
    if ROBOT_HOST != "auto":
        return
    events.append({"t": time.time(), "kind": "find", "text": "looking for the robot..."})
    host = await find_robot()
    ROBOT_HOST = robot.host = host or "192.168.4.1"
    events.append(
        {"t": time.time(), "kind": "find",
         "text": f"robot at {host}" if host else "no robot found; set ROBOT_HOST in .env"}
    )


@app.on_event("shutdown")
async def shutdown():
    if agent.mission:
        agent.mission.stop_requested = True
    await robot.close()


PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>Robo brain</title>
<style>
body{margin:0;background:#111;color:#eee;font-family:system-ui,sans-serif;padding:16px;
     display:grid;grid-template-columns:minmax(320px,1fr) minmax(320px,1fr);gap:16px}
h1{grid-column:1/-1;font-size:18px;margin:0}
img,canvas{width:100%;background:#000;border-radius:8px}
#log{height:320px;overflow:auto;background:#181818;border-radius:8px;padding:8px;font-size:12px;
     font-family:ui-monospace,monospace;white-space:pre-wrap}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px}
input,button,textarea{background:#333;color:#eee;border:0;border-radius:6px;padding:8px}
button{cursor:pointer}button.go{background:#2a7}button.stop{background:#a33}
#goal{flex:1;min-width:240px}
.k{color:#8a8}.err{color:#e66}.note{color:#cb8}
</style></head><body>
<h1>Robo brain <span id="status" class="k"></span></h1>
<div>
  <div class="row">
    <input id="goal" value="Explore the room, map it and avoid obstacles.">
    <input id="steps" type="number" min="5" max="300" value="60" style="width:70px" title="How many steps before the mission is stopped">
    <select id="mode" title="Explore saves pictures by skipping unchanged views. Target sends every picture.">
      <option value="auto" selected>auto</option>
      <option value="explore">explore &amp; map</option>
      <option value="target">find a target</option>
    </select>
    <button class="go" onclick="start()">Start</button>
    <button class="stop" onclick="stop()">Stop</button>
  </div>
  <img id="cam" alt="camera">
  <div class="row" style="margin-top:8px">
    <button onclick="drive('l')">◀ spin</button>
    <button onclick="drive('f')">▲ forward</button>
    <button onclick="drive('b')">▼ back</button>
    <button onclick="drive('r')">spin ▶</button>
    <button onclick="refresh(true)">📷 picture</button>
    <button onclick="post('/api/find')">🔎 find robot</button>
    <button onclick="calibrate()" title="Point at a wall 60-200cm away, then press">📐 measure speed</button>
    <button onclick="calibrateTurn()" title="Needs room to spin">🔄 measure turn</button>
  </div>
  <canvas id="map" height="320"></canvas>
</div>
<div>
  <div id="log"></div>
  <h3 style="font-size:14px">Memory</h3>
  <div id="notes" style="font-size:12px;color:#cb8"></div>
</div>
<script>
const $ = (id) => document.getElementById(id);
async function post(url, body){
  const r = await fetch(url, {method:'POST', headers:{'Content-Type':'application/json'},
                             body: body ? JSON.stringify(body) : null});
  if (!r.ok) alert((await r.json()).detail || r.statusText);
}
async function calibrate(){
  const r = await fetch('/api/calibrate', {method: 'POST'});
  const d = await r.json();
  alert(r.ok ? `Measured ${d.cm_per_sec_at_speed} cm/s at speed ${d.speed}.\n` +
               `Put this in server/.env:  ${d.put_in_env}` : (d.detail || 'failed'));
}
async function calibrateTurn(){
  const r = await fetch('/api/calibrate_turn', {method: 'POST'});
  const d = await r.json();
  alert(r.ok ? `A full turn took ${d.turned_full_circle_in}s at speed ${d.speed}` +
               ` (${d.deg_per_sec_at_speed} deg/s).\nPut this in server/.env:  ${d.put_in_env}`
             : (d.detail || 'failed'));
}
const start = () => post('/api/start', {goal: $('goal').value, mode: $('mode').value,
                                         max_steps: +$('steps').value});
const stop = () => post('/api/stop');
const drive = (d) => post('/api/drive?direction=' + d + '&ms=400');
function refresh(fresh){ $('cam').src = '/api/picture?t=' + Date.now() + (fresh ? '&fresh=1' : ''); }
refresh(false);

function drawMap(trail, pose){
  const c = $('map'), ctx = c.getContext('2d');
  c.width = c.clientWidth;
  ctx.fillStyle = '#000'; ctx.fillRect(0, 0, c.width, c.height);
  if (!trail.length) return;
  const xs = trail.map(p => p.x), ys = trail.map(p => p.y);
  const pad = 40;
  const min = Math.min(...xs, ...ys) - pad, max = Math.max(...xs, ...ys) + pad;
  const k = Math.min(c.width, c.height) / Math.max(max - min, 100);
  const X = (x) => (x - min) * k, Y = (y) => c.height - (y - min) * k;
  ctx.strokeStyle = '#2a7'; ctx.lineWidth = 2; ctx.beginPath();
  trail.forEach((p, i) => i ? ctx.lineTo(X(p.x), Y(p.y)) : ctx.moveTo(X(p.x), Y(p.y)));
  ctx.stroke();
  ctx.fillStyle = '#e66';
  ctx.beginPath(); ctx.arc(X(pose.x), Y(pose.y), 5, 0, 7); ctx.fill();
}

let lastEvent = 0;
async function tick(){
  try {
    const s = await (await fetch('/api/state')).json();
    const m = s.mission;
    $('status').textContent = `· ${s.model} (detail ${s.detail}, reasoning ${s.reasoning})` +
      ` · robot ${s.robot_host}` +
      (s.key_loaded ? '' : ' · NO API KEY') +
      (s.sonar && s.sonar.cm >= 0
        ? ` · ${s.sonar.blocked ? '⛔' : '📏'} ${s.sonar.cm}cm ahead` : '') +
      ` · speed ${s.speed}` +
      ` · index ${s.index_size} views` +
      (m ? ` · ${m.mode} · ${m.running ? 'running' : `idle (${m.ended || 'not started'})`}` +
           ` step ${m.steps}/${m.max_steps}` +
           ` · ${m.pictures} pictures, ${m.images_sent} sent, ${m.images_skipped} skipped` +
           (m.stuck_events ? ` · stuck ${m.stuck_events}x` : '') +
           ` · ${(m.tokens_in/1000).toFixed(0)}k in (${(m.tokens_cached/1000).toFixed(0)}k cached)` +
           ` / ${(m.tokens_out/1000).toFixed(1)}k out` +
           ` ≈ $${m.cost.toFixed(3)}` +
           (m.steps ? ` · ${(m.seconds/m.steps).toFixed(1)}s per step` : '') : '');
    $('log').innerHTML = s.events.map(e =>
      `<span class="${['error','stuck'].includes(e.kind) ? 'err' : e.kind === 'memory' ? 'note' : 'k'}">` +
      `[${e.kind}]</span> ${e.text.replace(/</g, '&lt;')}`).join('\\n');
    if (s.events.length && s.events[s.events.length-1].t !== lastEvent){
      lastEvent = s.events[s.events.length-1].t;
      $('log').scrollTop = $('log').scrollHeight;
      if (s.events.some(e => e.kind === 'picture' && e.t === lastEvent)) refresh(false);
    }
    $('notes').textContent = s.notes.join('\\n');
    drawMap(s.trail, s.pose);
  } catch (e) { $('status').textContent = '· server not answering'; }
}
tick(); setInterval(tick, 1500);
</script></body></html>"""


@app.get("/", response_class=HTMLResponse)
async def dashboard():  # not "index": that name holds the VisionIndex
    return PAGE
