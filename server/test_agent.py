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
from memory import Memory
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
    # The fake robot returns the same picture every time, so the second view is
    # recognised locally and never sent to the model
    checks.append(("only the first image sent", mission.images_sent == 1, str(mission.images_sent)))
    checks.append(("second view skipped", mission.images_skipped == 1, str(mission.images_skipped)))
    checks.append(("index holds both views", len(index.shots) == 2, str(len(index.shots))))
    checks.append(("skip was logged", any(k == "same" for k, t in events),
                   str([k for k, _ in events])))

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
    checks.append(("no image was pruned (only one was ever sent)", len(dropped) == 0, str(len(dropped))))

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

    failed = 0
    for name, ok, detail in checks:
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"  (got {detail})"))
        failed += not ok
    print(f"\n{len(checks) - failed}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
