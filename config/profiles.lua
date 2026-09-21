-- Runnable schema-3 engine, execution, placement, and residency catalog.
--
-- A task profile selects concrete execution profiles. Each execution fixes the
-- engine/backend, artifact quantization, context, batching, and KV-cache policy.
-- Each residency entry then defines startup, priority, pinning, and idle
-- eviction. Setup installs only engines referenced by the selected profile.
return {
  schema = 3,

  defaults = {
    validation = "certified",
    memory_safety_reserve_gib = 0.75,
    overload = "queue",
    eviction_policy = "weighted-lru",
  },

  -- An engine is the concrete executable/runtime. It is intentionally separate
  -- from the artifact format and model capability. Setup installs only engines
  -- referenced by the resolved execution profiles.
  engines = {
    ["mlx-lm"] = {
      status = "current", backend = "mlx", installer = "python-mlx",
      launcher = "mlx-lm", device_target = "mlx",
      hardware = {"apple-silicon"}, artifact_formats = {"mlx"},
    },
    ["mlx-vlm"] = {
      status = "current", backend = "mlx", installer = "python-mlx",
      launcher = "mlx-vlm", device_target = "mlx",
      hardware = {"apple-silicon"}, artifact_formats = {"mlx"},
    },
    ["mlx-audio"] = {
      status = "current", backend = "mlx", installer = "python-mlx",
      launcher = "mlx-audio", device_target = "mlx",
      hardware = {"apple-silicon"}, artifact_formats = {"mlx"},
    },
    ["llama-cpp"] = {
      status = "current", backend = "gguf", installer = "cmake-llama",
      launcher = "llama-server", device_target = "gguf",
      source_url = "https://github.com/ggml-org/llama.cpp.git",
      revision = "latest", runtime_directory = "llama.cpp",
      server_executable = "build-mica/bin/llama-server",
      quantizer_executable = "build-mica/bin/llama-quantize",
      build_targets = {"llama-server", "llama-quantize"},
      version_arguments = {"--version"},
      hardware = {"cpu", "metal", "cuda", "rocm", "sycl", "vulkan"},
      artifact_formats = {"gguf"},
    },
    ["audio-cpp"] = {
      status = "current", backend = "gguf", installer = "cmake-audio",
      launcher = "audio-server", device_target = "audio",
      source_url = "https://github.com/0xShug0/audio.cpp.git",
      revision = "latest", runtime_directory = "audio.cpp",
      server_executable = "build-mica/bin/audiocpp_server",
      quantizer_executable = "build-mica/bin/audiocpp_gguf",
      build_targets = {"audiocpp_server", "audiocpp_gguf"},
      version_arguments = {"--help"},
      hardware = {"cpu", "metal", "cuda", "rocm", "vulkan"},
      artifact_formats = {"gguf"},
    },
    ["prism-llama-cpp"] = {
      status = "candidate", backend = "gguf", installer = "cmake-llama",
      launcher = "llama-server", device_target = "gguf",
      source_url = "https://github.com/PrismML-Eng/llama.cpp.git",
      revision = "9a9394a895b96003ca842a6041cb28ac49a108f7",
      runtime_directory = "prism-llama.cpp",
      server_executable = "build-mica/bin/llama-server",
      build_targets = {"llama-server"}, version_arguments = {"--version"},
      hardware = {"cpu", "metal", "cuda", "rocm", "sycl", "vulkan"},
      artifact_formats = {"gguf"},
    },
    ["vllm"] = {
      status = "current", backend = "vllm", installer = "python-vllm",
      launcher = "vllm", device_target = "vllm",
      hardware = {"cpu", "cuda", "rocm", "xpu", "tpu", "metal"},
      artifact_formats = {"safetensors", "compressed-tensors", "mlx"},
    },

    -- Future text-to-music/lyrics-to-song engines. These declarations document
    -- accepted extension points; no current setup or server path installs them.
    ["sa3-mlx"] = {
      status = "planned", install = "engine-environment",
      hardware = {"apple-silicon"}, artifact_formats = {"mlx"},
    },
    ["sa3-tflite"] = {
      status = "planned", install = "native-runtime",
      hardware = {"macos-cpu", "linux-cpu", "windows-cpu"},
      artifact_formats = {"tflite"},
    },
    ["sa3-tensorrt"] = {
      status = "planned", install = "native-runtime",
      hardware = {"nvidia"}, artifact_formats = {"tensorrt"},
    },
    ["diffrhythm-pytorch"] = {
      status = "planned", install = "engine-environment",
      hardware = {"cuda"}, artifact_formats = {"safetensors", "pytorch"},
    },
  },

  hardware_selection = {
    -- The global user hard limit always wins. These tiers select behavior; they
    -- do not replace the limit or promise that an uncertified model fits.
    {maximum_budget_gib = 4, profile = "low-memory"},
    {maximum_budget_gib = 8, profile = "interactive"},
    {maximum_budget_gib = 12, profile = "balanced-all"},
    {maximum_budget_gib = 16, profile = "quality-interactive"},
  },

  execution_profiles = {
    ["spark-balanced-single"] = {
      model = "spark-x25-4b", engine = "mlx-lm", status = "candidate",
      artifact = {format = "mlx", quantization = "q4"},
      context = {max_input_tokens = 7168, max_output_tokens = 1024,
                 max_total_tokens = 8192},
      thinking = {enabled = true, max_reasoning_tokens = 768,
                  min_visible_output_tokens = 256},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 16},
      kv_cache = {precision = "q8", scheme = "uniform", group_size = 64,
                  quantize_after_tokens = 0, max_tokens = 8192},
    },

    ["spark-quality-single"] = {
      model = "spark-x25-4b", engine = "mlx-lm", status = "candidate",
      artifact = {format = "mlx", quantization = "q8"},
      context = {max_input_tokens = 7168, max_output_tokens = 1024,
                 max_total_tokens = 8192},
      thinking = {enabled = true, max_reasoning_tokens = 768,
                  min_visible_output_tokens = 256},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 16},
      kv_cache = {precision = "q8", scheme = "uniform", group_size = 64,
                  quantize_after_tokens = 0, max_tokens = 8192},
    },

    ["spark-capacity-long"] = {
      model = "spark-x25-4b", engine = "mlx-lm", status = "experimental",
      artifact = {format = "mlx", quantization = "q4"},
      context = {max_input_tokens = 7168, max_output_tokens = 1024,
                 max_total_tokens = 8192},
      thinking = {enabled = true, max_reasoning_tokens = 1536,
                  min_visible_output_tokens = 512},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 8},
      kv_cache = {precision = "q4", scheme = "uniform", group_size = 64,
                  quantize_after_tokens = 0, max_tokens = 8192},
    },

    ["spark-throughput-4"] = {
      model = "spark-x25-4b", engine = "mlx-lm", status = "experimental",
      artifact = {format = "mlx", quantization = "q4"},
      context = {max_input_tokens = 3584, max_output_tokens = 512,
                 max_total_tokens = 4096},
      thinking = {enabled = true, max_reasoning_tokens = 256,
                  min_visible_output_tokens = 256},
      batching = {max_concurrent_requests = 4, max_batch_tokens = 16384,
                  max_queued_requests = 64},
      kv_cache = {precision = "q4", scheme = "uniform", group_size = 64,
                  quantize_after_tokens = 0, max_tokens = 4096},
    },

    ["granite-streaming-single"] = {
      model = "granite-speech-5", engine = "mlx-audio", status = "candidate",
      artifact = {format = "mlx", quantization = "q4"},
      audio = {max_audio_seconds = 30, chunk_seconds = 5, overlap_seconds = 0.5},
      batching = {max_concurrent_requests = 1, max_queued_requests = 32},
      kv_cache = {precision = "not-applicable"},
    },

    ["granite-batch-4"] = {
      model = "granite-speech-5", engine = "mlx-audio", status = "experimental",
      artifact = {format = "mlx", quantization = "q8"},
      audio = {max_audio_seconds = 30, chunk_seconds = 30, overlap_seconds = 0},
      batching = {max_concurrent_requests = 4, max_queued_requests = 64},
      kv_cache = {precision = "not-applicable"},
    },

    ["audio8-balanced-single"] = {
      model = "audio8-tts-06b", engine = "mlx-audio", status = "candidate",
      artifact = {format = "mlx", quantization = "q8"},
      context = {max_input_tokens = 1024, max_output_tokens = 1024,
                 max_total_tokens = 2048, unit = "packed-text-audio-positions"},
      voice_cloning = {enabled = true, max_reference_seconds = 15},
      batching = {max_concurrent_requests = 1, max_queued_requests = 16},
      kv_cache = {precision = "runtime-managed"},
    },

    ["audio8-capacity-single"] = {
      model = "audio8-tts-06b", engine = "mlx-audio", status = "candidate",
      artifact = {format = "mlx", quantization = "q4"},
      context = {max_input_tokens = 1024, max_output_tokens = 1024,
                 max_total_tokens = 2048, unit = "packed-text-audio-positions"},
      voice_cloning = {enabled = true, max_reference_seconds = 15},
      batching = {max_concurrent_requests = 1, max_queued_requests = 16},
      kv_cache = {precision = "runtime-managed"},
    },

    ["minicpm-vision-quality"] = {
      model = "minicpm-v46-thinking", engine = "mlx-vlm", status = "candidate",
      artifact = {format = "mlx", quantization = "q8"},
      context = {max_input_tokens = 6144, max_output_tokens = 2048,
                 max_total_tokens = 8192},
      thinking = {enabled = true, max_reasoning_tokens = 1536,
                  min_visible_output_tokens = 512},
      media = {max_images = 4, max_image_slices = 9, max_video_frames = 32,
               downsample_mode = "16x"},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 8},
      kv_cache = {precision = "q8", scheme = "uniform", group_size = 64,
                  quantize_after_tokens = 0, max_tokens = 8192},
    },

    ["minicpm-vision-capacity"] = {
      model = "minicpm-v46-thinking", engine = "mlx-vlm", status = "candidate",
      artifact = {format = "mlx", quantization = "q4"},
      context = {max_input_tokens = 6144, max_output_tokens = 2048,
                 max_total_tokens = 8192},
      thinking = {enabled = true, max_reasoning_tokens = 1536,
                  min_visible_output_tokens = 512},
      media = {max_images = 4, max_image_slices = 9, max_video_frames = 32,
               downsample_mode = "16x"},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 8},
      kv_cache = {precision = "q4", scheme = "uniform", group_size = 64,
                  quantize_after_tokens = 0, max_tokens = 8192},
    },

    ["spark-gguf-balanced-single"] = {
      model = "spark-x25-4b", engine = "llama-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q4"},
      context = {max_input_tokens = 7168, max_output_tokens = 1024,
                 max_total_tokens = 8192},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 16},
      kv_cache = {precision = "q8", scheme = "uniform", max_tokens = 8192},
    },

    ["spark-gguf-quality-single"] = {
      model = "spark-x25-4b", engine = "llama-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q8"},
      context = {max_input_tokens = 7168, max_output_tokens = 1024,
                 max_total_tokens = 8192},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 16},
      kv_cache = {precision = "q8", scheme = "uniform", max_tokens = 8192},
    },

    ["spark-gguf-capacity-long"] = {
      model = "spark-x25-4b", engine = "llama-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q4"},
      context = {max_input_tokens = 7168, max_output_tokens = 1024,
                 max_total_tokens = 8192},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 8},
      kv_cache = {precision = "q4", scheme = "uniform", max_tokens = 8192},
    },

    ["spark-gguf-throughput-4"] = {
      model = "spark-x25-4b", engine = "llama-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q4"},
      context = {max_input_tokens = 3584, max_output_tokens = 512,
                 max_total_tokens = 4096},
      batching = {max_concurrent_requests = 4, max_batch_tokens = 16384,
                  max_queued_requests = 64},
      kv_cache = {precision = "q4", scheme = "uniform", max_tokens = 4096},
    },

    ["granite-gguf-streaming-single"] = {
      model = "granite-speech-5", engine = "audio-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q4"},
      audio = {max_audio_seconds = 30, chunk_seconds = 5, overlap_seconds = 0.5},
      batching = {max_concurrent_requests = 1, max_queued_requests = 32},
      kv_cache = {precision = "not-applicable"},
    },

    ["granite-gguf-batch-4"] = {
      model = "granite-speech-5", engine = "audio-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q8"},
      audio = {max_audio_seconds = 30, chunk_seconds = 30, overlap_seconds = 0},
      batching = {max_concurrent_requests = 4, max_queued_requests = 64},
      kv_cache = {precision = "not-applicable"},
    },

    ["audio8-gguf-balanced-single"] = {
      model = "audio8-tts-06b", engine = "audio-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q8"},
      context = {max_input_tokens = 1024, max_output_tokens = 1024,
                 max_total_tokens = 2048},
      batching = {max_concurrent_requests = 1, max_queued_requests = 16},
      kv_cache = {precision = "runtime-managed"},
    },

    ["audio8-gguf-capacity-single"] = {
      model = "audio8-tts-06b", engine = "audio-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q4"},
      context = {max_input_tokens = 1024, max_output_tokens = 1024,
                 max_total_tokens = 2048},
      batching = {max_concurrent_requests = 1, max_queued_requests = 16},
      kv_cache = {precision = "runtime-managed"},
    },

    ["minicpm-gguf-vision-quality"] = {
      model = "minicpm-v46-thinking", engine = "llama-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q8"},
      context = {max_input_tokens = 6144, max_output_tokens = 2048,
                 max_total_tokens = 8192},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 8},
      kv_cache = {precision = "q8", scheme = "uniform", max_tokens = 8192},
    },

    ["minicpm-gguf-vision-capacity"] = {
      model = "minicpm-v46-thinking", engine = "llama-cpp", status = "current",
      artifact = {format = "gguf", quantization = "q4"},
      context = {max_input_tokens = 6144, max_output_tokens = 2048,
                 max_total_tokens = 8192},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 8},
      kv_cache = {precision = "q4", scheme = "uniform", max_tokens = 8192},
    },

    ["bonsai-pq2-vision"] = {
      model = "ternary-bonsai-2-27b", engine = "prism-llama-cpp",
      status = "candidate",
      artifact = {format = "gguf", quantization = "pq2_0"},
      context = {max_input_tokens = 6144, max_output_tokens = 2048,
                 max_total_tokens = 8192},
      batching = {max_concurrent_requests = 1, max_batch_tokens = 8192,
                  max_queued_requests = 4},
      kv_cache = {precision = "q8", scheme = "uniform", max_tokens = 8192},
    },

    ["qwen-vllm-control"] = {
      model = "vllm-qwen3-06b-control", engine = "vllm", status = "current",
      artifact = {format = "mlx", quantization = "q4"},
      context = {max_input_tokens = 512, max_output_tokens = 256,
                 max_total_tokens = 1024},
      batching = {max_concurrent_requests = 4, max_batch_tokens = 4096,
                  max_queued_requests = 32},
      kv_cache = {precision = "auto"},
    },

    -- Future additions. Each profile pins one engine so its memory, quality,
    -- and latency certification cannot silently change with fallback behavior.
    ["sa3-small-music-apple"] = {
      model = "stable-audio-3-small-music", engine = "sa3-mlx",
      status = "planned",
      artifact = {format = "mlx", quantization = "upstream-optimized"},
      audio = {max_output_seconds = 120, channels = 2,
               modes = {"text-to-audio", "audio-to-audio", "inpainting"}},
      batching = {max_concurrent_requests = 1, max_queued_requests = 4},
      kv_cache = {precision = "not-applicable"},
    },

    ["sa3-small-music-native"] = {
      model = "stable-audio-3-small-music", engine = "audio-cpp",
      status = "planned", engine_features = {"stable_audio"},
      artifact = {format = "gguf", quantization = "q8"},
      audio = {max_output_seconds = 120, channels = 2,
               modes = {"text-to-audio", "audio-to-audio", "inpainting"}},
      batching = {max_concurrent_requests = 1, max_queued_requests = 4},
      kv_cache = {precision = "not-applicable"},
    },

    ["sa3-small-music-cpu"] = {
      model = "stable-audio-3-small-music", engine = "sa3-tflite",
      status = "planned",
      artifact = {format = "tflite", quantization = "upstream-optimized"},
      audio = {max_output_seconds = 120, channels = 2,
               modes = {"text-to-audio", "audio-to-audio", "inpainting"}},
      batching = {max_concurrent_requests = 1, max_queued_requests = 4},
      kv_cache = {precision = "not-applicable"},
    },

    ["sa3-small-music-nvidia"] = {
      model = "stable-audio-3-small-music", engine = "sa3-tensorrt",
      status = "planned",
      artifact = {format = "tensorrt", quantization = "upstream-optimized"},
      audio = {max_output_seconds = 120, channels = 2,
               modes = {"text-to-audio", "audio-to-audio", "inpainting"}},
      batching = {max_concurrent_requests = 1, max_queued_requests = 4},
      kv_cache = {precision = "not-applicable"},
    },

    ["sa3-medium-music-nvidia"] = {
      model = "stable-audio-3-medium", engine = "sa3-tensorrt",
      status = "planned",
      artifact = {format = "tensorrt", quantization = "upstream-optimized"},
      audio = {max_output_seconds = 380, channels = 2,
               modes = {"text-to-audio", "audio-to-audio", "inpainting"}},
      batching = {max_concurrent_requests = 1, max_queued_requests = 2},
      kv_cache = {precision = "not-applicable"},
    },

    ["sa3-medium-music-native"] = {
      model = "stable-audio-3-medium", engine = "audio-cpp",
      status = "planned", engine_features = {"stable_audio"},
      artifact = {format = "gguf", quantization = "q8"},
      audio = {max_output_seconds = 380, channels = 2,
               modes = {"text-to-audio", "audio-to-audio", "inpainting"}},
      batching = {max_concurrent_requests = 1, max_queued_requests = 2},
      kv_cache = {precision = "not-applicable"},
    },

    ["diffrhythm-lyrics-cuda"] = {
      model = "diffrhythm-1.2-full", engine = "diffrhythm-pytorch",
      status = "planned",
      artifact = {format = "safetensors", quantization = "native"},
      audio = {max_output_seconds = 285, channels = 2,
               modes = {"lyrics-to-song", "reference-audio-conditioning"}},
      batching = {max_concurrent_requests = 1, max_queued_requests = 2},
      kv_cache = {precision = "not-applicable"},
    },
  },

  -- Setup resolves an execution set once from the hardware profile, persists
  -- the concrete execution profile, then installs only that profile's engine.
  execution_sets = {
    ["music-default"] = {
      capability = "text-to-music",
      choices = {
        {when = {apple_silicon = true}, execution = "sa3-small-music-apple"},
        {when = {accelerator_vendor = "nvidia"}, execution = "sa3-small-music-nvidia"},
        {when = {cpu = true}, execution = "sa3-small-music-cpu"},
      },
    },
    ["music-quality-nvidia"] = {
      capability = "text-to-music",
      choices = {
        {when = {accelerator_vendor = "nvidia"}, execution = "sa3-medium-music-nvidia"},
      },
    },
    ["music-portable-native"] = {
      capability = "text-to-music",
      choices = {
        {when = {audio_cpp_supported = true}, execution = "sa3-small-music-native"},
      },
    },
    ["lyrics-to-song-nvidia"] = {
      capability = "lyrics-to-song",
      choices = {
        {when = {accelerator_runtime = "cuda"}, execution = "diffrhythm-lyrics-cuda"},
      },
    },
  },

  residency_profiles = {
    -- Recommended Apple Silicon assistant. Every selected execution uses an
    -- MLX artifact and engine; setup therefore installs no GGUF or vLLM stack.
    ["mica-assistant-mlx"] = {
      mode = "interactive",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-balanced-single",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-capacity-single",
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-capacity",
         residency = "on-demand", priority = 40, startup = false, idle_seconds = 30},
      },
    },

    interactive = {
      mode = "interactive",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-balanced-single",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-capacity-single",
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-capacity",
         residency = "on-demand", priority = 40, startup = false, idle_seconds = 30},
      },
    },

    ["quality-interactive"] = {
      mode = "interactive",
      maximum_ram_gib = 16,
      memory_safety_reserve_gib = 0.75,
      models = {
        {id = "spark-x25-4b", execution = "spark-quality-single",
         residency = "warm", priority = 100, startup = true, idle_seconds = 600},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-balanced-single",
         residency = "warm", priority = 80, startup = true, idle_seconds = 300},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-quality",
         residency = "on-demand", priority = 70, startup = false, idle_seconds = 60},
      },
    },

    ["balanced-all"] = {
      mode = "interactive",
      maximum_ram_gib = 12,
      memory_safety_reserve_gib = 0.75,
      models = {
        {id = "spark-x25-4b", execution = "spark-balanced-single",
         residency = "warm", priority = 100, startup = true, idle_seconds = 600},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-balanced-single",
         residency = "warm", priority = 80, startup = true, idle_seconds = 300},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-quality",
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
      },
    },

    ["text-batch"] = {
      mode = "batch",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-throughput-4",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-batch-4",
         residency = "ephemeral", priority = 40, startup = false},
        {id = "audio8-tts-06b", execution = "audio8-capacity-single",
         residency = "ephemeral", priority = 30, startup = false},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-capacity",
         residency = "ephemeral", priority = 20, startup = false},
      },
    },

    ["long-context"] = {
      mode = "long-context",
      maximum_ram_gib = 12,
      memory_safety_reserve_gib = 0.75,
      models = {
        {id = "spark-x25-4b", execution = "spark-capacity-long",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "ephemeral", priority = 30, startup = false},
        {id = "audio8-tts-06b", execution = "audio8-capacity-single",
         residency = "ephemeral", priority = 20, startup = false},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-capacity",
         residency = "ephemeral", priority = 10, startup = false},
      },
    },

    ["realtime-voice"] = {
      mode = "realtime",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      atomic_groups = {{"granite-speech-5", "spark-x25-4b", "audio8-tts-06b"}},
      models = {
        {id = "spark-x25-4b", execution = "spark-balanced-single",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "pinned", priority = 100, startup = true},
        {id = "audio8-tts-06b", execution = "audio8-capacity-single",
         residency = "pinned", priority = 100, startup = true},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-capacity",
         residency = "ephemeral", priority = 10, startup = false},
      },
    },

    ["vision-quality"] = {
      mode = "vision",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-quality",
         residency = "pinned", priority = 100, startup = true},
        {id = "spark-x25-4b", execution = "spark-balanced-single",
         residency = "on-demand", priority = 60, startup = false, idle_seconds = 60},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "ephemeral", priority = 30, startup = false},
        {id = "audio8-tts-06b", execution = "audio8-capacity-single",
         residency = "ephemeral", priority = 20, startup = false},
      },
    },

    ["low-memory"] = {
      mode = "exclusive",
      maximum_ram_gib = 4,
      memory_safety_reserve_gib = 0.25,
      maximum_resident_workers = 1,
      models = {
        {id = "spark-x25-4b", execution = "spark-balanced-single",
         residency = "ephemeral", priority = 100, startup = false},
        {id = "granite-speech-5", execution = "granite-streaming-single",
         residency = "ephemeral", priority = 90, startup = false},
        {id = "audio8-tts-06b", execution = "audio8-capacity-single",
         residency = "ephemeral", priority = 80, startup = false},
        {id = "minicpm-v46-thinking", execution = "minicpm-vision-capacity",
         residency = "ephemeral", priority = 70, startup = false},
      },
    },

    ["gguf-interactive"] = {
      mode = "interactive",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-balanced-single",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         residency = "on-demand", priority = 40, startup = false, idle_seconds = 30},
      },
    },

    -- Recommended portable native assistant. Every selected execution uses a
    -- GGUF artifact and either llama.cpp or audio.cpp. Python is not installed.
    ["mica-assistant-gguf"] = {
      mode = "interactive",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-balanced-single",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         residency = "on-demand", priority = 40, startup = false, idle_seconds = 30},
      },
    },

    -- Explicit processor-placement profiles used for heterogeneous host
    -- validation. accelerator:0 resolves to the detected runtime (CUDA,
    -- ROCm, XPU, or Metal) while preserving the physical device id.
    ["mica-assistant-gguf-cpu"] = {
      mode = "interactive-cpu",
      maximum_ram_gib = 16,
      maximum_vram_gib = 0,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-balanced-single",
         placement = {mode = "fixed", device = "cpu", gpu_layers = 0},
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         placement = {mode = "fixed", device = "cpu", gpu_layers = 0},
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         placement = {mode = "fixed", device = "cpu", gpu_layers = 0},
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         placement = {mode = "fixed", device = "cpu", gpu_layers = 0},
         residency = "on-demand", priority = 40, startup = false, idle_seconds = 30},
      },
    },

    ["mica-assistant-gguf-gpu"] = {
      mode = "interactive-gpu",
      maximum_ram_gib = 8,
      maximum_vram_gib = 16,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-balanced-single",
         placement = {mode = "fixed", device = "accelerator:0", gpu_layers = 99,
                      ram_reservation_gib = 0.5, vram_reservation_gib = 4.8},
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         placement = {mode = "fixed", device = "accelerator:0", gpu_layers = 99,
                      ram_reservation_gib = 0.5, vram_reservation_gib = 0.9},
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         placement = {mode = "fixed", device = "accelerator:0", gpu_layers = 99,
                      ram_reservation_gib = 0.5, vram_reservation_gib = 1.6},
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         placement = {mode = "fixed", device = "accelerator:0", gpu_layers = 99,
                      ram_reservation_gib = 0.5, vram_reservation_gib = 2.2},
         residency = "on-demand", priority = 40, startup = false, idle_seconds = 30},
      },
    },

    ["mica-assistant-gguf-mixed"] = {
      mode = "interactive-mixed",
      maximum_ram_gib = 8,
      maximum_vram_gib = 12,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-balanced-single",
         placement = {mode = "fixed", device = "accelerator:0", gpu_layers = 99,
                      ram_reservation_gib = 0.5, vram_reservation_gib = 4.8},
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         placement = {mode = "fixed", device = "cpu", gpu_layers = 0},
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         placement = {mode = "fixed", device = "cpu", gpu_layers = 0},
         residency = "warm", priority = 70, startup = true, idle_seconds = 180},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         placement = {mode = "fixed", device = "accelerator:0", gpu_layers = 99,
                      ram_reservation_gib = 0.5, vram_reservation_gib = 2.2},
         residency = "on-demand", priority = 40, startup = false, idle_seconds = 30},
      },
    },

    ["gguf-quality-interactive"] = {
      mode = "interactive",
      maximum_ram_gib = 16,
      memory_safety_reserve_gib = 0.75,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-quality-single",
         residency = "warm", priority = 100, startup = true, idle_seconds = 600},
        {id = "granite-speech-5", execution = "granite-gguf-batch-4",
         residency = "warm", priority = 90, startup = true, idle_seconds = 600},
        {id = "audio8-tts-06b", execution = "audio8-gguf-balanced-single",
         residency = "warm", priority = 80, startup = true, idle_seconds = 300},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-quality",
         residency = "on-demand", priority = 70, startup = false, idle_seconds = 60},
      },
    },

    ["gguf-text-batch"] = {
      mode = "batch",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-throughput-4",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         residency = "ephemeral", priority = 40, startup = false},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         residency = "ephemeral", priority = 30, startup = false},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         residency = "ephemeral", priority = 20, startup = false},
      },
    },

    ["gguf-long-context"] = {
      mode = "long-context",
      maximum_ram_gib = 12,
      memory_safety_reserve_gib = 0.75,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-capacity-long",
         residency = "pinned", priority = 100, startup = true},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         residency = "ephemeral", priority = 30, startup = false},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         residency = "ephemeral", priority = 20, startup = false},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         residency = "ephemeral", priority = 10, startup = false},
      },
    },

    ["gguf-low-memory"] = {
      mode = "exclusive",
      maximum_ram_gib = 6,
      memory_safety_reserve_gib = 0.25,
      maximum_resident_workers = 1,
      models = {
        {id = "spark-x25-4b", execution = "spark-gguf-balanced-single",
         residency = "ephemeral", priority = 100, startup = false},
        {id = "granite-speech-5", execution = "granite-gguf-streaming-single",
         residency = "ephemeral", priority = 90, startup = false},
        {id = "audio8-tts-06b", execution = "audio8-gguf-capacity-single",
         residency = "ephemeral", priority = 80, startup = false},
        {id = "minicpm-v46-thinking", execution = "minicpm-gguf-vision-capacity",
         residency = "ephemeral", priority = 70, startup = false},
      },
    },

    ["vllm-control"] = {
      mode = "validation",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "vllm-qwen3-06b-control", execution = "qwen-vllm-control",
         residency = "pinned", priority = 100, startup = true},
      },
    },

    ["bonsai-pq2-vision-cpu"] = {
      mode = "vision-cpu-validation",
      maximum_ram_gib = 16,
      maximum_vram_gib = 0,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "ternary-bonsai-2-27b", execution = "bonsai-pq2-vision",
         placement = {mode = "fixed", device = "cpu", gpu_layers = 0},
         residency = "on-demand", priority = 100, startup = false,
         idle_seconds = 30},
      },
    },

    ["bonsai-pq2-vision-gpu"] = {
      mode = "vision-gpu-validation",
      maximum_ram_gib = 8,
      maximum_vram_gib = 16,
      memory_safety_reserve_gib = 0.5,
      models = {
        {id = "ternary-bonsai-2-27b", execution = "bonsai-pq2-vision",
         placement = {mode = "fixed", device = "accelerator:0", gpu_layers = 99,
                      ram_reservation_gib = 0.75,
                      vram_reservation_gib = 12.0},
         residency = "on-demand", priority = 100, startup = false,
         idle_seconds = 30},
      },
    },
  },
}
