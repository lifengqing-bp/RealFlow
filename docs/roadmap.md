# RealFlow Roadmap

**RealFlow — Realtime Human-Agent Interaction Runtime**

Make continuous human interaction independent of asynchronous agent reasoning and
execution. This is a dependency-driven roadmap, not a calendar promise. A milestone
is complete only when its acceptance gates are demonstrated.

Baseline: `ee95f2f`, merged PR #3 (M2.1). Contracts are in
[architecture.md](architecture.md); M2.1 implementation details and limitations are
in [runtime-foundation.md](runtime-foundation.md). Planned APIs are not existing APIs.

## 1. Status and priorities

| Foundation | Status |
|---|---|
| RealFlow identity | Partial: CMake project renamed; `byteturn` paths, namespace, targets and metrics remain. |
| Streaming cascade and HTTP/SSE | Present; full event coverage and production validation remain open. |
| Session/timeline foundation | M2.1 landed: canonical metadata, bounded observation queues and lifecycle safeguards. |
| Real pipeline integration | Fixture exists; complete model/tool events and interruption policy remain open. |
| Runtime registry/admission/supervisor | Design in this revision; next implementation slice. |
| Native realtime path | `FullDuplexConversation` exists outside the unified engine boundary. |
| Custom agents, safe tools, context | Designed; concrete current Agent/ToolRegistry are narrower. |
| Durable replay, recovery, orchestration | Planned. |

“Present” is not a production-readiness claim. M2.1's local validation is not a
substitute for CI or high-concurrency/live-provider testing. Runtime ownership now
precedes additional adapters: safe individual sessions are insufficient without
bounded admission and reliable retirement across the whole process.

## 2. Public milestones

```text
M0 Identity / reproducible baseline
  -> M1 Stream / preserve streaming behavior
  -> M2 Converse / runtime ownership + unified continuous sessions
  -> M3 Delegate / custom agents, tools and context
  -> M4 Operate / measured resilience, recovery and replay
```

Safety, bounded queues, diagnostics and regression fixtures run through every
milestone. Do not postpone basic admission, privacy or failure handling until M4.

## 3. M0 — Identity and baseline

Finish ByteTurn → RealFlow naming in a separate compatibility change: namespace,
headers, targets, executable names, documentation and metric migration policy.
Preserve license and attribution notices. Publish reproducible Make and CMake builds
and executable examples that use the actual API.

**Gate:** both build paths and existing tests pass; mechanical renames do not change
behavior; breaking names and compatibility paths are documented.

## 4. M1 — Stream

Preserve bounded ASR input, FIFO agent history execution, streaming text, sentence
segmentation, incremental TTS, deadlines and cancellation. Keep HTTP TLS, retry,
response-size and SSE parsing behavior covered. Never retry already-delivered speech.
Test cancellation, overload, failed synthesis, fragmented SSE and local history
rollback without claiming rollback of external tool side effects.

**Gate:** a controlled fixture delivers audio before full text completion; bounded
queues and deadlines work; no stale output after cancellation; stage metrics retain
explicit endpoints. Report hardware/load conditions rather than claiming production
quality from a demo.

## 5. M2 — Converse

One continuous session contract for pipeline and native speech engines, owned by a
bounded runtime rather than independent ad hoc objects in each transport.

### M2.1 — Session/timeline foundation (landed, PR #3)

Canonical sequence/receive metadata is assigned before storage and notifications.
Retention, event sizes and asynchronous notification queues are bounded. Lifecycle
states, admitted-operation leases, partial-start cleanup and callback-safe stop
requests are implemented. Weak event sinks fence destroyed sessions.

**Evidence:** the PR records regression/integration and foundation tests plus local
sanitizer/build results. These are not live-provider or production-load results.
**Remaining:** `handle_event()` is not an authoritative serialized command queue;
notification loss is possible; providers/observers must cooperate with shutdown;
per-session threads have not been capacity-tested. Do not mark all of M2 complete.

### M2.2 — Runtime Manager, registry and Session Supervisor (next)

First implement a process-local ownership layer around existing sessions. Registry
and supervisor can be internal roles of `RuntimeManager`, not separate services.

**Admission and registry:** atomically reserve an ID/incarnation and capacity before
invoking a factory. Bound total reservations, queued starts and ID length. Count
starting/stopping/retiring entries against capacity. Return explicit rejection
reasons and separate startup/retirement tickets. Use handles scoped to runtime and
incarnation; stale callbacks cannot remove or fail a reused ID. Do not expose owning
session pointers.

**Remove and lifetime:** close entry admission immediately; stop cooperatively;
wait for admitted operations, session callbacks and dependency teardown before
releasing capacity. Handle removal before/during startup and shutdown races. Own
session-specific dependencies in a lifetime bundle. Binary-owned shared services
outlive manager shutdown. No joins/destruction inside registry locks or callbacks.

**Notify queue:** bound runtime notifications independently of per-session timelines.
Use ordered sequences, explicit drops and observer-failure counters. Publish identity,
phase and reason only. Registry mutation and definitive completion tickets are not
best-effort; a full notification queue must not lose removal or failure accounting.

**Supervisor:** reconcile authoritative lifecycle state and explicit fatal reports,
not only lifecycle notifications. Isolate factory/start/operation failures to one
session, clean up and retire it. Initial policy: stop-and-retire, **no auto-restart**.
Keep hung resources charged; bounded waits do not imply resources were destroyed.
Uncaught native thread exceptions, crashes and forced process exit need external
supervision. Do not promise hard cancellation or watchdog coverage in this slice.

**Binary shutdown:** `Running -> Draining -> Stopped`; fence global ingress, stop
reservations, reap sessions, finish notifications and join workers before releasing
shared services. Provide non-waiting callback requests and owner-thread waits.
Signal integration stays in the executable, not process-global library handlers.

**Acceptance gate:** deterministic tests cover concurrent duplicate/capacity adds,
full startup queue, factory/start failure rollback, remove during startup, operation
leases, stale handles/ID reuse, queue overflow and observer exceptions/re-entry,
independent-session failure, terminal state without notifications, callback draining,
and repeated/racing shutdown. A small executable demonstrates binary-owned lifetime.
All existing tests pass; sanitizer results are recorded without inventing coverage.

**Scope boundary:** the first slice is fixed-capacity, in-process, stop-and-retire.
Per-tenant quotas, shared scheduling, supervisor deadlines/watchdogs, process workers,
restart budgets, persistent registry and distributed leases are M4 follow-ups.

### M2.3 — Complete pipeline event/control integration

Wire conversation/model/tool events into one ingress without duplicates. Make state
a normalized projection and implement explicit commands. Replace interruption on
arbitrary audio frames with speech/policy signals; input remains live during output.
Keep the history executor internal rather than a global interaction controller.

**Gate:** the real pipeline through `ConversationSession` has complete metrics,
intentional cancellation, silence that does not interrupt, and preserved streaming,
deadline and stale-output behavior. This depends on M2.1, with M2.2 hosting examples.

### M2.4 — Native duplex and playback unification

Add the native engine adapter and a shared conformance suite. Separate generated,
queued, playing, stopped and completed output. Generation completion must not make
still-playing audio uninterruptible. Fence response/session generations; define
sample-frame offsets, formats and playback acknowledgement. Negotiate one owner
for EOU/interruption and reject unsupported required capabilities.

**Gate:** both real engine adapters pass lifecycle/input/cancellation tests, including
input during output, late packets, provider completion before playback completion,
format/offset checks and capability mismatch. A player fixture confirms flush; a
cancel return does not prove silence. No model-name-only integration claims.

### M2.5 — Interaction metrics and policy fixtures

Separate first generated audio, first audible acknowledgement, useful answer,
cancellation dispatch and physical stop. Add overlap/backchannel outcomes and small
noise/correction/EOU regression fixtures. Include runtime admission and retirement
measurements; derive SLOs from evidence rather than universal guessed numbers.

**M2 demonstration:** one binary safely hosts multiple managed sessions, selects
pipeline or native engine behind one API, keeps microphone input live during output,
and interrupts deliberately without stale output, resource leaks or lost retirement.

## 6. M3 — Delegate

**M3.1:** minimal structured asynchronous agent/delegation API, task identity,
immutable context versions, deadline/cancellation/budget propagation and one terminal
outcome. Adapt the existing loop, deterministic local logic and remote agents.
Conversation policy owns speech; acknowledgements do not replace meaningful results.

**M3.2:** tools separate validation, authorization, approval, invocation and audit.
Start with native/HTTP, then MCP as an adapter. Explicit idempotency rules protect
side effects; untrusted native code requires isolation. Never repeat an action merely
because a session or agent restarted.

**M3.3:** replaceable ContextBuilder with bounded task views; separate conversation,
agent history, task data and opt-in persistent memory. Preserve provenance, consent,
retention/deletion and heard-versus-unheard output semantics.

**Gate:** a slow task does not block interaction; user correction suppresses obsolete
results; terminal events/deadlines are tested; unauthorized tools cannot execute;
stopping speech cannot silently undo/repeat actions; local/remote adapters use the
same boundary. Private model reasoning is not required as progress data.

## 7. M4 — Operate

Build on M2.2 admission/supervision, rather than introducing lifecycle ownership here.
Add per-tenant/provider budgets, fairness, rate limits, cost accounting and circuit
breakers after measuring the initial runtime. Reduce per-session thread cost through
shared scheduling only while preserving the lifetime and admission contracts.

Add startup/stop/idle watchdog policies with observable deadlines and escalation.
Do not free live resources after timeouts. Test controlled process-worker termination
and external host supervision. Add capped restart/backoff with fresh incarnations,
context reconciliation and side-effect deduplication before advertising recovery.
Keep transparent migration and provider-state continuity deferred until supportable.

Extend fixtures into opt-in recording and deterministic control replay. Record schema,
policy versions, clock mappings, outcomes and gaps; disable live side effects.
Promote production failures into reviewed regression fixtures with privacy controls.

**Gate:** sustained declared load stays within bounds; fault injection yields explicit
outcomes; hard failures are isolated at the right process boundary; representative
incidents replay; recovery does not repeat actions or speech. Report p50/p95/p99,
CPU, memory, cost, admission loss and failed/cancelled samples with environment details.

## 8. Immediate implementation queue

| Order | Change | Required proof |
|---|---|---|
| Done | M2.1 session/timeline foundation. | Existing PR #3 evidence; continue regression coverage. |
| 1 | M2.2 runtime registry, admission, notify queue and stop-and-retire supervisor. | Capacity never releases early; failures and dropped notifications cannot orphan sessions. |
| 2 | Complete pipeline events and explicit controls. | Full model/tool/voice integration and input during output. |
| 3 | Native duplex/playback adapter. | Shared conformance, late-packet fencing and player-confirmed stop. |
| 4 | Minimal custom-agent delegation. | Slow task, correction and obsolete-result suppression. |

Review the runtime design first, then publish the bounded implementation separately.
Keep mechanical naming changes separate. Registry ownership is foundational, not an
excuse to implement every deployment/orchestration feature in the next PR.

## 9. Release discipline and later work

Every slice includes tests, an executable/reference path, API/migration notes and
explicit limitations. Run Make and CMake/CTest; use sanitizers for lifetime changes.
A checked-in test is not evidence it passed. Device latency needs transport/player
evidence; repository tests cannot certify a paid provider or production scale.

Later integrations are demand-led: RTC transports, verified native providers,
framework adapters and bounded sub-agent/handoff workflows. Defer a graph DSL,
autonomous swarms, plugin marketplace, model training and transparent cross-provider
migration. New multimodal media should extend timestamp/participant contracts rather
than redefine runtime/session lifetime.
