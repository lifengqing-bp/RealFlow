# RealFlow Architecture

**RealFlow — Realtime Human-Agent Interaction Runtime**

Human interaction is a continuous realtime stream. Agent reasoning and tool
execution are asynchronous work. RealFlow connects the two without requiring
an entire session to follow a sequential listen → think → speak loop.

This document defines the architectural direction and the migration contract.
The implementation snapshot below is based on `e312d8c` (PR #1). **Current**
means code exists, not that every production requirement has been verified.
**Target** and **planned** sections describe work that is not yet implemented.
Delivery order and acceptance gates are in [the roadmap](roadmap.md).

## 1. Scope and architectural decisions

RealFlow owns interaction lifetime, stream coordination, interruption policy,
output arbitration, and the boundary between conversation and agent execution.
It should accept both an ASR–LLM–TTS pipeline and a native speech-to-speech
backend without exposing vendor-specific session semantics to applications.

| Decision | Consequence |
|---|---|
| A session owns interaction lifetime. | A turn must not gate microphone input, playback, or unrelated work. |
| A logical turn is a derived view. | Retain turn IDs for compatibility and analysis, not as the universal execution container. |
| Conversation and agent execution are independent. | Reasoning or tools may continue while the user and assistant interact. |
| Capabilities select behavior. | No provider-name conditionals in the session or conversation policy. |
| Commands and observations are different. | A recorded event must not accidentally invoke a tool or trigger itself again. |
| Media, control, and agent work have separate execution paths. | Network calls, synthesis, and telemetry must not block an audio callback. |
| Cancellation is scoped. | Stopping speech is not automatically permission to cancel or repeat an external action. |
| Observability uses explicit endpoints. | Generated audio, accepted output, and audible playback are different events. |

The core remains portable C++17. Vendor SDKs and wire protocols belong in
adapters. RealFlow is not intended to replace a general planning framework,
workflow DSL, vector database, or model-training stack. Existing agent systems
should be attachable through a small asynchronous boundary.

## 2. Current implementation and migration gaps

Source paths still use `byteturn`. The CMake project is named `RealFlow`, but
namespace, include paths, library/executable names, and metric prefixes have
not completed the rename. Examples must use the symbols that actually exist;
`namespace realflow` is a migration target, not today's API.

| Component | Current responsibility | Remaining boundary work |
|---|---|---|
| [`ConversationSession`](../include/byteturn/conversation_session.h) | Owns an engine and an in-memory timeline; forwards audio and events. | Lifecycle race handling, failure cleanup, event validation, and explicit session events. |
| [`EventTimeline`](../src/event_timeline.cpp) | Appends event copies to a vector and returns copy snapshots. | Canonical sequence/timestamp assignment, bounded retention, ordered delivery, and persistence contracts. |
| [`ConversationEngine`](../include/byteturn/conversation_engine.h) | Defines start/input/event/stop, capabilities, and an orthogonal state snapshot. | Precise threading and command contracts; capability negotiation and semantic event normalization. |
| [`PipelineConversationEngine`](../src/pipeline_conversation_engine.cpp) | Wraps `Conversation` and forwards its private bridge-bus events. | Complete agent/model/tool event capture, external controls, and authoritative state projection. |
| [`Conversation`](../src/conversation.cpp) | Runs bounded ASR input, asynchronous agent submission, sentence segmentation, and incremental TTS. | Remove implicit interruption on arbitrary input frames and the internal exclusive state assumption. |
| [`FullDuplexConversation`](../src/full_duplex_conversation.cpp) | Keeps native input active during output; tracks response/sample offsets and playback acknowledgements. | Adapt to `ConversationEngine`; separate provider completion from playback completion. |
| [`AsyncSession`](../include/byteturn/session.h) / [`SessionExecutor`](../include/byteturn/executor.h) | Serialize mutable agent history per session; provide bounded pending work and cancellation. | Retain as a pipeline implementation detail, not as the lifetime of all interaction. |
| [`Agent`](../include/byteturn/agent.h) / [`ToolRegistry`](../include/byteturn/tool.h) | Concrete bounded LLM/tool loop, history, and named tool dispatch. | General asynchronous agent/delegation contracts and tool authorization. |
| [`RuntimeObserver`](../include/byteturn/observability.h) / [`CurlHttpTransport`](../include/byteturn/curl_transport.h) | Metrics, structured logging, and HTTP transport mechanics. | Unified event coverage, physical playback-stop metrics, and load/failure validation. |

### Important limits of the first refactor

The current timeline stores an event **before** `EventBus::publish` assigns its
sequence and possibly its timestamp. Stored and delivered metadata can differ.
Concurrent appenders can also be published in a different order from vector
insertion. The vector is unbounded, and publishing directly to the exposed bus
bypasses the timeline. It is therefore an inspection scaffold, **not yet a
canonical event journal or a durable source of truth**.

The pipeline adapter's bridge sees events emitted by `Conversation`. The
externally constructed `Agent` and `AsyncSession` are not automatically wired
into that bridge. Model/tool events and the `reasoning` snapshot can therefore
be incomplete. Its `handle_event` is currently a no-op.

`ConversationSession` checks its flags before calling the engine, but does not
protect an admitted call from racing `stop()`. `start()` calls external engine
code while holding the lifecycle mutex. Failure recovery and teardown require
further work. Until that work lands, callers must serialize lifecycle and input
calls, avoid lifecycle re-entry from callbacks, and keep borrowed dependencies
alive through shutdown. This is not a claim of general thread safety.

The overlap test uses a test engine. It proves that the new session abstraction
can represent simultaneous user/agent activity; it does not establish full-duplex
behavior or cancellation correctness for the real pipeline adapter.

## 3. Target runtime layout

The following is the **target**, not the current directory structure:

```text
Application / capture / player / RTC transport
                     |
          +----------v-----------------------------------------+
          | Interaction Runtime                               |
          | ConversationSession                               |
          |   canonical event ingress -> timeline -> state    |
          |   ConversationController / output arbitration     |
          |   ConversationEngine                              |
          |      + PipelineConversationEngine                 |
          |      + NativeDuplexConversationEngine (planned)    |
          +------------------------+--------------------------+
                                   | delegation / progress / results
          +------------------------v--------------------------+
          | Agent Runtime (planned)                           |
          | Custom agent / local LLM loop / remote agent       |
          | Tasks / context snapshots / cancellation / budget |
          +------------------------+--------------------------+
                                   |
          +------------------------v--------------------------+
          | Tool Runtime (planned)                            |
          | Native / HTTP / MCP adapters / application tools   |
          | Validation / authorization / approval / audit      |
          +---------------------------------------------------+

Providers attach where used: ASR/TTS/S2S to engines, LLMs to agents,
and external APIs to tools. They are not a mandatory serial layer.

Cross-cutting: scheduling, bounded queues, transport, metrics,
tracing, privacy, recording, replay, and evaluation.
```

### Execution paths

The **media path** moves timestamped audio through bounded queues and the
player. Audio ownership, format, channel count, and sample offsets are explicit.
The timeline holds metadata or optional recording references, not an unlimited
copy of every PCM buffer.

The **control path** orders admitted session observations and commands. A
per-session serialized reducer owns interaction state. It must not wait for
LLM inference, tool I/O, TTS completion, or slow observers. A serialized control
lane does not serialize the duration of all session work.

The **agent path** runs independently. Its progress and results return as
correlated observations. Mutable history may still require serialization within
one agent, while unrelated tasks or agents can execute concurrently.

## 4. Session and engine contract

### Existing interface

The public engine interface currently has this shape; the included types are
defined in [`conversation_engine.h`](../include/byteturn/conversation_engine.h):

```cpp
class ConversationEngine {
 public:
  virtual ~ConversationEngine() = default;
  virtual void start(ConversationEngineContext context) = 0;
  virtual bool push_audio(AudioFrame frame) = 0;
  virtual void handle_event(const Event& event) = 0;
  virtual void stop() = 0;
  virtual ConversationCapabilities capabilities() const = 0;
  virtual ConversationStateSnapshot state() const = 0;
};
```

### Required lifecycle semantics (target)

Use an explicit lifecycle: `Created → Starting → Running → Stopping → Stopped`,
with a failure path that still performs cleanup. Shutdown is idempotent; a
stopped session cannot silently restart. Startup failure releases subscriptions,
workers, provider handles, and partial state before reporting failure.

An input operation must either be admitted under a live session generation or
be rejected. Shutdown closes admission before cancelling and draining admitted
operations. It must not race destruction of resources used by an accepted call.
Do not hold a session mutex while invoking engine/provider/application code or
waiting for callbacks. Reentrant stop requests must be deferred to the control
lane rather than joining the calling worker.

The session owns the engine and timeline. Providers and legacy `AsyncSession`
are currently borrowed dependencies; their lifetime must cover all workers and
callbacks. `stop()` must quiesce work that can access session-owned memory.
External services may ignore cancellation, so use transport deadlines and
late-result fencing rather than detached threads with borrowed references.

### Capabilities and control ownership (target)

Distinguish three questions: can audio be sent and received concurrently, can
the backend interpret overlapping speech, and who controls interruption/EOU?
A bidirectional connection alone is not evidence of conversational full duplex.

The current capability struct contains booleans for native duplex,
simultaneous listen/speak, interruption, EOU, backchannel, and transcripts.
Extend it with negotiated audio formats, playback acknowledgement support,
delegation support, and policy ownership only as implementations require them.
Capability declaration and configured policy are not the same thing.

For each policy choose one owner: provider, runtime, or application. A provider
that owns turn-taking must not also receive duplicate runtime-generated commit
or cancellation commands. Unsupported required capabilities must fail setup or
select an explicit degraded mode; they must not be silently advertised.

## 5. Events, timeline, and interaction state

### Canonical admission (target)

```text
provider / media / task observation
                 |
       validate session + generation
                 |
       normalize semantic event
                 |
       assign sequence and receive time once
                 |
       append journal + reduce state
                 |
       ordered, bounded observer delivery
```

Each session has its own increasing sequence. Sequence defines local admission
order, not global physical causality. Preserve source timestamps separately;
remote clocks need an explicit mapping before cross-device durations can be
computed. Recorded metadata and subscriber metadata must be identical.

The target envelope includes schema version, session/event identity, local
sequence and receive timestamp, optional source time, origin, trace/cause IDs,
and optional participant, response, task, delegation, and legacy turn IDs.
A native response ID must not be treated as a user turn ID. Typed payloads and
versioned adapter metadata replace inference from arbitrary `name` strings.

Commands such as `CancelOutput`, `CancelTask`, and `CommitInput` are requested
actions. Observations such as `OutputStopped`, `TaskCancelled`, and
`InputCommitted` report outcomes. Record acceptance and outcome separately;
replaying an observation must not execute a live external side effect.

| Semantic family (target) | Existing events to map during migration |
|---|---|
| Session lifecycle | `RealtimeSessionStarted`, `RealtimeSessionClosed`; generic session events are still needed. |
| User speech and transcript | `InputSpeechStarted`, `InputSpeechEnded`, `TranscriptPartial`, `TranscriptFinal`. |
| Response/content generation | `TurnStarted`, `ModelTextDelta`, `ModelCompleted`; pipeline-specific events remain diagnostics. |
| Output generation and playback | `SpeechStarted`, `AudioOutput`, `SpeechCompleted`, `PlaybackStarted`, `PlaybackProgress`. |
| Interruption and output cancellation | `BargeInDetected`, `OutputCancelled`, `TurnCancelled`; cancellation scope needs normalization. |
| Delegation/task/progress/artifact | Planned; not supplied by the current event vocabulary. |
| Overlap/backchannel/policy decision | Planned; derive from explicit inputs and policy decisions, not every audio frame. |

`TranscriptPartial` is not automatically an append-only delta; an ASR adapter
must declare whether it emits replacement hypotheses or incremental text.
Similarly, legacy `SpeechStarted` marks generation/synthesis activity, not proof
that audio became audible. Do not rename events without preserving that meaning.

### Orthogonal state

The existing snapshot permits `user_speaking && agent_speaking`. The target
reducer tracks participant activity, active response IDs, playback state, and a
set of running tasks independently. UI booleans are projections of those sets,
not the authoritative representation of multiple tasks.

Generation, audible playback, reasoning, and floor ownership are independent.
A user backchannel need not cancel speech; an overlap is an observation, not a
policy decision. `LogicalTurn` becomes an optional analytical projection over
these events, without owning execution or forcing one response per utterance.

### Bounds, subscribers, and privacy

Use bounded in-memory retention with explicit eviction and gap metadata.
Snapshots must remain safe copies or immutable views with owned lifetime, not
unprotected spans over a mutating vector. Durable recording is a separate,
opt-in sink with its own retention and failure policy.

Do not run slow loggers on the media/control path. Isolate observer exceptions,
bound notification queues, and expose dropped-observation counters. A full
control queue must reject/degrade explicitly rather than silently lose a cancel
command. Unsubscription and recursive callback behavior need regression tests.

Payload-free logging does not make the current timeline payload-free: it stores
`Event::data` copies. Minimize transcript/tool content, configure retention,
restrict access, and keep credentials out of all event envelopes. Audio and
sensitive payload recording require explicit opt-in and deletion controls.

## 6. Engine paths and interruption

### Pipeline migration

```text
input audio -> bounded ASR worker -> final transcript
           -> AsyncSession / legacy Agent / SessionExecutor
           -> streaming text -> sentence segmenter
           -> bounded incremental TTS worker -> output sink -> player
```

Keep existing streaming, deadline, history rollback, and overload behavior
covered while moving ownership. `SessionExecutor` can continue serializing one
agent's mutable history; it must not stop microphone ingestion or control events.

Route model/tool and conversation events through one canonical ingress without
publishing them twice. Remove the current `push_audio()` rule that interrupts on
any frame while speaking. Require an explicit speech/policy signal instead.
A provider without overlapping-speech support must expose that limitation even
when RealFlow can continue buffering or forwarding input.

### Native duplex adapter (planned)

Wrap `FullDuplexConversation` as `NativeDuplexConversationEngine` and reuse its
continuous input, response IDs, cancellation, and playback acknowledgements.
Do not invent internal ASR, text-LLM, or TTS stages for a native S2S provider.
Phase metrics that the provider does not expose are unavailable, not zero.

Keep three states distinct: provider generation complete, output queued, and
playback complete. A response can finish generating while its audio is still
playing and remains interruptible. Audio offsets must have a documented unit
(prefer sample frames per channel), sample rate, channel count, and response ID.
Reject late audio for a cancelled generation even after reconnection.

No named model is supported merely because this abstraction exists. Each vendor
adapter requires its own protocol implementation, capability declaration, and
conformance tests against the API version it uses.

### Interruption protocol (target)

On confirmed interruption, the controller invalidates the output generation,
requests player flush/mute, and sends provider cancellation/truncation using the
last acknowledged audible position. Stale callbacks and audio are fenced by
session/response generation. Device acknowledgement determines when output
actually stopped; sending cancellation is not proof of silence.

Separately decide whether agent work is cancelled, superseded, or allowed to
continue. Results from obsolete context may be retained for audit but must not
speak or modify the new conversational context without reconciliation. An
already committed external action needs status handling or compensation, not
fictional rollback. Local history rollback does not undo a tool side effect.

## 7. Custom agents, delegation, tools, and context (planned)

These are design boundaries, not implemented public classes. Today's `Agent`
is the concrete LLM/tool loop, not yet an arbitrary-agent interface.

A custom-agent invocation takes structured content, task/delegation identity,
an immutable context snapshot and version, deadline, cancellation token,
resource budget, and authorized tool scope. It returns a task handle and emits
an asynchronous stream: accepted, progress, content delta, tool activity,
artifact/result, and exactly one terminal outcome.

The runtime should support a deterministic local agent, the existing LLM loop,
and a remote agent through the same boundary. Framework integrations are
adapters; the core does not import their graph or vendor SDK types. Transport
retries need event IDs/deduplication, not a claim of exactly-once network delivery.
Do not require access to private model reasoning. Only provider-exposed metadata
and explicitly public progress belong in user-facing events or logs.

```text
ConversationController -> Delegation -> AgentTask
       |                                  + ModelTask
       |                                  + ToolTask
       + acknowledgement/progress         + optional child tasks
       <---------------- correlated result / failure / cancellation
```

Delegation policy controls whether to remain silent, acknowledge once, or expose
meaningful progress. The conversation controller arbitrates speech. Agents do
not write directly to a shared speaker. Acknowledgement can shorten silence,
but does not shorten task completion or justify repetitive filler.

Cancellation, deadline, priority, and budget propagate through the task tree.
Child deadlines cannot exceed their parent's. Cancellation request and terminal
cancellation are distinct; a non-cooperative service's result must still be
fenced. Admission control and accounting enforce budgets rather than relying
only on prompt instructions.

Tool execution separates schema validation, authorization, user approval where
needed, execution, and audit. Treat tool descriptions/results as untrusted data.
MCP is an optional adapter, not the authority model or a core dependency. Retry
only under an explicit idempotency policy; reserve identifiers for deduplication
of externally visible actions. Arbitrary native plugin code is not sandboxed by
an interface and should run out of process when untrusted.

`ContextBuilder` derives a task-specific view from committed interaction events,
relevant tool results, application state, and permitted memory. Keep session
state, agent history, task-local data, and durable memory separate. An agent
receives a snapshot, not unrestricted mutable ownership of the timeline.
Cancelled/unheard output must be distinguishable from what the user heard.
Memory persistence is opt-in and requires provenance, retention, and deletion.

## 8. Observability, transport, and replay

### Metrics contract

Preserve existing `byteturn_*` metrics during an explicit naming migration.
Use the legacy provider and stage events for their existing diagnostics; unify
their coverage before treating session-wide dashboards as complete.

| Measurement | Required endpoints |
|---|---|
| Provider first packet / total | Request start → first response byte / request completion; retain DNS/connect/TLS breakdowns where available. |
| ASR final latency | Declared input EOU boundary → final transcript. |
| LLM TTFT / total | Model request start → first text token / request completion. |
| TTS first audio / total | Synthesis start → first generated frame / synthesis completion. |
| S2S first audio | Declared user speech-end boundary → first generated response audio. |
| First audible response | Same speech-end boundary → player acknowledgement of audible output. |
| Cancellation dispatch | Interruption decision → cancellation request accepted/dispatched. |
| Physical interruption stop | Interruption decision → player-confirmed flush/silence boundary. |
| Delegation duration | Accepted delegation → terminal outcome, labelled by outcome. |
| Time to useful answer | User endpoint → first audible substantive result; distinguish acknowledgement. |
| Overlap / backchannel | Explicit participant/playback intervals and declared policy labels. |

Some stage/playback-start metrics exist; the complete normalized metric set is
a target. In particular, the current `OutputCancelled` event does not prove
physical playback stopped. Publish unknown endpoints as unavailable. Do not add
pipeline component latencies to claim an end-to-end result when stages overlap.

Report p50/p95/p99 with hardware, concurrency, network conditions, audio format,
provider configuration, queue bounds, and cancelled/failed sample counts. Cost,
CPU, memory, queue occupancy, event loss, and correctness accompany latency.
Session/task IDs belong in traces, not unbounded metric labels.

A proposed delegation-hidden ratio may be explored as a diagnostic, but is not
a release KPI. Measure acknowledgement latency, meaningful-answer latency, and
task success separately so filler speech cannot improve the score artificially.

### Network boundary

Preserve `CurlHttpTransport` ownership of TLS verification, deadlines,
cancellation, response limits, `Retry-After`, and DNS/connect/TLS/first-byte/total
request timings. Protocol serialization stays in adapters. Existing streaming retry policy must continue to avoid restarting a
response after body delivery, which could duplicate already-consumed text/audio.
Even a request retried before body delivery may duplicate backend work or cost.
Tool-side retries require a stronger idempotency contract than inference retries.

WebSocket/WebRTC/RTC transports are integration work, not implied by the HTTP/SSE
implementation. Reconnection and engine fallback require explicit context,
capability, pending-output, and task reconciliation. Switching providers is not
a transparent continuation of identical native model state.

### Replay and evaluation (target)

Introduce fake engines, fake clocks, scripted providers, and recorded adapter
outcomes early. Deterministic replay applies to the control reducer and recorded
inputs, not to rerunning arbitrary live models or external tools. Replay must
disable real side effects. Policy versions, event schemas, clock mappings, and
recording gaps belong in the replay manifest.

Persisted sessions can later become regression fixtures for interruption,
late-results, overload, reconnects, and utterance policies. Collection requires
a privacy/consent policy; a metrics log alone is not a complete replay record.

## 9. Migration and acceptance

The next implementation sequence is deliberately narrower than the target:

1. Harden canonical timeline admission, lifecycle ownership, and callback tests.
2. Complete pipeline event wiring and explicit speech/interruption controls.
3. Bring native duplex behind the same engine contract and conformance suite.
4. Introduce the minimal custom-agent/delegation boundary, then tool policy and
   context adapters; preserve the existing agent as a compatibility backend.

Do not remove `Conversation`, `AsyncSession`, legacy turn IDs, or metric names
until their replacement path has equivalent behavior and tests. Keep mechanical
namespace/target renames separate from behavioral changes. Preserve license and
attribution notices during renaming.

Acceptance requires the existing pipeline regressions plus real-adapter overlap,
start/stop races, startup failure, stale callbacks/results, bounded queues,
ordered timeline metadata, and playback-aware cancellation tests. Run `make test`
and the canonical CMake/CTest build; add sanitizer runs for ownership/concurrency
changes and a transport/player integration test for physical stop latency.
A mock overlap test alone is not the completion gate.

See [roadmap.md](roadmap.md) for milestone dependencies and the immediate work
queue. This design does not commit RealFlow to a model release, pricing claim,
or vendor benchmark.
