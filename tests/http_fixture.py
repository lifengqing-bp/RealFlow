#!/usr/bin/env python3
"""Run C++ integration cases against a loopback-only HTTP fixture.

No keys, Internet access, fixed ports, third-party packages, or timing sleeps.
The timeout route uses a release event. The wrapper owns both child and server
lifetimes and returns the child's failure status to CTest/Make.
"""
from __future__ import annotations

import collections
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), Handler)
        self.counts: collections.Counter[str] = collections.Counter()
        self.lock = threading.Lock()
        self.release_timeout = threading.Event()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: FixtureServer

    def log_message(self, *_args: object) -> None:
        pass  # Never print headers, authorization, or request payloads.

    def reply(self, status: int, body: bytes, headers: dict[str, str] | None = None,
              *, fragmented: bool = False, truncate: bool = False) -> None:
        self.send_response(status)
        self.send_header("Content-Length", str(len(body) + (100 if truncate else 0)))
        self.send_header("Connection", "close")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        try:
            for i in range(0, len(body), 3 if fragmented else max(1, len(body))):
                self.wfile.write(body[i:i + (3 if fragmented else len(body))])
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when testing cancellation/limits/malformed streams.
        self.close_connection = True

    @staticmethod
    def event(delta: dict) -> bytes:
        payload = {"choices": [{"delta": delta}]}
        return ("data: " + json.dumps(payload, ensure_ascii=False) + "\n\n").encode()

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if parsed.path == "/counts":
            path = parse_qs(parsed.query)["path"][0]
            with self.server.lock:
                value = self.server.counts[path]
            self.reply(200, str(value).encode())
            return
        if parsed.path == "/release-timeout":
            self.server.release_timeout.set()
            self.reply(200, b"released")
            return
        with self.server.lock:
            self.server.counts[parsed.path] += 1
            attempt = self.server.counts[parsed.path]
        if parsed.path == "/echo":
            self.reply(200, body, {"X-FiXtUrE": "echo", "X-Method": self.command,
                                  "X-Auth": self.headers.get("Authorization", "")})
        elif parsed.path == "/retry":
            self.reply(503 if attempt < 3 else 200, b"retry" if attempt < 3 else b"ready",
                       {"Retry-After": "1"})
        elif parsed.path in {"/always-503", "/cancel-backoff", "/stream-503"}:
            self.reply(503, b"retryable body")
        elif parsed.path == "/bad-request":
            self.reply(400, b"fixture private error")
        elif parsed.path in {"/large", "/stream-large"}:
            self.reply(200, b"x" * 4096, fragmented=parsed.path == "/stream-large")
        elif parsed.path in {"/sink-throws", "/sink-stops"}:
            self.reply(200, b"first audio text bytes")
        elif parsed.path == "/truncated":
            self.reply(200, b"partial-body", truncate=True)
        elif parsed.path == "/timeout":
            self.server.release_timeout.wait(5)
            self.reply(200, b"too late")
        elif parsed.path == "/redirect":
            self.reply(302, b"", {"Location": "/unexpected-redirect-target"})
        elif parsed.path == "/bad/chat/completions":
            self.reply(200, b"data: {broken-json}\n\n", {"Content-Type": "text/event-stream"})
        elif parsed.path == "/v1/chat/completions":
            request = json.loads(body)
            if not request.get("stream") or self.headers.get("Accept") != "text/event-stream":
                self.reply(400, b"streaming contract violated")
                return
            if request["model"] == "after-done":
                data = self.event({"content": "Final answer!"}) + b"data: [DONE]\n\n"
                data += self.event({"tool_calls": [{"index": 0, "id": "stale", "function": {
                    "name": "effect", "arguments": "{}"}}]})
            elif request["messages"][-1]["role"] == "tool":
                if request["messages"][-1]["content"] != "42":
                    self.reply(400, b"tool result missing")
                    return
                data = self.event({"content": "答案是42。"}) + b"data: [DONE]\n\n"
            else:
                data = self.event({"tool_calls": [{"index": 0, "id": "call1", "function": {
                    "name": "double", "arguments": '{"value":'}}]})
                data += self.event({"tool_calls": [{"index": 0, "function": {"arguments": "21}"}}]})
                data += b"data: [DONE]\n\n"
            self.reply(200, data, {"Content-Type": "text/event-stream"}, fragmented=True)
        else:
            self.reply(404, b"unknown fixture route")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: http_fixture.py TEST_BINARY [--list | --case NAME]", file=sys.stderr)
        return 2
    binary = str(Path(sys.argv[1]).resolve())
    server = FixtureServer()
    thread = threading.Thread(target=server.serve_forever, name="http-fixture")
    thread.start()
    try:
        env = os.environ.copy()
        # Loopback requests must never be routed through a user's proxy.
        env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,localhost"
        result = subprocess.run(
            [binary, f"http://127.0.0.1:{server.server_port}", *sys.argv[2:]],
            env=env, timeout=60, check=False,
        )
        return result.returncode if result.returncode >= 0 else 128 - result.returncode
    except subprocess.TimeoutExpired:
        print("HTTP integration child exceeded 60 seconds", file=sys.stderr)
        return 124
    finally:
        server.release_timeout.set()
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
