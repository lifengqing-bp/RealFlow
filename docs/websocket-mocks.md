# Level 2: mock suppliers over WebSocket

The pipeline uses `WebSocketMockAsr`, `WebSocketMockLlm` and `WebSocketMockTts`
through the existing provider interfaces. Audio/history/text cross a real WebSocket
connection to `apps/mock_suppliers.py`; transcripts, deltas and PCM return over it.
The server runs in a separate process. No remote model credentials are needed.

This exercises connection setup, WebSocket framing, JSON serialization, streaming,
backpressure, disconnects and cancellation. It does not emulate a specific vendor's
API, recognize speech, synthesize intelligible speech, or measure model latency.
The protocol is deliberately local and small; production TLS/authentication and
vendor protocols belong in their own adapters.

## Build and run

Prerequisites: existing C++17/libcurl build dependencies, CMake 3.20+, Boost headers
(tested with 1.86), nlohmann/json headers (tested with 3.11.3), and Python 3.11+.
For example, on Ubuntu install `libboost-dev nlohmann-json3-dev cmake`; on macOS,
the corresponding Homebrew packages are `boost nlohmann-json cmake`.

```sh
python3 -m pip install -r apps/requirements-mocks.txt
cmake -S . -B build/ws -DREALFLOW_WEBSOCKET_MOCKS=ON
cmake --build build/ws -j2
ctest --test-dir build/ws --output-on-failure
```

For headers outside standard paths, set `REALFLOW_BOOST_INCLUDE_DIR` and
`REALFLOW_JSON_INCLUDE_DIR` to their include roots. Set `Python3_EXECUTABLE` if
the selected interpreter differs from the one containing `websockets`.
Boost and JSON are private dependencies of the optional `byteturn_ws_mocks`
target; no third-party types enter public headers and no dependencies are vendored.
The regular `make test` continues to run the core and Level 1 tests. Level 2 uses
the optional CMake target and is included in CTest only when enabled.

Terminal 1:

```sh
python3 apps/mock_suppliers.py --port 8765
```

Terminal 2:

```sh
./build/ws/websocket_mock_demo 8765
# Mock pipeline: finals=1 samples=320 completed=1 verified=1
```

The demo uses the same session/pipeline code as `mock_pipeline_demo`, with the
providers replaced at construction. It verifies event identity/order, terminal
completion and ASR, LLM and TTS metric counts. It generates silence, not speech.

```cpp
#include "byteturn/websocket_mock_providers.h"
byteturn::WebSocketMockConfig config;
config.port = 8765;
config.timeout = std::chrono::milliseconds(2000);
byteturn::WebSocketMockAsr asr(config);
byteturn::WebSocketMockLlm llm(config);
byteturn::WebSocketMockTts tts(config);
// Inject these into Agent / AsyncSession / PipelineConversationEngine as usual.
```

## Protocol

All messages are UTF-8 JSON in text WebSocket messages. PCM is represented as signed
16-bit sample arrays for inspectability; this is not a bandwidth-efficient media
format. Every reply repeats the positive integer request `id`. A request succeeds
only after a matching `done`. An unexpected event, wrong identity, malformed JSON,
invalid PCM, supplier `error`, or disconnect before completion fails the request.

| Endpoint | Request | Streamed replies | Connection lifetime |
|---|---|---|---|
| `/asr` | `audio`, with `audio.samples`, `sample_rate_hz`, `channels`, `end_of_utterance` | Zero or more `transcript` events (`text`, `final`), then `done` per push | Reused across pushes; increasing request IDs; reset/failure forces reconnect |
| `/llm` | `generate`, with `messages` containing role, content, name, tool calls and tool-call ID | `text_delta` events, then `done` with `tool_calls` | Fresh connection per model invocation |
| `/tts` | `synthesize`, with `text` | `audio` events with PCM metadata, then `done` | Fresh connection per text chunk |

Example LLM exchange:

```json
{"id":1,"type":"generate","messages":[{"role":"user","content":"Hello."}]}
{"id":1,"type":"text_delta","text":"You said: "}
{"id":1,"type":"text_delta","text":"Hello."}
{"id":1,"type":"done","tool_calls":[]}
```

ASR returns one partial on the first nonempty frame and the configured final on EOU
(`--transcript`, default `Hello.`). EOU can carry empty PCM and restarts partial
state for the next utterance. LLM echoes the last message; `echo VALUE` produces
a structured `echo` tool call, then acknowledges its result on the next invocation.
TTS emits two 160-sample, 16 kHz mono silence frames per nonempty text chunk.

## Ownership, cancellation and bounds

- Adapters add no threads or queues. The existing ASR, agent and TTS execution lanes
  drive Boost.Beast asynchronous I/O until each provider call finishes. They never
  perform network I/O on capture callbacks in the normal pipeline.
- Every call has one absolute deadline, including connect/upgrade, writes and all
  response frames. Client cancellation is checked between I/O pump intervals of
  at most 5 ms; OS scheduling and user callbacks can extend observed latency.
  There is no DNS resolution: both client and server use `127.0.0.1` only.
- LLM cancellation/rejected deltas fail the invocation; TTS cancellation/rejected
  PCM stops delivery. ASR reset advances an atomic epoch, cancels any pending push,
  and reconnects on the next push. Cancel/reset callers never operate on the socket
  or wait for the server. The owning provider call closes its socket and drains
  outstanding I/O handlers before returning. No cancelled connection is reused.
- The mock protocol uses connection termination for cancellation, without a second
  control queue or acknowledgement. Terminal LLM/TTS calls also release their
  sockets; it does not require a graceful WebSocket close handshake. The server
  does not retain work after a detected disconnect. No retries replay a request.
- Call ASR push serially; reset may race it. Serialize TTS begin/synthesis/end;
  cancel may race synthesis. Keep adapters alive until all provider calls drain.
  A callback already entered may finish; blocking application callbacks cannot
  be forcibly cancelled. Standalone TTS synthesize starts a fresh utterance;
  incremental chunks retain cancellation until the next begin.
- Defaults: client message limit 64 KiB, cumulative reply limit 1 MiB, 256 reply
  messages per request, 2-second deadline. JSON nesting is capped at 16. History
  and audio inputs are checked before serialization, then encoded size is checked.
  The server limits each message to 64 KiB, receive queue to four frames, write
  buffer high-water mark to 16 KiB, and active supplier handlers to 16. Excess
  handlers close with 1013. It has a 30-second idle timeout and 5-second request
  timeout. These development limits do not promise resistance to connection floods.
- An ASR provider exception becomes a payload-free `Error` observation named `asr`
  instead of escaping the audio worker. The session stays available for subsequent
  input. This does not automatically retry the lost frame or fabricate a transcript.

## Fault testing

```sh
python3 apps/mock_suppliers.py --fault-service llm --fault stall
ctest --test-dir build/ws -R websocket_mock_integration --output-on-failure
```

The integration runner owns its servers on OS-assigned ports, waits for explicit
readiness, drives the C++ clients and demo, and terminates/joins every process.
It covers real streaming and tools, ASR reset, sink rejection, fragmented messages,
ping handling, malformed JSON, wrong IDs, oversized messages, response budgets,
disconnect, supplier errors, deadline, invalid PCM, cancellation after the first
ASR/LLM/TTS callback, and ASR failure containment in a real session. Cancellation
tests synchronize on received callbacks, not sleeps.

Implementation references: [Boost.Beast WebSocket notes](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_websocket/notes.html)
and [Python websockets server API](https://websockets.readthedocs.io/en/stable/reference/asyncio/server.html).
