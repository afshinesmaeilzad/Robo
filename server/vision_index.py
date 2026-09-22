"""Local picture index: is this view new?

Every snapshot gets two cheap local fingerprints:

  * a difference hash (dHash) — 64 bits of coarse structure, robust to noise,
    exposure and small shifts;
  * an 8-bin-per-channel colour histogram — robust to small rotations.

Comparing those is microseconds of local work, and it answers two questions
before anything is sent to OpenAI:

  1. "Is this the same view I already showed the model?" If so the picture is
     not sent again: a line of text says the view is unchanged. That saves
     tokens, money and WiFi.
  2. "Have I been somewhere that looks like this before?" If so, the notes made
     there are handed to the model as text — a small retrieval step over the
     robot's own memory.

No model, no embeddings, no GPU: it has to run on the same laptop as the robot.
"""

from __future__ import annotations

import io
import json
import math
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from PIL import Image

HASH_SIZE = 8          # dHash grid: 8x8 comparisons = 64 bits
HIST_BINS = 8          # per channel
SAME_VIEW = 0.97       # at or above this, the view counts as unchanged
FAMILIAR = 0.80        # at or above this, the place looks familiar


@dataclass
class Shot:
    file: str
    phash: str          # 16 hex chars
    hist: list[float]
    x: float
    y: float
    heading: float
    t: float = field(default_factory=time.time)
    label: str | None = None
    description: str | None = None
    sent: bool = False  # was the image itself ever sent to the model?

    def where(self) -> str:
        return f"x={self.x:.0f}cm y={self.y:.0f}cm heading={self.heading:.0f}deg"


def _dhash(img: Image.Image) -> str:
    small = img.convert("L").resize((HASH_SIZE + 1, HASH_SIZE), Image.Resampling.BILINEAR)
    px = list(small.getdata())
    bits = 0
    for row in range(HASH_SIZE):
        base = row * (HASH_SIZE + 1)
        for col in range(HASH_SIZE):
            bits = (bits << 1) | int(px[base + col] < px[base + col + 1])
    return f"{bits:016x}"


def _histogram(img: Image.Image) -> list[float]:
    small = img.convert("RGB").resize((64, 64), Image.Resampling.BILINEAR)
    counts = [0.0] * (HIST_BINS * 3)
    for r, g, b in small.getdata():
        counts[r * HIST_BINS // 256] += 1
        counts[HIST_BINS + g * HIST_BINS // 256] += 1
        counts[2 * HIST_BINS + b * HIST_BINS // 256] += 1
    total = sum(counts) or 1.0
    return [c / total for c in counts]


def fingerprint(jpeg: bytes) -> tuple[str, list[float]]:
    img = Image.open(io.BytesIO(jpeg))
    return _dhash(img), _histogram(img)


def _hamming(a: str, b: str) -> int:
    return bin(int(a, 16) ^ int(b, 16)).count("1")


def _cosine(a: list[float], b: list[float]) -> float:
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    return dot / (na * nb) if na and nb else 0.0


def similarity(phash_a: str, hist_a: list[float], phash_b: str, hist_b: list[float]) -> float:
    """0 = nothing alike, 1 = the same picture. Structure counts more than colour."""
    structure = 1.0 - _hamming(phash_a, phash_b) / (HASH_SIZE * HASH_SIZE)
    colour = _cosine(hist_a, hist_b)
    return 0.65 * structure + 0.35 * colour


class VisionIndex:
    def __init__(self, data_dir: Path):
        self.file = data_dir / "vision_index.json"
        self.shots: list[Shot] = []
        if self.file.exists():
            try:
                self.shots = [Shot(**s) for s in json.loads(self.file.read_text())]
            except (ValueError, TypeError) as exc:
                print(f"vision index unreadable, starting empty: {exc}")

    def save(self) -> None:
        self.file.write_text(json.dumps([asdict(s) for s in self.shots]))

    def add(self, jpeg: bytes, file: str, x: float, y: float, heading: float, sent: bool) -> Shot:
        phash, hist = fingerprint(jpeg)
        shot = Shot(file=file, phash=phash, hist=hist, x=x, y=y, heading=heading, sent=sent)
        self.shots.append(shot)
        self.save()
        return shot

    def matches(self, jpeg: bytes, top: int = 3,
                min_score: float = FAMILIAR) -> list[tuple[float, Shot]]:
        """Earlier shots that look like this one, best first. Call before add()."""
        phash, hist = fingerprint(jpeg)
        scored = [(similarity(phash, hist, s.phash, s.hist), s) for s in self.shots]
        scored = [(sc, s) for sc, s in scored if sc >= min_score]
        scored.sort(key=lambda p: p[0], reverse=True)
        return scored[:top]

    def describe(self, label: str, description: str, shot: Shot | None) -> None:
        """Tie a note the model made to the picture it was looking at."""
        if shot is None:
            return
        shot.label, shot.description = label, description
        self.save()

    def context(self, matches: list[tuple[float, Shot]]) -> str:
        """Matching places, as a line of text for the model."""
        lines = []
        for score, shot in matches:
            what = f"{shot.label}: {shot.description}" if shot.label else "no note made there"
            lines.append(f"- {score * 100:.0f}% like a view from {shot.where()} ({what})")
        return "\n".join(lines)
