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

## Observability and network boundary

`EventBus` is the correlation backbone. Session and turn IDs connect structured
logs to model/tool/voice events. `RuntimeObserver` derives Prometheus counters
and duration histograms without placing provider-specific instrumentation in the
agent loop. Subscription removal waits for in-flight callbacks, so observers can
be safely destroyed while other sessions are active.

`CurlHttpTransport` owns HTTP mechanics: TLS verification, timeouts, response
limits, retries, `Retry-After`, cancellation, and DNS/connect/TLS/first-byte/total
timings. `OpenAiCompatibleLlm` owns only protocol serialization. HTTP retries can
increase cost for requests whose response was lost; applications may set
`max_attempts = 1` or disable `retry_http_errors` when duplicate inference is
unacceptable.

The streaming path never retries after response body delivery begins, because
doing so could duplicate already-spoken text. SSE events may split at any byte
boundary; the parser buffers complete lines and reconstructs text and tool-call
deltas before committing them to agent history.

Sentence boundaries feed a bounded TTS worker queue. This separates network
reading from synthesis while retaining backpressure. End-to-end metrics use the
input EOU event as their acoustic endpoint and the first produced audio frame as
their output endpoint; hardware playout latency is intentionally not inferred.
