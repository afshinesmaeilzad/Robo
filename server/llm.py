"""Talking to the model, over either of OpenAI's two APIs.

Chat Completions is simple and cheap, but some models refuse to combine function
tools with reasoning there:

    Function tools with reasoning_effort are not supported for gpt-6-luna in
    /v1/chat/completions. To use function tools, use /v1/responses or set
    reasoning_effort to 'none'.

Driving a robot wants both, so this wraps the two APIs behind one small
interface and picks whichever the run needs. The agent above does not care
which is in use.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any


@dataclass
class Call:
    """One tool call the model wants made."""

    id: str
    name: str
    args: dict


@dataclass
class Reply:
    text: str = ""
    calls: list[Call] = field(default_factory=list)
    tokens_in: int = 0
    tokens_out: int = 0
    tokens_cached: int = 0


class Brain:
    """A conversation with a model. Subclasses speak one API each."""

    def __init__(self, client, model: str, tools: list[dict]):
        self.client = client
        self.model = model
        self.tools = tools
        self.items: list[dict] = []

    # --- building the conversation ---

    def start(self, system: str, first_user: str) -> None:
        raise NotImplementedError

    def add_user_text(self, text: str) -> None:
        raise NotImplementedError

    def add_user_image(self, text: str, b64: str, detail: str) -> None:
        raise NotImplementedError

    def add_tool_result(self, call: Call, text: str) -> None:
        raise NotImplementedError

    def set_reminder(self, text: str) -> None:
        """Keep exactly one reminder, at the end, so the cached prefix survives."""
        self.items[:] = [i for i in self.items if not i.get("_reminder")]
        if text:
            self._append_reminder(text)

    def _append_reminder(self, text: str) -> None:
        raise NotImplementedError

    def prune_images(self, keep: int, note: str) -> int:
        """Drop the pixels of all but the newest `keep` pictures, keeping their text."""
        raise NotImplementedError

    async def ask(self) -> Reply:
        raise NotImplementedError

    # --- shared helpers ---

    @staticmethod
    def _args(raw: str) -> dict:
        try:
            return json.loads(raw or "{}")
        except ValueError:
            return {}

    def _clean(self, items: list[dict]) -> list[dict]:
        """Our private bookkeeping keys must never reach the API."""
        return [{k: v for k, v in i.items() if not k.startswith("_")} for i in items]


class ChatBrain(Brain):
    """Chat Completions: one message list, tools nested under "function"."""

    def api_tools(self) -> list[dict]:
        return [{"type": "function", "function": t} for t in self.tools]

    def start(self, system: str, first_user: str) -> None:
        self.items = [{"role": "system", "content": system},
                      {"role": "user", "content": first_user}]

    def add_user_text(self, text: str) -> None:
        self.items.append({"role": "user", "content": text})

    def add_user_image(self, text: str, b64: str, detail: str) -> None:
        self.items.append({
            "role": "user",
            "content": [
                {"type": "text", "text": text},
                {"type": "image_url",
                 "image_url": {"url": f"data:image/jpeg;base64,{b64}", "detail": detail}},
            ],
            "_image": True,
        })

    def add_tool_result(self, call: Call, text: str) -> None:
        self.items.append({"role": "tool", "tool_call_id": call.id, "content": text})

    def _append_reminder(self, text: str) -> None:
        self.items.append({"role": "user", "content": text, "_reminder": True})

    def prune_images(self, keep: int, note: str) -> int:
        seen = dropped = 0
        for item in reversed(self.items):
            if not item.get("_image"):
                continue
            seen += 1
            if seen > keep:
                text = next((p["text"] for p in item["content"] if p.get("type") == "text"), "")
                item["content"] = f"{text} {note}"
                item.pop("_image")
                dropped += 1
        return dropped

    async def ask(self) -> Reply:
        response = await self.client.chat.completions.create(
            model=self.model,
            messages=self._clean(self.items),
            tools=self.api_tools(),
            parallel_tool_calls=False,
        )
        message = response.choices[0].message
        self.items.append(message.model_dump(exclude_none=True))
        usage = getattr(response, "usage", None)
        details = getattr(usage, "prompt_tokens_details", None) if usage else None
        return Reply(
            text=message.content or "",
            calls=[Call(c.id, c.function.name, self._args(c.function.arguments))
                   for c in (message.tool_calls or [])],
            tokens_in=getattr(usage, "prompt_tokens", 0) or 0,
            tokens_out=getattr(usage, "completion_tokens", 0) or 0,
            tokens_cached=getattr(details, "cached_tokens", 0) or 0,
        )


class ResponsesBrain(Brain):
    """The Responses API: a list of items, and the only way to have tools and
    reasoning at once on some models. Reasoning items are fed back so the model
    keeps its train of thought between tool calls."""

    def __init__(self, client, model: str, tools: list[dict], effort: str = "medium"):
        super().__init__(client, model, tools)
        self.effort = effort
        self.system = ""

    def api_tools(self) -> list[dict]:
        return [{"type": "function", **t} for t in self.tools]

    def start(self, system: str, first_user: str) -> None:
        self.system = system
        self.items = [{"role": "user", "content": [{"type": "input_text", "text": first_user}]}]

    def add_user_text(self, text: str) -> None:
        self.items.append({"role": "user", "content": [{"type": "input_text", "text": text}]})

    def add_user_image(self, text: str, b64: str, detail: str) -> None:
        self.items.append({
            "role": "user",
            "content": [
                {"type": "input_text", "text": text},
                {"type": "input_image", "image_url": f"data:image/jpeg;base64,{b64}",
                 "detail": detail},
            ],
            "_image": True,
        })

    def add_tool_result(self, call: Call, text: str) -> None:
        self.items.append({"type": "function_call_output", "call_id": call.id, "output": text})

    def _append_reminder(self, text: str) -> None:
        self.items.append({"role": "user", "content": [{"type": "input_text", "text": text}],
                           "_reminder": True})

    def prune_images(self, keep: int, note: str) -> int:
        seen = dropped = 0
        for item in reversed(self.items):
            if not item.get("_image"):
                continue
            seen += 1
            if seen > keep:
                text = next((p["text"] for p in item["content"]
                             if p.get("type") == "input_text"), "")
                item["content"] = [{"type": "input_text", "text": f"{text} {note}"}]
                item.pop("_image")
                dropped += 1
        return dropped

    async def ask(self) -> Reply:
        response = await self.client.responses.create(
            model=self.model,
            instructions=self.system,
            input=self._clean(self.items),
            tools=self.api_tools(),
            reasoning={"effort": self.effort},
        )
        reply = Reply()
        for item in response.output:
            kind = getattr(item, "type", "")
            if kind == "function_call":
                reply.calls.append(Call(item.call_id, item.name, self._args(item.arguments)))
            elif kind == "message":
                reply.text += "".join(getattr(p, "text", "") or "" for p in item.content)
            # Reasoning items carry no text for us, but must go back in the next
            # request so the model can follow its own thinking across tool calls.
            self.items.append(item.model_dump(exclude_none=True))
        usage = getattr(response, "usage", None)
        details = getattr(usage, "input_tokens_details", None) if usage else None
        reply.tokens_in = getattr(usage, "input_tokens", 0) or 0
        reply.tokens_out = getattr(usage, "output_tokens", 0) or 0
        reply.tokens_cached = getattr(details, "cached_tokens", 0) or 0
        return reply


def make_brain(client, model: str, tools: list[dict], effort: str | None, api: str = "auto") -> Brain:
    """Responses when there is thinking to do, Chat Completions otherwise.

    Chat Completions is the cheaper, simpler path, and without reasoning there is
    nothing the Responses API adds here.
    """
    wants_thinking = bool(effort) and effort != "none"
    use_responses = api == "responses" or (api == "auto" and wants_thinking
                                           and hasattr(client, "responses"))
    if use_responses:
        return ResponsesBrain(client, model, tools, effort or "medium")
    return ChatBrain(client, model, tools)
