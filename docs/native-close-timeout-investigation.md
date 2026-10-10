# Native conversation close timeout investigation

Base: `13498ecb5fb11dd645fe0b00ecc470a37a925816` (main inspected 2026-10-10).

## Evidence and conclusion

The [failed GCC run](https://github.com/lifengqing-bp/RealFlow/actions/runs/37678507844)
built successfully and timed out after 90 seconds in conversation integration;
the last printed success was `native_server_and_client_commit_ownership`.
That log alone does not locate the blocked instruction.

Locally, GCC 13.3 Debug reproduced a hang in the next case,
`native_bounded_input_and_provider_rejection`. With diagnostic-only test changes
and the original production code, iteration 95 of a requested 2,000 fresh-process
runs exceeded a 10-second watchdog. The final output was:

```text
RUN native_bounded_input_and_provider_rejection
  native bounded input: two pushes and three errors observed; closing
```

This locates the local hang inside `close()`, after the provider gate and event
waits completed. Two earlier short-watchdog attempts also timed out, but are not
used as the primary localization evidence.

A deterministic linker-interposed regression with the original production code
failed 20/20 runs with exit 1:

```text
FAIL native_close_serializes_with_wait_predicate: close notified between predicate check and wait without acquiring the input mutex
```

The confirmed bug is a lost condition-variable wakeup. The worker checks
`stopping_ == false` and an empty queue while holding `input_mutex_`. The original
closer can set `stopping_` and notify without that mutex, before the worker
atomically unlocks and waits. The worker then sleeps with no further notification,
and the closer blocks in `join()`. Atomic memory ordering does not close this gap.
This is a reproduced explanation consistent with the historical CI failure;
there is no historical stack trace proving that the CI runner took this exact
interleaving. A later successful PR run cannot rule out this intermittent bug.

## Synchronization and lifetime review

- `Signal::set()` and its wait predicate use the same mutex. The signal is sticky;
  setting it before a wait does not lose readiness. No Signal defect was found.
- The fixture blocks the first provider push outside the native input mutex.
  Once entered, the worker has removed frame 1; frame 2 fills the one-slot queue,
  and frame 3 deterministically produces `audio_overload`.
- The release signal remains set for both provider pushes. The fixture waits for
  two pushes and all three errors before close. The final backpressure callback
  and the worker returning to its empty-queue wait can race with close.
- `Finally` is destroyed before the conversation owner and releases the fixture
  gate on assertion failure. Provider state and captured events outlive the
  conversation. The preceding commit-ownership case explicitly closes and joins
  each worker before destroying its provider; no cross-case resource leak was
  found in this review.
- Provider `push_audio` is contractually non-blocking/bounded. A provider violating
  that contract can still block a join; changing provider-close ordering is not
  part of this fix. Concurrent owner destruction or close from the input callback
  is not validated by this investigation.

## Change

Set the stopping predicate while holding `input_mutex_`. Release the mutex before
notification, join, and provider close. Keep queue bounds, cancellation behavior,
provider-close ordering, and the existing 90-second integration timeout unchanged.

The new Linux/libstdc++ CMake regression wraps only the test executable's wait,
notify, and mutex-lock calls. It parks the worker after the predicate check but
before the actual wait, with the input mutex still held. A correct closer's lock
attempt releases the gate, and the real mutex/CV handshake orders the notification
after the worker enters waiting. The old closer instead notifies while the gate
is parked; the wrapper records this violation and returns a permitted spurious
wake so the test fails cleanly rather than hanging. Five-second gate watchdogs
and a 15-second CTest cap diagnose harness failures; no sleeps determine success.
No hooks or ABI changes are added to production classes. This ABI-specific test
is skipped outside Linux/libstdc++; the portable integration suite still runs.

`RUN` lines identify the active case even if teardown never returns. The bounded
input case also logs the transition into and out of close. CI includes the new
regression in its existing repeat step. CMake runs this special regression;
`make test` continues to exercise the portable suites.

## Local validation after the fix

| Check | Result |
| --- | --- |
| GCC 13.3 Debug, full CTest | 12/12 passed |
| Forced check-to-wait regression, separate processes | 1,000/1,000 passed |
| Bounded-input/provider-rejection case, separate processes | 2,000/2,000 passed |
| Entire conversation integration suite (13 cases) | 200/200 runs passed |
| `make test -j 4` (GCC, `-O2`) | Passed |
| GCC ASan + UBSan, full CTest, `detect_leaks=0` | 12/12 passed |
| GCC TSan, conversation suite and forced regression | Each passed 20 repetitions, no reports |

Sanitizer builds use `-fno-omit-frame-pointer -fno-pie`, linker `-no-pie`.
The initial ASan/UBSan run with `detect_leaks=1` could not complete LeakSanitizer:
it reported a fatal ptrace/proc-access limitation (`Can't open /proc/.../task`).
Disabling leak detection allowed ASan/UBSan validation; this is **not** a local
leak-check pass. Two generated executables initially lacked execute permission;
restoring that permission resolved their CTest BAD_COMMAND errors. Neither issue
required repository code changes. Local Clang was unavailable; GitHub's existing
Clang ASan/UBSan job remains the independent check, including leak detection.

Reproduce the principal checks with:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake --parallel 4
ctest --test-dir build/cmake --output-on-failure
ctest --test-dir build/cmake -R native_close_wakeup_tests --repeat until-fail:1000
build/cmake/conversation_integration_tests --case native_bounded_input_and_provider_rejection
make test
```

To reproduce the deterministic failure, retain the new test/build files but
restore only `src/full_duplex_conversation.cpp` from the base commit in a disposable
checkout, rebuild, and run `native_close_wakeup_tests`. It must fail on the old
unlocked stopping predicate and pass with the lock restored.
