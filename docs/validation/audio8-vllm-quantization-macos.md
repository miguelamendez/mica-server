# Audio8 vLLM candidate generation on Apple M4

Date: 2026-09-17  
Host: Apple M4, 24 GiB unified memory  
Status: Q4/Q8 TTS calibration, reload, voice-reference, and batch smoke passed;
production calibration and native vLLM-Omni validation pending

## Policy and calibration

GPTQ W4A16/W8A16 uses group size 128 and keeps embeddings, acoustic codebook
embeddings, fast-decoder boundaries, and the waveform decoder at source
precision. The real two-record manifest includes one plain prompt and one
44.1-kHz zero-shot voice-reference prompt.

Both conversions completed all 24 slow-transformer layers and selected 140
linear modules. Their peak RSS was 6.98 GiB for Q4 and 6.99 GiB for Q8. With
only two samples, many Hessians were numerically unstable and LLM Compressor
fell back to round-to-nearest. These are structural artifacts, not quality
candidates.

## Real batch inference

Each test generated two 1.115-second waveforms (24 code frames) in one
Transformers call.

| Candidate | Mode | Wall time | Aggregate RTF | Peak RSS |
|---|---|---:|---:|---:|
| W4A16 | plain | 5.48 s | 2.46 | 3.88 GiB |
| W4A16 | voice reference | 16.76 s | 7.52 | 8.34 GiB |
| W8A16 | plain | 5.36 s | 2.41 | 4.39 GiB |
| W8A16 | voice reference | 18.72 s | 8.40 | 8.53 GiB |

The generated WAV files were non-empty. Listening quality and speaker
similarity were not scored in this pass. Transformers decompresses the packed
weights and is not a vLLM-Omni performance measurement.

## Remaining gates

1. Build the 512-record multilingual production manifest with balanced plain,
   designed-voice, and reference-voice cases.
2. Remove all GPTQ Hessian fallbacks and compare intelligibility, speaker
   similarity, prosody, and word separation with BF16.
3. Load the artifact in native vLLM-Omni on Linux/NVIDIA and run single,
   continuous-batch, latency, and memory tests.
4. Publish only after the audio quality and runtime acceptance fields pass.
