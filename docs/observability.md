# Observability is a test contract

RealFlow is the bridge between humans and AI. Correct audio/text output alone does
not prove that the intended interaction occurred: an obsolete tool might run, a
cancelled response might report success, or a dropped event might hide a failure.
Tests must check both the result and the observable evidence.

This is an M2.3/M2.5 slice, using the existing EventTimeline, EventBus and metric
registry. It does not introduce a telemetry service, new worker, generic trace
framework, device adapter or intent model. See [architecture.md](architecture.md)
and [roadmap.md](roadmap.md) for the wider design.

## Contract

For a controlled scenario, assert identity (`session_id`, `generation`, `turn_id`,
`trace_id`), endpoint cardinality, required causal order, outcomes, metric sample
counts, and loss/resource counters. Do not require unrelated threads to produce
one universal total order. Live latency is variable; metric arithmetic over fixed
timestamps must be exact.

`Conversation` passes its event bus to `AsyncSession` and the agent invocation.
The pipeline bridge admits conversation/model/tool events to the same canonical
session timeline. An invocation bus replaces the configured default for that call;
it does not mutate the agent's routing or publish duplicate events to two buses.
A borrowed invocation bus must outlive the task and its completion callbacks.

The test pipeline does not manually wire an Agent bus. It asserts two model
steps, one tool invocation, streamed audio before agent completion, and identical
retained/subscriber events. Model, ASR, TTS and tool metrics are then derived from
the same trace.

## Requests are not outcomes

| Event | Meaning |
|---|---|
| `ResponseCancelRequested` | Explicit pipeline cancellation requested for current/queued responses. Session-scoped, not a claim that one particular response was stopped or that a player is silent. |
| `TurnStarted` | The existing LLM/tool agent invocation began executing. |
| `TurnCompleted` | That invocation produced its final result. TTS may still be running; this is not a business-success or playback-completion guarantee. |
| `TurnCancelled` / `TurnFailed` | The started agent invocation terminated through its stop predicate or failure. A deadline can trigger the stop predicate. |
| `ConversationTurnCompleted` | The response-generation/TTS completion path finished. Not proof of physical playout. |
| `ConversationTurnCancelled` / `ConversationTurnFailed` | The started response path terminated, after draining its synthesis worker. Deadline failures use `name=deadline`. |

The agent owns its terminal observation; AsyncSession does not emit a second agent
cancellation after speech cleanup. For example, successful LLM output followed by
failed synthesis produces `TurnCompleted` and `ConversationTurnFailed`, not a
second contradictory agent outcome. The step-limit explanatory response remains
available, but the agent attempt is labelled `TurnFailed`. An incomplete LLM
stream (`complete=false`) is rejected instead of committing it as successful.

Cancellation does not undo external effects. `ToolCompleted` means the current
serialized invocation returned; its result may itself describe a domain error.
Queued work cancelled before execution has no `TurnStarted`; this change does not
add an exactly-once terminal journal for every executor reservation. Runtime
retirement still uses authoritative state/futures, never notification delivery.

## Deterministic metrics

`RuntimeObserver::observe(const Event&)` is the same synchronous reducer used by
live subscriptions. It reads `event.timestamp`, never the observer's wall clock.
`received_at` records timeline admission time and is kept separately. Timed events
must share a comparable local monotonic clock; do not subtract unrelated device,
remote-server or media clocks. Zero timestamps mean unspecified, so fixed-clock
fixtures start at a nonzero epoch.

| Metric suffix (existing `byteturn_` prefix) | Start -> end |
|---|---|
| `asr_final_latency_ms` | ASR end-of-utterance -> final transcript |
| `time_to_first_token_ms` | Model start -> first nonempty text token, within a model step |
| `model_duration_ms` | Model start -> model completion, within a step |
| `tool_duration_ms` | Tool start -> return, within the current serialized tool invocation |
| `turn_duration_ms` | Agent invocation start -> successful agent result |
| `tts_first_audio_latency_ms` | First TTS chunk submission -> first generated audio |
| `tts_total_duration_ms` | First TTS chunk submission -> synthesis completion |
| `s2s_first_audio_latency_ms` | Pipeline EOU (or native input speech end) -> first generated/output audio |
| `s2s_first_audible_latency_ms` | Native input speech end -> supplied playback-start acknowledgement |
| `conversation_turn_duration_ms` | Pipeline EOU (or native input speech end) -> response-generation completion |

**Migration:** TTS metrics now start at the first `TtsChunkStarted`, not generic
`SpeechStarted`. Native speech generation must not manufacture a TTS-stage metric.
The legacy `barge_in_stop_latency_ms` still measures cancellation dispatch/return,
not physical silence. Native input/response association is still the single-flight
legacy fallback; repeated unpaired speech-end events invalidate intervals instead
of selecting an arbitrary timestamp. Full native/playback unification is separate.

Pending interval keys include session incarnation and structured fields, avoiding
cross-generation and delimiter collisions within a runtime. Separate runtime
instances need separate observers/registries or globally unique session IDs; the
current event envelope does not carry a RuntimeManager ID. Interval count is bounded (4096 by
default; constructor override). Terminal events discard the relevant pending spans;
agent completion does not discard still-running speech. Successful durations are
not manufactured for cancellation/failure. Upstream timeline/string limits still
apply; direct legacy EventBus/replay producers must bound their input strings.

Missing endpoints produce no sample, not a zero. Invalid/negative intervals and
ambiguous duplicate starts are excluded. These conditions are inspectable through
`byteturn_observer_unmatched_endpoints_total`, `invalid_intervals_total`,
`duplicate_starts_total`, `span_limit_rejections_total`, and
`discarded_spans_total` (each with the same `byteturn_observer_` prefix).
`pending_spans()` exposes the remaining reducer state. Repeated end events cannot
produce another duration without a start; raw event counters count deliveries, not
an unbounded deduplication history.

## Test procedure

1. Use synthetic event timestamps for exact numerical assertions. The fixture
   checks ASR=20 ms, model TTFT=7 ms, model total=30 ms, tool=10 ms, TTS first
   audio=15 ms, and exact histogram buckets. No sleeps are used for this arithmetic.
2. Use condition variables/promises to put real pipeline workers at specific
   boundaries. Finite waits are deadlock guards, not simulated business latency.
   Assert causal relations, such as first audio before the held model completes.
3. Quiesce producers, then flush accepted notifications before inspecting results.
   Check `TimelineStats` for rejections, evictions, notification drops and observer
   failures. A successful `flush()` does not prove no events were lost.
4. Replay a complete retained snapshot into a fresh observer/registry and compare
   the metric export exactly. Do not feed replay into the already-live registry.

```cpp
byteturn::MetricsRegistry replay_metrics;
byteturn::RuntimeObserver replay(replay_metrics);
for (const auto& event : session.timeline().snapshot()) {
    replay.observe(event);  // Observation only: no tool execution or cancellation.
}
```

A bounded notification-overflow fixture deliberately loses one completion
notification while preserving it in the journal. Live metrics are incomplete;
reduction of the complete retained trace recovers the expected duration. A separate
retention test evicts the start event and verifies that no duration is invented.
This is deterministic metric reduction, not deterministic provider re-execution,
durable recording, or automatic recovery of an arbitrarily old truncated trace.

## Machine-readable logs and privacy

`event_json()` and `JsonEventLogger` share schema version 1: sequence, event name,
session ID, generation, turn ID, trace ID, name, `timestamp_ns`, and
`received_at_ns`. Timestamps are process-local monotonic nanoseconds, not UTC.
Control characters are escaped rather than removed, preserving identity. Logging
subscribes only after all callback state is initialized; unsubscription drains
in-flight callbacks.

Payload data remains absent by default. Transcripts, model output, tool arguments
and results require explicit opt-in. Correlation IDs and names must not embed
credentials or user payloads. Audio bytes are not written to event JSON.

Run `make test-observability` or CTest's `observability_contract_tests`; `make test`
includes the suite. Physical playback/device testing, persistent trace storage,
provider request IDs, runtime duration export and generic distributed tracing remain
separate work, not guarantees supplied by these local fixtures.
