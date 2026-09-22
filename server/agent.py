"""The exploring agent.

A GPT model with vision drives the robot through a handful of tools. The server
keeps the parts a language model is bad at: where the robot is (dead reckoning),
what it has already seen (memory), and the limits that stop it hurting itself
(how far one move may go, how many steps a mission may take).

Pictures are expensive, in tokens and in WiFi, so the model is pushed to plan
several moves from one picture with follow_path() rather than looking after
every step.
"""

from __future__ import annotations

import asyncio
import base64
import json
import time
from dataclasses import dataclass, field
from typing import Any, Callable

from openai import AsyncOpenAI

from memory import Memory, Note
from robot import Robot, RobotError

SYSTEM_PROMPT = """You are driving a small two-wheel robot with a camera, exploring an indoor room.

Your goals, in order:
1. Do not crash. The robot has no bumper or distance sensor: the camera is all you have.
2. Explore the space and build up a picture of it.
3. Remember what you find (rooms, furniture, doors, obstacles, dead ends) with remember().

How the robot moves:
- "f"/"b" drive forward/back, "l"/"r" spin in place, "fl"/"fr"/"bl"/"br" curve.
- Moves are measured in milliseconds. A 1000 ms forward move is roughly {cm_per_sec:.0f} cm;
  a 1000 ms spin is roughly {deg_per_sec:.0f} degrees. These are estimates, not exact.
- The camera looks forward and low. Something close and large in the picture is an
  obstacle. The floor in the lower half of the picture is the path ahead.

Working method:
- Take a picture with look(), then plan SEVERAL moves from it with follow_path().
  Pictures are costly, so do not look after every small move.
- In a tight spot, move in short steps (300-500 ms) and look more often.
- Before driving forward, be sure the floor ahead is clear in the last picture.
  If you are unsure, spin a little and look again instead of driving blind.
- The server tells you the estimated position after every move. It drifts, so
  trust the picture over the numbers.
- Call remember() whenever you see something worth keeping: a room, a doorway, a
  large object, a blocked path. Use recall() before exploring somewhere you may
  have been before.
- Call finish() when the goal is met or you cannot safely continue.

Be brief in your reasoning. Prefer acting to explaining."""

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "look",
            "description": "Take a picture with the robot's camera and look at it.",
            "parameters": {
                "type": "object",
                "properties": {
                    "reason": {"type": "string", "description": "Why you need to look now."},
                },
                "required": ["reason"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "move",
            "description": "One move, then stop. Use for small corrections.",
            "parameters": {
                "type": "object",
                "properties": {
                    "direction": {
                        "type": "string",
                        "enum": ["f", "b", "l", "r", "fl", "fr", "bl", "br"],
                    },
                    "ms": {"type": "integer", "minimum": 100, "maximum": 2000},
                },
                "required": ["direction", "ms"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "follow_path",
            "description": (
                "Several moves in a row, then one picture. Preferred way to travel: "
                "it costs one picture instead of one per move."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "steps": {
                        "type": "array",
                        "maxItems": 6,
                        "items": {
                            "type": "object",
                            "properties": {
                                "direction": {
                                    "type": "string",
                                    "enum": ["f", "b", "l", "r", "fl", "fr", "bl", "br"],
                                },
                                "ms": {"type": "integer", "minimum": 100, "maximum": 2000},
                            },
                            "required": ["direction", "ms"],
                        },
                    },
                    "purpose": {"type": "string", "description": "What this path is for."},
                },
                "required": ["steps"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "remember",
            "description": "Keep a note about this place, tied to where the robot is now.",
            "parameters": {
                "type": "object",
                "properties": {
                    "label": {"type": "string", "description": "Short name, e.g. 'kitchen door'."},
                    "description": {"type": "string"},
                    "tags": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["label", "description"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "recall",
            "description": "Search earlier notes.",
            "parameters": {
                "type": "object",
                "properties": {"query": {"type": "string"}},
                "required": ["query"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "finish",
            "description": "End the mission.",
            "parameters": {
                "type": "object",
                "properties": {"summary": {"type": "string"}},
                "required": ["summary"],
            },
        },
    },
]


@dataclass
class Mission:
    goal: str
    max_steps: int = 40
    running: bool = False
    stop_requested: bool = False
    steps: int = 0
    pictures: int = 0
    started: float = field(default_factory=time.time)
    finished_summary: str | None = None
    error: str | None = None


class Agent:
    def __init__(
        self,
        robot: Robot,
        memory: Memory,
        client: AsyncOpenAI,
        model: str,
        on_event: Callable[[str, str], None],
        picture_size: str = "qvga",
        keep_images: int = 3,
    ):
        self.robot = robot
        self.memory = memory
        self.client = client
        self.model = model
        self.on_event = on_event
        self.picture_size = picture_size
        self.keep_images = keep_images
        self.mission: Mission | None = None
        self.last_jpeg: bytes | None = None
        self.messages: list[dict[str, Any]] = []

    # ---------- helpers ----------

    def log(self, kind: str, text: str) -> None:
        self.on_event(kind, text)

    async def _take_picture(self) -> str:
        jpeg = await self.robot.picture(self.picture_size)
        self.last_jpeg = jpeg
        name = self.memory.save_snapshot(jpeg, f"{self.robot.pose.x:.0f}_{self.robot.pose.y:.0f}")
        if self.mission:
            self.mission.pictures += 1
        self.log("picture", f"{name} ({len(jpeg) / 1024:.1f} KB) at {self.robot.pose.as_text()}")
        return base64.b64encode(jpeg).decode()

    def _prune_images(self) -> None:
        """Keep only the newest images; older ones become a line of text."""
        seen = 0
        for msg in reversed(self.messages):
            if msg.get("role") != "user" or not isinstance(msg.get("content"), list):
                continue
            if not any(p.get("type") == "image_url" for p in msg["content"]):
                continue
            seen += 1
            if seen > self.keep_images:
                text = next((p["text"] for p in msg["content"] if p.get("type") == "text"), "")
                msg["content"] = f"{text} (picture dropped to save tokens)"

    # ---------- tools ----------

    async def _run_tool(self, name: str, args: dict) -> tuple[str, str | None]:
        """Returns (text for the model, base64 picture or None)."""
        pose = self.robot.pose
        if name == "look":
            b64 = await self._take_picture()
            return "Picture taken, see the next message.", b64

        if name == "move":
            await self.robot.move(args["direction"], args["ms"])
            self.log("move", f"{args['direction']} {args['ms']}ms -> {pose.as_text()}")
            return f"Moved. Estimated position: {pose.as_text()}", None

        if name == "follow_path":
            steps = args.get("steps", [])
            done = []
            for step in steps:
                if self.mission and self.mission.stop_requested:
                    break
                await self.robot.move(step["direction"], step["ms"])
                done.append(f"{step['direction']} {step['ms']}ms")
            self.log("path", f"{' + '.join(done) or 'nothing'} -> {pose.as_text()}")
            b64 = await self._take_picture()
            return (
                f"Path done ({len(done)} of {len(steps)} steps: {', '.join(done)}). "
                f"Estimated position: {pose.as_text()}. Picture in the next message."
            ), b64

        if name == "remember":
            note = self.memory.add(
                Note(
                    label=args["label"],
                    description=args["description"],
                    tags=args.get("tags", []),
                    x=pose.x,
                    y=pose.y,
                    heading=pose.heading,
                    image=self.memory.save_snapshot(self.last_jpeg) if self.last_jpeg else None,
                )
            )
            self.log("memory", note.as_text())
            return f"Remembered: {note.as_text()}", None

        if name == "recall":
            hits = self.memory.search(args["query"])
            self.log("recall", f"{args['query']} -> {len(hits)} note(s)")
            if not hits:
                return "Nothing remembered about that.", None
            return "\n".join(n.as_text() for n in hits), None

        if name == "finish":
            if self.mission:
                self.mission.finished_summary = args["summary"]
            self.log("finish", args["summary"])
            return "Mission ended.", None

        return f"Unknown tool {name}.", None

    # ---------- the loop ----------

    async def run(self, goal: str, max_steps: int) -> Mission:
        mission = Mission(goal=goal, max_steps=max_steps, running=True)
        self.mission = mission
        cal = self.robot.cal
        self.messages = [
            {
                "role": "system",
                "content": SYSTEM_PROMPT.format(
                    cm_per_sec=cal.cm_per_sec * cal.speed / 255,
                    deg_per_sec=cal.deg_per_sec * cal.speed / 255,
                ),
            },
            {
                "role": "user",
                "content": (
                    f"Goal: {goal}\n\n"
                    f"Starting position: {self.robot.pose.as_text()}\n"
                    f"What you remember so far:\n{self.memory.summary()}\n\n"
                    "Start by looking around."
                ),
            },
        ]
        self.log("start", goal)
        try:
            while mission.running and mission.steps < mission.max_steps:
                if mission.stop_requested:
                    self.log("stop", "stopped by the user")
                    break
                mission.steps += 1
                self._prune_images()
                response = await self.client.chat.completions.create(
                    model=self.model,
                    messages=self.messages,
                    tools=TOOLS,
                    parallel_tool_calls=False,
                )
                choice = response.choices[0].message
                self.messages.append(choice.model_dump(exclude_none=True))
                if choice.content:
                    self.log("think", choice.content.strip())
                if not choice.tool_calls:
                    self.messages.append(
                        {"role": "user", "content": "Use a tool, or call finish() to stop."}
                    )
                    continue

                for call in choice.tool_calls:
                    args = json.loads(call.function.arguments or "{}")
                    self.log("tool", f"{call.function.name}({json.dumps(args)[:200]})")
                    try:
                        text, b64 = await self._run_tool(call.function.name, args)
                    except RobotError as exc:
                        text, b64 = f"Robot problem: {exc}", None
                        self.log("error", str(exc))
                    self.messages.append(
                        {"role": "tool", "tool_call_id": call.id, "content": text}
                    )
                    if b64:
                        self.messages.append(
                            {
                                "role": "user",
                                "content": [
                                    {
                                        "type": "text",
                                        "text": f"Camera view at {self.robot.pose.as_text()}",
                                    },
                                    {
                                        "type": "image_url",
                                        "image_url": {"url": f"data:image/jpeg;base64,{b64}"},
                                    },
                                ],
                            }
                        )
                    if call.function.name == "finish":
                        mission.running = False
        except asyncio.CancelledError:
            self.log("stop", "mission cancelled")
            raise
        except Exception as exc:  # noqa: BLE001 - any failure ends the mission safely
            mission.error = str(exc)
            self.log("error", str(exc))
        finally:
            mission.running = False
            try:
                await self.robot.stop()
            except RobotError:
                pass
            self.memory.save()
            self.log("end", mission.finished_summary or mission.error or "mission over")
        return mission
