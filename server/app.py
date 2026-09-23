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

from agent import Agent
from discover import find_robot
from memory import Memory
from robot import Calibration, FakeRobot, Robot, RobotError
from vision_index import VisionIndex

load_dotenv()

ROBOT_HOST = os.getenv("ROBOT_HOST", "192.168.4.1")
MODEL = os.getenv("OPENAI_MODEL", "gpt-4.1")
PICTURE_SIZE = os.getenv("PICTURE_SIZE", "qvga")
IMAGE_DETAIL = os.getenv("IMAGE_DETAIL", "low")        # low | high | auto
REASONING = os.getenv("REASONING_EFFORT", "") or None  # none | minimal | low | ...
DATA_DIR = Path(os.getenv("DATA_DIR", Path(__file__).parent / "data"))
# Rough prices per million tokens, for the estimate on the dashboard only.
# Check what your account is actually charged; set these in .env.
PRICE_IN = float(os.getenv("PRICE_IN_PER_M", "0.40"))
PRICE_CACHED = float(os.getenv("PRICE_CACHED_PER_M", "0.10"))
PRICE_OUT = float(os.getenv("PRICE_OUT_PER_M", "1.60"))
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
    index=index,
    image_detail=IMAGE_DETAIL,
    reasoning_effort=REASONING,
)
task: asyncio.Task | None = None


class StartRequest(BaseModel):
    goal: str = "Explore the room, map it and avoid obstacles."
    max_steps: int = 40


@app.post("/api/start")
async def start(req: StartRequest):
    global task
    if client is None:
        raise HTTPException(400, "No OPENAI_API_KEY. Put it in server/.env and restart.")
    if task and not task.done():
        raise HTTPException(409, "A mission is already running.")
    task = asyncio.create_task(agent.run(req.goal, req.max_steps))
    return {"started": True, "goal": req.goal}


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
    except RobotError as exc:
        raise HTTPException(502, str(exc)) from exc
    return {"pose": vars(pose)}


@app.get("/api/state")
async def state():
    m = agent.mission
    return {
        "robot_host": ROBOT_HOST,
        "model": MODEL,
        "key_loaded": client is not None,
        "detail": IMAGE_DETAIL,
        "reasoning": REASONING or "default",
        "pose": vars(robot.pose),
        "trail": [vars(p) for p in robot.trail[-200:]],
        "notes": [n.as_text() for n in memory.notes[-30:]],
        "index_size": len(index.shots),
        "mission": None
        if not m
        else {
            "goal": m.goal,
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


@app.on_event("startup")
async def startup():
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
const start = () => post('/api/start', {goal: $('goal').value});
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
      ` · index ${s.index_size} views` +
      (m ? ` · ${m.running ? 'running' : 'idle'} step ${m.steps}/${m.max_steps}` +
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
