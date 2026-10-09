#include "mica_server/memory.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace mica {
namespace {
using json = nlohmann::json;
constexpr double gib = 1073741824.0;
double row_bytes(std::uint64_t width, const std::string& type) {
  if (type == "q4" || type == "q4_0") return ((width + 31) / 32) * 18.0;
  if (type == "q8" || type == "q8_0") return ((width + 31) / 32) * 34.0;
  if (type == "f16" || type == "bf16" || type == "auto" ||
      type == "runtime-managed" || type == "not-applicable") return width * 2.0;
  throw std::invalid_argument("unsupported cache precision: " + type);
}
bool active_role(const std::string& role, const std::string& method) {
  if (role == "config" || role == "license" || role == "tokenizer" || role == "processor") return false;
  if (role == "mtp-drafter") return method == "auto" || method == "mtp";
  if (role == "dflash-drafter") return method == "auto" || method == "dflash";
  return true;
}
}

json estimate_artifact_memory(const Artifact& artifact, int total_tokens, int sequences,
    const std::string& key_type, const std::string& value_type, const std::string& speculative,
    int micro_batch_tokens) {
  if (total_tokens <= 0 || sequences <= 0 || micro_batch_tokens <= 0)
    throw std::invalid_argument("positive context, sequence count and micro-batch size required");
  // Validate types even when there is no cache.
  row_bytes(32, key_type); row_bytes(32, value_type);
  const auto metadata = artifact.memory_estimate.is_object() ? artifact.memory_estimate : json::object();
  json warnings = json::array(), components = json::array();
  double weights = 0, omitted = 0;
  for (const auto& file : artifact.files) {
    const double amount = std::max(double(file.size_bytes), file.estimated_memory_mb >= 0 ? file.estimated_memory_mb * 1e6 : 0.0);
    const bool selected = active_role(file.role, speculative);
    if (selected) weights += amount; else if (active_role(file.role, "auto")) omitted += amount;
    components.push_back({{"role", file.role}, {"path", file.path}, {"selected", selected},
                         {"estimated_mb", amount / 1e6}, {"basis", "artifact-storage-proxy-not-measured"}});
  }
  if (artifact.files.empty()) weights = artifact.size_bytes + artifact.projector_size_bytes;
  double key = 0, value = 0;
  bool cache_known = metadata.value("kv_status", std::string("unknown")) != "unknown";
  if (metadata.contains("kv_groups") && !metadata.at("kv_groups").empty()) {
    for (const auto& group : metadata.at("kv_groups")) {
      if (!active_role(group.value("component_role", std::string("model")), speculative)) continue;
      auto capacity = static_cast<double>(total_tokens);
      if (group.value("capacity_tokens", 0) > 0) capacity = std::min(capacity, double(group.at("capacity_tokens").get<int>()));
      // Native interleaved SWA reserves its window plus an incoming micro-batch.
      // Charging this per sequence is conservative for unified batched caches.
      if (group.value("sliding_window_tokens", 0) > 0)
        capacity = std::min(capacity, double(group.at("sliding_window_tokens").get<int>()) + micro_batch_tokens);
      capacity = std::ceil(capacity / 256) * 256; // Conservative native cache alignment.
      const auto count = group.at("layers").get<int>();
      key += capacity * sequences * count * row_bytes(group.at("key_width").get<std::uint64_t>(),
          group.value("key_precision", key_type));
      value += capacity * sequences * count * row_bytes(group.at("value_width").get<std::uint64_t>(),
          group.value("value_precision", value_type));
    }
  } else if (metadata.value("kv_status", std::string()) != "not-applicable" && artifact.kv_bytes_per_token_f16 > 0) {
    // Legacy fallback is explicitly approximate: layout and block rounding unknown.
    key = double(artifact.kv_bytes_per_token_f16) / 4 * total_tokens * sequences * row_bytes(32, key_type) / 32;
    value = double(artifact.kv_bytes_per_token_f16) / 4 * total_tokens * sequences * row_bytes(32, value_type) / 32;
    warnings.push_back("Unverified legacy KV layout; estimate is provisional.");
  }
  if (!cache_known) warnings.push_back("Cache/recurrent/drafter layout incomplete; do not treat this estimate as certified capacity.");
  if (artifact.format == "mlx") warnings.push_back("Requested quantization is an estimate; protected cache layers and runtime version can alter allocation.");
  if (metadata.contains("notes")) for (const auto& note : metadata.at("notes")) warnings.push_back(note);
  const double state = metadata.value("fixed_state_mb", 0.0) * 1e6 * sequences;
  // Retain the existing conservative base reservation as the workspace floor.
  const double base = std::max(std::max(0.0, artifact.reservation_gib * gib - omitted), weights * 1.1 + 0.25 * gib);
  const double overhead = std::max(base - weights, metadata.value("runtime_overhead_mb", 0.0) * 1e6);
  return {{"status", "estimated"}, {"total_tokens", total_tokens}, {"sequences", sequences},
          {"key_precision", key_type}, {"value_precision", value_type},
          {"micro_batch_tokens", micro_batch_tokens},
          {"components", components}, {"weights_mb", weights / 1e6},
          {"kv_k_mb", key / 1e6}, {"kv_v_mb", value / 1e6}, {"kv_total_mb", (key + value) / 1e6},
          {"fixed_state_mb", state / 1e6}, {"runtime_overhead_mb", overhead / 1e6},
          {"estimated_total_mb", (weights + key + value + state + overhead) / 1e6},
          {"estimated_total_gib", (weights + key + value + state + overhead) / gib},
          {"cache_layout_verified", cache_known}, {"warnings", warnings}};
}

json model_memory_report(const ModelDefinition& model) {
  json artifacts = json::array();
  for (const auto& [engine, variants] : model.engine_artifacts) for (const auto& [quant, artifact] : variants) {
    json checkpoints = json::array();
    std::vector<int> contexts{8192, 16384, 32768, 69632, 131072};
    if (model.gguf_context_tokens > 0 && model.gguf_context_tokens < 8192) contexts.insert(contexts.begin(), model.gguf_context_tokens);
    for (const int context : contexts) {
      const bool no_kv = artifact.memory_estimate.is_object() && artifact.memory_estimate.value("kv_status", std::string()) == "not-applicable";
      const bool supported = (no_kv && model.capability != "embedding") ||
          (model.gguf_context_tokens > 0 && context <= model.gguf_context_tokens);
      json item = {{"total_tokens", context}, {"supported", supported}, {"cache", json::array()}};
      if (supported) for (const auto& k : {"q4", "q8"}) for (const auto& v : {"q4", "q8"})
        item["cache"].push_back(estimate_artifact_memory(artifact, context, 1, k, v));
      checkpoints.push_back(std::move(item));
    }
    artifacts.push_back({{"engine", engine}, {"artifact", to_string(quant)},
                         {"metadata", artifact.memory_estimate}, {"checkpoints", checkpoints}});
  }
  return {{"model", model.id}, {"units", "MB=1000000 bytes; GiB=1073741824 bytes"},
          {"native_context_tokens", model.gguf_context_tokens}, {"artifacts", artifacts},
          {"note", "Planning estimates, not measured peaks. Quantization availability is engine-dependent."}};
}

json workload_memory_report(const Registry& registry, const Profile& profile,
    double budget_gib, double global_limit_gib) {
  if (budget_gib < 0 || global_limit_gib < 0 || !std::isfinite(budget_gib) || !std::isfinite(global_limit_gib))
    throw std::invalid_argument("invalid memory budget");
  if (budget_gib == 0) budget_gib = profile.memory_limit_gib;
  if (budget_gib == 0) budget_gib = global_limit_gib;
  const bool budget_valid = (global_limit_gib == 0 || budget_gib <= global_limit_gib) &&
      (profile.memory_limit_gib == 0 || budget_gib <= profile.memory_limit_gib);
  double sum = 0, largest = 0, kept = 0, largest_swap = 0;
  bool verified = true;
  json models = json::array();
  for (const auto& policy : profile.model_policies) {
    const auto& artifact = registry.model(policy.id).artifact_for(policy.backend, policy.quantization, policy.engine);
    const auto k = policy.kv_cache_k_precision.empty() ? policy.kv_cache_precision : policy.kv_cache_k_precision;
    const auto v = policy.kv_cache_v_precision.empty() ? policy.kv_cache_precision : policy.kv_cache_v_precision;
    auto configured_artifact = artifact;
    if (configured_artifact.memory_estimate.is_object() && configured_artifact.memory_estimate.contains("kv_groups"))
      for (auto& group : configured_artifact.memory_estimate["kv_groups"])
        if (group.value("component_role", std::string()) == "mtp-drafter" || group.value("component_role", std::string()) == "dflash-drafter") {
          group["key_precision"] = policy.draft_kv_cache_precision;
          group["value_precision"] = policy.draft_kv_cache_precision;
        }
    auto estimate = estimate_artifact_memory(configured_artifact, policy.max_total_tokens,
        policy.max_concurrent_requests, k, v, policy.speculative_method,
        policy.micro_batch_size > 0 ? policy.micro_batch_size : 512);
    // Host prompt-cache copies are independent of live KV storage.
    double amount = estimate.at("estimated_total_gib").get<double>() + std::max(0, policy.ram_cache_mib) / 1024.0;
    const bool keep = std::find(profile.balanced_keep_models.begin(), profile.balanced_keep_models.end(), policy.id) != profile.balanced_keep_models.end() ||
        (profile.residency_strategy.empty() && policy.residency == Residency::pinned);
    sum += amount; largest = std::max(largest, amount);
    if (keep) kept += amount; else largest_swap = std::max(largest_swap, amount);
    verified = verified && estimate.at("cache_layout_verified").get<bool>();
    models.push_back({{"model", policy.id}, {"engine", policy.engine}, {"priority", policy.priority},
                     {"balanced_keep", keep}, {"estimated_gib", amount}, {"estimate", estimate}});
  }
  const double safety = profile.memory_safety_reserve_gib;
  json strategies = json::object();
  for (const auto& [name, amount] : std::vector<std::pair<std::string, double>>{
       {"sequential", largest + safety}, {"all", sum + safety}, {"balanced", kept + largest_swap + safety}}) {
    strategies[name] = {{"required_gib", amount}, {"required_mb", amount * gib / 1e6},
        {"fits_estimate", budget_gib > 0 && budget_valid ? json(amount <= budget_gib + 1e-9) : json(nullptr)}};
  }
  return {{"workload", profile.name}, {"status", "estimated"}, {"models", models},
          {"budget_gib", budget_gib > 0 ? json(budget_gib) : json(nullptr)},
          {"global_limit_gib", global_limit_gib > 0 ? json(global_limit_gib) : json(nullptr)},
          {"budget_valid", budget_valid}, {"cache_layouts_verified", verified},
          {"selected_strategy", profile.residency_strategy.empty() ? "balanced" : profile.residency_strategy},
          {"safety_reserve_gib", safety}, {"strategies", strategies},
          {"assumptions", "Independent workers; no cross-process weight/cache sharing. Sequential waits for unload. Balanced minimum retains keep_resident plus the largest swappable worker; priority controls optional residency."}};
}

void validate_memory_strategy(const Registry& registry, const Profile& profile, double global_limit_gib) {
  if (profile.memory_limit_gib > global_limit_gib + 1e-9)
    throw std::runtime_error("workload memory.limit_gib exceeds global allocation");
  if (profile.residency_strategy.empty()) return; // Preserve undeclared legacy policy.
  const auto report = workload_memory_report(registry, profile, 0, global_limit_gib);
  if (!report.at("strategies").at(profile.residency_strategy).at("fits_estimate").get<bool>())
    throw std::runtime_error("selected residency strategy exceeds workload/global memory budget");
}

void apply_residency_strategy(Profile& profile) {
  if (profile.residency_strategy.empty()) return;
  if (profile.residency_strategy != "all" && profile.residency_strategy != "sequential" && profile.residency_strategy != "balanced")
    throw std::invalid_argument("strategy must be all, sequential or balanced");
  for (auto& policy : profile.model_policies) {
    const bool keep = std::find(profile.balanced_keep_models.begin(), profile.balanced_keep_models.end(), policy.id) != profile.balanced_keep_models.end();
    if (profile.residency_strategy == "all") { policy.residency = Residency::pinned; policy.startup = true; }
    else if (profile.residency_strategy == "sequential") policy.residency = policy.startup ? Residency::warm : Residency::on_demand;
    else if (keep) { policy.residency = Residency::pinned; policy.startup = true; }
    else if (policy.residency == Residency::pinned) policy.residency = Residency::warm;
  }
  profile.maximum_resident_workers = profile.residency_strategy == "sequential" ? 1 : 0;
}
}  // namespace mica
