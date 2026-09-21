# RealFlow Roadmap

**RealFlow — Realtime Human-Agent Interaction Runtime**

> Make continuous human interaction independent of asynchronous agent reasoning
> and execution.

This is a dependency-driven roadmap, not a calendar promise. Milestones are
complete only when their acceptance gates are demonstrated. The implementation
snapshot is based on `e312d8c` (PR #1); the detailed contracts and known gaps are
in [architecture.md](architecture.md).

## 1. Scope and status

RealFlow's focus is realtime interaction: session lifetime, continuous input,
output arbitration, interruption correctness, asynchronous delegation, and
measurable behavior under load. Integrate general agent frameworks rather than
rebuilding their planning engines.

| Foundation | Status at the snapshot |
|---|---|
| RealFlow project identity | Partial: CMake project renamed; `byteturn` paths, namespace, targets, and metrics remain. |
| Streaming ASR–agent–TTS pipeline | Present: bounded workers, streaming text, segmentation, incremental TTS, deadlines, and cancellation. |
| HTTP/SSE and observability | Present; unified event coverage and production validation remain open. |
| Native realtime path | Present as `FullDuplexConversation`; not yet a `ConversationEngine` implementation. |
| Continuous session abstraction | Initial `ConversationSession`, `EventTimeline`, engine interface, and pipeline wrapper are merged. |
| Overlap validation | Test-engine representation test exists; real-engine conformance is still required. |
| Custom agent/delegation/tool runtime | Design only; today's `Agent` and `ToolRegistry` are narrower implementations. |
| Durable replay, advanced resilience, orchestration | Planned. |

“Present” means source exists. It is not a claim that production readiness or all
build, sanitizer, transport, and load-test gates have already passed.

## 2. Public milestones and dependencies

```text
M0 Identity and baseline
           |
M1 Stream — preserve and verify the existing streaming path
           |
M2 Converse — one safe continuous session for pipeline and native duplex
           |
M3 Delegate — custom asynchronous agents, tools, and context
           |
M4 Operate — production resilience, recording, replay, and evaluation
           |
Later: demand-led framework adapters and bounded orchestration

Safety, bounded resources, test fixtures, and observability run through every
milestone. They are not postponed until M4.
```

| Milestone | User-visible outcome | Architectural gate |
|---|---|---|
| M0 — Identity | A consistently named project with a reproducible baseline. | Naming/migration policy and build coverage. |
| M1 — Stream | A reliable, measurable streaming reference path. | Existing behaviors remain covered before further refactoring. |
| M2 — Converse | Pipeline and native duplex share one session contract. | Canonical events, safe lifecycle, explicit interruption, real-engine conformance. |
| M3 — Delegate | Conversation continues while a custom agent performs work. | Structured task events, scoped cancellation, safe tools, versioned context. |
| M4 — Operate | Failures can be diagnosed, reproduced, and handled within declared limits. | Load/failure gates, privacy-aware recording, policy replay, explicit recovery. |

M1 stabilization and M2 work may overlap. The merged abstraction skeleton is
progress toward M2, not evidence that M1 or M2 is complete.

## 3. M0 — Identity and reproducible baseline

**Goal:** complete ByteTurn → RealFlow without mixing mechanical changes with
behavioral refactoring.

Finish the project/package, namespace, include-path, target, binary, example,
and documentation rename in a dedicated change. Define whether old headers and
build targets remain compatibility aliases and document the removal policy.
Metric dashboards need a deliberate migration; do not silently rename exported
series. Preserve `LICENSE-ByteTurn` and other attribution notices as applicable.

**Acceptance gate:** both supported build paths run the same relevant tests;
examples match public names; compatibility behavior and breaking changes are
explicit. Capture a baseline before and after the rename. No undocumented
`realflow` namespace or binary may appear in runnable examples before it exists.

## 4. M1 — Stream

**Goal:** retain a useful streaming ASR → Agent/LLM → sentence segmentation →
TTS implementation throughout the migration.

Preserve bounded ASR/TTS queues, serialized mutable agent history, streaming
text/tool fragments, cooperative cancellation, submission-time deadlines,
provider error handling, and current stage metrics. Verify that CMake and the
Makefile include all new sources/tests. Add a reproducible fake-provider example
that requires neither credentials nor a hosted model.

Retain the HTTP transport's TLS, timeout, cancellation, response-limit, and
streaming retry behavior. Test fragmented SSE, cancellation during streaming,
queue overflow, failed synthesis, and history rollback. History rollback must
not be documented as undoing completed external tool actions.

**Acceptance gate:** the existing regression suite and both build paths pass;
streaming reaches audio before full text completion in a controlled fixture;
queue/time limits are enforced; cancellation produces no stale output; stage
metrics retain documented endpoints. Record load and hardware assumptions rather
than declaring production quality from a CLI demonstration.

## 5. M2 — Converse

**Goal:** a single continuous interaction lifetime across pipeline and native
speech-to-speech engines, without a mandatory turn-based session loop.

### M2.1 — Harden the session and timeline (current priority)

Implement one per-session admission/normalization path. Assign sequence and
receive timestamp once, before storage and delivery. Separate commands from
observations; reject foreign-session or stale-generation input; bound retention
and observer queues. Record gaps, overload, and observer failures explicitly.

Replace the current flag-only lifecycle with defined startup, running, stopping,
and failure transitions. Protect admitted operations against teardown, clean up
partial startup, and remove external callbacks from locked lifecycle sections.
Define callback re-entry and borrowed-provider lifetimes. Add fake-clock and
scripted-engine fixtures here, not only during the later replay milestone.

**Acceptance gate:** snapshots and subscribers agree on normalized metadata and
ordering under concurrent producers; memory/queues stay within declared bounds;
start failure and repeated stop release resources; no accepted call touches
destroyed state; callback re-entry cannot deadlock the session.

### M2.2 — Complete the pipeline adapter

Route conversation, model, and tool observations through the same session
ingress, without duplicate delivery or self-publishing loops. Make state a
projection of normalized events. Implement explicit control handling rather than
leaving `handle_event` as a no-op.

Remove interruption triggered by arbitrary audio frames. Use validated speech
and interruption policy signals; keep input ingestion independent of output.
Demote the legacy exclusive `Listening/Thinking/Speaking` state to internal
compatibility or remove it once covered. `SessionExecutor` remains an agent
history lane, not the global interaction controller.

**Acceptance gate:** the real pipeline runs through `ConversationSession`; its
model/tool/voice events and metrics are complete; silence/input frames alone do
not cancel output; intentional speech and cancellation follow declared policy;
existing streaming and deadline behavior remains covered.

### M2.3 — Unify native duplex and playback control

Add `NativeDuplexConversationEngine` around `FullDuplexConversation`. Reuse its
continuous input and response/sample offsets. Separate generated, queued,
playing, stopped, and completed output. Keep completed-generation audio
interruptible until playback actually finishes. Fence late audio/callbacks by
session and response generation.

Extend capabilities only where needed, including audio formats, policy owner,
and playback reporting. Negotiate provider/runtime/application ownership of EOU
and interruption to prevent duplicate commits or cancellations. Backchannels
and user overlap must be representable without forcing an interruption.

**Acceptance gate:** both engines pass one lifecycle, input, ordering, and
cancellation conformance suite. Tests include input while output is active,
provider completion before playback completion, late packets after cancellation,
format/offset correctness, and capability mismatch. A player/transport fixture
confirms output flush; a cancellation API return alone is insufficient.

### M2.4 — Interaction metrics and policy fixtures

Separate first generated audio, first audible acknowledgement, first useful
answer, cancellation dispatch, and physical output stop. Add explicit overlap
and interruption outcome measurements. Begin a small regression corpus for
noise, backchannels, corrections, delayed events, and premature EOU decisions.

**M2 exit demonstration:** an application selects either engine through the same
session API, keeps microphone input live during output, and handles a deliberate
interruption without stale output or state corruption. The evidence must use
real engine adapters, not only a fake engine that toggles two booleans.

## 6. M3 — Delegate

**Goal:** custom agents perform asynchronous work without owning the speaker or
blocking the interaction control path.

### M3.1 — Minimal agent and delegation contract

Introduce structured requests, task/delegation IDs, deadlines, cancellation,
budgets, and asynchronous progress/content/result events. Provide adapters for
the existing LLM/tool loop, a deterministic local agent, and a remote agent.
Keep the new interface small; a string-returning synchronous `Run()` is not the
primary contract.

Use context versions and output generations to reject obsolete results. Give
each invocation one terminal outcome, with deduplication for retried remote
events. Separate cancellation request from confirmed termination. A task that
ignores cancellation may finish externally without being allowed to speak locally.

Delegation policy decides whether to acknowledge once, remain silent, or expose
meaningful progress. Output arbitration remains in the conversation controller.
Private model reasoning is neither required nor a public progress contract.

**Acceptance gate:** the same conversation accepts local and remote custom-agent
implementations; a delayed task does not block audio/control; a corrected user
request supersedes obsolete output; cancellation and deadline propagation are
tested; partial results and errors are structured and correlated.

### M3.2 — Safe tool execution

Build provider-independent tool discovery/invocation around validation,
authorization, approval for sensitive actions, cancellation, and audit. Support
native and HTTP tools first; add MCP as an adapter when the execution contract
is stable. Do not make an external protocol the internal security model.

Specify idempotency and retry behavior for side-effecting tools. Distinguish
read-only cancelled work from already committed actions requiring status
reconciliation or compensation. Do not run untrusted native plugins in process
merely because they implement a tool interface.

**Acceptance gate:** unauthorized calls are blocked before execution; replay never
runs live side effects; retry and late-result tests cannot duplicate an externally
visible action; approvals and terminal outcomes are traceable.

### M3.3 — Context and memory boundaries

Create a replaceable `ContextBuilder` that supplies immutable, versioned task
views. Separate conversation state, agent history, task-local data, and durable
memory. Start with bounded context selection; add summarization, retrieval, and
memory adapters only for demonstrated needs.

**Acceptance gate:** tasks see a reproducible context version; cancelled/unheard
assistant output is not represented as heard; sensitive data is scoped and
redacted; persistent memory is opt-in with provenance and deletion behavior.

**M3 exit demonstration:** while a slow read-only task is running, the assistant
acknowledges once, continues listening, accepts a user correction, and presents
only the relevant result. A separate side-effecting-tool fixture proves that
stopping speech cannot silently undo or repeat an action.

## 7. M4 — Operate

**Goal:** operate within declared resource, correctness, privacy, and recovery
limits. Expand the safeguards introduced in M1–M3 rather than adding them late.

### Resource and failure handling

Add admission control, per-session/provider limits, fairness, cost accounting,
rate-limit handling, circuit breakers, and explicit degraded modes. Test long
sessions, slow consumers, provider disconnects, tool timeouts, and stalled audio
output. Drain sessions safely on shutdown.

Fallback must state which context, capabilities, pending audio, and tasks can be
preserved. Start with explicit session/engine restart policies; defer transparent
session migration until provider state semantics make it supportable.

### Recording, replay, and evaluation

Extend early fixtures into privacy-aware recording and replay. Persist versioned
control events and selected provider/tool outcomes; audio/payload recording stays
opt-in with retention and deletion policies. Include event gaps, clock mappings,
and policy versions in the manifest.

Replay deterministically tests reducers and recorded outcomes; it does not
promise that a live model/tool call will reproduce the same answer. Side effects
are disabled. Promote failures into reviewed regression fixtures and compare
interaction correctness alongside latency.

### Operations gate

Declare hardware, provider/configuration, workload, concurrency, queue limits,
network assumptions, and percentile methodology. Establish SLO thresholds from
measured baselines rather than inventing universal latency numbers. Report task
success, interruption correctness, audible gaps, p50/p95/p99 latency, CPU,
memory, cost, and sample exclusions.

**Acceptance gate:** sustained load stays within declared bounds; fault injection
produces explicit outcomes rather than silent hangs; representative failures
replay as regression tests; privacy controls are exercised; recovery does not
replay externally visible actions or stale speech.

## 8. Later — integrations and bounded orchestration

Prioritize adapters requested by real applications: RTC transports, verified
native speech providers, existing agent frameworks, and selected tools. Each
adapter needs a capability profile and conformance tests, not just an example
that connects successfully. Do not add an integration based only on a vendor's
model name or a benchmark claim.

Sub-agents, parallel delegation, handoff, durable background tasks, and
human-in-the-loop workflows are demand-led extensions after the task/context/
permission contracts stabilize. A bounded task tree can support these without
turning RealFlow into another general workflow framework. Video/avatar support
should extend timestamped media and participant contracts rather than redefine
session lifetime.

## 9. Immediate implementation queue

The most valuable next change is to **finish the continuous-session foundation
already introduced**, not to add more provider names.

| Order | Small, reviewable change | Required proof |
|---|---|---|
| 1 | Canonical, bounded `EventTimeline` admission and ordered observer delivery. | Stored/delivered sequence and timestamp equality; concurrent order; capacity behavior. |
| 2 | Session start/stop, startup failure, callback, and teardown safety. | Race/re-entry/failure tests and sanitizer evidence where supported. |
| 3 | Complete pipeline event wiring and explicit interruption controls. | Real pipeline integration test with model/tool metrics and input during output. |
| 4 | Native duplex engine adapter with separate playback lifecycle. | Shared conformance tests, late-packet fencing, and player-confirmed stop. |
| 5 | Minimal custom-agent/delegation vertical slice. | Slow task + user correction + obsolete-result suppression demonstration. |

Keep mechanical renames in a separate review. Steps 1–2 are prerequisites for
calling the new session API reliable; they can be developed alongside the
adapter work but must pass before M2 is declared complete.

## 10. Release and scope discipline

For each milestone, include an executable reference example, regression tests,
API/migration notes, capability limitations, and measured evidence. Run
`make test` and CMake/CTest; use sanitizers for lifetime/concurrency changes and
transport/player tests for audible-output claims. A checked-in test is not proof
that it has run successfully.

Explicitly defer a generic graph DSL, autonomous swarms, a plugin marketplace,
model training, proprietary evaluation claims, and transparent cross-provider
session migration. A proposed feature should improve realtime human–agent
interaction; otherwise prefer an integration over expanding the core.

The architecture remains provider-neutral. Native duplex, cascade, custom
agents, and future media are implementations of the runtime contracts, not
reasons to replace those contracts on each model release.
