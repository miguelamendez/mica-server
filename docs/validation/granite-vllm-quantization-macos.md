# Granite Speech 5 vLLM candidate generation on Apple M4

Date: 2026-09-17  
Host: Apple M4, 24 GiB unified memory  
Status: Q4/Q8 CTC calibration, reload, and batch smoke passed; vLLM serving
remains blocked by the absence of a native Granite Speech 5 loader

## Toolchain and protected layers

Transformers 5.17.0 recognizes `GraniteSpeech5ForCTC`; the compatible
quantization environment uses LLM Compressor 0.13.1a20260824 and
compressed-tensors 0.18.1a20260914. The stable LLM Compressor release cannot
coexist with this Transformers version.

The input projection, midpoint feedback projection, encoder output projection,
and tied `ctc_head` stay at source precision. An initial Q4 reload failed
because `ctc_head` had been packed while Transformers still treated it as a
tied output weight. Protecting both sides corrected the artifact.

## Structural and real inference results

The structural manifest contains one real 1.584-second, 16-kHz waveform.

| Candidate | Checkpoint | Conversion peak | Batch-2 time | Aggregate RTF | Inference peak |
|---|---:|---:|---:|---:|---:|
| W4A16 | 286 MiB | 2.93 GiB | 2.07 s | 0.654 | 1.57 GiB |
| W8A16 | 494 MiB | 3.09 GiB | 2.39 s | 0.755 | 1.78 GiB |

Both streams in both batches decoded `mic a warm up transcription`, exactly
matching the full-precision checkpoint on this fixture. The full-precision
single-stream reference took 0.887 s (RTF 0.560); compressed Q4 single-stream
took 1.478 s (RTF 0.933). The compressed Transformers path is slower because
it decompresses weights and is not a native serving kernel.

## Remaining gates

1. Build the 512-record production audio calibration and held-out WER set with
   varied duration, accents, noise, and silence.
2. Compare Q4/Q8 WER and long-form chunking with BF16.
3. Add or adopt a native Granite Speech 5 CTC vLLM loader and transcription
   route, then run continuous batching and memory tests on target hardware.
4. Do not publish a vLLM-served artifact while the loader gate is blocked.
