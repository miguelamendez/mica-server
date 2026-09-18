#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include "mica_server/catalog.hpp"
#include "mica_server/config.hpp"
#include "mica_server/scheduler.hpp"

int main() {
  const auto config = std::filesystem::path(__FILE__).parent_path().parent_path() / "config";
  const auto registry = mica::load_registry(config);
  const auto& all = registry.profile("all");
  const auto& spark = registry.model("spark-x25-4b");
  assert(spark.repositories.at(mica::Backend::mlx) ==
         "miguelamendez/mica-spark-x25-4b");
  assert(spark.repositories.at(mica::Backend::gguf) ==
         "miguelamendez/mica-spark-x25-4b");
  assert(!spark.description.empty());
  assert(!spark.tags.empty());
  assert(spark.gguf_context_tokens == 8192);
  assert(spark.gguf_parallel_slots == 4);
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .repository_pattern == "mlx/q4");
  const auto& gguf_long = registry.profile("gguf-long");
  assert(gguf_long.backend == mica::Backend::gguf);
  assert(gguf_long.max_input_tokens == 16384);
  assert(gguf_long.max_total_tokens == 17408);
  assert(gguf_long.max_concurrent_requests == 1);
  assert(gguf_long.kv_cache_precision == "q4");
  const auto& interactive = registry.profile("interactive");
  assert(interactive.schema == 2);
  assert(interactive.maximum_ram_gib == 8.0);
  assert(interactive.memory_safety_reserve_gib == 0.5);
  assert(interactive.model_policies.size() == 4);
  const auto* interactive_text = interactive.policy_for("spark-x25-4b");
  assert(interactive_text != nullptr);
  assert(interactive_text->engine == "mlx-lm");
  assert(interactive_text->backend == mica::Backend::mlx);
  assert(interactive_text->quantization == mica::Quantization::q4);
  assert(interactive_text->residency == mica::Residency::pinned);
  assert(interactive_text->startup);
  const auto& gguf_interactive = registry.profile("gguf-interactive");
  assert(gguf_interactive.schema == 2);
  assert(gguf_interactive.backend == mica::Backend::gguf);
  assert(gguf_interactive.policy_for("spark-x25-4b")->engine == "llama-cpp");
  assert(gguf_interactive.policy_for("granite-speech-5")->engine == "audio-cpp");
  const auto& mlx_assistant = registry.profile("mica-assistant-mlx");
  assert(mlx_assistant.model_policies.size() == 4);
  for (const auto& policy : mlx_assistant.model_policies) {
    assert(policy.backend == mica::Backend::mlx);
    assert(policy.engine.starts_with("mlx-"));
  }
  const auto& gguf_assistant = registry.profile("mica-assistant-gguf");
  assert(gguf_assistant.model_policies.size() == 4);
  for (const auto& policy : gguf_assistant.model_policies) {
    assert(policy.backend == mica::Backend::gguf);
    assert(policy.engine == "llama-cpp" || policy.engine == "audio-cpp");
  }
  {
    std::ifstream catalog(config.parent_path() / "profiles/catalog.json");
    const auto document = nlohmann::json::parse(catalog);
    const auto& profiles = document.at("profiles");
    const auto gptq = std::find_if(profiles.begin(), profiles.end(), [](const auto& item) {
      return item.value("id", "") == "mica-assistant-gptq";
    });
    assert(gptq != profiles.end());
    assert(!gptq->value("available", true));
  }
  const auto schema2_startup = mica::plan_profile_startup(
      registry, gguf_interactive, mica::Backend::gguf,
      mica::Quantization::q4, 7.5);
  assert(!schema2_startup.error);
  assert(schema2_startup.admitted.size() == 3);
  assert(schema2_startup.reserved_gib > 7.29 &&
         schema2_startup.reserved_gib < 7.31);
  const auto& vllm_control = registry.model("vllm-qwen3-06b-control");
  assert(vllm_control.artifacts.at(mica::Backend::vllm)
             .at(mica::Quantization::q4).supported);
  assert(mica::normalize_modality("tts") == "tts");
  assert(mica::modality_capability("text-to-text") == "text");
  assert(mica::modality_capability("img-text-to-text") == "vision");
  assert(mica::parse_backend("vllm") == mica::Backend::vllm);
  assert(mica::parse_vllm_device("cpu") == mica::VllmDevice::cpu);
  assert(mica::parse_vllm_device("rocm") == mica::VllmDevice::rocm);
  assert(mica::parse_vllm_device("xpu") == mica::VllmDevice::xpu);
  assert(mica::parse_vllm_device("tpu") == mica::VllmDevice::tpu);
  assert(mica::parse_quantization("native") == mica::Quantization::native);
  assert(registry.vlm_tool.enabled);
  assert(registry.vlm_tool.name == "vlm_tool");
  assert(registry.vlm_tool.model_id == "minicpm-v46-thinking");
  assert(registry.vlm_tool.max_images_per_call == 8);
  assert(registry.vlm_tool.max_videos_per_call == 1);
  assert(registry.vlm_tool.max_document_pages_per_call == 8);
  assert(registry.vlm_tool.max_video_frames == 32);

  mica::HardwareInfo apple;
  apple.os = "macos";
  apple.apple_silicon = true;
  apple.mlx_target = "metal";
  apple.vllm_target = "metal";
  assert(apple.supports_vllm());
  assert(apple.recommended_vllm_device() == mica::VllmDevice::metal);
  mica::HardwareInfo linux_cpu;
  linux_cpu.os = "linux";
  assert(linux_cpu.supports_vllm());
  assert(linux_cpu.recommended_vllm_device() == mica::VllmDevice::cpu);
  mica::HardwareInfo linux_cuda = linux_cpu;
  linux_cuda.vllm_target = "cuda";
  linux_cuda.nvidia_detected = true;
  linux_cuda.nvidia_vram_gib = {12.0};
  assert(linux_cuda.recommended_vllm_device() == mica::VllmDevice::cuda);

  const auto metal_memory = mica::vllm_memory_utilization(
      mica::VllmDevice::metal, 8.0, 0.0, 24.0);
  assert(metal_memory.has_value());
  assert(*metal_memory > 0.333 && *metal_memory < 0.334);
  const auto cuda_memory = mica::vllm_memory_utilization(
      mica::VllmDevice::cuda, 64.0, 12.0, 24.0);
  assert(cuda_memory.has_value() && *cuda_memory == 0.5);
  assert(!mica::vllm_memory_utilization(
              mica::VllmDevice::cpu, 8.0, 0.0, 24.0)
              .has_value());
  assert(!mica::vllm_memory_utilization(
              mica::VllmDevice::metal, 8.0, 0.0, 0.0)
              .has_value());

  const auto custom_root = std::filesystem::temp_directory_path() /
                           ("mica-server-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(custom_root / "mica-server");
  {
    std::ofstream custom(custom_root / "mica-server/custom-models.json");
    custom << R"({"schema":1,"models":{"tiny-custom":{"id":"tiny-custom","source_repo":"owner/repo","source_url":"https://huggingface.co/owner/repo","modality":"text-to-text","capability":"text","license":"apache-2.0","description":"test","enabled":true,"variants":{"mlx":{"q4":{"status":"ready","artifact":"q4","reservation_gib":0.5}}}}}})";
  }
  {
    std::ofstream runtime(custom_root / "mica-server/runtime.json");
    runtime << R"({"installed_backends":["vllm"],"configured_models":{},"downloads":{}})";
  }
  mica::configure_vllm_custom_model({custom_root, "tiny-custom", 1.25, false});
  auto with_custom = mica::load_registry(config);
  mica::merge_custom_models(with_custom, custom_root);
  assert(with_custom.model("tiny-custom").capability == "text");
  assert(with_custom.model("tiny-custom").artifacts.at(mica::Backend::mlx)
             .at(mica::Quantization::q4).supported);
  assert(with_custom.model("tiny-custom").artifacts.at(mica::Backend::vllm)
             .at(mica::Quantization::native).supported);
  assert(with_custom.profile("all").models.back() == "tiny-custom");
  std::filesystem::remove_all(custom_root);

  const auto fits = mica::plan_startup(registry, all, mica::Backend::mlx,
                                       mica::Quantization::q4, 8.0);
  assert(!fits.error);
  assert(fits.admitted.size() == 3);
  assert(fits.admitted[0] == "spark-x25-4b");
  assert(fits.admitted[1] == "granite-speech-5");
  assert(fits.admitted[2] == "audio8-tts-06b");
  assert(fits.skipped.size() == 1);
  assert(fits.skipped[0] == "minicpm-v46-thinking");

  const auto baseline = mica::plan_startup(registry, all, mica::Backend::mlx,
                                           mica::Quantization::q4, 4.2);
  assert(!baseline.error);
  assert(baseline.admitted.size() == 2);
  assert(baseline.skipped.size() == 2);

  const auto too_small = mica::plan_startup(registry, all, mica::Backend::mlx,
                                            mica::Quantization::q4, 4.0);
  assert(too_small.error);

  const auto vllm_unconfigured = mica::plan_startup(
      registry, all, mica::Backend::vllm, mica::Quantization::q4, 8.0);
  assert(!vllm_unconfigured.error);
  assert(vllm_unconfigured.admitted.empty());
  assert(vllm_unconfigured.skipped.size() == 4);

  std::vector<mica::ResidentModel> residents = {
      {"new-small", 1.0, 200, false, 0},
      {"busy", 5.0, 1, true, 1},
      {"expired-small", 1.0, 100, true, 0},
      {"expired-large", 3.0, 100, true, 0},
  };
  const auto ranked = mica::rank_eviction_candidates(std::move(residents));
  assert(ranked.size() == 3);
  assert(ranked[0].id == "expired-large");
  assert(ranked[1].id == "expired-small");
  assert(ranked[2].id == "new-small");

  std::cout << "scheduler tests passed\n";
  return 0;
}
