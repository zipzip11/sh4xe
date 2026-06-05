#!/usr/bin/env python3
"""Minimal, dependency-free client for an MCP "streamable HTTP" server.

Both the IDA (`ida-pro-mcp`) and Ghidra MCP servers expose the same JSON-RPC 2.0
surface at ``POST <base>/mcp``: a ``tools/call`` request returns a result whose
``content[]`` carries the tool output as text (usually JSON). This client wraps
that single round-trip with the standard library only -- no ``requests``, no MCP
SDK -- so the SDK tooling stays installable on a bare Python 3.8+.

It tolerates both reply framings these servers use:
  * a plain ``application/json`` body, and
  * an SSE-framed body (``event: message`` / ``data: {...}`` lines).

Discovery of the right port is intentionally out of scope: pass the base URL
explicitly (e.g. ``http://127.0.0.1:13337`` for the live IDA instance). Find a
server's port with ``netstat -ano -p tcp`` and probe ``/mcp`` with ``tools/list``.
"""

from __future__ import annotations

import itertools
import json
import urllib.error
import urllib.request
from typing import Any


class McpError(RuntimeError):
    """A transport-level or tool-level MCP failure."""


class McpClient:
    def __init__(self, base_url: str, timeout: float = 60.0) -> None:
        # Accept either ".../mcp" or a bare base; normalise to the /mcp endpoint.
        base_url = base_url.rstrip("/")
        self.endpoint = base_url if base_url.endswith("/mcp") else base_url + "/mcp"
        self.timeout = timeout
        self._ids = itertools.count(1)

    def _rpc(self, method: str, params: dict[str, Any] | None = None) -> Any:
        payload = {"jsonrpc": "2.0", "id": next(self._ids), "method": method}
        if params is not None:
            payload["params"] = params
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            self.endpoint,
            data=data,
            method="POST",
            headers={
                "Content-Type": "application/json",
                # Some servers reply with SSE; advertise both so either is valid.
                "Accept": "application/json, text/event-stream",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                body = resp.read().decode("utf-8", "replace")
        except urllib.error.URLError as exc:
            raise McpError(f"{self.endpoint}: {exc}") from exc

        message = _parse_body(body)
        if "error" in message:
            raise McpError(f"{method}: {message['error']}")
        return message.get("result")

    def list_tools(self) -> list[str]:
        result = self._rpc("tools/list") or {}
        return [t.get("name", "") for t in result.get("tools", [])]

    def call(self, name: str, arguments: dict[str, Any] | None = None) -> Any:
        """Invoke a tool and return its decoded payload.

        The text content is JSON-decoded when possible, otherwise returned raw.
        """
        result = self._rpc("tools/call", {"name": name, "arguments": arguments or {}})
        if not isinstance(result, dict):
            return result
        if result.get("isError"):
            text = _content_text(result)
            raise McpError(f"{name}: {text}")
        # Prefer a structured result if the server provides one.
        if "structuredContent" in result and result["structuredContent"] is not None:
            return result["structuredContent"]
        text = _content_text(result)
        if text is None:
            return result
        try:
            return json.loads(text)
        except (json.JSONDecodeError, TypeError):
            return text


def _content_text(result: dict[str, Any]) -> str | None:
    parts = result.get("content")
    if not isinstance(parts, list):
        return None
    chunks = [p.get("text", "") for p in parts if isinstance(p, dict) and p.get("type") == "text"]
    return "\n".join(chunks) if chunks else None


def _parse_body(body: str) -> dict[str, Any]:
    body = body.strip()
    if not body:
        raise McpError("empty response body")
    # Plain JSON.
    if body[0] == "{":
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            pass
    # SSE framing: collect the last well-formed `data:` JSON object.
    last: dict[str, Any] | None = None
    for line in body.splitlines():
        line = line.strip()
        if line.startswith("data:"):
            chunk = line[len("data:"):].strip()
            try:
                last = json.loads(chunk)
            except json.JSONDecodeError:
                continue
    if last is None:
        raise McpError(f"unparseable MCP response: {body[:200]!r}")
    return last


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Probe an MCP /mcp endpoint and list its tools.")
    parser.add_argument("url", help="base URL, e.g. http://127.0.0.1:13337")
    args = parser.parse_args()
    client = McpClient(args.url)
    for tool in client.list_tools():
        print(tool)
