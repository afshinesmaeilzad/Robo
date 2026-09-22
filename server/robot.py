"""Talking to the robot.

Drive commands go over the robot's WebSocket (/ws), which both robo_wifi and
robo_lite speak. Pictures come from GET /jpg.

The robot stops by itself after 500 ms without a command, so a move is sent
repeatedly for as long as it should last and then followed by a stop.
"""

from __future__ import annotations

import asyncio
import math
import time
from dataclasses import dataclass, field

import httpx
import websockets
from websockets.protocol import State

DIRECTIONS = {"f", "b", "l", "r", "fl", "fr", "bl", "br", "s"}

# How the robot moves per second at full speed (255). Measure yours and put the
# numbers in .env: drive forward 2 s, measure the distance; spin 2 s, count turns.
@dataclass
class Calibration:
    cm_per_sec: float = 20.0
    deg_per_sec: float = 180.0
    speed: int = 200  # PWM used for exploring, 0-255


@dataclass
class Pose:
    """Where the robot thinks it is, by dead reckoning. Start = (0, 0) facing 0°."""

    x: float = 0.0
    y: float = 0.0
    heading: float = 0.0  # degrees, 0 = the direction it faced at start
    distance: float = 0.0  # total travelled, cm

    def as_text(self) -> str:
        return f"x={self.x:.0f}cm y={self.y:.0f}cm heading={self.heading:.0f}deg"


@dataclass
class Move:
    direction: str
    ms: int
    t: float = field(default_factory=time.time)


class RobotError(RuntimeError):
    pass


class Robot:
    """One robot, one WebSocket. Commands are serialised: one move at a time."""

    def __init__(self, host: str, cal: Calibration, timeout: float = 5.0):
        self.host = host
        self.cal = cal
        self.timeout = timeout
        self.pose = Pose()
        self.trail: list[Pose] = [Pose()]
        self.moves: list[Move] = []
        self._ws: websockets.ClientConnection | None = None
        self._lock = asyncio.Lock()
        self._http = httpx.AsyncClient(timeout=timeout)

    # ---------- connection ----------

    def connected(self) -> bool:
        return self._ws is not None and self._ws.state is State.OPEN

    async def connect(self) -> None:
        if self.connected():
            return
        if self._ws is not None:  # a dead link: drop it and make a new one
            self._ws = None
        url = f"ws://{self.host}/ws"
        try:
            self._ws = await asyncio.wait_for(websockets.connect(url, max_size=None), self.timeout)
        except Exception as exc:  # noqa: BLE001 - surfaced to the caller as one error
            raise RobotError(f"cannot reach the robot at {url}: {exc}") from exc

    async def close(self) -> None:
        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:  # noqa: BLE001 - already gone is fine
                pass
            self._ws = None
        await self._http.aclose()

    async def _send(self, text: str) -> None:
        await self.connect()
        assert self._ws is not None
        try:
            await self._ws.send(text)
        except Exception as exc:  # noqa: BLE001
            self._ws = None
            raise RobotError(f"lost the drive link: {exc}") from exc

    # ---------- driving ----------

    async def stop(self) -> None:
        await self._send("s")

    async def move(self, direction: str, ms: int) -> Pose:
        """Drive in one direction for ms milliseconds, then stop."""
        if direction not in DIRECTIONS:
            raise RobotError(f"unknown direction {direction!r}")
        ms = max(0, min(int(ms), 2000))  # never run away on one command
        async with self._lock:
            await self.connect()
            end = time.monotonic() + ms / 1000
            while time.monotonic() < end:
                await self._send(direction)
                await asyncio.sleep(min(0.2, max(0.0, end - time.monotonic())))
            await self.stop()
            self._integrate(direction, ms)
            self.moves.append(Move(direction, ms))
            self.trail.append(Pose(**vars(self.pose)))
            return self.pose

    def _integrate(self, direction: str, ms: int) -> None:
        """Dead reckoning: estimate the new pose from the command that was sent."""
        secs = ms / 1000
        scale = self.cal.speed / 255
        dist = self.cal.cm_per_sec * scale * secs
        turn = self.cal.deg_per_sec * scale * secs
        if direction == "s":
            return
        if direction in ("l", "r"):  # spin in place
            self.pose.heading += turn if direction == "l" else -turn
        else:
            back = direction.startswith("b")
            if direction in ("fl", "bl"):
                self.pose.heading += turn / 3  # one wheel slowed: a wide curve
            elif direction in ("fr", "br"):
                self.pose.heading -= turn / 3
            step = -dist if back else dist
            rad = math.radians(self.pose.heading)
            self.pose.x += step * math.cos(rad)
            self.pose.y += step * math.sin(rad)
            self.pose.distance += abs(step)
        self.pose.heading = (self.pose.heading + 180) % 360 - 180

    # ---------- camera ----------

    async def picture(self, size: str | None = None) -> bytes:
        """One JPEG. `size` is qqvga, qvga or vga.

        A picture can take a moment (warm-up frames, and the flash settling in
        the dark), and WiFi drops the odd packet, so one retry is worth it.
        """
        async with self._lock:
            if size:
                await self._http.get(f"http://{self.host}/set", params={"size": size})
            last = "unknown"
            for attempt in range(2):
                try:
                    r = await self._http.get(f"http://{self.host}/jpg", timeout=10.0)
                    r.raise_for_status()
                    return r.content
                except Exception as exc:  # noqa: BLE001
                    # Timeouts stringify to "", which says nothing in a log
                    last = str(exc) or type(exc).__name__
                    if attempt == 0:
                        await asyncio.sleep(0.3)
            raise RobotError(f"no picture from the robot ({last})")

    async def set_speed(self, speed: int) -> None:
        self.cal.speed = max(80, min(int(speed), 255))
        await self._http.get(f"http://{self.host}/set", params={"speed": self.cal.speed})

    async def light(self, on: bool) -> None:
        await self._http.get(f"http://{self.host}/set", params={"led": 1 if on else 0})

    async def info(self) -> dict:
        try:
            r = await self._http.get(f"http://{self.host}/info")
            return r.json()
        except Exception:  # noqa: BLE001 - /info is optional (robo_lite has none)
            return {}


class FakeRobot(Robot):
    """Offline stand-in, so the server can run without hardware (ROBOT_HOST=fake).

    Its "camera" draws a view that depends on where the robot is, so moving
    really does change the picture, as on the real thing.
    """

    def __init__(self, cal: Calibration, frozen: bool = False):
        super().__init__("fake", cal)
        self.frozen = frozen  # frozen=True: the view never changes (a stuck robot)

    async def connect(self) -> None:
        return

    async def close(self) -> None:
        await self._http.aclose()

    async def _send(self, text: str) -> None:
        return

    async def picture(self, size: str | None = None) -> bytes:
        import io

        from PIL import Image, ImageDraw

        p = Pose() if self.frozen else self.pose
        img = Image.new("RGB", (160, 120), (40, 40, 48))
        d = ImageDraw.Draw(img)
        # A "floor" whose horizon moves with the heading, and a marker that
        # slides past as the robot drives: enough for the picture to change.
        horizon = 60 + int(p.heading) % 40
        d.rectangle([0, horizon, 160, 120], fill=(90, 80, 70))
        spot = int(p.x * 2) % 160
        d.ellipse([spot - 12, horizon - 24, spot + 12, horizon], fill=(200, 180, 60))
        d.text((4, 4), f"{p.x:.0f},{p.y:.0f} {p.heading:.0f}deg", fill=(230, 230, 230))
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=70)
        return buf.getvalue()

    async def info(self) -> dict:
        return {"up": 0, "boots": 0, "reset": "fake", "rssi": -30, "cam": 1}
