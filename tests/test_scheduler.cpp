#include <algorithm>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include "mica_server/catalog.hpp"
#include "mica_server/base64.hpp"
#include "mica_server/config.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/generation_metrics.hpp"
#include "mica_server/machine.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/scheduler.hpp"

int main() {
  {
    using nlohmann::json;
    const auto metrics = mica::generation_metrics(json{{"timings", {
        {"predicted_n", 100}, {"predicted_ms", 500}, {"prompt_ms", 9000}}}});
    assert(metrics.at("decode_tokens_per_second") == 200);
    assert(metrics.at("decode_seconds") == 0.5);
    assert(metrics.at("completion_tokens") == 100);
    const auto direct = mica::generation_metrics(json{{"timings", {{"predicted_per_second", 25.5}}}});
    assert(direct.at("decode_tokens_per_second") == 25.5);
    for (const auto& response : {json::object(), json{{"usage", {{"completion_tokens", 100}}}},
        json{{"timings", {{"predicted_n", 0}, {"predicted_ms", 0}}}},
        json{{"timings", {{"predicted_per_second", "invalid"}}}}}) {
      assert(!mica::generation_metrics(response).at("available").get<bool>());
    }
  }
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
      assert(item.at("schema") == 5);
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
  assert(spark.gguf_context_tokens == 1048576);
  assert(spark.gguf_parallel_slots == 4);
  assert((spark.thinking_modes == std::vector<std::string>{"none", "on"}));
  assert(spark.thinking_budget_supported);
  assert((spark.input_modalities == std::vector<std::string>{"text"}));
  assert((spark.output_modalities == std::vector<std::string>{"text"}));
  assert(std::find(spark.abilities.begin(), spark.abilities.end(),
                   "text_generation") != spark.abilities.end());
  assert(!spark.supported_interactions.empty());
  assert(spark.supported_interactions.front().operation == "chat.generate");
  {
    const auto& researcher = registry.profile("local-researcher-gguf");
    assert(researcher.residency_strategy == "sequential");
    assert(researcher.maximum_resident_workers == 1);
    assert(mica::select_profile_model(registry, researcher, "embeddings", {"text"}, "") == "embeddinggemma-2");
    assert(mica::select_profile_model(registry, researcher, "embeddings", {"text", "image"}, "") == "embeddinggemma-2");
    assert(mica::select_profile_model(registry, researcher, "chat", {"text", "audio"}, "gemma4-12b") == "gemma4-12b");
    bool rejected = false;
    try { (void)mica::select_profile_model(registry, researcher, "chat", {"text"}, "embeddinggemma-2"); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    auto document = mica::profile_to_document(researcher);
    auto working = registry;
    auto roundtrip = mica::profile_from_document(working, document);
    assert(roundtrip.default_models.at("embeddings") == "embeddinggemma-2");
    document["models"][1]["context"] = {{"max_input_tokens", 8193}, {"max_output_tokens", 0}, {"max_total_tokens", 8193}};
    rejected = false;
    try { (void)mica::profile_from_document(working, document); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
  const auto& qwen_vision = registry.model("qwen35-9b");
  assert(qwen_vision.gguf_context_tokens == 262144);
  assert(qwen_vision.artifact_for(mica::Backend::gguf, mica::Quantization::q4,
                                "llama-cpp").image_min_tokens == 1024);
  assert(qwen_vision.artifact_for(mica::Backend::gguf, mica::Quantization::q4,
                                "llama-cpp").files.size() == 2);
  const auto& qwen_coder = registry.profile("mica-coder-qwen-gguf");
  assert(qwen_coder.model_policies.size() == 3);
  for (const auto& policy : qwen_coder.model_policies) {
    assert(policy.engine == "llama-cpp");
    assert(policy.max_input_tokens == 65536);
    assert(policy.max_output_tokens == 16384);
  }
  assert(mica::select_profile_model(registry, qwen_coder, "chat", {"text"}) ==
         "spark-x25-4b");
  assert(mica::select_profile_model(registry, qwen_coder, "chat", {"text", "image"}) ==
         "qwen35-9b");
  const auto& audio8 = registry.model("audio8-tts-06b");
  assert(audio8.supported_interactions.front().operation ==
         "audio.synthesize_speech");
  assert(audio8.supported_interactions.front().optional_inputs ==
         std::vector<std::string>{"audio"});
  assert(registry.engine("mlx-audio").endpoint_contracts.size() == 2);
  const auto& diar_model = registry.model("nemotron-3-diarization");
  assert(diar_model.capability == "diar");
  assert(diar_model.artifact_for(mica::Backend::mlx, mica::Quantization::q8,
                               "mlx-audio-diarization").size_bytes == 106613900ULL);
  assert(diar_model.artifact_for(mica::Backend::gguf, mica::parse_quantization("bf16"),
                               "audio-cpp").size_bytes == 198720928ULL);
  const auto& diar_contract = registry.engine("mlx-audio-diarization").endpoint_contracts.front();
  assert(diar_contract.path == "/v1/audio/diarizations");
  assert(diar_contract.adapter_call == "audio_diarization");
  assert(!diar_contract.streaming);
  assert(mica::select_profile_model(registry, registry.profile("diarization"),
                                   "diar", {"audio"}) == diar_model.id);
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
  const auto& gsq = registry.model("qwen38-27b-gsq-rco");
  assert(gsq.references.size() >= 7);
  assert(gsq.references.at(1).kind == "code");
  assert(gsq.references.at(1).url == "https://github.com/IST-DASLab/GSQ");
  assert(gsq.references.at(3).url == "https://github.com/IST-DASLab/RCO");
  assert(gsq.references.at(5).kind == "reproducibility");
  assert(!gsq.references.at(5).description.empty());
  const auto has_gsq_reference = [&gsq](const std::string& url) {
    return std::any_of(gsq.references.begin(), gsq.references.end(),
                       [&url](const auto& reference) {
                         return reference.url == url && !reference.description.empty();
                       });
  };
  assert(has_gsq_reference("https://huggingface.co/ISTA-DASLab/Qwen3.8-27B-GSQ-RCO-GGUF/blob/d562806dbafae37109975e970aae91b43e73b440/mmproj-Qwen3.8-27B-BF16.gguf"));
  assert(has_gsq_reference("https://huggingface.co/z-lab/Qwen3.8-27B-DFlash2-GGUF/blob/2d9571f8ce46e151f61c6499c99dee6079e1d610/Qwen3.8-27B-DFlash2-Q4_K_M.gguf"));
  assert(has_gsq_reference("https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/blob/4ca720788d1e01f1bff70c033e0d0028fd02e502/MTP/mtp-Qwen3.8-27B-Q4_0.gguf"));
  // Reference links must not silently add optional drafters to this bundle.
  const auto& gsq_files = gsq.artifact_for(mica::Backend::gguf,
                                         mica::parse_quantization("iq3_xxs"),
                                         "llama-cpp").files;
  assert(gsq_files.size() == 2);
  assert(gsq_files.at(0).role == "model");
  assert(gsq_files.at(1).role == "vision-projector");
  assert(spark.references.size() == 2);
  assert(spark.memory_profile);
  assert(spark.memory_profile->profiler == "scripts/profile_model_memory.py");
  assert(spark.memory_profile->results == "artifacts/model-memory/spark-x25-4b");
  assert(spark.references.front().kind == "documentation");
  assert(spark.references.front().url ==
         "https://huggingface.co/XHToken/Spark-X2.5-4B/blob/main/config.json");
  assert(!registry.model("audio8-tts-06b").references.empty());
  assert(registry.model("audio8-tts-06b").gguf_context_tokens == 2048);
  assert(registry.model("granite-speech-5").gguf_context_tokens == 0);
  assert(!spark.recommended_context_tokens && !spark.max_output_tokens);
  assert(registry.model("vllm-qwen3-06b-control").recommended_context_tokens == 32768);
  {
    char temporary[] = "/tmp/mica-reference-tests-XXXXXX";
    const auto created = mkdtemp(temporary);
    assert(created != nullptr);
    const auto directory = std::filesystem::path(created);
    const auto copied_config = directory / "config";
    std::filesystem::copy(config, copied_config,
                          std::filesystem::copy_options::recursive);
    const auto manifest = copied_config / "model-manifests/qwen38-27b-gsq-rco.yaml";
    const auto original = mica::read_profile_file(manifest);
    const auto invalid_references = std::vector<nlohmann::json>{
        nlohmann::json::object(),
        nlohmann::json::array({{{"kind", "unknown"}, {"title", "test"}, {"url", "https://example.com"}}}),
        nlohmann::json::array({{{"kind", "code"}, {"title", ""}, {"url", "https://example.com"}}}),
        nlohmann::json::array({{{"kind", "code"}, {"title", "test"}, {"url", "file:///tmp/code"}}}),
        nlohmann::json::array({{{"kind", "code"}, {"title", "test"}, {"url", "https://"}}}),
        nlohmann::json::array({{{"kind", "code"}, {"title", "test"}, {"url", "https://example.com/path with spaces"}}}),
        nlohmann::json::array({{{"kind", "code"}, {"title", "test"}, {"url", "https://example.com"}, {"download", true}}}),
        nlohmann::json::array({{{"kind", "code"}, {"url", "https://example.com"}}})};
    for (const auto& references : invalid_references) {
      auto document = original;
      document["references"] = references;
      // JSON is valid YAML syntax; this fixture remains a .yaml manifest.
      { std::ofstream output(manifest); output << document.dump(2) << '\n'; }
      bool rejected = false;
      try { (void)mica::load_registry(copied_config); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);
    }
    mica::write_profile_file(manifest, original);
    for (const auto& field : {"native_context_tokens", "recommended_context_tokens", "max_output_tokens"}) {
      for (const auto& value : {nlohmann::json(0), nlohmann::json(-1),
                               nlohmann::json(true), nlohmann::json(1024.5),
                               nlohmann::json(2147483648LL),
                               nlohmann::json::object({{"tokens", 1024}})}) {
        auto document = original;
        document[field] = value;
        mica::write_profile_file(manifest, document);
        bool rejected = false;
        try { (void)mica::load_registry(copied_config); }
        catch (const std::exception&) { rejected = true; }
        assert(rejected);
      }
    }
    for (const auto& field : {"recommended_context_tokens", "max_output_tokens"}) {
      auto document = original;
      document[field] = original.at("native_context_tokens").get<int>() + 1;
      mica::write_profile_file(manifest, document);
      bool rejected = false;
      try { (void)mica::load_registry(copied_config); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);
      document[field] = nullptr;
      mica::write_profile_file(manifest, document);
      (void)mica::load_registry(copied_config);
    }
    {
      auto document = original;
      document["declared_context_tokens"] = 262144;
      mica::write_profile_file(manifest, document);
      bool rejected = false;
      try { (void)mica::load_registry(copied_config); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);  // No silently accepted legacy model-manifest fields.
    }
    mica::write_profile_file(manifest, original);
    for (const auto& metadata : std::vector<nlohmann::json>{
        {{"profiler", "/tmp/script.py"}, {"results", "artifacts/report"}},
        {{"profiler", "scripts/tool.py"}, {"results", "../outside"}},
        {{"profiler", "scripts/tool.py"}, {"results", ""}},
        {{"profiler", "scripts/tool.py"}},
        {{"profiler", "scripts/tool.py"}, {"results", "artifacts/report"}, {"execute", true}}}) {
      auto document = original;
      document["memory_profile"] = metadata;
      mica::write_profile_file(manifest, document);
      bool rejected = false;
      try { (void)mica::load_registry(copied_config); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);
    }
    mica::write_profile_file(manifest, original);
    const auto vision_manifest = copied_config / "model-manifests/qwen35-9b.yaml";
    const auto vision_original = mica::read_profile_file(vision_manifest);
    for (const auto& floor : std::vector<nlohmann::json>{-1, 16385, 1.5, "1024"}) {
      auto document = vision_original;
      document["artifacts"][0]["image_min_tokens"] = floor;
      mica::write_profile_file(vision_manifest, document);
      bool rejected = false;
      try { (void)mica::load_registry(copied_config); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);
    }
    std::filesystem::remove_all(directory);
  }
  const auto& gsq_artifact = gsq.artifact_for(
      mica::Backend::gguf, mica::parse_quantization("iq3_xxs"), "llama-cpp");
  assert(gsq_artifact.quantization_type == "IQ3_XXS");
  {
    const auto& modes = registry.profile("qwen27b-modes");
    assert(modes.model_policies.size() == 2);
    assert(modes.maximum_resident_workers == 1);
    assert(mica::select_profile_model(registry, modes, "chat", {"text"}) ==
           "qwen38-27b-text-dflash");
    assert(mica::select_profile_model(registry, modes, "chat", {"text", "image"}) ==
           "qwen38-27b-vision-mtp");
    const auto document = mica::profile_to_document(modes);
    auto working = registry;
    const auto roundtrip = mica::profile_from_document(working, document);
    for (const auto& policy : modes.model_policies) {
      assert(mica::profile_native_options(policy) ==
             mica::profile_native_options(*roundtrip.policy_for(policy.id)));
    }
    for (const auto& invalid : std::vector<nlohmann::json>{
             {{"speculative", {{"method", "mtp"}}}},
             {{"speculative", {{"method", "dflash"}, {"max_draft_tokens", 0}}}},
             {{"speculative", {{"method", "dflash"}, {"gpu_layers", -2}}}},
             {{"speculative", {{"kv_cache_precision", "f16"}}}},
             {{"batching", {{"max_concurrent_requests", 1}, {"token_batch_size", 128}, {"micro_batch_size", 512}}}},
             {{"kv_cache", {{"precision", "q4"}, {"ram_cache_mib", -2}}}},
             {{"placement", {{"mode", "fixed"}, {"device", "cuda:0"}, {"projector_on_cpu", true}}}}}) {
      auto bad = document;
      bad["models"][0].update(invalid);
      bool rejected = false;
      try { (void)mica::profile_from_document(working, bad); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);
    }
    const auto& draft_artifact = registry.model("qwen38-27b-text-dflash").artifact_for(
        mica::Backend::gguf, mica::parse_quantization("iq3_xxs"), "llama-cpp");
    assert(draft_artifact.projector_pattern.empty());
    assert(draft_artifact.files.size() == 2);
    assert(draft_artifact.files[1].role == "dflash-drafter");
    assert(draft_artifact.files[1].repository == "z-lab/Qwen3.8-27B-DFlash2-GGUF");
    const auto& vision_artifact = registry.model("qwen38-27b-vision-mtp").artifact_for(
        mica::Backend::gguf, mica::parse_quantization("iq3_xxs"), "llama-cpp");
    assert(vision_artifact.image_min_tokens == 1024 && vision_artifact.image_max_tokens == 1024);
    assert(vision_artifact.files.size() == 3);
    assert(vision_artifact.files[2].repository_path == "MTP/mtp-Qwen3.8-27B-Q4_0.gguf");
  }
  assert(gsq_artifact.files.size() == 2);
  assert(gsq_artifact.files.at(0).size_bytes == 10094357632ULL);
  assert(gsq_artifact.files.at(1).role == "vision-projector");
  assert(gsq_artifact.files.at(1).sha256 ==
         "13cb7bebccbd04afc8f4090cb949ecf8937cdf7377c5799b1a0c594e7c0d3e16");
  const auto& comparison = registry.profile("qwen38-gsq-bonsai-comparison");
  assert(comparison.required_ram_gib == 16.0);
  assert(comparison.maximum_resident_workers == 1);
  assert(comparison.model_policies.size() == 2);
  assert(comparison.policy_for(gsq.id)->max_total_tokens == 8192);
  assert(comparison.policy_for("ternary-bonsai-2-27b")->max_total_tokens == 8192);
  const auto& coder = registry.profile("mica-coder-bonsai-macos");
  const auto& bonsai_thinking = registry.model("ternary-bonsai-2-27b");
  assert((bonsai_thinking.thinking_modes ==
          std::vector<std::string>{"none", "low", "medium", "xhigh"}));
  assert(bonsai_thinking.thinking_budget_supported);
  assert((registry.model("minicpm-v46-thinking").thinking_modes ==
          std::vector<std::string>{"none", "on"}));
  assert(coder.description.find("coding") != std::string::npos);
  assert(coder.schema == 5);
  assert(coder.default_chat_model == "spark-x25-4b");
  assert(coder.default_models.at("text") == "spark-x25-4b");
  assert(mica::select_profile_model(registry, coder, "chat", {"text"}) ==
         "spark-x25-4b");
  assert(mica::select_profile_model(registry, coder, "chat", {"text", "image"}) ==
         "minicpm-v46-thinking");
  assert(mica::select_profile_model(registry, coder, "chat", {"text", "video"}) ==
         "minicpm-v46-thinking");
  assert(mica::select_profile_model(registry, coder, "chat", {"text", "image", "video"}) ==
         "minicpm-v46-thinking");
  {
    auto incompatible = registry;
    incompatible.engines.at("mlx-vlm").endpoint_contracts.front().optional_inputs.clear();
    bool rejected = false;
    try {
      (void)mica::select_profile_model(incompatible, coder, "chat",
                                       {"text", "image"}, "minicpm-v46-thinking");
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("does not support") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  assert(mica::select_profile_model(registry, coder, "chat", {"text"},
                                    "spark-x25-4b@mlx:q4") ==
         "spark-x25-4b@mlx:q4");
  {
    auto ambiguous = coder;
    ambiguous.default_chat_model.clear();
    ambiguous.default_models.clear();
    assert(mica::select_profile_model(registry, ambiguous, "chat", {"text"}) ==
           "spark-x25-4b");
    bool rejected = false;
    try {
      (void)mica::select_profile_model(registry, coder, "asr", {"audio"},
                                       "spark-x25-4b");
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("does not support") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  assert(coder.required_ram_gib == 16.0);
  assert(coder.maximum_resident_workers == 1);
  assert(coder.model_policies.size() == 4);
  for (const auto& policy : coder.model_policies) {
    assert(policy.max_input_tokens == 65536);
    assert(policy.max_output_tokens == 16384);
    assert(policy.max_total_tokens == 81920);
    assert(policy.max_concurrent_requests == 1);
  }
  assert(registry.model("minicpm-v46-thinking").gguf_context_tokens == 262144);
  {
    auto oversized = mica::profile_to_document(coder);
    auto& context = oversized["models"][0]["context"];
    context["max_input_tokens"] = 262144;
    context["max_total_tokens"] = 278528;
    auto strict = registry;
    bool rejected = false;
    try { (void)mica::profile_from_document(strict, oversized); }
    catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("exceeds declared context") != std::string::npos;
    }
    assert(rejected);
    auto experimental = registry;
    experimental.ignore_context_limit = true;
    assert(mica::profile_from_document(experimental, oversized)
               .policy_for("ternary-bonsai-2-27b")->max_total_tokens == 278528);
    context["max_total_tokens"] = 262144;
    rejected = false;
    try { (void)mica::profile_from_document(experimental, oversized); }
    catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("inconsistent context limits") != std::string::npos;
    }
    assert(rejected);
  }
  assert(coder.policy_for("ternary-bonsai-2-27b")->engine == "prism-llama-cpp");
  assert(coder.policy_for("spark-x25-4b")->engine == "mlx-lm");
  assert(coder.policy_for("spark-x25-4b")->startup);
  assert(!coder.policy_for("ternary-bonsai-2-27b")->startup);
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
  for (const auto& policy : coder_roundtrip.model_policies) {
    assert(policy.max_input_tokens == 65536);
    assert(policy.max_output_tokens == 16384);
    assert(policy.max_total_tokens == 81920);
  }
  assert(coder_roundtrip.default_chat_model == "spark-x25-4b");
  assert(coder_roundtrip.default_models == coder.default_models);
  {
    auto document = mica::profile_to_document(coder);
    document["selection"]["defaults"].push_back({{"operation", "text.generate"},
        {"required_inputs", {"text"}}, {"model", "spark-x25-4b"}});
    auto working = registry;
    const auto completion = mica::profile_from_document(working, document);
    assert(completion.default_models.at("completion") == "spark-x25-4b");
    assert(mica::select_profile_model(working, completion, "completion", {"text"}) ==
           "spark-x25-4b");
  }
  {
    auto missing_description = mica::profile_to_document(coder);
    missing_description["description"] = "";
    auto working = registry;
    bool rejected = false;
    try {
      (void)mica::profile_from_document(working, missing_description);
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("intended-use description") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  assert(coder_roundtrip.policy_for("ternary-bonsai-2-27b")->quantization ==
         mica::Quantization("pq2_0"));
  {
    auto invalid = mica::profile_to_document(coder);
    invalid["selection"]["defaults"].push_back(
        {{"operation", "audio.transcribe"},
         {"required_inputs", {"audio"}}, {"model", "spark-x25-4b"}});
    auto working = registry;
    bool rejected = false;
    try {
      (void)mica::profile_from_document(working, invalid);
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("invalid profile default asr") !=
                 std::string::npos;
    }
    assert(rejected);
  }
  const auto& gguf_long = registry.profile("gguf-long");
  assert(gguf_long.backend == mica::Backend::gguf);
  assert(gguf_long.max_input_tokens == 16384);
  assert(gguf_long.max_total_tokens == 17408);
  assert(gguf_long.max_concurrent_requests == 1);
  assert(gguf_long.kv_cache_precision == "q4");
  const auto& interactive = registry.profile("interactive");
  assert(interactive.schema == 5);
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
      rejected = std::string(error.what()).find("schema-4-or-newer workloads") !=
                 std::string::npos;
    }
    assert(rejected);
    document["models"][0].erase("execution");
    auto model = std::find_if(validation_registry.models.begin(),
                              validation_registry.models.end(), [](const auto& entry) {
      return entry.id == "spark-x25-4b";
    });
    model->max_output_tokens = 512;
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
  assert(gguf_interactive.schema == 5);
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
    assert(round_trip.schema == 5);
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
    const std::set<std::string> expected = {"mac_assistant", "mac_coder", "mac_meetings",
        "mac_visual_extraction", "gpu_16g_assistant", "gpu_16g_coder",
        "gpu_16g_meetings", "gpu_16g_visual_extraction"};
    std::set<std::string> visible;
    for (const auto& [id, profile] : registry.profiles)
      if (profile.catalog_visible) visible.insert(id);
    auto local_expected = expected;
    local_expected.insert("local-researcher-gguf");
    assert(visible == local_expected);
    assert(profiles.size() == expected.size());
    for (auto item : profiles) {
      const auto id = item.at("id").get<std::string>();
      assert(expected.contains(id));
      item.erase("available");
      auto working = registry;
      const auto parsed = mica::profile_from_document(working, item);
      assert(mica::profile_to_document(parsed) ==
             mica::profile_to_document(registry.profile(id)));
    }
    const auto& coder = registry.profile("gpu_16g_coder");
    assert(coder.maximum_resident_workers == 1);
    assert(coder.default_chat_model == "qwen38-27b-text-dflash");
    assert(coder.policy_for("qwen38-27b-text-dflash")->speculative_method == "dflash");
    assert(coder.policy_for("qwen38-27b-vision-mtp")->speculative_method == "mtp");
    assert(!registry.profile("mac_coder").policy_for("qwen38-27b-text-dflash"));
    const auto& visual = registry.profile("mac_visual_extraction");
    assert(visual.model_policies.size() == 3);
    assert(visual.policy_for("minicpm-v46-thinking"));
    assert(visual.policy_for("qwen35-4b"));
    assert(visual.policy_for("qwen35-9b"));
    for (const auto& id : {"mac_meetings", "gpu_16g_meetings"}) {
      const auto& meeting = registry.profile(id);
      assert(meeting.policy_for("granite-speech-5"));
      assert(meeting.policy_for("nemotron-3-diarization"));
      assert(meeting.policy_for("spark-x25-4b"));
      assert(!meeting.policy_for("audio8-tts-06b"));
    }
    const auto& gpu_visual = registry.profile("gpu_16g_visual_extraction");
    assert(gpu_visual.default_models.at("image") == "qwen38-27b-vision-mtp");
    assert(gpu_visual.default_models.at("video") == "qwen35-9b");
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
    assert(ledger.at("data").size() == static_cast<std::size_t>(std::count_if(
        registry.models.begin(), registry.models.end(),
        [](const auto& model) { return model.catalog_visible; })));
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
    assert(spark_entry->at("abilities").at(0) == "text_generation");
    assert(spark_entry->at("supported_interactions").at(0).at("operation") ==
           "chat.generate");
    assert(spark_entry->at("license") == "apache-2.0");
  assert(spark_entry->at("references").size() == spark.references.size());
  assert(spark_entry->at("native_context_tokens") == 1048576);
  assert(spark_entry->at("recommended_context_tokens").is_null());
  assert(spark_entry->at("max_output_tokens").is_null());
  assert(!spark_entry->contains("supported_context_tokens"));
  assert(spark_entry->at("memory_profile").at("results") == spark.memory_profile->results);
  assert(spark_entry->at("references").at(0).at("url") ==
         spark.references.front().url);
    const auto gsq_entry = std::find_if(
        ledger.at("data").begin(), ledger.at("data").end(), [](const auto& item) {
          return item.value("id", "") == "qwen38-27b-gsq-rco";
        });
    assert(gsq_entry != ledger.at("data").end());
    assert(gsq_entry->at("references").size() == gsq.references.size());
    assert(gsq_entry->at("references").at(3).at("url") ==
           "https://github.com/IST-DASLab/RCO");
    assert(gsq_entry->at("references").at(5).at("description") ==
           gsq.references.at(5).description);
    assert(spark_entry->at("variants").at("mlx").at(0).contains(
        "quantization_type"));
    assert(spark_entry->at("variants").at("mlx").at(0).at("size_bytes")
               .get<std::uint64_t>() > 0);
    const auto bonsai_entry = std::find_if(
        ledger.at("data").begin(), ledger.at("data").end(), [](const auto& item) {
          return item.value("id", "") == "ternary-bonsai-2-27b";
        });
    assert(bonsai_entry != ledger.at("data").end());
    assert(std::find(bonsai_entry->at("modalities").begin(),
                     bonsai_entry->at("modalities").end(),
                     "video-text-to-text") == bonsai_entry->at("modalities").end());
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
    const auto video_only = mica::registry_catalog(
        registry, std::nullopt, std::nullopt, false, std::nullopt,
        std::string("video-text-to-text"));
    assert(video_only.at("data").size() == 4);
    assert(std::any_of(video_only.at("data").begin(), video_only.at("data").end(),
                      [](const auto& item) { return item.value("id", "") == "minicpm-v46-thinking"; }));
    assert(std::any_of(video_only.at("data").begin(), video_only.at("data").end(),
                      [](const auto& item) { return item.value("id", "") == "qwen35-4b"; }));
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
  assert(mica::physical_cores_from_lscpu("# Core,Socket\n0,0\n1,0\n0,0\n1,0\n") == 2);
  assert(mica::physical_cores_from_lscpu("0,0\n0,1\n") == 2);
  assert(mica::physical_cores_from_lscpu("\n# unavailable\n-,-\n-1,0\n0,0,extra\ninvalid\n") == 0);
  assert(mica::physical_cores_from_lscpu(" 0,0 \n1,0\r\n") == 2);
  const auto cuda_fixture =
      mica::load_hardware_profile(hardware_fixtures / "linux-cuda.json");
  {
    const auto& modes = registry.profile("qwen27b-modes");
    const auto& draft_policy = *modes.policy_for("qwen38-27b-text-dflash");
    const auto& draft_artifact = registry.model(draft_policy.id).artifact_for(
        mica::Backend::gguf, draft_policy.quantization, draft_policy.engine);
    const auto gpu = mica::resolve_model_placement(draft_policy, draft_artifact, cuda_fixture);
    assert(gpu.ram_reservation_gib == 12);
    assert(gpu.vram_reservation_gib < 15 && gpu.vram_reservation_gib > 14);
    auto cpu_draft = draft_policy;
    cpu_draft.draft_gpu_layers = 0;
    const auto cpu = mica::resolve_model_placement(cpu_draft, draft_artifact, cuda_fixture);
    assert(cpu.ram_reservation_gib > gpu.ram_reservation_gib);
    assert(cpu.vram_reservation_gib < gpu.vram_reservation_gib);
    auto large_file = draft_artifact;
    large_file.reservation_gib = 1;
    large_file.files[1].size_bytes = 10ULL * 1024 * 1024 * 1024;
    assert(mica::resolve_model_placement(draft_policy, large_file, cuda_fixture)
               .vram_reservation_gib > 20);
    const auto& vision_policy = *modes.policy_for("qwen38-27b-vision-mtp");
    const auto& vision_artifact = registry.model(vision_policy.id).artifact_for(
        mica::Backend::gguf, vision_policy.quantization, vision_policy.engine);
    auto projector_cpu = vision_policy;
    projector_cpu.projector_on_cpu = true;
    projector_cpu.vram_reservation_gib = 15;
    const auto projector = mica::resolve_model_placement(projector_cpu, vision_artifact, cuda_fixture);
    assert(projector.vram_reservation_gib == 15);  // Never lower an explicit floor.
    assert(projector.ram_reservation_gib > 13);
    const auto& maximum = registry.profile("qwen27b-modes-max-context").model_policies.front();
    const auto& target = registry.model(maximum.id).artifact_for(
        mica::Backend::gguf, maximum.quantization, maximum.engine);
    const auto partial = mica::resolve_model_placement(maximum, target, cuda_fixture);
    assert(partial.vram_reservation_gib < 15);
    assert(partial.ram_reservation_gib == 12);
    auto all_gpu = maximum;
    all_gpu.gpu_layers = 99;
    assert(mica::resolve_model_placement(all_gpu, target, cuda_fixture).vram_reservation_gib > 15);
  }
  const auto rocm_fixture =
      mica::load_hardware_profile(hardware_fixtures / "linux-rocm.json");
  const auto xpu_fixture =
      mica::load_hardware_profile(hardware_fixtures / "linux-xpu.json");
  const auto metal_fixture =
      mica::load_hardware_profile(hardware_fixtures / "mac-metal.json");
  {
    auto copy = registry;
    auto automatic = registry.profile("diarization");
    const auto test_root = std::filesystem::temp_directory_path() /
        ("mica-engine-selection-" + std::to_string(getpid()));
    mica::resolve_profile_engines(copy, automatic, metal_fixture, test_root);
    assert(automatic.model_policies.front().engine == "mlx-audio-diarization");
    const auto binary = test_root / "runtimes/audio.cpp/build-mica/bin/audiocpp_server";
    std::filesystem::create_directories(binary.parent_path());
    std::ofstream(binary) << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(binary, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    mica::resolve_profile_engines(copy, automatic, metal_fixture, test_root);
    assert(automatic.model_policies.front().engine == "audio-cpp");
    automatic.engine_policy = "manifest-order";
    mica::resolve_profile_engines(copy, automatic, metal_fixture, test_root);
    assert(automatic.model_policies.front().engine == "mlx-audio-diarization");
    mica::resolve_profile_engines(copy, automatic, cuda_fixture, test_root);
    assert(automatic.model_policies.front().engine == "audio-cpp");
    const auto document = mica::profile_to_document(automatic);
    assert(!document.at("models").front().contains("engine"));
    assert(document.at("models").front().contains("artifact"));
    std::filesystem::remove_all(test_root);
  }
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
    assert(plan.reserved_ram_gib > 7.92 && plan.reserved_ram_gib < 7.93);
    assert(plan.reserved_vram_gib == 0.0);
  }
  {
    const auto plan = mica::plan_profile_startup_resources(
        registry, gguf_gpu, mica::Backend::gguf, mica::Quantization::q4,
        7.5, 15.5, cuda_fixture);
    assert(!plan.error);
    assert(plan.admitted.size() == 3);
    assert(plan.reserved_ram_gib == 1.5);
    assert(plan.reserved_vram_gib > 7.92 && plan.reserved_vram_gib < 7.93);
  }
  {
    const auto plan = mica::plan_profile_startup_resources(
        registry, gguf_mixed, mica::Backend::gguf, mica::Quantization::q4,
        7.5, 11.5, cuda_fixture);
    assert(!plan.error);
    assert(plan.admitted.size() == 3);
    assert(plan.reserved_ram_gib > 3.02 && plan.reserved_ram_gib < 3.03);
    assert(plan.reserved_vram_gib > 5.39 && plan.reserved_vram_gib < 5.40);
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
    assert(plan.reserved_vram_by_device_gib.at("cuda:0") > 5.08);
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
    assert(cuda.vram_reservation_gib > 5.39 && cuda.vram_reservation_gib < 5.40);
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
    assert(metal.ram_reservation_gib > 5.39 && metal.ram_reservation_gib < 5.40);
    assert(metal.gpu_layers == 99);
    auto batched = policy;
    batched.max_concurrent_requests = 4;
    assert(mica::resolve_model_placement(batched, artifact, metal_fixture)
               .ram_reservation_gib > metal.ram_reservation_gib);
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
    assert(cpu_placement.ram_reservation_gib > 12.0);
    assert(cpu_placement.vram_reservation_gib == 0.0);
    const auto gpu_placement = mica::resolve_model_placement(
        *bonsai_gpu.policy_for(bonsai.id), bonsai_artifact, cuda_fixture);
    assert(gpu_placement.device == "cuda:0");
    assert(gpu_placement.ram_reservation_gib == 0.75);
    assert(gpu_placement.vram_reservation_gib > 12.0);
  }
  {
    auto mismatched = registry;
    auto model = std::find_if(
        mismatched.models.begin(), mismatched.models.end(), [](const auto& item) {
          return item.id == "ternary-bonsai-2-27b";
        });
    model->artifacts.at(mica::Backend::gguf).at(pq2).engine = "llama-cpp";
    model->engine_artifacts.at("prism-llama-cpp").at(pq2).engine = "llama-cpp";
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
