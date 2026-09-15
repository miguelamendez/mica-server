-- Runtime paths are relative to the model's curated repository in Miguel's HF namespace.
-- Setup installs runtimes only. The server downloads a selected artifact on first load.
-- vLLM artifacts are deliberately disabled until each device-specific runtime passes the
-- same real-inference smoke checks. vLLM is a serving backend, not a generic Q4/Q8 converter.
mica.settings {
  default_hf_repo = "miguelamendez/mica-server-catalog",
  llama_cpp_revision = "latest",
  audio_cpp_revision = "latest",
}

mica.model {
  id = "spark-x25-4b",
  capability = "text",
  description = "Spark-X2.5-4B text model; the audited official checkpoint contains no native MTP or DFlash drafter.",
  tags = {"text-generation", "no-native-mtp", "commercial-use", "apache-2.0"},
  source_repo = "XHToken/Spark-X2.5-4B",
  mlx_converter = "mlx_vlm.convert",
  mlx_repo = "miguelamendez/mica-spark-x25-4b-mlx",
  gguf_repo = "miguelamendez/mica-spark-x25-4b-gguf",
  priority = 10,
  required = true,
  mlx_q4_path = "q4",
  mlx_q4_ram_gib = 3.2,
  mlx_q8_path = "q8",
  mlx_q8_ram_gib = 5.4,
  gguf_supported = true,
  gguf_q4_path = "Spark-X2.5-4B-Q4_K_M.gguf",
  gguf_q4_ram_gib = 3.2,
  gguf_q8_path = "Spark-X2.5-4B-Q8_0.gguf",
  gguf_q8_ram_gib = 5.1,
  vllm_supported = false,
  vllm_reason = "Spark-X2.5 has not passed upstream vLLM or vLLM-Metal inference validation",
}

mica.model {
  id = "granite-speech-5",
  capability = "asr",
  description = "Granite Speech 5.0 470M TurboCTC automatic speech recognition model.",
  tags = {"automatic-speech-recognition", "audio", "commercial-use", "apache-2.0"},
  source_repo = "ibm-granite/granite-speech-5.0-470m-turboctc",
  mlx_converter = "mlx_audio.convert",
  mlx_quantization_profile = "granite-speech5-quality",
  mlx_repo = "miguelamendez/mica-granite-speech-5-mlx",
  gguf_repo = "miguelamendez/mica-granite-speech-5-gguf",
  priority = 20,
  required = true,
  mlx_q4_path = "q4",
  mlx_q4_ram_gib = 0.9,
  mlx_q8_path = "q8",
  mlx_q8_ram_gib = 1.2,
  gguf_supported = true,
  gguf_q4_path = "granite-speech-5.0-470m-turboctc-q4_k_m.gguf",
  gguf_q4_ram_gib = 0.9,
  gguf_q8_path = "granite-speech-5.0-470m-turboctc-q8_0.gguf",
  gguf_q8_ram_gib = 1.2,
  vllm_supported = false,
  vllm_reason = "Granite TurboCTC has not passed a vLLM transcription worker smoke test",
}

mica.model {
  id = "audio8-tts-06b",
  capability = "tts",
  description = "Audio8 TTS Preview 0.6B local text-to-speech model.",
  tags = {"text-to-speech", "audio", "commercial-use", "apache-2.0"},
  source_repo = "Audio8/Audio8-TTS-Preview-0.6b",
  mlx_converter = "mlx_audio.convert",
  mlx_quantization_profile = "audio8-quality",
  mlx_repo = "miguelamendez/mica-audio8-tts-06b-mlx",
  gguf_repo = "miguelamendez/mica-audio8-tts-06b-gguf",
  priority = 30,
  required = false,
  mlx_q4_path = "q4",
  mlx_q4_ram_gib = 1.2,
  mlx_q8_path = "q8",
  mlx_q8_ram_gib = 1.8,
  gguf_supported = true,
  gguf_q4_path = "audio8-tts-preview-0.6b-q4_k_m.gguf",
  gguf_q4_ram_gib = 1.2,
  gguf_q8_path = "audio8-tts-preview-0.6b-q8_0.gguf",
  gguf_q8_ram_gib = 1.8,
  vllm_supported = false,
  vllm_reason = "Audio8 TTS is not a validated vLLM or vLLM-Omni speech worker",
}

mica.model {
  id = "minicpm-v46-thinking",
  capability = "vision",
  description = "MiniCPM-V 4.6 Thinking multimodal model for image and video understanding.",
  tags = {"image-text-to-text", "video-text-to-text", "thinking", "commercial-use", "apache-2.0"},
  source_repo = "openbmb/MiniCPM-V-4.6-Thinking",
  mlx_converter = "mlx_vlm.convert",
  mlx_extract_mtp = true,
  mlx_repo = "miguelamendez/mica-minicpm-v46-thinking-mlx",
  gguf_repo = "miguelamendez/mica-minicpm-v46-thinking-gguf",
  priority = 40,
  required = false,
  mlx_q4_path = "q4",
  mlx_q4_ram_gib = 2.2,
  mlx_q8_path = "q8",
  mlx_q8_ram_gib = 3.2,
  gguf_supported = true,
  gguf_q4_path = "MiniCPM-V-4_6-Thinking-Q4_K_M.gguf",
  gguf_q4_projector = "mmproj-model-f16.gguf",
  gguf_q4_ram_gib = 2.2,
  gguf_q8_path = "MiniCPM-V-4_6-Thinking-Q8_0.gguf",
  gguf_q8_projector = "mmproj-model-f16.gguf",
  gguf_q8_ram_gib = 2.6,
  vllm_supported = false,
  vllm_reason = "Upstream vLLM supports MiniCPM-V 4.6, but vLLM-Metal does not list this vision family",
}

mica.profile {
  name = "all",
  quantization = "q4",
  models = {"spark-x25-4b", "granite-speech-5", "audio8-tts-06b", "minicpm-v46-thinking"},
}

mica.profile {
  name = "core",
  quantization = "q4",
  models = {"spark-x25-4b", "granite-speech-5"},
}

mica.profile {
  name = "quality",
  quantization = "q8",
  models = {"spark-x25-4b", "granite-speech-5", "audio8-tts-06b", "minicpm-v46-thinking"},
}
