#include <algorithm>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iostream>
#include <set>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include "mica_server/catalog.hpp"
#include "mica_server/base64.hpp"
#include "mica_server/config.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/machine.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/scheduler.hpp"

int main() {
  assert(mica::base64_encode("").empty());
  assert(mica::base64_encode("f") == "Zg==");
  assert(mica::base64_encode("fo") == "Zm8=");
  assert(mica::base64_encode("foo") == "Zm9v");
  assert(mica::base64_encode("foobar") == "Zm9vYmFy");
  assert(mica::base64_encode(std::string("\0\xff", 2)) == "AP8=");
  const auto config = std::filesystem::path(__FILE__).parent_path().parent_path() / "config";
  const auto registry = mica::load_registry(config);
  {
    const auto catalog = mica::read_profile_file(config.parent_path() /
        "profiles/catalog.yaml");
    for (const auto& item : catalog.at("profiles")) {
      if (!item.value("available", true)) continue;
      assert(item.at("schema") == 4);
      auto working = registry;
      const auto profile = mica::profile_from_document(working, item);
      assert(profile.name == item.at("id").get<std::string>());
    }
  }
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
  assert((spark.thinking_modes == std::vector<std::string>{"none", "on"}));
  assert(spark.thinking_budget_supported);
  assert((spark.input_modalities == std::vector<std::string>{"text"}));
  assert((spark.output_modalities == std::vector<std::string>{"text"}));
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .repository_pattern == "mlx/q4");
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .engine == "mlx-lm");
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .quantization_type == "mlx-affine-q4-g64");
  assert(spark.artifacts.at(mica::Backend::mlx).at(mica::Quantization::q4)
             .size_bytes == 2327288292ULL);
  assert(spark.repository_revisions.at(mica::Backend::mlx) ==
         "6c242d2945f11bb3587c437a1bf1088666ca6e2f");
  assert(registry.engine("prism-llama-cpp").cmake_definitions.at("metal")
             .at("GGML_METAL") == "ON");
  const auto& yaml_bonsai_artifact = registry.model("ternary-bonsai-2-27b")
                                    .artifacts.at(mica::Backend::gguf)
                                    .at(mica::Quantization("pq2_0"));
  assert(yaml_bonsai_artifact.files.size() == 2);
  assert(yaml_bonsai_artifact.files.at(1).role == "vision-projector");
  assert(yaml_bonsai_artifact.files.at(1).sha256 ==
         "e287342d92332fa3577ed1d42e921dac9370c08da58ba9337fa450f6cc76cfd7");
  const auto& coder = registry.profile("mica-coder-bonsai-macos");
  const auto& bonsai_thinking = registry.model("ternary-bonsai-2-27b");
  assert((bonsai_thinking.thinking_modes ==
          std::vector<std::string>{"none", "low", "medium", "xhigh"}));
  assert(bonsai_thinking.thinking_budget_supported);
  assert((registry.model("minicpm-v46-thinking").thinking_modes ==
          std::vector<std::string>{"none", "on"}));
  assert(coder.description.find("coding") != std::string::npos);
  assert(coder.default_chat_model == "ternary-bonsai-2-27b");
  assert(coder.required_ram_gib == 16.0);
  assert(coder.maximum_resident_workers == 1);
  assert(coder.model_policies.size() == 4);
  assert(coder.policy_for("ternary-bonsai-2-27b")->engine == "prism-llama-cpp");
  assert(coder.policy_for("spark-x25-4b")->engine == "mlx-lm");
  assert(coder.policy_for("ling-3-tiny")->engine == "llama-cpp");
  assert(coder.policy_for("ling-3-tiny")->quantization == mica::Quantization::q4);
  assert(coder.policy_for("minicpm-v46-thinking")->engine == "mlx-vlm");
  const auto& ling = registry.model("ling-3-tiny");
  assert(ling.source_repo == "inclusionAI/Ling-3.0-tiny");
  assert(ling.artifacts.at(mica::Backend::gguf).at(mica::Quantization::q4).engine ==
         "llama-cpp");
  assert(ling.artifacts.at(mica::Backend::gguf).at(mica::Quantization::q8).size_bytes ==
         8408187808ULL);
  assert(ling.artifacts.at(mica::Backend::gguf).at(mica::Quantization::q4)
             .minimum_engine_commit ==
         "373336672029b12e09f272bc027cc801345a3fd6");
  auto roundtrip_registry = registry;
  const auto coder_roundtrip = mica::profile_from_document(
      roundtrip_registry, mica::profile_to_document(coder));
  assert(coder_roundtrip.model_policies.size() == 4);
  assert(coder_roundtrip.default_chat_model == "ternary-bonsai-2-27b");
  assert(coder_roundtrip.policy_for("ternary-bonsai-2-27b")->quantization ==
         mica::Quantization("pq2_0"));
  const auto& gguf_long = registry.profile("gguf-long");
  assert(gguf_long.backend == mica::Backend::gguf);
  assert(gguf_long.max_input_tokens == 16384);
  assert(gguf_long.max_total_tokens == 17408);
  assert(gguf_long.max_concurrent_requests == 1);
  assert(gguf_long.kv_cache_precision == "q4");
  const auto& interactive = registry.profile("interactive");
  assert(interactive.schema == 4);
  assert(interactive.required_ram_gib == 8.0);
  assert(interactive.memory_safety_reserve_gib == 0.5);
  assert(interactive.model_policies.size() == 4);
  const auto* interactive_text = interactive.policy_for("spark-x25-4b");
  assert(interactive_text != nullptr);
  assert(interactive_text->engine == "mlx-lm");
  assert(interactive_text->backend == mica::Backend::mlx);
  assert(interactive_text->quantization == mica::Quantization::q4);
  assert(interactive_text->residency == mica::Residency::pinned);
  assert(interactive_text->startup);
  {
    auto validation_registry = registry;
    auto document = mica::profile_to_document(interactive);
    document["models"][0]["execution"] = "old-template";
    bool rejected = false;
    try {
      (void)mica::profile_from_document(validation_registry, document);
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("schema-4 tasks") !=
                 std::string::npos;
    }
    assert(rejected);
    document["models"][0].erase("execution");
    auto model = std::find_if(validation_registry.models.begin(),
                              validation_registry.models.end(), [](const auto& entry) {
      return entry.id == "spark-x25-4b";
    });
    model->supported_output_tokens =
        mica::ModelDefinition::TokenLimitClaim{512, "test-fixture"};
    rejected = false;
    try {
      (void)mica::profile_from_document(validation_registry, document);
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("supported output") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  const auto& gguf_interactive = registry.profile("gguf-interactive");
  assert(gguf_interactive.schema == 4);
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
    assert(round_trip.schema == 4);
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
    assert(ledger.at("data").size() == 7);
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
    assert(pq2_variant.at("files").size() == 2);
    assert(pq2_variant.at("files").at(1).at("role") == "vision-projector");
    assert(pq2_variant.at("download_size_bytes") ==
           7206168928ULL + 931145856ULL);
    const auto ling_entry = std::find_if(
        ledger.at("data").begin(), ledger.at("data").end(), [](const auto& item) {
          return item.value("id", "") == "ling-3-tiny";
        });
    assert(ling_entry != ledger.at("data").end());
    assert(ling_entry->at("license") == "mit");
    const auto& ling_variants = ling_entry->at("variants").at("gguf");
    assert(ling_variants.size() == 2);
    for (const auto& variant : ling_variants) {
      assert(variant.at("minimum_engine_commit") ==
             "373336672029b12e09f272bc027cc801345a3fd6");
    }
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
    std::ifstream schema(config.parent_path() /
                         "schemas/machine-policy-v1.schema.json");
    const auto document = nlohmann::json::parse(schema);
    assert(document.at("properties").at("schema").at("const") == 1);
    const auto machine_fixture = config.parent_path() /
        "tests/fixtures/machine/linux-cuda.yaml";
    const auto policy = mica::load_machine_policy(machine_fixture);
    const auto resolved = mica::resolve_machine_policy(cuda_fixture, policy);
    assert(resolved.inference_ram_gib == 16.0);
    assert(resolved.dedicated_memory_gib.at("cuda:0") == 12.0);
    assert(resolved.allowed_devices.contains("cpu"));
    assert(resolved.allowed_devices.contains("cuda:0"));
    assert(resolved.cpu_threads == 8);
    assert(resolved.build_ram_gib == 12.0);
    assert(resolved.build_parallel_jobs == 1);
    const auto defaults =
        mica::resolve_machine_policy(metal_fixture, mica::MachinePolicy{});
    assert(defaults.inference_ram_gib == 24.0);
    assert(defaults.allowed_devices.contains("metal:0"));
    assert(defaults.dedicated_memory_gib.empty());
    assert(defaults.build_ram_gib == 16.0);
    assert(defaults.build_parallel_jobs == 2);
    const auto cpu_fixture = mica::load_hardware_profile(
        hardware_fixtures / "linux-cpu.json");
    const auto cpu_only = mica::resolve_machine_policy(
        cpu_fixture, mica::MachinePolicy{});
    assert(cpu_only.allowed_devices == std::set<std::string>{"cpu"});
    assert(cpu_only.dedicated_memory_gib.empty());
    auto two_gpus = cuda_fixture;
    auto second_gpu = two_gpus.accelerators.front();
    second_gpu.id = "1";
    second_gpu.memory_gib = 16.0;
    two_gpus.accelerators.push_back(second_gpu);
    const auto selected_gpu = mica::resolve_machine_policy(
        two_gpus, mica::machine_policy_from_document(
            {{"schema", 1}, {"allowed_devices", {"cpu", "cuda:1"}},
             {"limits", {{"inference", {{"dedicated_memory_gib",
              {{"cuda:1", 10}}}}}}}}));
    assert(!selected_gpu.allowed_devices.contains("cuda:0"));
    assert(selected_gpu.allowed_devices.contains("cuda:1"));
    assert(selected_gpu.dedicated_memory_gib.size() == 1);
    assert(selected_gpu.dedicated_memory_gib.at("cuda:1") == 10.0);
    const auto reject = [&](const nlohmann::json& input,
                            const mica::HardwareInfo& hardware) {
      bool rejected = false;
      try {
        mica::resolve_machine_policy(hardware,
            mica::machine_policy_from_document(input));
      } catch (const std::exception&) {
        rejected = true;
      }
      assert(rejected);
    };
    reject({{"schema", 1}, {"unrecognized", true}}, cuda_fixture);
    reject({{"schema", 1}, {"allowed_devices", {"cpu", "cuda:9"}}},
           cuda_fixture);
    reject({{"schema", 1}, {"limits", {{"inference", {{"ram_gib", 65}}}}}},
           cuda_fixture);
    reject({{"schema", 1}, {"limits", {{"inference", {{"dedicated_memory_gib",
             {{"metal:0", 8}}}}}}}}, metal_fixture);
    reject({{"schema", 1}, {"allowed_devices", {"cpu", "cuda:0"}},
            {"limits", {{"inference", {{"dedicated_memory_gib",
             {{"cuda:0", 0}}}}}}}}, cuda_fixture);
    const auto disabled = mica::resolve_machine_policy(cuda_fixture,
        mica::machine_policy_from_document({{"schema", 1},
            {"limits", {{"inference", {{"dedicated_memory_gib",
             {{"cuda:0", 0}}}}}}}}));
    assert(!disabled.allowed_devices.contains("cuda:0"));
    assert(disabled.dedicated_memory_gib.empty());
    const auto policy_directory = std::filesystem::temp_directory_path() /
        ("mica-machine-policy-test-" + std::to_string(getpid()));
    const auto policy_path = policy_directory / "machine.yaml";
    assert(mica::ensure_machine_policy_if_absent(policy_path));
    assert(mica::load_machine_policy(policy_path).allowed_devices == std::nullopt);
    mica::write_profile_file(policy_path,
        {{"schema", 1}, {"allowed_devices", {"cpu"}}});
    assert(!mica::ensure_machine_policy_if_absent(policy_path));
    const auto preserved = mica::load_machine_policy(policy_path);
    assert(preserved.allowed_devices.has_value());
    assert(*preserved.allowed_devices == std::vector<std::string>{"cpu"});
    const auto legacy_path = policy_directory / "runtime.json";
    {
      std::ofstream legacy(legacy_path);
      legacy << R"({"max_ram_gib":10,"max_vram_gib":12})";
    }
    const auto migrated = mica::legacy_machine_policy_from_runtime(
        legacy_path, cuda_fixture);
    assert(migrated.has_value());
    assert(migrated->inference_ram_gib == 10.0);
    assert(migrated->dedicated_memory_gib.at("cuda:0") == 12.0);
    const auto migrated_path = policy_directory / "migrated.yaml";
    assert(mica::ensure_machine_policy_if_absent(migrated_path, *migrated));
    const auto restored = mica::load_machine_policy(migrated_path);
    assert(restored.inference_ram_gib == 10.0);
    assert(restored.dedicated_memory_gib.at("cuda:0") == 12.0);
    const auto ambiguous = mica::legacy_machine_policy_from_runtime(
        legacy_path, two_gpus);
    assert(ambiguous->inference_ram_gib == 10.0);
    assert(ambiguous->dedicated_memory_gib.empty());
    std::filesystem::remove(policy_path);
    std::filesystem::remove(migrated_path);
    std::filesystem::remove(legacy_path);
    std::filesystem::remove(policy_directory);
  }
  {
    const auto yaml_path = std::filesystem::temp_directory_path() /
        ("mica-system-profile-test-" + std::to_string(getpid()) + ".yaml");
    mica::write_hardware_profile(metal_fixture, yaml_path);
    const auto yaml_facts = mica::read_profile_file(yaml_path);
    assert(!yaml_facts.contains("backend_targets"));
    assert(!yaml_facts.contains("build_policy"));
    const auto restored = mica::load_hardware_profile(yaml_path);
    assert(restored.gguf_target == "metal");
    assert(restored.ram_gib == 24.0);
    std::filesystem::remove(yaml_path);
  }
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
    const std::map<std::string, double> limits{{"cuda:0", 12.0},
                                                {"cuda:1", 16.0}};
    const std::map<std::string, double> used{{"cuda:0", 8.0},
                                              {"cuda:1", 1.0}};
    assert(mica::fits_device_reservation(used, limits, "cuda:1", 14.0, 16.0));
    assert(!mica::fits_device_reservation(used, limits, "cuda:0", 5.0, 16.0));
    assert(!mica::fits_device_reservation(used, limits, "cuda:2", 1.0, 16.0));
    auto two_gpus = cuda_fixture;
    auto second_gpu = two_gpus.accelerators.front();
    second_gpu.id = "1";
    second_gpu.memory_gib = 16.0;
    two_gpus.accelerators.push_back(second_gpu);
    auto distributed = gguf_gpu;
    distributed.model_policies.clear();
    auto spark_policy = *gguf_gpu.policy_for("spark-x25-4b");
    spark_policy.startup = true;
    spark_policy.residency = mica::Residency::pinned;
    distributed.model_policies.push_back(spark_policy);
    auto vision_policy = *gguf_gpu.policy_for("minicpm-v46-thinking");
    vision_policy.startup = true;
    vision_policy.residency = mica::Residency::pinned;
    vision_policy.device = "accelerator:1";
    distributed.model_policies.push_back(vision_policy);
    const std::map<std::string, double> per_gpu_limit{{"cuda:0", 6.0},
                                                        {"cuda:1", 6.0}};
    const auto plan = mica::plan_profile_startup_resources(
        registry, distributed, mica::Backend::gguf, mica::Quantization::q4,
        4.0, 6.0, two_gpus, per_gpu_limit);
    assert(!plan.error);
    assert(plan.admitted.size() == 2);
    assert(plan.reserved_vram_gib > 6.0);
    assert(plan.reserved_vram_by_device_gib.at("cuda:0") == 4.8);
    assert(plan.reserved_vram_by_device_gib.at("cuda:1") > 0.0);
    auto constrained = per_gpu_limit;
    constrained["cuda:1"] = 1.0;
    const auto rejected = mica::plan_profile_startup_resources(
        registry, distributed, mica::Backend::gguf, mica::Quantization::q4,
        4.0, 6.0, two_gpus, constrained);
    assert(rejected.error.has_value());
    assert(rejected.error->find("VRAM") != std::string::npos);
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
      rejected = std::string(error.what()).find("artifact and engine do not match") !=
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
