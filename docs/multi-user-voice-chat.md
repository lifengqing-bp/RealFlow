# Single-process multi-user voice chat (M2.2a)

`apps/voice_chat_adapter.{h,cpp}` provides `realflow_app::VoiceChatConnection`.
The executable acceptance suite uses two trusted client contexts and the existing
Mock ASR/LLM/TTS providers through the real managed pipeline. No authentication
service, WebSocket server, second session registry, supervisor or timer thread is
introduced. This is an application binding, not a production transport server.

## Ownership and concurrency

The transport creates exactly one binding for each authenticated connection and
supplies `AuthenticatedClient{user_id, connection_id}` from its trusted security
context on every request. **Never populate this context from request JSON.**
Connection IDs must be server-issued, unique per connection and never reused on
reconnect. The same user on another connection has a separate binding and cannot
operate the first connection's session. The transport bounds its connection count;
RuntimeManager independently bounds all accepted session reservations.

The binding owns only the immutable principal, current admission tickets, media
route and control state. RuntimeManager alone owns session resources and capacity.
The manager must outlive all bindings. A factory receives the server-issued handle
and per-incarnation media sinks; return an engine and session-local providers,
agent history, executor and other dependencies in `SessionResources`. Do not
capture a raw connection pointer or share mutable provider/history state between
users. The runtime destroys the engine before its dependencies and resolves
`retired` only after resource teardown and registry erase.

Control/input operations serialize on a per-connection mutex. Existing pipeline
`push_audio` only enqueues input; providers must honor the bounded engine-call
contract. There is no global application lock, new worker or detached task. Media
callbacks and canonical timeline observations enqueue under a separate short
mutex without calling application code. Callbacks capture weak routes; replacing
or destroying a binding cannot give an old callback access to a new route.

The small internal engine wrapper subscribes to the existing session timeline
before engine startup. Routed observations retain canonical session, generation
and sequence. No changes to RuntimeManager or provider interfaces are required.

## Operations

| Operation | Contract |
|---|---|
| `start(client)` | Authorize, reserve via `add()`, return admission and tickets. Concurrent/repeated starts while Starting/Running share one handle. Admission rejection exposes the original runtime reason. |
| `push_audio(client, handle, frame)` | Check principal and complete handle; accept only after `started == Started`, validate frame size/layout, then use RuntimeManager's operation lease. `InputRejected` reports running-engine backpressure. |
| `status(client, handle)` | Derive Starting/Running/Stopping/Retired from tickets and `find()`. Unauthorized/stale requests disclose no session state. Startup/retirement outcomes remain distinct. |
| `stop(client, handle)` | Close input/output admission, clear local output, expose a sticky player flush request, call `remove(handle)`. Repeated stops are harmless. |
| `take_output(client, handle)` | Drain a bounded batch containing only that connection's audio, transcripts and observations. Every item includes the complete runtime handle. |
| `acknowledge_player_stop(client, handle)` | Record the player's explicit flush/stop acknowledgement for this incarnation. It is independent of runtime retirement. |
| `disconnect()` | Trusted transport hook using the same removal path; the binding cannot restart. Destruction also requests disconnect without waiting for retirement. |
| `heartbeat(client, now)` / `tick(now)` | Trusted host clock and liveness hooks. Expired bindings cannot be revived by late heartbeats. |

Do not treat `Accepted` as Running or a successful Stop response as Retired.
While stopping, retries cannot create a replacement. A restart on the same live
connection requires both retirement and player-stop acknowledgement, then obtains
a fresh RuntimeManager generation. Reconnecting creates a new connection binding
with fresh history/provider state; it does not resume the old session.

A stopped, failed or disconnected instance retains runtime capacity until actual
retirement. Keep the returned retirement ticket when discarding a disconnected
binding, so the host can report pending cleanup. The binding does not turn a
cleanup timeout into successful retirement or claim a disappeared player is silent.

## Transport and player obligations

Run `tick(steady_clock::now())` for each live binding from the host loop at a bounded
cadence even with no inbound messages. Default liveness timeout is 30 seconds;
with a one-second host cadence, silent loss is detected within 31 seconds of the
last authenticated heartbeat. Tests inject clock values, without wall-clock sleeps.
The adapter deliberately does not create another transport liveness thread.

On the transport's serialized send/control lane, drain `take_output()`, inspect
Status for `flush_requested`, and send a stop/flush command containing that handle.
The player clears buffered PCM, closes that incarnation and acknowledges only
after completing the flush. Reject every later packet for a closed or different
incarnation, including packets already drained into a transport send buffer before
Stop. Client disconnection leaves player acknowledgement unknown. The acceptance
player fixture verifies this contract; it does not claim physical-device silence.

Output defaults to 256 messages, each at most 64 KiB payload. Input frames are
bounded by the same byte limit before pipeline admission. An output size/count
limit or allocation failure closes and clears the route, records loss, and the
next operation/tick reports fatal failure through RuntimeManager and requests
flush. Poll status/output and tick promptly; lifecycle correctness does not rely
on best-effort runtime notifications. Ordinary provider Error observations remain
non-fatal, following the runtime contract.

The bound applies to the adapter queue. Provider buffers, timeline journals and
transport/player queues have their own bounds. Slow/non-cooperative factories,
providers or callbacks can delay retirement indefinitely; this slice does not add
watchdogs or process isolation. Stop cancels active work but does not undo completed
tool side effects. Native-engine conformance remains a later M2.4 integration gate.

## Run the acceptance suite

```sh
make test-voice-chat
# Full required regression path:
make -j4 test

cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake --parallel 4
ctest --test-dir build/cmake --output-on-failure

# Inspect or run a single deterministic scenario:
./build/voice_chat_acceptance_tests --list
./build/voice_chat_acceptance_tests --case two_clients_independent_pipeline_and_player_flush
```

Seven scenarios exercise: concurrent independent ASR inputs, transcripts, PCM and
agent histories; canonical observation identities/causal order; authorization for
all client operations; duplicate/racing Start and Stop; stop during gated startup;
capacity retention during gated teardown; startup queue rejection and queued
cancellation; startup and engine failure isolation; active model cancellation;
explicit/RAII/silent disconnect; reconnection; old input/control/media/observation
callbacks; player flush; and input/output byte and message-count limits. Assertions
use completion tickets, synchronization gates and observable completion conditions,
not sleeps to guess readiness. Tests check resource destructors, terminal reasons,
no orphan sessions, failure counts and output-loss counters.

The target is included in default CTest and `make test`, so the existing GCC and
ASan/UBSan CI jobs cover it without extra packages or live credentials.
