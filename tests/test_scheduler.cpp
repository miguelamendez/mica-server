#include <cassert>
#include <fstream>
#include <iostream>

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
         "miguelamendez/mica-spark-x25-4b-mlx");
  assert(spark.repositories.at(mica::Backend::gguf) ==
         "miguelamendez/mica-spark-x25-4b-gguf");
  assert(!spark.description.empty());
  assert(!spark.tags.empty());
  assert(mica::normalize_modality("tts") == "tts");
  assert(mica::modality_capability("text-to-text") == "text");
  assert(mica::modality_capability("img-text-to-text") == "vision");
  assert(mica::parse_backend("vllm") == mica::Backend::vllm);
  assert(mica::parse_vllm_device("cpu") == mica::VllmDevice::cpu);
  assert(mica::parse_quantization("native") == mica::Quantization::native);

  mica::HardwareInfo apple;
  apple.os = "macos";
  apple.apple_silicon = true;
  assert(apple.supports_vllm());
  assert(apple.recommended_vllm_device() == mica::VllmDevice::metal);
  mica::HardwareInfo linux_cpu;
  linux_cpu.os = "linux";
  assert(linux_cpu.supports_vllm());
  assert(linux_cpu.recommended_vllm_device() == mica::VllmDevice::cpu);
  mica::HardwareInfo linux_cuda = linux_cpu;
  linux_cuda.nvidia_detected = true;
  linux_cuda.nvidia_vram_gib = {12.0};
  assert(linux_cuda.recommended_vllm_device() == mica::VllmDevice::cuda);

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
  assert(fits.admitted.size() == 4);
  assert(fits.admitted[0] == "spark-x25-4b");
  assert(fits.admitted[1] == "granite-speech-5");

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
