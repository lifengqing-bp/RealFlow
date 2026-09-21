# RealFlow Architecture

**RealFlow — Realtime Human-Agent Interaction Runtime**

Human interaction is a continuous realtime stream. Agent reasoning and tool
execution are asynchronous work. RealFlow connects them without making an entire
session follow a sequential listen → think → speak loop.

The implementation baseline is `ee95f2f` (merged PR #3, M2.1). **Current** means
source exists, not production readiness. **Target** describes contracts still to
implement. See [roadmap.md](roadmap.md) for delivery gates and
[runtime-foundation.md](runtime-foundation.md) for the current API and its limits.

## 1. Architectural decisions

| Decision | Consequence |
|---|---|
| The application binary owns a Runtime Manager. | A session cannot own the process, shared executor, or the runtime that supervises it. |
| A session owns one interaction lifetime. | Turns remain compatibility/analytical views, not gates on microphone input or unrelated work. |
| Conversation and agent execution are independent. | Tools can run while human interaction continues. |
| Registry state is authoritative for membership. | Cleanup never depends on a best-effort terminal notification reaching a subscriber. |
| Commands and observations are different. | Replaying an observation must not execute a tool or cancel a new session. |
| Media, control, task, and notification paths are separate. | Slow inference or observers must not run on the capture callback. |
| Cancellation is scoped. | Stopping speech neither undoes nor necessarily cancels an external action. |
| Capabilities select behavior. | Provider names must not determine core session policy. |
| Measurements have explicit endpoints. | Generated, queued, audible, and stopped audio are different facts. |

The core remains portable C++17. Vendor SDKs and protocols belong in adapters.
RealFlow is not a general planning framework, workflow DSL, database, or model
training system. Integrate those systems through a small asynchronous boundary.

## 2. Current implementation

Names still use `byteturn` for compatibility; only the CMake project is RealFlow.
Do not present `namespace realflow` or unimplemented provider adapters as APIs.

| Component | Current responsibility | Remaining work |
|---|---|---|
| [`ConversationSession`](../include/byteturn/conversation_session.h) | Explicit lifecycle, operation leases, startup cleanup, cooperative shutdown, weak event sinks. | Supervision above sessions; typed authoritative commands; runtime failure reporting. |
| [`EventTimeline`](../include/byteturn/event_timeline.h) | Canonical per-session metadata, bounded retention and asynchronous observation delivery. | Durable recording, schema evolution, authoritative reducer integration. |
| [`ConversationEngine`](../include/byteturn/conversation_engine.h) | Engine lifecycle, input, capability and orthogonal state contracts. | Complete command and capability negotiation. |
| [`PipelineConversationEngine`](../src/pipeline_conversation_engine.cpp) | Wraps the existing cascade and its conversation event bridge. | Complete model/tool events, explicit interruption controls, authoritative state. |
| [`Conversation`](../src/conversation.cpp) | Bounded ASR worker, agent submission, sentence segmentation and incremental TTS. | Remove arbitrary-frame interruption and exclusive internal state assumptions. |
| [`FullDuplexConversation`](../src/full_duplex_conversation.cpp) | Native concurrent I/O, response/sample offsets, playback-aware cancellation. | Unified engine adapter; generation versus playback completion. |
| [`AsyncSession`](../include/byteturn/session.h) / [`SessionExecutor`](../include/byteturn/executor.h) | Bounded FIFO work and mutable history serialization. | Remain a pipeline detail, not global interaction lifetime. |
| [`Agent`](../include/byteturn/agent.h) / [`ToolRegistry`](../include/byteturn/tool.h) | Concrete LLM/tool loop and named dispatch. | General agents, delegation, authorization and context versions. |
| [`RuntimeObserver`](../include/byteturn/observability.h) | Stage metrics and payload-redacted logging. | Unified event coverage and physical playback-stop measurements. |

M2.1 fixed storage/publication metadata disagreement and added lifecycle tests.
Timeline notifications remain best effort; direct publication to its legacy bus
bypasses the journal. `handle_event()` is a compatibility hook, not a serialized
command queue. Pipeline model/tool events are not automatically connected by
constructing the wrapper. A fake overlap test proves representation, not actual
full-duplex policy or production readiness. Per-session lifecycle and notification
threads remain; high-concurrency efficiency has not been established.

## 3. Ownership and execution layout (target)

```text
Application binary / service host
  ├── signal integration, readiness, process exit policy
  ├── binary-owned shared transports / provider clients / executors
  └── RuntimeManager
        ├── SessionRegistry: reservations, identities, ownership, limits
        ├── SessionSupervisor: authoritative outcomes, stop/reap policy
        ├── bounded startup work and runtime notification queues
        └── managed session resources
              ├── dependency lifetime bundle
              └── ConversationSession
                    ├── EventTimeline / control reducer
                    ├── ConversationController / output arbitration
                    └── ConversationEngine
                          ├── PipelineConversationEngine
                          └── NativeDuplexConversationEngine (planned)
                                   │ delegation / results
                              Agent Runtime (planned)
                                   │ authorized calls
                              Tool Runtime (planned)
```

Registry and supervisor are roles, initially implemented inside one
`RuntimeManager`; they do not each require a new service or thread. Providers
attach where used: ASR/TTS/S2S at engines, LLMs at agents, external APIs at tools.

The media path carries bounded, owned audio buffers. The control path orders
commands and reduces interaction state without awaiting task I/O. The agent path
runs asynchronous work. Notification paths expose facts without owning those
operations. A serialized control lane does not serialize the lifetime of tasks.

## 4. Runtime Manager and Session Supervisor (target)

### Binary lifecycle ownership

The executable owns the Runtime Manager and outlives every callback it installs.
The manager owns session resources until teardown is complete; transports retain
opaque handles, not owning session pointers. A handle includes runtime-instance
identity, session ID, and a monotonically increasing incarnation. Reusing a human
session ID must never make a late remove/failure/audio callback target its successor.
Handles are process-local; distributed resume requires a separate epoch protocol.

Session-specific providers, agents and executors may be held in a dependency
bundle. Destroy the session/engine and drain its callbacks **before** releasing
that bundle. Binary-owned borrowed dependencies must outlive manager shutdown.
Factories must return these lifetime dependencies rather than leaving borrowed
objects on their stack. Never destroy arbitrary factory captures under a registry
lock. No owning session pointer escapes admission or lookup APIs.

The first implementation may use a fixed startup worker pool, one supervisory
scan lane and one runtime notification dispatcher. This does not replace M2.1's
per-session threads. Declare all thread/queue counts and benchmark before claiming
large-scale capacity. No detached cleanup work is allowed.

### Registry and admission

Use one authoritative membership map, protected by a short critical section.
Entries retain identity, phase, terminal reason, admitted-operation count,
resource ownership, and completion tickets. Public snapshots contain metadata,
not mutable session internals.

```text
Absent -> Reserved -> Starting -> Running -> StopRequested -> Retiring -> Absent
                 \ startup failure/cancel -> StopRequested -----------/
```

`max_sessions` covers **all reserved entries**, including starting, failed-but-not-
reaped, stopping and retiring sessions. Bound pending starts and session ID size
separately. Limits are hard admission bounds, not promises about provider cost.
Per-tenant/provider quotas and byte-accurate accounting follow later.

`add(id, factory)` first validates, reserves capacity and an incarnation atomically,
then schedules construction/start outside the registry lock. Reject duplicate IDs,
full capacity, full startup queue and a draining runtime explicitly. Rejection must
not invoke the factory. Acceptance is not startup success: return separate startup
and retirement completion tickets. A failed start releases resources and resolves
both outcomes; failure does not leak capacity.

`remove(handle)` is idempotent and non-waiting: close that entry's input admission,
record the first applicable reason and request cooperative stop. It is not an
immediate map erase. Removal while reserved may skip the factory; removal during
construction/start must fence the result and clean up anything already created.
Do not release the slot until admitted calls, engine stop, notification callbacks,
and dependency teardown are quiescent. Only then resolve retirement and allow ID
reuse. A retirement ticket remains valid after the map entry disappears.

Lookup/input use an internal operation lease. A call admitted before removal can
finish; calls admitted after the removal linearization point are rejected. Do not
hold the registry lock while calling engines, factories, observers or destructors.
A capacity limit therefore cannot be bypassed by retaining public `shared_ptr`s.

### Notify queues are not control queues

| Path | Authority and overload contract |
|---|---|
| Registry mutation / remove / failure report | Direct validated state transition; never relies on observer delivery or a free startup queue slot. |
| Startup queue | Bounded; reject a new add before acceptance when full. |
| Per-session control queue (future) | Explicit command acceptance and outcome; never silently drop accepted cancel commands. |
| Runtime notify queue | Bounded, asynchronous, best effort; drop newest on overflow and expose sequence gaps/counters. |
| Session timeline notify queue | Existing M2.1 bounded observations; independent of runtime membership. |

Runtime notifications contain identity, transition, reason, and sequence, not
transcripts, credentials or tool arguments. Invoke observers outside registry
locks; isolate observer exceptions. Slow observers may lose notifications, but
cannot prevent stop requests, startup failure accounting or authoritative terminal
reconciliation. Use registry snapshots for current membership and completion
tickets for definitive outcomes. A notification flush means enqueued callbacks
finished, not that nothing was dropped. Do not retain an unbounded terminal history.

### Abnormal exit supervision

A notification consumer is not a supervisor. The supervisor reconciles authoritative
session lifecycle/failure state and explicit fatal reports, even when notify queues
are saturated. It initiates cooperative stop, records a structured outcome and
reaps resources. A session failure must not automatically stop unrelated sessions.

| Failure | Required response |
|---|---|
| Factory/startup exception or invalid engine | Fail admission startup; clean partial resources; retire the reservation. |
| Exception from an admitted engine operation | Fail/fence that session and request stop; do not unwind into the media caller. |
| Explicit fatal provider/worker report | Record failure through the control path and stop the matching incarnation. |
| Session ends without a manager removal | Reconcile its authoritative terminal state and retire it. |
| Recoverable provider error observation | Keep it observable; do not classify every `EventType::Error` as session-fatal. |
| Stuck startup/stop/observer | Keep capacity charged and report incomplete shutdown; never free live resources. |
| Process crash, memory corruption, forced kill | External process supervision; no safe in-process recovery promise. |

The initial policy is **stop and retire**, not automatic restart. Later restart
budgets/backoff require a new incarnation, fresh resources, explicit context
reconciliation and no automatic replay of side effects. A timeout is a decision
boundary, not evidence that a C++ thread or remote operation has terminated.
Native workers must catch/report their own exceptions; an uncaught thread exception
is outside the manager's recoverable boundary. Watchdogs and process-isolated
providers are later work, not implied by a registry scan.

### Runtime shutdown

`Running -> Draining -> Stopped`: close global add/input admission, request stop
for all reservations, finish or cancel startup, drain admitted work, reap sessions,
then finish runtime notifications and join manager workers. The host releases shared
services last. Startup racing shutdown must never escape as a new Running session.
`request_shutdown()` is the callback-safe request; blocking shutdown belongs on
an owning thread. A bounded wait may time out while resources stay owned; the
RAII destructor still joins cooperatively. Callback destruction of the manager or
session is forbidden. Destruction is not a forced-cancellation mechanism.

Signal handling belongs in the executable: translate OS signals onto a normal
thread before calling C++ APIs. Do not allocate, lock or join inside an async signal
handler. A deployment supervisor owns any final process termination deadline.
The library neither exits the process nor installs global signal handlers.

## 5. Session, engine and event contracts

The current engine API is in
[`conversation_engine.h`](../include/byteturn/conversation_engine.h).
`start/stop` run on a lifecycle lane; input/state calls hold admission leases.
Engine methods must be bounded/thread-safe; stop must quiesce providers even after
partial startup. Session stop requests are callback-safe, but owner destruction is
not. Session lifetime remains non-restartable.

The target canonical ingress validates session/incarnation, assigns local sequence
and receive time once, records the event, reduces state, then offers a notification.
M2.1 implements canonical journal admission and bounded delivery, not the complete
reducer. Local sequence is not global physical causality; remote source timestamps
need an explicit clock mapping. Durable recording is separate and opt-in.

Commands (`CancelOutput`, `CancelTask`, `CommitInput`) request actions; observations
report acceptance/outcomes. Use typed payloads, schema versions and distinct response,
task/delegation and legacy-turn IDs as adapters migrate. Do not equate a native
response ID with an input utterance. Generic `SessionStarted/Stopped/Failed` already
exist; provider connection events remain separate diagnostics.

Orthogonal state tracks participant, generation, audible playback and task activity.
A user backchannel/overlap is not automatically an interruption. Booleans such as
`user_speaking && agent_speaking` are projections, not a complete multi-task state
model. `LogicalTurn` is a derived view. ASR partials may replace earlier hypotheses;
legacy `SpeechStarted` means synthesis/generation, not necessarily audible speech.

Retain safe copy/owned snapshots and bounded event/notification sizes. Default log
redaction does not remove payloads from the timeline: `Event::data` can contain
sensitive content. Scope access, minimize retention, and require consent/retention/
deletion policies for persistent media or content recording.

## 6. Engines, capabilities and interruption (target)

Complete pipeline conversation/model/tool event wiring without duplicate publication.
Keep existing streaming ASR → agent/LLM → sentence segmentation → incremental TTS.
Replace cancellation on arbitrary input frames with explicit speech/policy controls.
History serialization stays internal; it cannot gate continuous microphone input.

Wrap native realtime I/O behind `NativeDuplexConversationEngine`. Do not invent
internal ASR/LLM/TTS stages for a native S2S provider. Capabilities distinguish
concurrent transport I/O, interpretation of overlapping speech, and policy ownership.
Choose exactly one owner for EOU/interruption: provider, runtime or application.
Reject unsupported requirements or select an explicit degraded mode.

Keep generation complete, output queued and playback complete distinct. Audio remains
interruptible after generation ends. Define offsets as sample frames per channel
with rate, channel count and response/generation identity. Confirmed interruption
invalidates output, requests player flush/mute and provider truncation at the last
acknowledged audible position. Device acknowledgement, not a cancel return, proves
silence. Fence late packets/results after cancellation and reconnection.

No named model is supported by having this interface. Each adapter needs a verified
protocol, declared capabilities and conformance tests. Cross-provider fallback must
reconcile context, tasks and output; native model state is not transparently portable.

## 7. Agents, tools and context (planned)

Custom agents receive structured input, immutable versioned context, task/delegation
identity, deadlines, cancellation, budgets and authorized tool scope. They emit
accepted/progress/content/tool/artifact/result events and one terminal outcome.
Support the existing loop, deterministic local logic and remote agents without
importing framework SDKs into the core. Private model reasoning is not required.

Delegation policy chooses silence, one acknowledgement or meaningful progress; the
conversation controller owns the speaker. Acknowledgement reduces conversational
silence, not task duration. Propagate child budgets/deadlines, fence obsolete results
and deduplicate remote events. Cancellation request is not confirmed termination.

Tools separate validation, authorization, approval, execution and audit. MCP is an
adapter, not the security model. Treat tool content as untrusted; run untrusted native
plugins out of process. Retry side effects only under explicit idempotency policy.
History rollback cannot undo an external action; reconcile or compensate it instead.
`ContextBuilder` supplies scoped snapshots, not mutable whole-session history.
Keep task context, agent history and optional persistent memory distinct. Unheard
assistant audio must not be committed as heard user context.

## 8. Observability, transport and replay

Preserve `byteturn_*` metrics during a separate naming migration. Provider timings
retain DNS/connect/TLS/first-byte/total endpoints. ASR final uses declared EOU → final
transcript; LLM TTFT and TTS first audio start at their respective request boundaries.
S2S first generated audio, first audible acknowledgement, first useful answer,
cancellation dispatch and player-confirmed stop must remain separate measurements.
Unknown endpoints are unavailable, not zero. Do not add overlapping stage latencies.

Runtime metrics add reserved/starting/running/stopping/retiring counts, rejection
reasons, startup/retirement duration, failures, notification depth/high-watermark,
drops and observer failures. IDs belong in traces, not unbounded metric labels.
Define latency/CPU/memory thresholds from declared hardware, concurrency, provider,
network, audio and queue settings. Report failed/cancelled samples. A proposed
“delegation hidden ratio” is diagnostic only; filler speech is not a success KPI.

`CurlHttpTransport` retains TLS verification, cancellation, deadlines, response limits
and retry mechanics; protocol serialization remains in adapters. Never restart a
stream after body delivery; earlier retries can still duplicate work or cost.
WebSocket/WebRTC integrations are not supplied by HTTP/SSE support alone.

Use fake clocks, scripted engines/providers and recorded outcomes early. Determinism
applies to control/reconciliation logic and recorded inputs, not live model calls.
Replay disables side effects and records schema/policy versions, clock mappings and
gaps. Payload/audio persistence is opt-in, bounded and privacy-controlled.

## 9. Migration and acceptance

Next: Runtime Manager admission/ownership/supervision, then complete pipeline controls,
then native duplex/playback unification, followed by minimal custom-agent delegation.
Keep namespace/target renames separate and preserve license/attribution notices.
Do not remove legacy interfaces until their replacements have equivalent tests.

Run `make test`, CMake/CTest, and ownership/concurrency sanitizers where supported.
Validate duplicate/capacity admission, add/remove/shutdown races, ID reuse, failed
startup, missing/dropped notifications, observer re-entry, delayed destruction and
failure isolation. Later engine gates require real-adapter overlap, late output and
player-confirmed stop, not just booleans or a successful CLI connection.
