# Session and timeline foundation (M2.1 implementation slice)

This change follows the architecture and roadmap proposed in PR #2. It hardens
existing APIs rather than declaring the entire continuous-interaction milestone
complete. It is based on upstream `e312d8c25da4d8513e6deab70a70bf58438a1014`.
The `byteturn` namespace, include paths, C++17 baseline, and metric prefixes are
preserved. Existing standalone `Conversation`, `FullDuplexConversation`,
`Agent`, and `AsyncSession` remain available.

## Canonical, bounded event admission

`EventTimeline::append` is the admission boundary. It validates/fills session and
generation identity, derives missing trace identity, assigns its own sequence,
and assigns `received_at` exactly once before retaining or notifying. A supplied
`timestamp` is preserved for existing latency calculations; otherwise it defaults
to `received_at`. Neither field is a wall-clock or cross-host clock guarantee.
Ordering is per timeline and by admission, not by source timestamp.

The journal and its dispatcher share immutable admitted events. Subscribers see
the same sequence, receive timestamp, source timestamp, and identities as the
journal. A legacy `EventBus` sequence is not carried forward as timeline order.

`TimelineConfig` sets retention count, pending notification count, aggregate
string-byte limit per event, session identity, generation, and an optional clock
for tests. Limits must be positive. The event-size limit includes session, turn,
trace, name, and data strings. It is a logical payload bound, not an exact RSS
limit; allocator overhead, callers' snapshot copies, and subscriber-retained
copies are outside it. PCM is not carried by this event type. Allocation and a
short admission mutex remain: this is not a lock-free or hard-realtime API.

Retention evicts the oldest event. The separate notification queue drops the
newest notification when full; the event itself remains admitted to the journal.
One in-flight notification is additional to the pending queue limit.
`TimelineAppendResult` distinguishes rejection from successful admission without
an enqueued notification. `TimelineStats` exposes retention, eviction, rejection,
queue high-water mark, notification drops, and observer failures. First/last
lost sequence describe a span containing gaps, not a contiguous loss range.
Observer-queue allocation failure after journal admission is also counted as a
notification drop, with a separate allocation-failure counter.

Notifications run asynchronously on one owned dispatcher per timeline. Slow
observers do not run on the producer/control path, but can delay all observers
on that timeline and exhaust its shared queue. This is not per-subscriber
isolation. Observer callbacks must eventually return. Do not drive authoritative
state transitions or tool execution from this best-effort observation channel.

`flush(timeout)` waits for notifications enqueued before the call, not future
admissions, and returns false in callbacks or on timeout. Successful flush does
not prove no drops: inspect `stats()`. Snapshot recovery is possible only while
needed events remain retained. This is not a durable journal or complete replay
implementation. In-memory event data may contain transcripts or tool arguments;
exporting snapshots requires application privacy policy. JSON payload logging
remains disabled by default.

## Session lifecycle and ownership

The lifecycle is `Created -> Starting -> Running -> Stopping -> Stopped`, with
`Failed` as a terminal state after startup or cleanup failure. Stop before start
is valid. Stopped/failed sessions cannot restart. Generic `SessionStarted`,
`SessionStopped`, and `SessionFailed` events are distinct from native provider
connection events; tests must not assume only interaction events are retained.

An owned lifecycle worker invokes engine startup and cleanup without holding the
session mutex. Concurrent start calls share a single startup. Startup failure
runs partial-start cleanup before rethrowing the original exception. A cleanup
exception is recorded without replacing an earlier startup exception. Each
attempted startup gets at most one engine stop call.

An operation lease protects each admitted input, legacy event call, observation,
and state read. Stopping rejects new leases and waits for accepted calls before
calling engine stop. The engine must make running calls thread-safe and bounded;
its stop must quiesce its asynchronous callbacks. The lifecycle worker is not a
replacement for the engine's own thread-safety or cancellation contracts.

Use `request_stop()` from provider, audio, or application callbacks. It never
waits. On ordinary external threads, `stop()` waits for engine quiescence; from
an admitted engine call, lifecycle call, or EventBus callback, it only requests
stop to avoid waiting for itself. `wait_stopped(timeout)` is available to external
owners. Arbitrary provider callbacks that are not part of those tracked stacks
must use `request_stop`, not assume automatic detection.

`stop()` is not an observer-drain barrier. Use `timeline().flush()` separately
before inspecting asynchronously updated observer state. Destruction joins the
lifecycle worker and drains/joins the timeline before owned members disappear.
Never destroy the session or timeline from one of its callbacks. Borrowed
providers, `Agent`, `AsyncSession`, `SessionExecutor`, external event buses, and
callback captures must outlive all dependent work and shutdown. An uncooperative
engine/provider or permanently blocked observer can prevent shutdown; this slice
does not promise forced cancellation or a hard shutdown deadline.

## Observations, compatibility, and adapter integration

New code that only records an observation should call `observe(Event)`. It does
not invoke an engine action. `handle_event(Event)` remains a compatibility hook:
it validates and normalizes before passing the admitted event to the engine.
It is not a serialized command queue, and concurrent engine effects are not
ordered merely because their observations have timeline sequence numbers.
Typed command/reducer semantics are part of the next adapter milestone.

`ConversationEngineContext::emit` is a weak-lifetime admission sink. Late use
returns `Closed` instead of keeping a session alive. Engines must still quiesce
their own callbacks: a weak sink does not protect arbitrary raw captures. The
legacy raw timeline pointer remains for migration. `PipelineConversationEngine`
now prefers the sink, checks that its `AsyncSession` identity matches, and is
covered by a real adapter integration test with fake ASR/LLM/TTS backends.

Direct `timeline.bus().publish()` bypasses journal admission. Use the bus only as
an observation bridge for legacy metrics/loggers. The legacy standalone
`EventBus::publish()` stays synchronous. Its nested-unsubscription guard tracks
ancestor callback frames; subscribers must not mutually wait for unsubscription
across different concurrent callback threads. An external bus shared by several
timelines can deliver callbacks concurrently; its consumers must be thread-safe.

The default configuration creates one lifecycle thread and one observation thread
per session, in addition to engine workers. A shared scheduler is a later scale
optimization; this patch makes no high-concurrency throughput claim.

## Migration example

The following is an integration fragment; `engine` is an application-provided
`std::unique_ptr<byteturn::ConversationEngine>`.

```cpp
byteturn::TimelineConfig limits;
limits.max_events = 2048;
limits.max_pending_notifications = 512;
limits.max_event_bytes = 16 * 1024;

byteturn::ConversationSession session("session-1", std::move(engine), nullptr, limits);
std::atomic<std::uint64_t> observed{0};
auto subscription = session.timeline().subscribe([&](const byteturn::Event&) {
    ++observed; // Never perform blocking network/tool work here.
});

session.start();
// Feed audio through session.push_audio(frame).
// Provider/application callback shutdown: session.request_stop().
session.stop();
const bool drained = session.timeline().flush();
const auto health = session.timeline().stats();
// Check drained AND health.dropped_notifications before relying on telemetry.
session.timeline().unsubscribe(subscription);
```

Include `<atomic>`, `<cstdint>`, `<memory>`, `<utility>`, and
`byteturn/conversation_session.h` when adapting the fragment.

## Verification and remaining work

`make test` runs the original regression executable (including a real pipeline
session integration test) and `runtime_foundation_tests`. The latter has fake
clock, concurrent admission, overflow, observer re-entry/unsubscription,
startup/cleanup failure, admitted-operation teardown, and shutdown stress cases.
CMake registers both executables with bounded CTest timeouts. `make test-foundation`
is also available and does not require libcurl at link time.

This slice does not implement complete model/tool event wiring, explicit pipeline
interruption policy, an authoritative state reducer, a native-duplex engine
adapter, generation-versus-playback lifecycle separation, player-confirmed stop,
external agent delegation, a provider integration, persistence, or deterministic
replay. In particular, raw input frames still trigger the legacy pipeline's
interruption behavior; that is a named M2.2 follow-up, not fixed by a safe session
wrapper. Native `FullDuplexConversation` remains a separate path.
