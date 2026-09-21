# Architecture

ByteTurn borrows MiniMax Code's service boundary, not its TypeScript source.
MiniMax Code describes its main path as:

`TUI / exec / ACP -> CliService -> Applications -> Session / Turn / Agent -> model providers / tools`

ByteTurn translates that separation to realtime voice:

`audio / text / RTC -> Conversation -> {Session / Agent cascade | realtime speech model}`

## Current components

| Component | Responsibility | Must not own |
|---|---|---|
| App/transport | Capture and render user I/O | Prompt policy, tool loop |
| Conversation | Cascaded ASR/agent/TTS state and cancellation | Vendor SDK types |
| FullDuplexConversation | Concurrent audio I/O, playout tracking, barge-in | Prompt/tool policy, vendor SDK types |
| Agent | History, LLM/tool loop, step budget | Audio devices, networking |
| ToolRegistry | Dispatch named capabilities | Conversation state |
| Providers | Adapt ASR, LLM, and TTS backends | Product/session policy |

## Turn lifecycle

### Cascaded mode

1. Audio frames enter a bounded `Conversation` queue; an ASR worker drains it.
2. Partial transcripts are observable but do not start an agent turn.
3. A final transcript enqueues a `TurnContext` on the session executor and
   returns immediately.
4. The LLM may emit tool calls; results return to history and the loop repeats.
5. Final assistant text streams through TTS.
6. New input during playback increments the generation and cancels stale audio.

`SessionExecutor` provides multiple worker lanes with FIFO serialization per
session, bounded pending queues, and deadlines measured from submission rather
than execution. `AsyncSession` binds each stateful `Agent` to its lane. Provider
calls remain synchronous inside a worker, but audio capture and ASR callbacks
only perform bounded copies or turn submission.

Each turn owns a `TurnContext`: session/turn/trace identity, a monotonic
deadline, cooperative cancellation, and lifecycle state. LLM transports and
cancellable tools share the same `stop_requested` condition. Cancellation,
deadline expiry, tool failure, and provider failure roll history back to the
checkpoint taken before the user message.

### Native full-duplex mode

1. A bounded worker continuously forwards microphone frames to a
   `RealtimeSpeechSession`, including while model audio is playing.
2. Provider VAD events or explicit client VAD calls mark speech start/end.
3. Provider audio is tagged with a response ID and monotonic sample offset.
4. The player acknowledges playback start and the last audible sample.
5. New user speech cancels the active response and truncates provider context
   at the acknowledged position. Late packets for that response are discarded.

The provider session contract requires thread-safe input, commit, cancellation,
and close operations. `close()` must quiesce callbacks before returning. The
runtime deliberately does not infer speech from arbitrary audio frames: client
VAD, provider VAD, and acoustic echo cancellation belong at the media boundary.

## Planned service layers

- `TransportService`: PCM/WebSocket/RTC adapters.
- `SessionService`: persistence, configuration, and history compaction policy.
- `TurnService`: policy budgets and queue-wait metrics beyond the current
  unified deadline/cancellation context.
- `AgentService`: model/tool loop and context management.
- `PermissionService`: tool authorization and sandbox policy.
- `EventSink`: structured transcript, model, tool, audio, and error events.

## Latency contract

Track ASR finalization, LLM time-to-first-token, TTS time-to-first-audio,
end-to-end response latency, speech-end-to-first-audible-audio, and
interruption-to-cancel. Physical interruption-to-silence must be measured by
the transport/player after it drains or flushes its device buffer. Averages
alone are not sufficient; report p50, p95, and p99 under a declared concurrency
level.

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
