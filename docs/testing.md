# Testing RealFlow

The regression contract includes outputs **and** observable behavior: event
identity, required causal order, terminal outcomes, latency sample counts,
retention/loss accounting, and cancellation boundaries. See
[observability.md](observability.md) for the event and metric contract.

## Run the tests

Requirements: C++17, libcurl development headers/library, threads, and CMake
3.20+ or Make. The local HTTP integration fixture additionally needs Python
3.9+ (standard library only). No vendor account, API key, microphone, or external
network service is needed by the tests.

```sh
make test                  # Original suites and all new unit/integration cases
make test-unit             # Components, session/timeline, runtime manager
make test-integration      # Conversations, HTTP, controls, observability

cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake --parallel 2
ctest --test-dir build/cmake --output-on-failure
ctest --test-dir build/cmake -L unit --output-on-failure
ctest --test-dir build/cmake -L integration --output-on-failure
ctest --test-dir build/cmake -L http --output-on-failure
```

`REALFLOW_ENABLE_HTTP_TESTS` defaults to `ON`. An embedded build without Python
can configure `-DREALFLOW_ENABLE_HTTP_TESTS=OFF`; that deliberately excludes the
HTTP suite and is **not** the full test configuration. CI leaves it enabled.
Make shares runtime object files across the test executables rather than
recompiling the runtime once per suite. When changing compiler or flags, run
`make clean` or use a separate CMake build directory.

CTest registers one entry per executable/suite, not one per named case. The
three new suites contain **52 named cases: 26 unit + 13 conversation integration
+ 13 HTTP integration**. The existing five regression suites and manager demo
remain registered, for nine CTest entries in the full configuration.

Run an individual new case, or list the available names:

```sh
./build/cmake/component_unit_tests --list
./build/cmake/component_unit_tests --case segmentation_partial_utf8
./build/cmake/conversation_integration_tests --case pipeline_model_failure_then_recovery
python3 tests/http_fixture.py build/cmake/http_transport_integration_tests \
  --case agent_tool_loop_over_real_http_sse
```

The HTTP binary receives its fixture URL from the wrapper; do not point it at a
production service. Unknown case names and malformed runner arguments fail
rather than silently running zero tests.

## Coverage map

| Suite | Test boundary |
| --- | --- |
| `byteturn_tests` | Existing mixed regression suite: agent/executor, adapter streaming, TTS, metrics, conversation and native-duplex smoke paths. |
| `runtime_foundation_tests` | Canonical timeline metadata/order, bounded retention/notification loss, observer re-entry, session leases, startup/shutdown races and quiescence. |
| `runtime_manager_tests` | Admission, generation handles, retirement, bounded queues, dependency ownership and manager shutdown. |
| `interaction_control_tests` | Explicit response cancellation, correction input preservation, stale text/audio/tool suppression and cancellation leases. |
| `observability_contract_tests` | Exact timestamp arithmetic, trace loss, privacy, replayed metrics, failed/successful terminal distinctions and observer lifetime. |
| `component_unit_tests` | Protocol/parser, segmentation, incremental synthesis and small component boundary cases. |
| `conversation_integration_tests` | Real pipeline/manager/native-conversation components composed with controlled providers. |
| `http_transport_integration_tests` | Actual libcurl sockets, HTTP framing, retry/timeout/cancellation, SSE adapter and agent/tool execution. |

Labels are intentionally coarse: `unit`, `integration`, `http`, `concurrency`,
`regression`, and `smoke`. A fake engine test establishes runtime behavior; it
does not establish a real model's full-duplex capability.

## New unit cases

All names below are runnable with `component_unit_tests --case NAME`.

| Cases | Assertions |
| --- | --- |
| `request_contract`, `missing_model_rejected` | Endpoint, authorization, escaped request content and required configuration. |
| `response_unicode_and_null_content`, `response_invalid_json_rejected`, `invalid_surrogates_rejected` | Unicode/surrogate decoding, null content, malformed or trailing JSON and invalid pairs. |
| `http_error_payload_not_exposed` | Provider error bodies are not copied into diagnostic exceptions. |
| `sse_every_split_boundary`, `sse_one_byte_and_multiline` | Every two-chunk boundary of a representative stream; byte-at-a-time input, CRLF, comments, multiline data and EOF framing. |
| `sse_tool_calls_interleave` | Interleaved tool-call fragments assemble by index, not arrival adjacency. |
| `sse_incomplete_is_not_success`, `sse_terminal_ignores_late_payload` | Missing terminal markers cannot claim completion; text, malformed JSON and tool payloads after `[DONE]` are fenced. |
| `sse_failure_and_consumer_stop`, `cancellation_predicate_is_forwarded` | Malformed streaming data, rejected deltas and cancellation propagation. |
| `segmentation_limits_and_reset`, `segmentation_decimal_and_flush` | Invalid limits, decimal punctuation, flush, reset and buffered suffix behavior. |
| `segmentation_utf8_hard_limit`, `segmentation_partial_utf8` | Hard limits preserve codepoint boundaries; fragmented UTF-8 is retained until complete; truncated final codepoints fail explicitly. |
| `tts_finish_fences_push`, `tts_exception_surfaces_from_finish`, `tts_sink_exception_surfaces` | Finalization closes input; synthesis and consumer failures reach the caller. |
| `tts_cancel_unblocks_backpressure`, `tts_external_cancel_fences_audio`, `tts_invalid_capacity` | Bounded queue unblock, late-audio rejection and capacity validation. |
| `tool_registration_and_errors`, `tool_cancellation_before_and_after_side_effect` | Duplicate/unknown tools and handler failure; pre-cancel avoids invocation, while post-effect cancellation does not pretend to undo an action. |
| `curl_config_and_retry_policy` | Invalid transport limits, retry classification and `Retry-After` parsing using an explicit reference time. |

UTF-8 tests cover segmentation of valid UTF-8 with fragmented transport chunks;
the segmenter is not presented as a complete untrusted-text UTF-8 validator.

## Conversation integration cases

The pipeline fixture uses the actual `ConversationSession`,
`PipelineConversationEngine`, `Conversation`, `Agent`, `AsyncSession`,
`SessionExecutor`, timeline, and metrics observer. Only ASR/LLM/TTS provider
boundaries are faked. The native fixture exercises `FullDuplexConversation`
directly; it does not imply that a native `ConversationEngine` adapter exists.

| Cases | Assertions |
| --- | --- |
| `pipeline_partial_and_empty_input_do_not_delegate` | Partial/empty input produces no unintended model or speech work. |
| `pipeline_two_turns_have_separate_identity` | Separate response identities and ordered history, canonical event envelopes, success terminals, loss-free trace and metric cleanup. |
| `pipeline_model_failure_then_recovery` | Failed model work rolls back history; a subsequent request succeeds in the same session. |
| `pipeline_tts_failure_then_recovery` | Model success is distinguished from speech failure; later synthesis still works. |
| `pipeline_wrong_session_fails_before_work` | Mismatched session binding is rejected before provider work. |
| `managed_pipelines_are_isolated_on_shared_executor` | A blocked session does not block another lane; cancelling A leaves B's output/history intact; retirement drains resources. |
| `native_configuration_validation` | Invalid configuration is rejected before provider connection. |
| `native_server_and_client_commit_ownership` | Server/client speech-boundary ownership and explicit client commits are not conflated. |
| `native_bounded_input_and_provider_rejection` | Queue overload and provider rejection have distinct observable evidence. |
| `native_playback_ack_clamped_and_stale_packets_dropped` | Playback offsets are bounded and monotonic; cancellation uses audible position; stale response packets cannot reach output. |
| `native_completed_generation_still_has_unheard_playback` | Generation completion does not retire buffered playout; later acknowledgement and barge-in remain effective. |
| `native_fully_played_response_does_not_cancel_again` | Fully consumed responses do not receive spurious cancellation. |
| `native_rejected_audio_and_idempotent_close` | Output rejection cancels the response and repeated close performs one shutdown. |

## Real HTTP integration cases

`http_fixture.py` binds only to `127.0.0.1` on an OS-assigned port, starts the C++
client, and shuts down the server in `finally`. It uses Python's standard
library and a per-run request counter to verify retries and absence of unwanted
requests. It disables proxying for loopback, has a child-process timeout, and
does not log request headers, transcripts or bodies.

| Cases | Assertions |
| --- | --- |
| `real_http_round_trip_and_metrics` | Actual request/response bytes, headers and transport metric samples. |
| `retry_count_and_retry_after_cap`, `retry_budget_and_non_retryable_status` | Exact server request counts, capped retry delay, exhausted budget and non-retryable errors. |
| `streaming_status_body_is_not_replayed` | A failed status after body delivery is not transparently replayed. |
| `response_limits_apply_to_buffered_and_streaming` | Oversized responses are rejected for both transfer modes. |
| `sink_exception_and_rejection_stop_transfer` | Consumer exceptions cross the C callback boundary safely; rejection stops delivery. |
| `truncated_stream_is_not_success` | A short response relative to `Content-Length` fails instead of masquerading as completion. |
| `cancellation_before_io_and_during_retry` | Cancellation before transfer makes zero requests; cancellation during retry prevents the next request. |
| `timeout_and_redirect_policy` | A deliberately blocked response times out; redirects are not silently followed. |
| `secure_default_rejects_plain_http` | The default HTTPS-only setting prevents a plain HTTP request. Other local tests explicitly opt into insecure loopback HTTP. |
| `malformed_sse_fails_over_real_transport` | A parser failure is propagated through a real streaming transfer. |
| `agent_tool_loop_over_real_http_sse` | HTTP/SSE -> Agent -> Tool -> HTTP/SSE completes with one tool execution and coherent events/metrics. |
| `terminal_sse_cannot_dispatch_a_late_tool` | A tool call sent after `[DONE]` cannot execute a side effect. |

This is real socket/HTTP integration, not a claim of real-provider, WebSocket,
WebRTC, TLS-certificate, microphone, VAD or acoustic echo cancellation testing.

## Repetition, sanitizers and CI

```sh
ctest --test-dir build/cmake --output-on-failure --repeat until-fail:10 \
  -R 'component_unit_tests|conversation_integration_tests|http_transport_integration_tests|runtime_foundation_tests|runtime_manager_tests|interaction_control_tests'

cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/asan --parallel 2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan --output-on-failure
```

The [CI workflow](../.github/workflows/tests.yml) runs GCC and Clang with
ASan/UBSan, keeps the HTTP fixture enabled, repeats six suites ten times in the
GCC job, and runs `make test`. JUnit results, CTest logs, the source revision and
a tracked-source archive are retained for seven days to make failures
reproducible. These are executable checks, not proof that all races are absent;
TSan and device/network fault campaigns remain separate follow-up work.

Concurrency fixtures use explicit signals and futures, with finite safety
timeouts, rather than sleeping to guess readiness. Exact latency arithmetic
belongs to controlled-clock observability tests; integration tests do not assert
provider performance based on wall-clock timing on shared CI machines. Assertions
are made after workers/notifications drain and outside callbacks whose exceptions
are intentionally contained by the runtime.

## Regressions reproduced before fixes

Six new cases failed against the pre-change runtime and pass with four focused
fixes: SSE terminal fencing (unit and HTTP tests), UTF-8 boundary preservation
(two unit tests), rejecting TTS input after `finish()`, and retaining native
response state until buffered playback is consumed or cancelled. The tests are
not weakened to accept the old behavior. Runtime fixes are limited to these
reproduced defects; no new agent framework or provider integration is introduced.

Coverage percentages, real-world dialogue quality, hardware playout-to-silence
latency, vendor interoperability and production capacity are not claimed by this
suite. Physical output stopping still needs player/device acknowledgement.
