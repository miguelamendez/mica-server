-- Legacy acceptance profile declarations. Model definitions now come from
-- config/model-manifests/*.yaml; engine definitions come from config/engines/*.yaml.
mica.settings {
  default_hf_repo = "miguelamendez/mica-server-catalog",
}

mica.profile {
  name = "all",
  catalog_visible = false,
  quantization = "q4",
  models = {"spark-x25-4b", "granite-speech-5", "audio8-tts-06b", "minicpm-v46-thinking"},
}

mica.profile {
  name = "core",
  catalog_visible = false,
  quantization = "q4",
  models = {"spark-x25-4b", "granite-speech-5"},
}

mica.profile {
  name = "quality",
  catalog_visible = false,
  quantization = "q8",
  models = {"spark-x25-4b", "granite-speech-5", "audio8-tts-06b", "minicpm-v46-thinking"},
}

-- Acceptance profiles are real selectable Mica profiles. The three context
-- tiers use the same public routes while binding one concrete backend and a
-- deterministic context/concurrency/KV policy. Modality-wide endpoint tests
-- continue to use `all`; these profiles isolate text context and batching.
mica.profile {
  name = "mlx-small",
  catalog_visible = false,
  backend = "mlx",
  quantization = "q4",
  models = {"spark-x25-4b"},
  max_input_tokens = 512,
  max_output_tokens = 256,
  max_total_tokens = 1024,
  max_concurrent_requests = 4,
  kv_cache_precision = "q8",
}

mica.profile {
  name = "mlx-medium",
  catalog_visible = false,
  backend = "mlx",
  quantization = "q4",
  models = {"spark-x25-4b"},
  max_input_tokens = 4096,
  max_output_tokens = 512,
  max_total_tokens = 4608,
  max_concurrent_requests = 2,
  kv_cache_precision = "q8",
}

mica.profile {
  name = "mlx-long",
  catalog_visible = false,
  backend = "mlx",
  quantization = "q4",
  models = {"spark-x25-4b"},
  max_input_tokens = 16384,
  max_output_tokens = 1024,
  max_total_tokens = 17408,
  max_concurrent_requests = 1,
  kv_cache_precision = "q4",
}

mica.profile {
  name = "gguf-small",
  catalog_visible = false,
  backend = "gguf",
  quantization = "q4",
  models = {"spark-x25-4b"},
  max_input_tokens = 512,
  max_output_tokens = 256,
  max_total_tokens = 1024,
  max_concurrent_requests = 4,
  kv_cache_precision = "q8",
}

mica.profile {
  name = "gguf-medium",
  catalog_visible = false,
  backend = "gguf",
  quantization = "q4",
  models = {"spark-x25-4b"},
  max_input_tokens = 4096,
  max_output_tokens = 512,
  max_total_tokens = 4608,
  max_concurrent_requests = 2,
  kv_cache_precision = "q8",
}

mica.profile {
  name = "gguf-long",
  catalog_visible = false,
  backend = "gguf",
  quantization = "q4",
  models = {"spark-x25-4b"},
  max_input_tokens = 16384,
  max_output_tokens = 1024,
  max_total_tokens = 17408,
  max_concurrent_requests = 1,
  kv_cache_precision = "q4",
}

mica.profile {
  name = "vllm-small",
  catalog_visible = false,
  backend = "vllm",
  quantization = "q4",
  models = {"vllm-qwen3-06b-control"},
  max_input_tokens = 512,
  max_output_tokens = 256,
  max_total_tokens = 1024,
  max_concurrent_requests = 4,
  kv_cache_precision = "auto",
}

mica.profile {
  name = "vllm-medium",
  catalog_visible = false,
  backend = "vllm",
  quantization = "q4",
  models = {"vllm-qwen3-06b-control"},
  max_input_tokens = 4096,
  max_output_tokens = 512,
  max_total_tokens = 4608,
  max_concurrent_requests = 2,
  kv_cache_precision = "auto",
}

mica.profile {
  name = "vllm-long",
  catalog_visible = false,
  backend = "vllm",
  quantization = "q4",
  models = {"vllm-qwen3-06b-control"},
  max_input_tokens = 16384,
  max_output_tokens = 1024,
  max_total_tokens = 17408,
  max_concurrent_requests = 1,
  kv_cache_precision = "auto",
}
