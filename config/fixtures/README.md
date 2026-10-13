# ASR warm-up fixture

`asr-warmup.wav` is mono 16-bit PCM at 16 kHz. It contains:

> Daddy, can you read me a bedtime story before I go to sleep?

It was resampled from this project's generated Audio8 Q8 test output,
`artifacts/benchmarks/audio8-gguf/q8_0-plain.wav`; provenance and generation
settings are in that directory's `manifest.json`. Audio8's upstream model is
Apache-2.0 licensed. This is synthetic speech, not a recording of a user.

The server uses this packaged fixture for actual ASR/diarization warm-up so a
normal installation does not need a separate speech synthesizer. It is a
startup smoke test, not a transcription or speaker-diarization quality dataset.
