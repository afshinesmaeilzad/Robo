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
from vision_index import (FAMILIAR, SAME_VIEW, STUCK_VIEW, Shot, VisionIndex,
                          fingerprint, similarity)

# Below this much travel, an early finish() is questioned once: a model that
# reads "stop when you get there" literally tends to stop at the first glimpse.
MIN_TRAVEL_CM = 150
# A target mission that stops after a few pictures from one spot has not searched:
# it has usually just believed an old note.
MIN_TARGET_LOOKS = 6
MIN_SEARCH_CM = 100

BASE_PROMPT = """You are driving a small two-wheel robot with a camera around an indoor space.

Rule one: do not crash. The robot has no bumper or distance sensor: the camera is all you have.

How the robot moves:
- "f"/"b" drive forward/back, "l"/"r" spin in place, "fl"/"fr"/"bl"/"br" curve.
- Moves are measured in milliseconds. A 1000 ms forward move is roughly {cm_per_sec:.0f} cm;
  a 1000 ms spin is roughly {deg_per_sec:.0f} degrees. These are estimates, not exact.
- The camera looks forward and low. Something close and large in the picture is an
  obstacle. The floor in the lower half of the picture is the path ahead.
- Before driving forward, be sure the floor ahead is clear in the last picture.
  If you are unsure, spin a little and look again instead of driving blind.
- The server tells you the estimated position after every move. It drifts, so
  trust the picture over the numbers.
- If the robot is blocked, the server notices (the view does not change after a
  move), backs it out and turns it. You are told when that happens: pick a
  different direction, do not push the same way again.
- "Robot problem" messages are usually a brief WiFi hiccup, not a broken robot.
  Try the same thing again, or take a picture; only give up if several attempts
  in a row fail.

Be brief in your reasoning. Prefer acting to explaining."""

EXPLORE_PROMPT = """
This mission is EXPLORING AND MAPPING.

- Cover ground. When the floor ahead is clearly open, send several long steps at
  once: follow_path with 3-6 steps of 1000-2000 ms. Crossing a room or a corridor
  takes tens of seconds of driving, not one short nudge. Short 300-500 ms steps
  are only for tight spots and fine aiming.
- To go through a doorway or down a corridor: turn to face it, check the picture,
  then drive through it in one long path rather than stopping every few
  centimetres.
- Pictures cost money and WiFi, so plan several moves from each one rather than
  looking after every step.
- To save more, the server compares each new picture with the ones it has. If the
  view has not changed it tells you in words instead of sending the picture
  again, and if a view matches somewhere you have been it shows you the notes
  made there. Those hints mean you are going in circles.
- Call remember() for anything worth keeping: a room, a doorway, a large object,
  a blocked path. Use recall() before exploring somewhere you may have been.
- Call finish() only when the goal is really met, or you are truly blocked, or
  you have run out of steps. Reaching the entrance of somewhere is not the same
  as having been there: go in, look around and note what is inside first.
  "Stop for a new mission" means stop when the task is done, not at the first
  glimpse of the target."""

TARGET_PROMPT = """
This mission is FINDING AND APPROACHING A TARGET.

- Every picture is sent to you in full, because the target can move and small
  changes matter. Look often: after most moves, and always before closing in.
- Searching: spin in place in steps of about 400-600 ms, looking after each, to
  sweep the whole room before driving off. Then move to a new spot and sweep again.
- Search the WHOLE picture each time, not just the middle: the target is often
  small, near the floor, at the edge of the frame, or half behind furniture. Say
  what you can see of it before deciding it is not there.
- If something might be the target but is too small or too dark to be sure, drive
  a little closer and look again before ruling it out.
- Once you can see the target: centre it in the picture by turning, then approach
  in short steps (300-600 ms), looking each time. It grows in the frame as you
  get nearer.
- Close in slowly. Stop while it is still a little ahead: the camera cannot see
  the ground right in front of the wheels.
- Do not chase a person or a pet that is moving away, and do not drive at
  anything breakable or anything that could be hurt. If the target moves, wait
  and look again rather than charging after it.
- Notes from earlier runs are HINTS ABOUT WHERE TO LOOK, never proof. Things get
  moved, including by the person who set you this task. A note saying the target
  was found before does not mean it is there now.
- Only say you have found the target if you can SEE IT IN THE PICTURE YOU JUST
  TOOK. Describe where it is in that frame. If you cannot see it, you have not
  found it, whatever your notes say.
- Search properly: sweep from where you are, and if the target is not there,
  DRIVE to another part of the room (1-2 m, several long steps) and sweep again.
  Turning on the spot only ever shows you one place. Cover several places before
  concluding it is not there.
- Call remember() when you find the target, with where it was.
- Call finish(seen_now=true) when the target is in the picture you just took and
  you have stopped near it. Use finish(seen_now=false) only after searching
  several places without finding it."""

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
                "properties": {
                    "summary": {"type": "string"},
                    "seen_now": {
                        "type": "boolean",
                        "description": (
                            "Target missions: true only if the target is visible in the "
                            "picture you have just taken. An earlier note is not seeing it."
                        ),
                    },
                },
                "required": ["summary"],
            },
        },
    },
]


def mode_for(goal: str) -> str:
    """Guess the kind of mission from how it is worded."""
    words = goal.lower()
    hunting = ("find", "look for", "search for", "locate", "follow", "chase",
               "track", "attack", "fetch", "go to the", "approach", "hunt")
    return "target" if any(w in words for w in hunting) else "explore"


@dataclass
class Mission:
    goal: str
    mode: str = "explore"   # explore = map it and save pictures; target = see everything
    max_steps: int = 40
    running: bool = False
    stop_requested: bool = False
    steps: int = 0
    pictures: int = 0      # taken by the camera
    images_sent: int = 0   # actually sent to the model
    images_skipped: int = 0  # unchanged views, sent as text instead
    stuck_events: int = 0    # moves that changed nothing, so an escape was driven
    tokens_in: int = 0
    tokens_out: int = 0
    tokens_cached: int = 0  # part of tokens_in that was served from cache
    started: float = field(default_factory=time.time)
    finished_summary: str | None = None
    error: str | None = None
    ended: str = ""  # why it stopped, in plain words


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
        index: VisionIndex | None = None,
        image_detail: str = "low",
        reasoning_effort: str | None = None,
        target_picture_size: str = "vga",
        target_image_detail: str = "high",
    ):
        self.robot = robot
        self.memory = memory
        self.client = client
        self.model = model
        self.on_event = on_event
        self.picture_size = picture_size
        self.keep_images = keep_images
        # "low" sends the picture at a fixed small token cost: enough to see a
        # floor, a doorway or a chair leg, and far cheaper than "high".
        self.image_detail = image_detail
        # Looking FOR something needs a good look at it. At low detail the model
        # gets a heavily downscaled picture: enough to see whether the floor is
        # clear, nowhere near enough to recognise a small object across a room.
        self.target_picture_size = target_picture_size
        self.target_image_detail = target_image_detail
        self.reasoning_effort = reasoning_effort  # e.g. "none" on models that think
        self.index = index
        self.mission: Mission | None = None
        self.last_jpeg: bytes | None = None
        self.last_shot: Shot | None = None
        self.last_change: float = 0.0   # how alike the last two pictures were
        self.escape_turn = "r"          # alternates, so escapes don't repeat
        self.finish_questioned = False  # an early finish is questioned once
        self.start_distance = 0.0       # how far the robot had driven when this mission began
        self.messages: list[dict[str, Any]] = []

    # ---------- helpers ----------

    def detail_now(self) -> str:
        if self.mission and self.mission.mode == "target":
            return self.target_image_detail
        return self.image_detail

    def log(self, kind: str, text: str) -> None:
        self.on_event(kind, text)

    async def _take_picture(self) -> tuple[str, str | None]:
        """Take a picture. Returns (what to tell the model, image or None).

        The image is only sent when the view has actually changed: comparing it
        with what the model has already seen is local work, sending it again is
        not. Familiar places come with the notes made there.
        """
        hunting = bool(self.mission and self.mission.mode == "target")
        jpeg = await self.robot.picture(
            self.target_picture_size if hunting else self.picture_size
        )
        self.last_jpeg = jpeg
        pose = self.robot.pose
        name = self.memory.save_snapshot(jpeg, f"{pose.x:.0f}_{pose.y:.0f}")
        if self.mission:
            self.mission.pictures += 1

        # A target mission needs every picture: the target moves, and the
        # difference between "chair" and "chair with the ball behind it" is
        # exactly what a similarity score throws away.
        if self.index is not None and self.mission and self.mission.mode == "target":
            self.index.add(jpeg, name, pose.x, pose.y, pose.heading, sent=True)
            self.mission.images_sent += 1
            self.log("picture", f"{name} ({len(jpeg) / 1024:.1f} KB) at {pose.as_text()}")
            return "Picture taken, see the next message.", base64.b64encode(jpeg).decode()

        if self.index is None:
            self.log("picture", f"{name} ({len(jpeg) / 1024:.1f} KB) at {pose.as_text()}")
            return "Picture taken, see the next message.", base64.b64encode(jpeg).decode()

        # The new picture is not in the index yet, so nothing needs excluding
        matches = self.index.matches(jpeg)
        best = matches[0] if matches else None
        # How much did the view change since the previous picture? That is what
        # tells a blocked robot from a moving one.
        if self.last_shot is not None:
            phash, hist = fingerprint(jpeg)
            self.last_change = similarity(phash, hist, self.last_shot.phash, self.last_shot.hist)
        else:
            self.last_change = 0.0
        unchanged = (
            best is not None
            and best[0] >= SAME_VIEW
            and best[1].sent
        )
        shot = self.index.add(jpeg, name, pose.x, pose.y, pose.heading, sent=not unchanged)
        self.last_shot = shot

        if unchanged:
            score, old = best
            if self.mission:
                self.mission.images_skipped += 1
            self.log("same", f"{name} is {score * 100:.0f}% the view from {old.where()}: not sent")
            note = f" Note made there: {old.label}: {old.description}." if old.label else ""
            return (
                f"The view has not changed ({score * 100:.0f}% the same as the picture from "
                f"{old.where()}), so it is not sent again.{note} "
                "If you expected it to change, the robot may not have moved: try a longer "
                "move, or turn."
            ), None

        if self.mission:
            self.mission.images_sent += 1
        self.log("picture", f"{name} ({len(jpeg) / 1024:.1f} KB) at {pose.as_text()}")
        text = "Picture taken, see the next message."
        familiar = [m for m in matches if m[0] >= FAMILIAR]
        if familiar:
            self.log("familiar", f"{len(familiar)} similar view(s) seen before")
            text += "\nThis place looks familiar:\n" + self.index.context(familiar)
        return text, base64.b64encode(jpeg).decode()

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

    async def _escape(self) -> str:
        """Back out of whatever the robot is caught on and turn away from it."""
        self.escape_turn = "l" if self.escape_turn == "r" else "r"
        if self.mission:
            self.mission.stuck_events += 1
        self.log("stuck", f"view unchanged after moving; backing up and turning {self.escape_turn}")
        try:
            await self.robot.move("b", 600)
            await self.robot.move(self.escape_turn, 500)
        except RobotError as exc:
            return f"Tried to back out but the robot did not answer: {exc}"
        return (
            "The view did not change after that move, so the robot is stuck: the wheels are "
            "blocked, or it is pressed against something the camera cannot see well. "
            f"I have backed it up and turned it {'left' if self.escape_turn == 'l' else 'right'}. "
            "Do not push the same way again: pick another direction."
        )

    async def _look_after_move(self, moved_ms: int) -> tuple[str, str | None]:
        """Picture after a move, with a stuck check when the move was long enough."""
        text, b64 = await self._take_picture()
        real_move = moved_ms >= 300  # shorter nudges may genuinely change nothing
        if real_move and self.last_change >= STUCK_VIEW:
            escape = await self._escape()
            text2, b64_2 = await self._take_picture()
            return f"{escape}\n{text2}", b64_2 or b64
        return text, b64

    # ---------- tools ----------

    async def _run_tool(self, name: str, args: dict) -> tuple[str, str | None]:
        """Returns (text for the model, base64 picture or None)."""
        pose = self.robot.pose
        if name == "look":
            return await self._take_picture()

        if name == "move":
            await self.robot.move(args["direction"], args["ms"])
            self.log("move", f"{args['direction']} {args['ms']}ms -> {pose.as_text()}")
            if self.last_jpeg is None:  # nothing to compare with yet
                return f"Moved. Estimated position: {pose.as_text()}", None
            text, b64 = await self._look_after_move(args["ms"])
            return f"Moved. Estimated position: {pose.as_text()}. {text}", b64

        if name == "follow_path":
            steps = args.get("steps", [])
            done = []
            for step in steps:
                if self.mission and self.mission.stop_requested:
                    break
                await self.robot.move(step["direction"], step["ms"])
                done.append(f"{step['direction']} {step['ms']}ms")
            self.log("path", f"{' + '.join(done) or 'nothing'} -> {pose.as_text()}")
            text, b64 = await self._look_after_move(sum(s["ms"] for s in steps[: len(done)]))
            return (
                f"Path done ({len(done)} of {len(steps)} steps: {', '.join(done)}). "
                f"Estimated position: {pose.as_text()}. {text}"
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
            if self.index is not None:
                self.index.describe(args["label"], args["description"], self.last_shot)
            self.log("memory", note.as_text())
            return f"Remembered: {note.as_text()}", None

        if name == "recall":
            hits = self.memory.search(args["query"])
            self.log("recall", f"{args['query']} -> {len(hits)} note(s)")
            if not hits:
                return "Nothing remembered about that.", None
            return "\n".join(n.as_text() for n in hits), None

        if name == "finish":
            m = self.mission
            # Finishing after a few centimetres usually means the goal was read
            # too literally. Push back once; obey if it insists.
            # Seeing the target now is a good reason to stop. Believing an old
            # note after two looks from one spot is not.
            if (m and m.mode == "target" and not args.get("seen_now")
                    and not self.finish_questioned
                    and (m.pictures < MIN_TARGET_LOOKS
                         or self.robot.pose.distance - self.start_distance < MIN_SEARCH_CM)):
                self.finish_questioned = True
                moved = self.robot.pose.distance - self.start_distance
                self.log("keep-going",
                         f"finish after {m.pictures} pictures and {moved:.0f}cm of searching")
                return (
                    f"You have taken {m.pictures} pictures and driven {moved:.0f} cm. That is "
                    "one spot, not a search, and an earlier note is not proof: the target may "
                    "have been moved since. Unless you can see the target in the picture you "
                    "just took, drive to another part of the room and sweep again from there. "
                    "Call finish() again if you truly cannot find it."
                ), None
            if (m and m.mode == "explore" and m.steps < m.max_steps // 3
                    and self.robot.pose.distance < MIN_TRAVEL_CM
                    and not self.finish_questioned):
                self.finish_questioned = True
                self.log("keep-going", f"finish after only {self.robot.pose.distance:.0f}cm")
                return (
                    f"You have driven only {self.robot.pose.distance:.0f} cm and used "
                    f"{m.steps} of {m.max_steps} steps. That is not much of the space yet. "
                    "Unless you are blocked, keep going: drive further, look around the "
                    "next corner, and note what you find. Call finish() again if you are "
                    "certain the goal is met."
                ), None
            if m:
                m.finished_summary = args["summary"]
            self.log("finish", args["summary"])
            return "Mission ended.", None

        return f"Unknown tool {name}.", None

    async def _ask(self):
        """One model call. Drops reasoning_effort if this model does not take it."""
        kwargs = dict(
            model=self.model,
            messages=self.messages,
            tools=TOOLS,
            parallel_tool_calls=False,
        )
        if self.reasoning_effort:
            kwargs["reasoning_effort"] = self.reasoning_effort
        try:
            return await self.client.chat.completions.create(**kwargs)
        except Exception as exc:  # noqa: BLE001
            if "reasoning_effort" not in kwargs or "reasoning" not in str(exc).lower():
                raise
            self.log("model", f"this model rejected reasoning_effort, dropping it: {exc}")
            self.reasoning_effort = None
            kwargs.pop("reasoning_effort")
            return await self.client.chat.completions.create(**kwargs)

    # ---------- the loop ----------

    async def run(self, goal: str, max_steps: int, mode: str = "auto") -> Mission:
        if mode not in ("explore", "target"):
            mode = mode_for(goal)
        mission = Mission(goal=goal, mode=mode, max_steps=max_steps, running=True)
        self.mission = mission
        self.finish_questioned = False
        self.start_distance = self.robot.pose.distance
        cal = self.robot.cal
        self.messages = [
            {
                "role": "system",
                "content": BASE_PROMPT.format(
                    cm_per_sec=cal.cm_per_sec * cal.speed / 255,
                    deg_per_sec=cal.deg_per_sec * cal.speed / 255,
                ) + (EXPLORE_PROMPT if mode == "explore" else TARGET_PROMPT),
            },
            {
                "role": "user",
                "content": (
                    f"Goal: {goal}\n\n"
                    f"Starting position: {self.robot.pose.as_text()}\n"
                    f"Notes from earlier runs (they may be out of date; the room and the "
                    f"things in it can have been moved since):\n"
                    f"{self.memory.summary(with_age=True)}\n\n"
                    + ("Those notes are hints about where to look. Trust only what you "
                       "see in the pictures you take now.\n\n" if mode == "target" else "")
                    + "Start by looking around."
                ),
            },
        ]
        self.log("start", f"[{mode}] {goal}")
        try:
            while mission.running and mission.steps < mission.max_steps:
                if mission.stop_requested:
                    self.log("stop", "stopped by the user")
                    break
                mission.steps += 1
                left = mission.max_steps - mission.steps
                if left in (5, 2):
                    self.messages.append({
                        "role": "user",
                        "content": (
                            f"{left} steps left before this mission is stopped. Finish what "
                            "you are doing: if you can see the target or have something worth "
                            "keeping, remember() it now and call finish()."
                        ),
                    })
                self._prune_images()
                response = await self._ask()
                usage = getattr(response, "usage", None)
                if usage:
                    mission.tokens_in += usage.prompt_tokens or 0
                    mission.tokens_out += usage.completion_tokens or 0
                    details = getattr(usage, "prompt_tokens_details", None)
                    mission.tokens_cached += getattr(details, "cached_tokens", 0) or 0
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
                                        "image_url": {
                                            "url": f"data:image/jpeg;base64,{b64}",
                                            "detail": self.detail_now(),
                                        },
                                    },
                                ],
                            }
                        )
                    # Only a finish that was accepted ends the mission: an
                    # early one is answered with "keep going" instead.
                    if call.function.name == "finish" and mission.finished_summary:
                        mission.running = False
        except asyncio.CancelledError:
            self.log("stop", "mission cancelled")
            raise
        except Exception as exc:  # noqa: BLE001 - any failure ends the mission safely
            mission.error = str(exc)
            self.log("error", str(exc))
        finally:
            if mission.stop_requested:
                mission.ended = "stopped by you"
            elif mission.error:
                mission.ended = f"error: {mission.error}"
            elif mission.finished_summary:
                mission.ended = "the model called finish()"
            elif mission.steps >= mission.max_steps:
                mission.ended = (
                    f"ran out of steps ({mission.max_steps}). Raise the step limit and "
                    "start again to carry on."
                )
            else:
                mission.ended = "stopped"
            mission.running = False
            try:
                await self.robot.stop()
            except RobotError:
                pass
            self.memory.save()
            self.log("end", f"{mission.ended}"
                            + (f" - {mission.finished_summary}" if mission.finished_summary else ""))
        return mission
