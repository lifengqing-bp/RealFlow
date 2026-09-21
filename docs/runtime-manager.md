# Runtime Manager: M2.2 implementation

This slice implements the process-local ownership and stop-and-retire design in
[architecture.md](architecture.md) and [roadmap.md](roadmap.md), following M2.1's
[session/timeline foundation](runtime-foundation.md). See the design-first PR #4.
The public API is [`runtime_manager.h`](../include/byteturn/runtime_manager.h).
The existing `byteturn` namespace, C++17 baseline and provider behavior are unchanged.

## Ownership

The executable owns `RuntimeManager`. Its registry retains each reservation until
session/engine destruction and release of the manager's dependency ownership.
`SessionResources` puts a type-erased shared dependency bundle before the owned
engine; dependencies therefore survive constructor failure and engine teardown.
Factories may borrow binary-owned dependencies only when they outlive the manager.
External/shared references can outlive retirement: the limit bounds managed session
ownership, not arbitrary application allocations or remote provider resources.

Transport/application code receives a value `SessionHandle`, never an owning session
pointer. The handle carries runtime-instance ID, string session ID and incarnation.
The first two IDs may recur elsewhere; the full handle must match for input, remove
or failure reporting. Handles are not durable/distributed identifiers.

There are a fixed number of startup workers (default two), one registry supervisor
and one runtime notification dispatcher. Each ConversationSession still creates its
M2.1 lifecycle and timeline threads. This is a correctness foundation, not a claim
of CPU-efficient operation at thousands of sessions. The supervisor scans at a
configurable interval (default 5 ms), independently of observation delivery.

## Admission and completion

`add(id, factory)` returns `SessionAdmission`. Invalid IDs/factories, duplicate IDs,
capacity exhaustion, a full startup queue, draining, and identity exhaustion have
distinct statuses. Allocation failure before admission may throw. Rejected factories
are not invoked. The manager increments rejection counters by reason.

Acceptance reserves identity/capacity before construction, not after a provider
connects. `max_sessions` includes Reserved, Starting, Running, StopRequested and
Retiring entries. `max_pending_starts` separately limits work not yet picked up.
The maximum ID length bounds identity strings in entries and notifications.
Factories construct resources outside the registry lock on startup workers.

Each accepted admission has two shared futures:

- `started`: Started, or the failure/removal/shutdown reason that prevented startup.
- `retired`: the final outcome after teardown and registry erase; never Started.

These futures, not notifications, are definitive. They remain valid after removal.
Futures contain outcomes only, not session ownership. A Running snapshot has no
terminal outcome (`None`); a Reserved notification never implies successful startup.

## Removal and abnormal termination

`remove(handle)` closes admission and requests stop without waiting. Repeated valid
requests are idempotent. Stale incarnations cannot affect a replacement. Input holds
an internal operation lease; accepted calls drain before destruction. A call admitted
at the manager may still be rejected by a concurrently stopping session.

A cancelled reservation still in the startup queue is removed from that queue and
reaped by the supervisor without waiting for a free startup worker. Its factory is
not invoked, and its captures are destroyed outside the registry lock. Removal during
factory/start fences the result but cannot forcibly interrupt arbitrary C++ code.

The supervisor reads authoritative SessionLifecycle state and waits for the closed
session timeline's enqueued callbacks to drain before destroying it. A blocked session
observer keeps that session charged, without blocking scans or stop delivery to other
sessions. Dependency destruction runs on one reaper lane; a non-cooperative dependency
destructor can delay other **retirements**, though direct removal/stop requests still
proceed. This limitation is not process isolation.

Exceptions from manager-forwarded engine input/control become EngineFailure and request
stop. `fail(handle)` is the explicit callback-safe fatal report for provider/worker
code. Ordinary Error observations do not imply a fatal session failure. A later fatal
cause can replace a normal remove reason; the first fatal cause wins. A lifecycle
cleanup failure is reconciled even without any notification consumer.

The manager's terminal outcome is separate from the session's cleanup lifecycle: a
ReportedFailure can finish cooperative cleanup as SessionLifecycle::Stopped while the
retirement ticket still reports the fatal reason. No error text, credentials, audio
or transcript payload is copied into runtime notifications.

Do not let exceptions escape native worker thread entry points. Engine `stop()` must
quiesce callbacks even when it reports an error. Memory corruption, process crashes,
forced kills and stuck native work require the application's process supervisor;
this module provides neither auto-restart, a watchdog nor a thread-kill mechanism.

## Notifications and observations

Pass one optional NotifySink to the constructor. Its queue is bounded independently
of all session timelines. Callbacks run in enqueue order outside registry locks.
An observer exception is counted; queue/allocation overflow drops the newest
notification and records counts and first/last possible lost sequence. There is at
most one notification callback in flight in addition to the configured pending bound.
The sequence counter saturates rather than wrapping; no new notification is enqueued
when that identity space is exhausted.

Notifications are historical observations. A delayed Started callback may arrive after
its session has been removed; consult `find(handle)`/`snapshot()` rather than treating
it as current authority. Incarnations make reentrant stale remove/failure reports safe.
A full notify queue cannot lose a removal request or terminal completion ticket.

`flush_notifications(timeout)` waits for previously enqueued callbacks, not future
ones or dropped notifications. Registry snapshots show current membership; the manager
keeps no unbounded terminal-history list. `stats()` returns phase counts, admission
rejections, retirement/failure counts, queue depth/high-watermark and loss/error counters.
It does not yet export runtime duration histograms or a Prometheus integration.

`observe(handle, Event)` is observation-only and returns explicit timeline admission
status. `handle_event()` still forwards the legacy session hook; a true return means
the hook was forwarded, not that a typed command was accepted. Commands, payload-size
rejections inside that legacy hook, and a serialized reducer remain follow-up work.

## Shutdown and callback rules

`request_shutdown()` atomically closes global admission and requests stop on all
entries. `wait_shutdown(timeout)` may report timeout while the manager still owns live
resources. `shutdown()` joins workers; repeated/concurrent owner calls are supported.
After sessions retire, the runtime notification dispatcher drains before Stopped is
reported. This means an observer that never returns can keep shutdown incomplete.
The destructor performs cooperative shutdown, not an implicit force-kill on timeout.

Known runtime and EventBus callbacks cannot make blocking shutdown/flush waits; they
return after requesting shutdown or report false. Arbitrary external provider callbacks
cannot all be detected: they **must** use `request_shutdown()` and must not wait on their
own retirement/startup tickets. Never destroy a manager/session from its callbacks.
Factories and dependency destructors must also not wait for their own lifecycle work.
Binary-owned callback captures and borrowed services must outlive complete shutdown.

OS signals must be converted to requests on an ordinary application thread. The library
does not install handlers, call exit, or manipulate deployment restart policies.

## Example and verification

[`runtime_manager_demo.cpp`](../apps/runtime_manager_demo.cpp) is an executable
ownership example with a fake engine and explicit dependency bundle:

```sh
make -j2 all test
./build/runtime_manager_demo
# RealFlow runtime: admitted=2 retired=2 live=0

make test-runtime
```

The runtime test executable covers atomic duplicates/capacity, invalid admission,
startup saturation, factory/start failure, cancellation before/during construction,
operation leases, incarnation reuse, independent failure, missing/dropped notifications,
observer re-entry and draining, dependency ordering/capacity, and shutdown races.
The existing regression and foundation executables remain part of `make test`.
CMake registers the new runtime tests and demonstration with CTest as well.

Next work is complete pipeline events/explicit controls and the unified native-duplex
playback contract. Per-tenant quotas, shared scheduling, duration metrics, watchdogs,
restart/backoff, persistent registry and process-isolated workers remain planned.
