"""Offline check of the agent loop: no robot, no OpenAI account.

A stub model plays a short scripted mission, so the tool handling, the image
attachments, the dead reckoning and the memory file are all exercised.

    python test_agent.py
"""

from __future__ import annotations

import asyncio
import json
import tempfile
from pathlib import Path
from types import SimpleNamespace

from agent import Agent
from memory import Memory, Note
from robot import Calibration, FakeRobot
from vision_index import VisionIndex, similarity, fingerprint


def tool_call(id_: str, name: str, **args):
    return SimpleNamespace(
        id=id_,
        type="function",
        function=SimpleNamespace(name=name, arguments=json.dumps(args)),
    )


class StubModel:
    """Replays a script of assistant turns in place of the OpenAI client."""

    def __init__(self, script):
        self.script = list(script)
        self.seen: list[list[dict]] = []
        self.chat = SimpleNamespace(completions=SimpleNamespace(create=self._create))

    async def _create(self, **kwargs):
        self.seen.append(kwargs["messages"])
        content, calls = self.script.pop(0)
        message = SimpleNamespace(
            role="assistant",
            content=content,
            tool_calls=calls or None,
            model_dump=lambda exclude_none=True: {
                "role": "assistant",
                "content": content,
                "tool_calls": [
                    {
                        "id": c.id,
                        "type": "function",
                        "function": {"name": c.function.name, "arguments": c.function.arguments},
                    }
                    for c in (calls or [])
                ],
            },
        )
        return SimpleNamespace(choices=[SimpleNamespace(message=message)])


def Image_bytes() -> bytes:
    """A picture that looks nothing like the fake robot's grey frame."""
    import io

    from PIL import Image

    img = Image.new("RGB", (64, 64))
    img.putdata([((x * 4) % 256, 255 - (y * 4) % 256, (x + y) % 256)
                 for y in range(64) for x in range(64)])
    buf = io.BytesIO()
    img.save(buf, "JPEG")
    return buf.getvalue()


async def main() -> int:
    tmp = Path(tempfile.mkdtemp())
    robot = FakeRobot(Calibration(cm_per_sec=20, deg_per_sec=180, speed=255))
    memory = Memory(tmp)
    events: list[tuple[str, str]] = []

    script = [
        ("Looking first.", [tool_call("1", "look", reason="start")]),
        (
            "Path is clear, driving.",
            [tool_call("2", "follow_path", steps=[{"direction": "f", "ms": 1000},
                                                  {"direction": "l", "ms": 500}], purpose="cross")],
        ),
        ("A table.", [tool_call("3", "remember", label="table", description="wooden table",
                                tags=["furniture"])]),
        ("Checking memory.", [tool_call("4", "recall", query="table")]),
        ("Done.", [tool_call("5", "finish", summary="explored a bit")]),
    ]
    model = StubModel(script)
    index = VisionIndex(tmp)
    agent = Agent(robot, memory, model, "stub", lambda k, t: events.append((k, t)),
                  keep_images=1, index=index)

    mission = await agent.run("test mission", max_steps=10)

    checks: list[tuple[str, bool, str]] = []
    checks.append(("mission ended", not mission.running, str(mission.running)))
    checks.append(("finish summary kept", mission.finished_summary == "explored a bit",
                   str(mission.finished_summary)))
    checks.append(("no error", mission.error is None, str(mission.error)))
    checks.append(("two pictures taken", mission.pictures == 2, str(mission.pictures)))
    checks.append(("moving robot is not 'stuck'", mission.stuck_events == 0,
                   str(mission.stuck_events)))
    checks.append(("both changed views sent", mission.images_sent == 2, str(mission.images_sent)))
    checks.append(("index holds both views", len(index.shots) == 2, str(len(index.shots))))

    # 1000 ms forward at 20 cm/s then a 500 ms spin at 180 deg/s
    checks.append(("moved forward ~20cm", 19 < robot.pose.x < 21, f"x={robot.pose.x:.1f}"))
    checks.append(("turned ~90 deg", 89 < robot.pose.heading < 91, f"{robot.pose.heading:.1f}"))
    checks.append(("trail recorded", len(robot.trail) == 3, str(len(robot.trail))))

    checks.append(("note saved", len(memory.notes) == 1, str(len(memory.notes))))
    checks.append(("note has a position", memory.notes and memory.notes[0].x > 19,
                   str(memory.notes[0].x if memory.notes else "-")))
    checks.append(("memory file written", (tmp / "memories.json").exists(), str(tmp)))
    checks.append(("snapshots written", len(list((tmp / "snapshots").glob("*.jpg"))) >= 2,
                   str(len(list((tmp / "snapshots").glob("*.jpg"))))))
    checks.append(("recall found the note",
                   any(k == "recall" and "1 note" in t for k, t in events),
                   str([t for k, t in events if k == "recall"])))

    # The last request must carry one image, and the older one must be pruned
    last = model.seen[-1]
    images = [m for m in last if isinstance(m.get("content"), list)
              and any(p.get("type") == "image_url" for p in m["content"])]
    dropped = [m for m in last if isinstance(m.get("content"), str)
               and "picture dropped" in m["content"]]
    checks.append(("one image kept in context", len(images) == 1, str(len(images))))
    checks.append(("older image pruned", len(dropped) == 1, str(len(dropped))))

    # Fingerprints: same picture matches itself, a different one does not
    same = robot_jpeg = await robot.picture()
    other = bytes(Image_bytes())
    fa, ha = fingerprint(robot_jpeg)
    fb, hb = fingerprint(same)
    fc, hc = fingerprint(other)
    checks.append(("identical pictures score ~1", similarity(fa, ha, fb, hb) > 0.99,
                   f"{similarity(fa, ha, fb, hb):.3f}"))
    checks.append(("different pictures score low", similarity(fa, ha, fc, hc) < 0.9,
                   f"{similarity(fa, ha, fc, hc):.3f}"))

    # --- an unchanged view (robot did not move between two looks) is not sent twice ---
    events.clear()
    tmp_same = Path(tempfile.mkdtemp())
    robot_same = FakeRobot(Calibration(), frozen=True)
    agent_same = Agent(robot_same, Memory(tmp_same),
                       StubModel([("Look.", [tool_call("1", "look", reason="a")]),
                                  ("Again.", [tool_call("2", "look", reason="b")]),
                                  ("Done.", [tool_call("3", "finish", summary="x")])]),
                       "stub", lambda k, t: events.append((k, t)), index=VisionIndex(tmp_same))
    m_same = await agent_same.run("same view", max_steps=5)
    checks.append(("unchanged view sent once", m_same.images_sent == 1, str(m_same.images_sent)))
    checks.append(("unchanged view skipped once", m_same.images_skipped == 1,
                   str(m_same.images_skipped)))
    checks.append(("skip was logged", any(k == "same" for k, t in events),
                   str([k for k, _ in events])))

    # --- stuck detection: a frozen view after a real move must trigger the escape ---
    events.clear()
    stuck_script = [
        ("Look.", [tool_call("1", "look", reason="start")]),
        ("Drive.", [tool_call("2", "move", direction="f", ms=1000)]),
        ("Give up.", [tool_call("3", "finish", summary="blocked")]),
    ]
    robot2 = FakeRobot(Calibration(cm_per_sec=20, deg_per_sec=180, speed=255), frozen=True)
    tmp2 = Path(tempfile.mkdtemp())
    agent2 = Agent(robot2, Memory(tmp2), StubModel(stuck_script), "stub",
                   lambda k, t: events.append((k, t)), index=VisionIndex(tmp2))
    m2 = await agent2.run("stuck test", max_steps=5)

    checks.append(("stuck was detected", m2.stuck_events == 1, str(m2.stuck_events)))
    checks.append(("stuck was logged", any(k == "stuck" for k, t in events),
                   str([k for k, _ in events])))
    # escape = back 600 ms + turn 500 ms, on top of the 1000 ms forward move
    escape = [mv for mv in robot2.moves if mv.direction in ("b", "l", "r")]
    checks.append(("backed out", any(mv.direction == "b" and mv.ms == 600 for mv in escape),
                   str([(mv.direction, mv.ms) for mv in escape])))
    checks.append(("turned away", any(mv.direction in ("l", "r") and mv.ms == 500 for mv in escape),
                   str([(mv.direction, mv.ms) for mv in escape])))
    told = any("stuck" in msg.get("content", "")
               for msgs in agent2.messages and [agent2.messages] for msg in msgs
               if msg.get("role") == "tool" and isinstance(msg.get("content"), str))
    checks.append(("model was told", told, "no 'stuck' in any tool reply"))

    # a short nudge must NOT count as stuck
    events.clear()
    nudge_script = [
        ("Look.", [tool_call("1", "look", reason="start")]),
        ("Nudge.", [tool_call("2", "move", direction="f", ms=150)]),
        ("Stop.", [tool_call("3", "finish", summary="done")]),
    ]
    tmp3 = Path(tempfile.mkdtemp())
    robot3 = FakeRobot(Calibration(cm_per_sec=20, deg_per_sec=180, speed=255), frozen=True)
    agent3 = Agent(robot3, Memory(tmp3), StubModel(nudge_script), "stub",
                   lambda k, t: events.append((k, t)), index=VisionIndex(tmp3))
    m3 = await agent3.run("nudge test", max_steps=5)
    checks.append(("short nudge is not 'stuck'", m3.stuck_events == 0, str(m3.stuck_events)))

    # --- an early finish is questioned once, then obeyed ---
    events.clear()
    tmp4 = Path(tempfile.mkdtemp())
    robot4 = FakeRobot(Calibration(cm_per_sec=20, deg_per_sec=180, speed=255))
    agent4 = Agent(robot4, Memory(tmp4),
                   StubModel([("Done already.", [tool_call("1", "finish", summary="early")]),
                              ("Fine, more.", [tool_call("2", "follow_path",
                                                         steps=[{"direction": "f", "ms": 2000}],
                                                         purpose="go on")]),
                              ("Now done.", [tool_call("3", "finish", summary="proper")])]),
                   "stub", lambda k, t: events.append((k, t)), index=VisionIndex(tmp4))
    m4 = await agent4.run("explore", max_steps=40)
    checks.append(("early finish questioned", any(k == "keep-going" for k, t in events),
                   str([k for k, _ in events])))
    checks.append(("mission carried on", m4.steps == 3, str(m4.steps)))
    checks.append(("second finish obeyed", m4.finished_summary == "proper",
                   str(m4.finished_summary)))

    # ... but a finish late in a mission is obeyed at once
    events.clear()
    tmp5 = Path(tempfile.mkdtemp())
    robot5 = FakeRobot(Calibration(cm_per_sec=20, deg_per_sec=180, speed=255))
    agent5 = Agent(robot5, Memory(tmp5),
                   StubModel([("Enough.", [tool_call("1", "finish", summary="late")])]),
                   "stub", lambda k, t: events.append((k, t)), index=VisionIndex(tmp5))
    m5 = await agent5.run("explore", max_steps=2)  # step 1 of 2 is not "early"
    checks.append(("late finish obeyed at once", m5.finished_summary == "late",
                   str(m5.finished_summary)))

    # --- mission modes: a target mission sends every picture, explore skips ---
    from agent import mode_for
    guesses = {
        "Explore the room, map it and avoid obstacles.": "explore",
        "find the red ball and go to it": "target",
        "follow the cat": "target",
        "map the corridor": "explore",
    }
    ok = all(mode_for(g) == want for g, want in guesses.items())
    checks.append(("mission mode guessed from the goal", ok,
                   str({g: mode_for(g) for g in guesses})))

    events.clear()
    tmp6 = Path(tempfile.mkdtemp())
    robot6 = FakeRobot(Calibration(), frozen=True)  # identical view every time
    agent6 = Agent(robot6, Memory(tmp6),
                   StubModel([("Look.", [tool_call("1", "look", reason="a")]),
                              ("Again.", [tool_call("2", "look", reason="b")]),
                              ("Done.", [tool_call("3", "finish", summary="found it",
                                                    seen_now=True)])]),
                   "stub", lambda k, t: events.append((k, t)), index=VisionIndex(tmp6))
    m6 = await agent6.run("find the red ball", max_steps=5)
    checks.append(("target mission picked", m6.mode == "target", m6.mode))
    checks.append(("target mission sends both identical views", m6.images_sent == 2,
                   str(m6.images_sent)))
    checks.append(("target mission skips nothing", m6.images_skipped == 0,
                   str(m6.images_skipped)))
    checks.append(("seeing the target now ends the mission at once",
                   m6.finished_summary == "found it", str(m6.finished_summary)))

    # --- a target mission cannot finish on an old note after one look ---
    events.clear()
    tmp7 = Path(tempfile.mkdtemp())
    mem7 = Memory(tmp7)
    mem7.add(Note(label="bear lamp", description="found it here last time",
                  x=0, y=0, heading=0))
    robot7 = FakeRobot(Calibration(cm_per_sec=20, deg_per_sec=180, speed=255))
    stub7 = StubModel([("Already found it.", [tool_call("1", "look", reason="check")]),
                              ("It is known.", [tool_call("2", "finish", summary="found earlier")]),
                              ("Fine, searching.",
                               [tool_call("3", "follow_path",
                                          steps=[{"direction": "f", "ms": 2000},
                                                 {"direction": "f", "ms": 2000},
                                                 {"direction": "f", "ms": 2000}],
                                          purpose="search elsewhere")]),
                              ("Looking.", [tool_call("4", "look", reason="sweep")]),
                              ("Looking.", [tool_call("5", "look", reason="sweep")]),
                              ("Looking.", [tool_call("6", "look", reason="sweep")]),
                              ("Looking.", [tool_call("7", "look", reason="sweep")]),
                              ("Not here.", [tool_call("8", "finish", summary="really not there")])])
    agent7 = Agent(robot7, mem7, stub7,
                   "stub", lambda k, t: events.append((k, t)), index=VisionIndex(tmp7))
    m7 = await agent7.run("find the bear lamp", max_steps=12)
    checks.append(("old note does not end a target mission",
                   any(k == "keep-going" for k, t in events), str([k for k, _ in events])))
    checks.append(("it had to search first", m7.finished_summary == "really not there",
                   str(m7.finished_summary)))
    checks.append(("searching moved the robot",
                   robot7.pose.distance - 0 > 100, f"{robot7.pose.distance:.0f}cm"))

    # notes are handed over with their age and a warning
    first = stub7.seen[0] if stub7.seen else []
    opening = next((m["content"] for m in first if m.get("role") == "user"), "")
    checks.append(("notes are dated for the model", "min ago" in opening or "just now" in opening,
                   opening[:120]))
    checks.append(("notes are flagged as possibly stale", "out of date" in opening,
                   opening[:120]))

    failed = 0
    for name, ok, detail in checks:
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"  (got {detail})"))
        failed += not ok
    print(f"\n{len(checks) - failed}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
