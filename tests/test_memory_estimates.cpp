#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include "mica_server/config.hpp"
#include "mica_server/memory.hpp"
#include "mica_server/profiles.hpp"

using nlohmann::json;
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }
int main() {
  mica::Artifact a;
  a.supported = true; a.engine = "llama-cpp"; a.format = "gguf";
  a.reservation_gib = 1;
  a.files.push_back({}); a.files.back().role = "model";
  a.files.back().size_bytes = 1000000000;
  a.memory_estimate = {{"kv_status", "verified"}, {"kv_groups", json::array({
      {{"layers", 8}, {"key_width", 1024}, {"value_width", 1024}}})}};
  auto q4 = mica::estimate_artifact_memory(a, 8192, 1, "q4", "q4");
  assert(near(q4.at("kv_total_mb"), 8192.0 * 8 * 2 * 32 * 18 / 1e6));
  auto mixed = mica::estimate_artifact_memory(a, 8192, 1, "q8", "q4");
  assert(near(mixed.at("kv_k_mb"), 8192.0 * 8 * 32 * 34 / 1e6));
  assert(near(mixed.at("kv_v_mb"), q4.at("kv_v_mb")));
  assert(near(mica::estimate_artifact_memory(a, 8192, 2).at("kv_total_mb"),
              2 * mica::estimate_artifact_memory(a, 8192).at("kv_total_mb").get<double>()));
  a.memory_estimate["kv_groups"][0]["capacity_tokens"] = 512;
  assert(near(mica::estimate_artifact_memory(a, 8192).at("kv_total_mb"),
              mica::estimate_artifact_memory(a, 131072).at("kv_total_mb")));
  a.memory_estimate["kv_groups"][0].erase("capacity_tokens");
  a.memory_estimate["kv_groups"][0]["sliding_window_tokens"] = 1024;
  auto swa = mica::estimate_artifact_memory(a, 8192, 1, "q8", "q8", "none", 1024);
  assert(near(swa.at("kv_total_mb"), 2048.0 * 8 * 2 * 32 * 34 / 1e6));
  assert(mica::estimate_artifact_memory(a, 8192, 1, "q8", "q8", "none", 2048).at("kv_total_mb") > swa.at("kv_total_mb"));
  a.memory_estimate["kv_groups"][0]["value_width"] = 0;
  assert(mica::estimate_artifact_memory(a, 8192).at("kv_v_mb") == 0);
  a.memory_estimate["kv_groups"] = json::array();
  a.memory_estimate["kv_status"] = "not-applicable";
  assert(mica::estimate_artifact_memory(a, 8192).at("kv_total_mb") == 0);
  a.files.push_back({}); a.files.back().role = "dflash-drafter";
  a.files.back().size_bytes = 100000000;
  assert(mica::estimate_artifact_memory(a, 8192, 1, "q8", "q8", "none").at("weights_mb") == 1000);
  assert(mica::estimate_artifact_memory(a, 8192).at("weights_mb") == 1100);
  const auto directory = std::filesystem::path(__FILE__).parent_path().parent_path() / "config";
  auto registry = mica::load_registry(directory);
  auto profile = registry.profile("mica-coder-spark-ling-qwen4b-macos");
  profile.memory_limit_gib = 16;
  profile.residency_strategy = "sequential";
  profile.balanced_keep_models = {"ling-3-tiny"};
  auto report = mica::workload_memory_report(registry, profile, 16, 16);
  const auto& estimates = report.at("strategies");
  assert(estimates.at("all").at("required_gib") > estimates.at("sequential").at("required_gib"));
  assert(estimates.at("balanced").at("required_gib") >= estimates.at("sequential").at("required_gib"));
  assert(estimates.at("all").at("required_gib") >= estimates.at("balanced").at("required_gib"));
  assert(!mica::workload_memory_report(registry, profile, 17, 16).at("budget_valid").get<bool>());
  bool rejected = false;
  try { mica::validate_memory_strategy(registry, profile, 15); } catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
  profile.memory_limit_gib = 0.001;
  rejected = false;
  try { mica::validate_memory_strategy(registry, profile, 16); } catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
  profile.memory_limit_gib = 16;
  mica::apply_residency_strategy(profile);
  assert(profile.maximum_resident_workers == 1);
  for (const auto& policy : profile.model_policies) assert(policy.residency != mica::Residency::pinned);
  profile.residency_strategy = "balanced";
  mica::apply_residency_strategy(profile);
  assert(profile.policy_for("ling-3-tiny")->residency == mica::Residency::pinned);
  profile.residency_strategy = "all";
  mica::apply_residency_strategy(profile);
  for (const auto& policy : profile.model_policies) assert(policy.residency == mica::Residency::pinned && policy.startup);
  const auto document = mica::profile_to_document(profile);
  auto parsed = mica::profile_from_document(registry, document);
  assert(parsed.memory_limit_gib == 16 && parsed.residency_strategy == "all");
  assert(parsed.balanced_keep_models == profile.balanced_keep_models);
  auto minimal = document;
  minimal["memory"].erase("required_ram_gib"); minimal["memory"].erase("required_vram_gib");
  assert(mica::profile_from_document(registry, minimal).residency_strategy == "all");
  auto model_report = mica::model_memory_report(registry.model("audio8-tts-06b"));
  assert(!model_report.at("artifacts")[0].at("checkpoints")[1].at("supported").get<bool>());
  for (const auto& model : registry.models) for (const auto& [engine, variants] : model.engine_artifacts)
    for (const auto& [quant, artifact] : variants) {
      assert(artifact.memory_estimate.is_object());
      for (const auto& file : artifact.files) assert(file.estimated_memory_mb >= 0);
    }
  std::cout << "Memory formula, mixed precision, MLA, SWA, concurrency, component selection, strategies, budgets and manifest coverage passed.\n";
}
