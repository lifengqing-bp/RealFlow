# Explicit response cancellation

RealFlow is the bridge between a human and AI. Input is evidence of what the human
wants; it is not automatically an instruction to stop work. In particular, a PCM
frame (including silence, echo, or background sound) is not a barge-in decision.

This implements a small part of [M2.3](roadmap.md):
explicit cancellation in the existing pipeline. No new dispatcher, intent model,
agent framework, or execution thread is introduced.

## API and scope

`ConversationSession::cancel_response()` and
`RuntimeManager::cancel_response(handle)` forward one explicit policy decision to
`ConversationEngine::cancel_response()`. The pipeline implements it using the
existing `Conversation::interrupt()` cancellation path.

- `true` means the cancellation request was forwarded/accepted, not that all work
  has finished or a playback device is silent. It is valid when no response is active.
- `false` means unsupported, not running, or (through the manager) invalid/stale
  session identity. A manager-forwarded exception returns false and triggers its
  existing engine-failure retirement policy.
- The pipeline requests cancellation of its current and queued response tasks and
  TTS generation. It does **not** stop the session, clear input audio, reset ASR,
  or invalidate an ASR callback already processing the human's next utterance.
- Check `ConversationCapabilities::response_cancellation` for support. Existing
  engines inherit an unsupported default. Native engine unification is still pending.

For example, application interaction policy can explicitly request cancellation:

```cpp
#include "byteturn/runtime_manager.h"

bool on_confirmed_cancel_intent(byteturn::RuntimeManager& runtime,
                               const byteturn::SessionHandle& handle) {
  return runtime.cancel_response(handle);
}
```

Continue calling `push_audio()` during output and cancellation. Do not call the
new operation on every frame. `observe()` and the pipeline's legacy `handle_event()`
remain observation/compatibility paths; replaying `TurnCancelled` or
`InputSpeechStarted` does not execute this operation.

## Correction flow

An application confirms a correction, cancels the obsolete response, and lets ASR
finish the replacement request. The transcript callback runs before the pipeline
submits that final transcript, so policy can also request cancellation there before
allowing the replacement final to proceed. Without an explicit cancellation, finals
retain the existing FIFO submission behavior; no semantic supersession is inferred.

The regression fixture starts a Tokyo request, holds an Osaka partial in ASR, and
queues the Osaka final before cancelling Tokyo. Both inputs survive. A deliberately
uncooperative old LLM emits a late token and a tool call: the token is rejected and
that tool is not invoked. Only the Osaka response follows the previously delivered
Tokyo audio. A separate fixture checks late TTS audio is rejected after cancellation.
These are controlled fake-provider tests, not semantic intent-recognition evidence.

Input utterance correlation is separate from the response generation fence. TTS
utterances start on the existing serialized response lane, not when ASR merely queues
a final. Failed response synthesis is drained before that provider is reused. Late
response completion/failure must not clear newer response state.

## Boundaries

Cancellation is cooperative. Already-executed tool effects are not reversed, and a
provider that never returns can still delay the next task on the legacy FIFO lane.
This API cancels a response; it is **not** a speech-only pause that preserves its task.

Already-admitted audio callbacks may finish; audio already handed to a player must
be fenced/flushed by that player. Do not make physical stop correctness depend on a
best-effort timeline notification. Playback acknowledgement, unheard-history
reconciliation, and separate speech-only cancellation remain follow-up work. Model
history already committed before a playback interruption is not rolled back here.

`TtsProvider::cancel()` must be a thread-safe, non-waiting signal: it must not join
workers or invoke application callbacks. Cleanup drains synthesis separately.
Existing lifecycle/operation leases guard the public request against teardown.
Input, cancellation and final submission can race; no new total-order command log
is claimed. Use explicit application ordering when cancelling and replacing a request.
Rebuild consumers of the updated C++ interfaces; no binary ABI compatibility is promised.

## Validation

`make test-interaction` runs the focused fixtures; `make test` also runs the existing
pipeline, session/timeline and Runtime Manager suites. CMake registers the same new
executable as `interaction_control_tests`. The suite covers raw input versus actions,
correction preservation, stale handles, late model/tool/audio output, unsupported
engines, callback re-entry, lifecycle leases and manager-contained control failures.

## Native duplex playback contract

`NativeDuplexConversationEngine` borrows a `RealtimeSpeechProvider` and reuses
`FullDuplexConversation`; providers and output sinks must outlive engine teardown.
Its private event bus forwards each observation only to the session timeline sink.
Do not republish transcript/audio callbacks as canonical observations.

`playback_started(response_id)` and
`acknowledge_playback(response_id, played_samples)` are explicit control APIs on
`ConversationEngine`, `ConversationSession`, and `RuntimeManager` (the manager
also requires a `SessionHandle`). The capability is `playback_acknowledgement`.
The default, including pipeline, returns `false` without effects. Session/runtime
admission also returns `false` before startup, during stop, or for a stale handle.
Native reports return `false` for inactive/mismatched responses; for active
responses, duplicate/regressive reports succeed without another event. Progress
is cumulative, monotonic and clamped to delivered mono samples at the configured
output rate. Reports after generation completion remain valid while audio is
unheard. A new response resets the sample offset; old-response audio/acks are
ignored. Providers must supply unique response identities within a connection.

Native `cancel_response()` uses the same acknowledged offset as VAD barge-in,
but emits `ResponseCancelRequested`, not `BargeInDetected`. Input remains live.
A true return acknowledges dispatch, including an idle no-op. `OutputCancelled`
records provider cancellation/truncation dispatch, **not physical silence**.
Already queued player audio and callbacks already in flight are not recalled;
a player flush/stopped acknowledgement remains future work. Stop drains admitted
controls, joins input, and waits for provider `close()` to quiesce callbacks before
retiring resources. Callback code must request shutdown rather than wait for it.

The adapter preserves configured server VAD ownership. With client EOU,
`AudioFrame::end_of_utterance` commits input; client interruption policy calls
`cancel_response()` explicitly. `handle_event()` is never a playback/VAD command.
The shared conformance suite covers continuous input during output, unsupported
pipeline reports, native playback/sample fencing and barge-in offset, late audio,
canonical event counts/order, handle reuse, and gated callback shutdown. It uses
local fixtures only; vendor validation, negotiated formats/capabilities and
player-confirmed physical stop remain outside this slice.
