# BytePlus speech providers

Status: HTTP streaming TTS implemented; streaming ASR pending official binary
protocol verification. No live supplier calls have been validated yet.

## Sources and scope

Checked the BytePlus Seed Speech documentation on 2026-09-24:

- [HTTP streaming TTS](https://docs.byteplus.com/en/docs/byteplusvoice/unidirectional_tts_http)
- [Bidirectional TTS](https://docs.byteplus.com/en/docs/byteplusvoice/streaming_tts)
- [Bidirectional ASR](https://docs.byteplus.com/en/docs/byteplusvoice/asrbidirect)
- [ASR streaming guide](https://docs.byteplus.com/en/docs/byteplusvoice/asrstreamingguide)

`BytePlusTts` implements the official HTTP streaming endpoint, not the Level 2
mock protocol. It reuses `HttpTransport` and the existing incremental TTS worker.
There are no new provider threads, queues, or lifecycle abstractions. Each
`synthesize_chunk` sends a complete text block and streams its audio; it does not
maintain the cross-block prosody/context of BytePlus bidirectional TTS sessions.

The provider selects PCM, mono, signed 16-bit little-endian output. Sample rate
defaults to 24000 Hz and is explicit on every output `AudioFrame`. It does not
resample or decode MP3/Opus. Choose a speaker enabled for the configured resource.

## Build and test

Requires the existing libcurl dependency plus nlohmann/json headers. The BytePlus
target is optional; the normal zero-setup Make build remains unchanged.

```sh
cmake -S . -B build-byteplus -DREALFLOW_BYTEPLUS=ON
cmake --build build-byteplus -j2
ctest --test-dir build-byteplus --output-on-failure

# Alternatively, JSON_INCLUDE_DIR is the directory containing nlohmann/.
make test-byteplus JSON_INCLUDE_DIR=/path/to/include
make build/byteplus_tts_demo JSON_INCLUDE_DIR=/path/to/include
```

The tests inject transport fragments and check request headers, streaming PCM
content and format, every two-fragment split, byte-at-a-time delivery, odd PCM
sample boundaries, terminal/error handling, bounds, cancellation and consumer
rejection. These are offline protocol tests, not proof of live account access.

## Run

Set `BYTEPLUS_API_KEY` and `BYTEPLUS_TTS_SPEAKER` in the environment using your
normal secret-management mechanism. `BYTEPLUS_TTS_RESOURCE_ID` defaults to
`seed-tts-2.0`. No credentials are stored in source or printed by the demo.

```sh
./build/byteplus_tts_demo 'Hello from RealFlow.' output.pcm
ffplay -f s16le -ar 24000 -ac 1 output.pcm
```

Running the demo calls the real service and may incur usage charges. The demo
refuses an existing output file. A failed request can leave partial PCM; only a
successful exit indicates completed synthesis.

## Runtime contract

The injected transport must outlive the provider and finish all callbacks before
`perform()` returns. Use `CurlHttpTransport` with HTTPS/TLS verification enabled,
finite timeouts, and `max_attempts = 1`: retrying a request after an ambiguous
network failure can incur another charge. Streaming errors are never silently
replayed. Configure the transport's response limit to match the provider's limit.

Serialize synthesis and `begin_utterance()` on the existing TTS worker. Only
`cancel()` may run concurrently. It sets an atomic flag without blocking; body
callbacks and curl's progress callback observe it. A callback already running can
finish, and stalled network cancellation latency depends on curl's progress
callback cadence/timeouts. Drain active synthesis before destruction.

`begin_utterance()` clears cancellation. `synthesize_chunk()` preserves it between
chunks; standalone `synthesize()` starts a new utterance. Consumer rejection
throws and stops further audio delivery for that request. The existing runtime
still owns stale-generation suppression and playback interruption.

The parser bounds total bytes, individual JSON objects, nesting depth and text
input. It handles arbitrary HTTP fragmentation and validates base64. Success
requires both HTTP 200 and the documented `20000000` terminal record; EOF alone
is never success. Service/parser errors omit response text. Already delivered
audio cannot be retracted when a later record fails.

Existing TTS first-audio/total metrics remain owned by the runtime. Pass its
`MetricsRegistry` to `CurlTransportConfig::metrics` for provider first-packet and
request timings. This change does not add another competing latency definition.

## ASR follow-up

The public docs identify `wss://voice.ap-southeast-1.bytepluses.com/api/v3/sauc/bigmodel_async`,
`X-Api-Key`, and resource `volc.seedasr.sauc.duration`. Unlike our mock protocol,
this endpoint only responds when recognition changes. A blocking read after
every audio write would prevent further input during silence.

The linked `sauc_python.zip` protocol implementation requires BytePlus sign-in.
It was not retrieved; ASR binary framing and final-sequence handling remain
unverified. Do not label the mock ASR adapter as BytePlus-compatible or implement
against the example's decoded JSON wrapper as though it were the wire format.
The next change must verify that protocol, preserve independent send/receive
progress, bound retained data, and test reset/finalization and delayed responses.
