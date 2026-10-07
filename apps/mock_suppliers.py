#!/usr/bin/env python3
"""Local WebSocket ASR/LLM/TTS fixtures. This isn't a vendor API emulator."""
import argparse
import asyncio
import json
import signal

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

MAX_MESSAGE = 65536


class Suppliers:
    def __init__(self, args):
        self.args = args
        self.active = 0

    async def handler(self, socket):
        if self.active >= self.args.max_connections:
            await socket.close(1013, "mock capacity reached")
            return
        self.active += 1
        partial = 0
        try:
            path = socket.request.path
            if path not in ("/asr", "/llm", "/tts"):
                await socket.close(1008, "unknown mock endpoint")
                return
            while True:
                raw = await asyncio.wait_for(socket.recv(), timeout=30)
                if not isinstance(raw, str):
                    raise ValueError("expected text WebSocket message")
                request = json.loads(raw)
                request_id = request["id"]
                if type(request_id) is not int or not 0 < request_id < 2**64:
                    raise ValueError("invalid request id")
                async with asyncio.timeout(5):
                    fault = self.args.fault if path == "/" + self.args.fault_service else "none"
                    if fault == "stall":
                        # Wake immediately if the cancelling client disconnects.
                        await socket.wait_closed()
                        return
                    if fault == "disconnect":
                        await socket.close(1011, "injected disconnect")
                        return
                    if fault == "malformed":
                        await socket.send("{not-json")
                        return
                    if fault == "oversize":
                        await socket.send("x" * (MAX_MESSAGE + 1))
                        return
                    if fault == "wrong_id":
                        request_id += 1

                    async def send(kind, **fields):
                        data = json.dumps(dict(id=request_id, type=kind, **fields))
                        if len(data.encode()) > MAX_MESSAGE:
                            raise ValueError("mock response too large")
                        if fault == "fragment":
                            middle = len(data) // 2
                            await socket.send([data[:middle], data[middle:]])
                        else:
                            await socket.send(data)

                    if fault == "error":
                        await send("error", code="injected")
                        return
                    if fault == "ping":
                        pong = await socket.ping(b"mock")
                        await asyncio.wait_for(pong, timeout=1)
                    if path == "/asr":
                        if request["type"] != "audio":
                            raise ValueError("expected audio")
                        audio = request["audio"]
                        samples = audio["samples"]
                        channels = audio["channels"]
                        if (type(channels) is not int or not 1 <= channels <= 8 or
                                type(audio["sample_rate_hz"]) is not int or
                                not 0 < audio["sample_rate_hz"] <= 192000 or
                                type(audio["end_of_utterance"]) is not bool or
                                not isinstance(samples, list) or len(samples) > 8192 or
                                len(samples) % channels or any(type(s) is not int or
                                not -32768 <= s <= 32767 for s in samples)):
                            raise ValueError("invalid PCM")
                        if audio["end_of_utterance"]:
                            await send("transcript", text=self.args.transcript, final=True)
                            partial = 0
                        elif samples and not partial:
                            await send("transcript", text=self.args.transcript[:3], final=False)
                            partial = 1
                        if fault == "stall_after_first":
                            await socket.wait_closed()
                            return
                        await send("done")
                    elif path == "/llm":
                        if request["type"] != "generate":
                            raise ValueError("expected generate")
                        messages = request["messages"]
                        if not isinstance(messages, list) or len(messages) > 1024:
                            raise ValueError("invalid history")
                        last = messages[-1]["content"] if messages else "Hello."
                        if not isinstance(last, str):
                            raise ValueError("invalid content")
                        if messages and messages[-1]["role"] == "user" and last.startswith("echo "):
                            await send("done", tool_calls=[dict(id="mock-echo", name="echo", arguments=last[5:])])
                            return
                        prefix = "The tool returned: " if messages and messages[-1]["role"] == "tool" else "You said: "
                        await send("text_delta", text=prefix)
                        if fault == "stall_after_first":
                            await socket.wait_closed()
                            return
                        await send("text_delta", text=last)
                        await send("done", tool_calls=[])
                        return
                    else:
                        if request["type"] != "synthesize" or not isinstance(request["text"], str):
                            raise ValueError("expected synthesize")
                        if request["text"]:
                            for index in range(2):
                                pcm = [40000] if fault == "bad_pcm" else [0] * 160
                                await send("audio", audio=dict(samples=pcm, sample_rate_hz=16000, channels=1))
                                if fault == "stall_after_first" and index == 0:
                                    await socket.wait_closed()
                                    return
                        await send("done")
                        return
        except (ConnectionClosed, asyncio.TimeoutError):
            pass
        except (KeyError, TypeError, ValueError, RecursionError):
            await socket.close(1008, "invalid mock request")
        finally:
            self.active -= 1


async def main(args):
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    suppliers = Suppliers(args)
    async with serve(suppliers.handler, "127.0.0.1", args.port,
                     max_size=MAX_MESSAGE, max_queue=4, write_limit=16384,
                     open_timeout=2, close_timeout=1, ping_interval=None,
                     compression=None) as server:
        # One machine-readable readiness line, no user payload logging.
        print(json.dumps({"ready": True, "port": server.sockets[0].getsockname()[1]}), flush=True)
        await stop.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--max-connections", type=int, default=16)
    parser.add_argument("--transcript", default="Hello.")
    parser.add_argument("--fault-service", choices=("asr", "llm", "tts"), default="llm")
    parser.add_argument("--fault", choices=("none", "stall", "stall_after_first", "disconnect",
                        "malformed", "oversize", "wrong_id", "error", "fragment", "ping", "bad_pcm"),
                        default="none")
    options = parser.parse_args()
    if (not 0 <= options.port <= 65535 or not 1 <= options.max_connections <= 128 or
            len(options.transcript.encode()) > 4096):
        parser.error("port, connection limit or transcript outside mock bounds")
    asyncio.run(main(options))
