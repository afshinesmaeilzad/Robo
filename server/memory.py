"""What the robot remembers between runs.

Every note is one line in data/memories.json, with the estimated pose it was
made from and, if there was a picture, the file it was seen in. Snapshots live
in data/snapshots/.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path


@dataclass
class Note:
    label: str
    description: str
    x: float
    y: float
    heading: float
    tags: list[str] = field(default_factory=list)
    image: str | None = None
    t: float = field(default_factory=time.time)

    def age(self) -> str:
        secs = max(0, time.time() - self.t)
        if secs < 90:
            return "just now"
        if secs < 3600:
            return f"{secs / 60:.0f} min ago"
        if secs < 86400:
            return f"{secs / 3600:.0f} h ago"
        return f"{secs / 86400:.0f} days ago"

    def as_text(self, with_age: bool = False) -> str:
        where = f"at x={self.x:.0f}cm y={self.y:.0f}cm"
        tags = f" [{', '.join(self.tags)}]" if self.tags else ""
        when = f" ({self.age()})" if with_age else ""
        return f"{self.label}{tags} {where}{when}: {self.description}"


class Memory:
    def __init__(self, data_dir: Path):
        self.dir = data_dir
        self.snapshots = data_dir / "snapshots"
        self.snapshots.mkdir(parents=True, exist_ok=True)
        self.file = data_dir / "memories.json"
        self.notes: list[Note] = []
        self._load()

    def _load(self) -> None:
        if not self.file.exists():
            return
        try:
            raw = json.loads(self.file.read_text())
            self.notes = [Note(**n) for n in raw]
        except (ValueError, TypeError) as exc:
            print(f"memory file unreadable, starting empty: {exc}")

    def save(self) -> None:
        self.file.write_text(json.dumps([asdict(n) for n in self.notes], indent=1))

    def add(self, note: Note) -> Note:
        self.notes.append(note)
        self.save()
        return note

    def save_snapshot(self, jpeg: bytes, tag: str = "") -> str:
        name = f"{int(time.time() * 1000)}{'-' + tag if tag else ''}.jpg"
        (self.snapshots / name).write_bytes(jpeg)
        return name

    def search(self, query: str, limit: int = 8) -> list[Note]:
        """Plain word matching: enough for a few hundred notes, and predictable."""
        words = [w for w in query.lower().split() if len(w) > 2]
        if not words:
            return self.notes[-limit:]
        scored = []
        for n in self.notes:
            hay = f"{n.label} {n.description} {' '.join(n.tags)}".lower()
            score = sum(w in hay for w in words)
            if score:
                scored.append((score, n))
        scored.sort(key=lambda s: (s[0], s[1].t), reverse=True)
        return [n for _, n in scored[:limit]]

    def summary(self, limit: int = 20, with_age: bool = False) -> str:
        if not self.notes:
            return "Nothing remembered yet."
        return "\n".join(n.as_text(with_age) for n in self.notes[-limit:])
