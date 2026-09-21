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

