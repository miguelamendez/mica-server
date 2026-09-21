#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include "mica_server/catalog.hpp"
#include "mica_server/config.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/profiles.hpp"
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
  assert(spark.catalog_visible);
  assert(spark.gguf_context_tokens == 8192);
  assert(spark.gguf_parallel_slots == 4);
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .repository_pattern == "mlx/q4");
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .engine == "mlx-lm");
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .quantization_type == "mlx-affine-q4-g64");
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .size_bytes == 2327288292ULL);
  const auto& gguf_long = registry.profile("gguf-long");
  assert(gguf_long.backend == mica::Backend::gguf);
  assert(gguf_long.max_input_tokens == 16384);
  assert(gguf_long.max_total_tokens == 17408);
  assert(gguf_long.max_concurrent_requests == 1);
  assert(gguf_long.kv_cache_precision == "q4");
  const auto& interactive = registry.profile("interactive");
  assert(interactive.schema == 3);
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
  assert(gguf_interactive.schema == 3);
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
  const auto& gguf_cpu = registry.profile("mica-assistant-gguf-cpu");
  const auto& gguf_gpu = registry.profile("mica-assistant-gguf-gpu");
  const auto& gguf_mixed = registry.profile("mica-assistant-gguf-mixed");
  for (const auto& policy : gguf_cpu.model_policies) {
    assert(policy.placement_mode == "fixed");
    assert(policy.device == "cpu");
    assert(policy.gpu_layers == 0);
  }
  for (const auto& policy : gguf_gpu.model_policies) {
    assert(policy.placement_mode == "fixed");
    assert(policy.device == "accelerator:0");
    assert(policy.gpu_layers == 99);
    assert(policy.ram_reservation_gib == 0.5);
    assert(policy.vram_reservation_gib > 0);
  }
  assert(gguf_mixed.policy_for("spark-x25-4b")->device == "accelerator:0");
  assert(gguf_mixed.policy_for("granite-speech-5")->device == "cpu");
  assert(gguf_mixed.policy_for("audio8-tts-06b")->device == "cpu");
  assert(gguf_mixed.policy_for("minicpm-v46-thinking")->device ==
         "accelerator:0");
  const auto& prism_engine = registry.engine("prism-llama-cpp");
  assert(prism_engine.backend == mica::Backend::gguf);
  assert(prism_engine.installer == "cmake-llama");
  assert(prism_engine.launcher == "llama-server");
  assert(prism_engine.runtime_directory == "prism-llama.cpp");
  assert(prism_engine.revision ==
         "9a9394a895b96003ca842a6041cb28ac49a108f7");
  const auto& bonsai = registry.model("ternary-bonsai-2-27b");
  const auto pq2 = mica::parse_quantization("pq2_0");
  const auto& bonsai_artifact = bonsai.artifacts.at(mica::Backend::gguf).at(pq2);
  assert(bonsai_artifact.supported);
  assert(bonsai_artifact.engine == "prism-llama-cpp");
  assert(bonsai_artifact.quantization_type == "PQ2_0");
  assert(bonsai_artifact.size_bytes == 7206168928ULL);
  assert(bonsai_artifact.projector_size_bytes == 931145856ULL);
  assert(bonsai_artifact.sha256 ==
         "3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1");
  assert(bonsai_artifact.projector_sha256 ==
         "e287342d92332fa3577ed1d42e921dac9370c08da58ba9337fa450f6cc76cfd7");
  assert(bonsai.repository_revisions.at(mica::Backend::gguf) ==
         "6ed5e12bf84b7a63069882c91dd9e9218647d17b");
  const auto& bonsai_cpu = registry.profile("bonsai-pq2-vision-cpu");
  const auto& bonsai_gpu = registry.profile("bonsai-pq2-vision-gpu");
  assert(bonsai_cpu.policy_for(bonsai.id)->engine == "prism-llama-cpp");
  assert(bonsai_cpu.policy_for(bonsai.id)->quantization == pq2);
  assert(bonsai_cpu.policy_for(bonsai.id)->device == "cpu");
  assert(bonsai_gpu.policy_for(bonsai.id)->device == "accelerator:0");
  {
    std::ifstream schema(config.parent_path() / "schemas/profile-v3.schema.json");
    const auto document = nlohmann::json::parse(schema);
    assert(document.at("$schema") ==
           "https://json-schema.org/draft/2020-12/schema");
    assert(document.at("properties").at("schema").at("const") == 3);
  }
  {
    bool rejected = false;
    try {
      (void)mica::read_profile_file(
          config.parent_path() / "tests/fixtures/profiles/removed-profile.json");
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find(".yaml or .yml") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  {
    auto validation_registry = registry;
    bool rejected = false;
    try {
      (void)mica::profile_from_document(
          validation_registry,
          nlohmann::json{{"schema", 2}, {"id", "old-profile"},
                         {"models", nlohmann::json::array()}});
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("schema 3") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  {
    auto validation_registry = registry;
    auto document = mica::profile_to_document(interactive);
    const auto round_trip =
        mica::profile_from_document(validation_registry, document);
    assert(round_trip.schema == 3);
    assert(round_trip.name == interactive.name);
    document["misspelled_memory_policy"] = true;
    bool rejected = false;
    try {
      (void)mica::profile_from_document(validation_registry, document);
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("unknown field") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  {
    const auto document = mica::read_profile_file(
        config.parent_path() / "profiles/catalog.yaml");
    const auto& profiles = document.at("profiles");
    const auto gptq = std::find_if(profiles.begin(), profiles.end(), [](const auto& item) {
      return item.value("id", "") == "mica-assistant-gptq";
    });
    assert(gptq != profiles.end());
    assert(!gptq->value("available", true));
  }
  const auto schema3_startup = mica::plan_profile_startup(
      registry, gguf_interactive, mica::Backend::gguf,
      mica::Quantization::q4, 7.5);
  assert(!schema3_startup.error);
  assert(schema3_startup.admitted.size() == 3);
  assert(schema3_startup.reserved_gib > 7.29 &&
         schema3_startup.reserved_gib < 7.31);
  const auto& vllm_control = registry.model("vllm-qwen3-06b-control");
  assert(!vllm_control.catalog_visible);
  assert(vllm_control.artifacts.at(mica::Backend::vllm)
             .at(mica::Quantization::q4).supported);
  assert(mica::normalize_modality("tts") == "tts");
  assert(mica::normalize_modality("video-text-to-text") == "img-text-to-text");
  assert(mica::normalize_modality("image-video-to-text") == "img-text-to-text");
  assert(mica::modality_capability("text-to-text") == "text");
  assert(mica::modality_capability("img-text-to-text") == "vision");
  {
    const auto ledger = mica::registry_catalog(registry);
    assert(ledger.at("schema") == 2);
    assert(ledger.at("data").size() == 5);
    const auto hidden = std::find_if(
        ledger.at("data").begin(), ledger.at("data").end(), [](const auto& item) {
          return item.value("id", "") == "vllm-qwen3-06b-control";
        });
    assert(hidden == ledger.at("data").end());
    const auto spark_entry = std::find_if(
        ledger.at("data").begin(), ledger.at("data").end(), [](const auto& item) {
          return item.value("id", "") == "spark-x25-4b";
        });
    assert(spark_entry != ledger.at("data").end());
    assert(spark_entry->at("description").is_string());
    assert(spark_entry->at("license") == "apache-2.0");
    assert(spark_entry->at("variants").at("mlx").at(0).contains(
        "quantization_type"));
    assert(spark_entry->at("variants").at("mlx").at(0).at("size_bytes")
               .get<std::uint64_t>() > 0);
    const auto bonsai_entry = std::find_if(
        ledger.at("data").begin(), ledger.at("data").end(), [](const auto& item) {
          return item.value("id", "") == "ternary-bonsai-2-27b";
        });
    assert(bonsai_entry != ledger.at("data").end());
    const auto& pq2_variant = bonsai_entry->at("variants").at("gguf").at(0);
    assert(pq2_variant.at("quantization") == "pq2_0");
    assert(pq2_variant.at("quantization_type") == "PQ2_0");
    assert(pq2_variant.at("engine") == "prism-llama-cpp");
    assert(pq2_variant.at("sha256") == bonsai_artifact.sha256);
  }
  {
    const auto audio_ledger = mica::registry_catalog(
        registry, std::optional<std::string>{"tts"},
        std::optional<mica::Backend>{mica::Backend::mlx}, false,
        std::optional<std::string>{"mlx-audio"});
    assert(audio_ledger.at("data").size() == 1);
    assert(audio_ledger.at("data").at(0).at("id") == "audio8-tts-06b");
  }
  assert(mica::parse_backend("vllm") == mica::Backend::vllm);
  assert(mica::parse_vllm_device("cpu") == mica::VllmDevice::cpu);
  assert(mica::parse_vllm_device("rocm") == mica::VllmDevice::rocm);
  assert(mica::parse_vllm_device("xpu") == mica::VllmDevice::xpu);
  assert(mica::parse_vllm_device("tpu") == mica::VllmDevice::tpu);
  assert(mica::parse_quantization("native") == mica::Quantization::native);
  assert(mica::to_string(mica::parse_quantization("pq2_0")) == "pq2_0");
  {
    bool rejected = false;
    try {
      (void)mica::parse_quantization("PQ2_0");
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);
  }
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

  const auto hardware_fixtures = config.parent_path() / "tests/fixtures/hardware";
  const auto cuda_fixture =
      mica::load_hardware_profile(hardware_fixtures / "linux-cuda.json");
  const auto rocm_fixture =
      mica::load_hardware_profile(hardware_fixtures / "linux-rocm.json");
  const auto xpu_fixture =
      mica::load_hardware_profile(hardware_fixtures / "linux-xpu.json");
  const auto metal_fixture =
      mica::load_hardware_profile(hardware_fixtures / "mac-metal.json");
  {
    const auto plan = mica::plan_profile_startup_resources(
        registry, gguf_cpu, mica::Backend::gguf, mica::Quantization::q4,
        15.5, 0.0, cuda_fixture);
    assert(!plan.error);
    assert(plan.admitted.size() == 3);
    assert(plan.reserved_ram_gib > 7.29 && plan.reserved_ram_gib < 7.31);
    assert(plan.reserved_vram_gib == 0.0);
  }
  {
    const auto plan = mica::plan_profile_startup_resources(
        registry, gguf_gpu, mica::Backend::gguf, mica::Quantization::q4,
        7.5, 15.5, cuda_fixture);
    assert(!plan.error);
    assert(plan.admitted.size() == 3);
    assert(plan.reserved_ram_gib == 1.5);
    assert(plan.reserved_vram_gib > 7.29 && plan.reserved_vram_gib < 7.31);
  }
  {
    const auto plan = mica::plan_profile_startup_resources(
        registry, gguf_mixed, mica::Backend::gguf, mica::Quantization::q4,
        7.5, 11.5, cuda_fixture);
    assert(!plan.error);
    assert(plan.admitted.size() == 3);
    assert(plan.reserved_ram_gib > 2.99 && plan.reserved_ram_gib < 3.01);
    assert(plan.reserved_vram_gib > 4.79 && plan.reserved_vram_gib < 4.81);
  }
  {
    const auto& policy = *gguf_gpu.policy_for("spark-x25-4b");
    const auto& artifact = spark.artifacts.at(mica::Backend::gguf)
                               .at(mica::Quantization::q4);
    const auto cuda = mica::resolve_model_placement(policy, artifact,
                                                     cuda_fixture);
    assert(cuda.device == "cuda:0");
    assert(!cuda.unified_memory);
    assert(cuda.ram_reservation_gib == 0.5);
    assert(cuda.vram_reservation_gib == 4.8);
    const auto rocm = mica::resolve_model_placement(policy, artifact,
                                                     rocm_fixture);
    assert(rocm.device == "hip:0");
    const auto xpu = mica::resolve_model_placement(policy, artifact,
                                                    xpu_fixture);
    assert(xpu.device == "sycl:0");
  }
  {
    auto policy = *gguf_gpu.policy_for("audio8-tts-06b");
    const auto& artifact = registry.model(policy.id)
                               .artifacts.at(mica::Backend::gguf)
                               .at(mica::Quantization::q4);
    const auto xpu = mica::resolve_model_placement(policy, artifact,
                                                    xpu_fixture);
    assert(xpu.device == "vulkan:0");
  }
  {
    auto policy = *gguf_gpu.policy_for("spark-x25-4b");
    const auto& artifact = spark.artifacts.at(mica::Backend::gguf)
                               .at(mica::Quantization::q4);
    const auto metal = mica::resolve_model_placement(policy, artifact,
                                                      metal_fixture);
    assert(metal.device == "metal:0");
    assert(metal.unified_memory);
    assert(metal.vram_reservation_gib == 0.0);
    assert(metal.ram_reservation_gib == 4.8);
  }

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

  {
    const auto cpu_placement = mica::resolve_model_placement(
        *bonsai_cpu.policy_for(bonsai.id), bonsai_artifact, cuda_fixture);
    assert(cpu_placement.device == "cpu");
    assert(cpu_placement.ram_reservation_gib == 12.0);
    assert(cpu_placement.vram_reservation_gib == 0.0);
    const auto gpu_placement = mica::resolve_model_placement(
        *bonsai_gpu.policy_for(bonsai.id), bonsai_artifact, cuda_fixture);
    assert(gpu_placement.device == "cuda:0");
    assert(gpu_placement.ram_reservation_gib == 0.75);
    assert(gpu_placement.vram_reservation_gib == 12.0);
  }
  {
    auto mismatched = registry;
    auto model = std::find_if(
        mismatched.models.begin(), mismatched.models.end(), [](const auto& item) {
          return item.id == "ternary-bonsai-2-27b";
        });
    model->artifacts.at(mica::Backend::gguf).at(pq2).engine = "llama-cpp";
    bool rejected = false;
    try {
      (void)mica::profile_from_document(
          mismatched, mica::profile_to_document(bonsai_cpu));
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("requires engine llama-cpp") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  assert(!mica::vllm_memory_utilization(
              mica::VllmDevice::metal, 8.0, 0.0, 0.0)
              .has_value());

  const auto custom_root = std::filesystem::temp_directory_path() /
                           ("mica-server-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(custom_root / "config");
  std::filesystem::create_directories(custom_root / "state");
  {
    std::ofstream custom(custom_root / "config/custom-models.json");
    custom << R"({"schema":1,"models":{"tiny-custom":{"id":"tiny-custom","source_repo":"owner/repo","source_url":"https://huggingface.co/owner/repo","modality":"text-to-text","capability":"text","license":"apache-2.0","description":"test","enabled":true,"variants":{"mlx":{"q4":{"status":"ready","artifact":"q4","reservation_gib":0.5}}}}}})";
  }
  {
    std::ofstream runtime(custom_root / "state/runtime.json");
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
