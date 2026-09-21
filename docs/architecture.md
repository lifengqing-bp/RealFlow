# Architecture

ByteTurn borrows MiniMax Code's service boundary, not its TypeScript source.
MiniMax Code describes its main path as:

`TUI / exec / ACP -> CliService -> Applications -> Session / Turn / Agent -> model providers / tools`

ByteTurn translates that separation to realtime voice:

`audio / text / RTC -> Conversation -> Session / Turn / Agent -> ASR / LLM / TTS / tools`

## Current components

| Component | Responsibility | Must not own |
|---|---|---|
| App/transport | Capture and render user I/O | Prompt policy, tool loop |
| Conversation | Listening/thinking/speaking state, barge-in | Vendor SDK types |
| Agent | History, LLM/tool loop, step budget | Audio devices, networking |
| ToolRegistry | Dispatch named capabilities | Conversation state |
| Providers | Adapt ASR, LLM, and TTS backends | Product/session policy |

## Turn lifecycle

1. Audio frames reach ASR through `Conversation`.
2. Partial transcripts are observable but do not start an agent turn.
3. A final transcript starts the bounded agent loop.
4. The LLM may emit tool calls; results return to history and the loop repeats.
5. Final assistant text streams through TTS.
6. New input during playback increments the generation and cancels stale audio.

`SessionExecutor` now provides multiple worker lanes with FIFO serialization per
session, and `AsyncSession` binds each stateful `Agent` to its lane. Provider
calls remain synchronous inside a worker; the audio callback path should hand
off work rather than invoking a blocking provider directly.

## Planned service layers

- `TransportService`: PCM/WebSocket/RTC adapters.
- `SessionService`: persistence, configuration, and history compaction policy.
- `TurnService`: deadlines and per-turn metrics beyond cooperative cancellation.
- `AgentService`: model/tool loop and context management.
- `PermissionService`: tool authorization and sandbox policy.
- `EventSink`: structured transcript, model, tool, audio, and error events.

## Latency contract

Track ASR finalization, LLM time-to-first-token, TTS time-to-first-audio,
end-to-end response latency, and interruption-to-silence. Averages alone are not
sufficient; report p50, p95, and p99 under a declared concurrency level.
