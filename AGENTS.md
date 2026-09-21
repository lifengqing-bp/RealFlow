# ByteTurn agent guide

## Product goal

Build a low-latency, voice-first agent harness in portable C++. ASR, LLM, TTS,
transport, and vendor SDKs must remain replaceable. Optimize for continuous
conversation, interruption correctness, observability, and CPU efficiency.

## Architecture boundaries

- `apps/` adapts user-facing transports and must not own agent policy.
- `Conversation` owns realtime interaction state: ASR input, utterance events,
  barge-in, cancellation, and TTS output.
- `Agent` owns session history and the bounded LLM/tool loop.
- Provider interfaces contain no vendor SDK types.
- Tool implementations must not depend on audio or transport details.
- Audio callbacks must remain non-blocking once an asynchronous executor lands.

## Design rules

**Less is more. Solve the current problem with the smallest clear design.**

- Build for demonstrated requirements, not hypothetical future use cases.
  A roadmap item is not a reason to implement infrastructure early.
- Prefer existing components and direct code over new abstractions, layers,
  services, queues, threads, or configuration options.
- Before adding structure, explain the concrete problem, why a simpler option
  is insufficient, and the ownership, concurrency, and maintenance costs.
- Keep changes small and reversible. Remove obsolete code and consolidate
  duplicate responsibilities before adding new mechanisms.
- Simplicity must preserve correctness: explicit ownership, safe cancellation,
  bounded resources, security, and regression tests remain essential.

Review question: **Can we meet the same requirement safely with fewer concepts
and moving parts?**

## Engineering rules

- Use C++17 unless a change explicitly raises the baseline.
- Prefer explicit ownership and cancellation tokens over detached work.
- Add a regression test for state transitions, lifetime, or concurrency changes.
- Never log audio, transcripts, credentials, or tool arguments by default.
- Measure end-to-end latency; do not infer it from isolated provider timings.

## Verification

Run `make test` for every change. Run the CLI smoke test when changing the agent
loop. CMake is the canonical integration build; the Makefile is the zero-setup
developer path.

Observable behavior is part of the test contract, not just debugging output.
Assert event identity, required causal order, terminal outcomes, metric counts and
resource/loss counters alongside inputs and outputs. Use controlled timestamps for
exact latency arithmetic and synchronization gates for concurrent interactions;
do not infer readiness from sleeps or require unrelated threads to interleave alike.
Drain producers and notifications before assertions, check trace completeness, and
never turn a missing endpoint into a zero-duration sample. See
[the observability contract](docs/observability.md).
