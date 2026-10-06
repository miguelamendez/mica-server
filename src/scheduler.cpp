#include "mica_server/scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mica {
namespace {

std::string backend_target(const ProfileModel& policy,
                           const HardwareInfo& hardware) {
  if (policy.backend == Backend::mlx) return hardware.mlx_target;
  if (policy.backend == Backend::vllm) return hardware.vllm_target;
  if (policy.device_target == "audio") return hardware.audio_target;
  return hardware.gguf_target;
}

std::string device_runtime(const std::string& device) {
  const auto separator = device.find(':');
  return separator == std::string::npos ? device : device.substr(0, separator);
}

std::string device_id(const std::string& device) {
  const auto separator = device.find(':');
  return separator == std::string::npos ? "0" : device.substr(separator + 1);
}

}  // namespace

bool fits_device_reservation(const std::map<std::string, double>& used_gib,
                             const std::map<std::string, double>& limits_gib,
                             const std::string& device, double requested_gib,
                             double common_limit_gib) {
  if (!std::isfinite(requested_gib) || requested_gib < 0 ||
      !std::isfinite(common_limit_gib) || common_limit_gib < 0) return false;
  if (requested_gib == 0) return true;
  if (device.empty()) return false;
  double limit = common_limit_gib;
  if (!limits_gib.empty()) {
    const auto found = limits_gib.find(device);
    if (found == limits_gib.end() || !std::isfinite(found->second) ||
        found->second < 0) return false;
    limit = std::min(limit, found->second);
  }
  const auto used = used_gib.find(device);
  return (used == used_gib.end() ? 0.0 : used->second) + requested_gib <=
         limit + 1e-9;
}

ResolvedModelPlacement resolve_model_placement(
    const ProfileModel& policy, const Artifact& artifact,
    const HardwareInfo& hardware) {
  ResolvedModelPlacement resolved;
  const auto target = backend_target(policy, hardware);
  if (policy.placement_mode == "fixed" && policy.device == "cpu") {
    resolved.device = "cpu";
  } else {
    const auto requested = policy.placement_mode == "fixed"
                               ? policy.device
                               : std::string("accelerator:0");
    const auto requested_runtime = device_runtime(requested);
    const auto id = device_id(requested);
    const auto accelerator = std::find_if(
        hardware.accelerators.begin(), hardware.accelerators.end(),
        [&](const auto& item) { return item.id == id; });
    // A logical accelerator placement follows the engine's native target.
    // The same translation is needed for physical runtimes whose engine name
    // differs: ROCm -> HIP and XPU -> SYCL/Vulkan for native engines.
    const bool logical_device = requested_runtime == "auto" ||
                                requested_runtime == "accelerator";
    const bool physical_runtime =
        accelerator != hardware.accelerators.end() &&
        requested_runtime == accelerator->runtime;
    auto runtime = logical_device || physical_runtime ? target : requested_runtime;
    if (runtime.empty() || runtime == "unsupported") runtime = "cpu";
    resolved.device = runtime == "cpu" || runtime == "tpu"
                          ? runtime
                          : runtime + ":" + id;
    resolved.unified_memory = runtime == "metal" ||
                              (accelerator != hardware.accelerators.end() &&
                               accelerator->unified_memory);
  }

  const bool system_memory_only = resolved.device == "cpu" ||
                                  resolved.device == "tpu" ||
                                  resolved.unified_memory;
  // File bytes are a conservative lower bound, not a measured peak. KV grows
  // with context AND concurrent sequences; quantized caches include scales.
  std::uint64_t file_bytes = artifact.size_bytes + artifact.projector_size_bytes;
  if (!artifact.files.empty()) {
    file_bytes = 0;
    for (const auto& file : artifact.files) file_bytes += file.size_bytes;
  }
  const double file_gib = static_cast<double>(file_bytes) / (1024.0 * 1024 * 1024);
  const double cache_factor = policy.kv_cache_precision == "q4" ? 0.3125 :
                              policy.kv_cache_precision == "q8" ? 0.5625 : 1.0;
  const double kv_gib = static_cast<double>(artifact.kv_bytes_per_token_f16) *
      policy.max_total_tokens * policy.max_concurrent_requests * cache_factor /
      (1024.0 * 1024 * 1024);
  const double base_gib = std::max(artifact.reservation_gib, file_gib * 1.1 + 0.25);
  const double footprint = base_gib + kv_gib;
  const double host_cache_gib = policy.ram_cache_mib > 0 ? policy.ram_cache_mib / 1024.0 : 0;
  if (system_memory_only) {
    // A discrete profile may declare only its small host-side overhead. If
    // that same logical placement resolves to unified memory, the complete
    // model must still be charged to RAM rather than just that overhead.
    resolved.ram_reservation_gib = policy.ram_reservation_gib >= 0
                                       ? std::max(policy.ram_reservation_gib,
                                                  footprint + host_cache_gib)
                                       : footprint + host_cache_gib;
  } else {
    resolved.ram_reservation_gib = policy.ram_reservation_gib >= 0
                                       ? std::max(policy.ram_reservation_gib, 0.5 + host_cache_gib)
                                       : 0.5 + host_cache_gib;
    resolved.vram_reservation_gib = policy.vram_reservation_gib >= 0
                                        ? std::max(policy.vram_reservation_gib, footprint)
                                        : footprint;
    if (policy.projector_on_cpu && artifact.projector_size_bytes > 0) {
      const double projector_gib = artifact.projector_size_bytes / (1024.0 * 1024 * 1024);
      resolved.vram_reservation_gib = std::max(
          policy.vram_reservation_gib >= 0 ? policy.vram_reservation_gib : 0,
          resolved.vram_reservation_gib - projector_gib * 0.9);
      resolved.ram_reservation_gib += projector_gib * 1.25 + 0.25;
    }
    if (policy.draft_gpu_layers == 0 && policy.speculative_method != "none") {
      for (const auto& file : artifact.files) {
        if (file.role != "mtp-drafter" && file.role != "dflash-drafter") continue;
        const double draft_gib = file.size_bytes / (1024.0 * 1024 * 1024);
        resolved.ram_reservation_gib += draft_gib * 1.25 + 0.25;
        resolved.vram_reservation_gib = std::max(
            policy.vram_reservation_gib >= 0 ? policy.vram_reservation_gib : 0,
            resolved.vram_reservation_gib - draft_gib * 0.9);
      }
    }
    if (policy.backend == Backend::gguf && artifact.gguf_block_count > 0 &&
        policy.gpu_layers > 0 && policy.gpu_layers < artifact.gguf_block_count &&
        artifact.projector_pattern.empty() &&
        std::none_of(artifact.files.begin(), artifact.files.end(), [](const auto& f) {
          return f.role == "mtp-drafter" || f.role == "dflash-drafter";
        })) {
      // Conservative split: embeddings, allocator and graph overhead remain
      // charged to GPU; add 0.5 GiB rather than assuming uniform layer costs.
      // Only verified block counts and plain text GGUFs use this estimate.
      const double fraction = static_cast<double>(policy.gpu_layers) / artifact.gguf_block_count;
      resolved.vram_reservation_gib = std::max(
          policy.vram_reservation_gib >= 0 ? policy.vram_reservation_gib : 0,
          footprint * fraction + 0.5);
      resolved.ram_reservation_gib = std::max(
          policy.ram_reservation_gib >= 0 ? policy.ram_reservation_gib : 0.5,
          footprint * (1.0 - fraction) + 0.5 + host_cache_gib);
    }
  }
  resolved.gpu_layers = policy.gpu_layers >= 0
                            ? policy.gpu_layers
                            : resolved.device == "cpu" || resolved.device == "tpu" ? 0 : 99;
  return resolved;
}

StartupPlan plan_startup(const Registry& registry, const Profile& profile,
                         Backend backend, Quantization quantization,
                         double max_ram_gib) {
  StartupPlan plan;
  std::vector<const ModelDefinition*> selected;
  selected.reserve(profile.models.size());
  for (const auto& id : profile.models) selected.push_back(&registry.model(id));
  std::stable_sort(selected.begin(), selected.end(), [backend](const auto* left, const auto* right) {
    if (left->required_for(backend) != right->required_for(backend)) {
      return left->required_for(backend) > right->required_for(backend);
    }
    if (left->startup_priority != right->startup_priority) {
      return left->startup_priority < right->startup_priority;
    }
    return left->id < right->id;
  });

  for (const auto* model : selected) {
    const auto backend_it = model->artifacts.find(backend);
    if (backend_it == model->artifacts.end()) {
      if (model->required_for(backend)) plan.error = model->id + ": backend is not declared";
      else plan.skipped.push_back(model->id);
      if (plan.error) return plan;
      continue;
    }
    const auto artifact_it = backend_it->second.find(quantization);
    if (artifact_it == backend_it->second.end() || !artifact_it->second.supported) {
      const std::string reason = artifact_it == backend_it->second.end()
                                     ? "quantization is not declared"
                                     : artifact_it->second.reason;
      if (model->required_for(backend)) plan.error = model->id + ": " + reason;
      else plan.skipped.push_back(model->id);
      if (plan.error) return plan;
      continue;
    }
    const double reservation = artifact_it->second.reservation_gib;
    if (reservation <= 0) {
      if (model->required_for(backend)) plan.error = model->id + ": missing measured reservation";
      else plan.skipped.push_back(model->id);
      if (plan.error) return plan;
      continue;
    }
    if (plan.reserved_gib + reservation <= max_ram_gib + 1e-9) {
      plan.admitted.push_back(model->id);
      plan.reserved_gib += reservation;
    } else if (model->required_for(backend)) {
      const double deficit = plan.reserved_gib + reservation - max_ram_gib;
      plan.error = model->id + ": required startup baseline exceeds RAM budget by " +
                   std::to_string(std::ceil(deficit * 10.0) / 10.0) + " GiB";
      return plan;
    } else {
      plan.skipped.push_back(model->id);
    }
  }
  return plan;
}

StartupPlan plan_profile_startup(const Registry& registry, const Profile& profile,
                                 Backend backend, Quantization quantization,
                                 double max_ram_gib) {
  if (profile.schema < 3) {
    return plan_startup(registry, profile, backend, quantization, max_ram_gib);
  }
  StartupPlan plan;
  std::vector<const ProfileModel*> selected;
  for (const auto& policy : profile.model_policies) {
    if (policy.backend == backend && policy.quantization == quantization) {
      selected.push_back(&policy);
    }
  }
  std::stable_sort(selected.begin(), selected.end(), [](const auto* left, const auto* right) {
    if (left->startup != right->startup) return left->startup > right->startup;
    if (left->priority != right->priority) return left->priority > right->priority;
    return left->id < right->id;
  });
  for (const auto* policy : selected) {
    if (!policy->startup) {
      plan.skipped.push_back(policy->id);
      continue;
    }
    const auto& model = registry.model(policy->id);
    const auto& artifact = model.artifact_for(backend, quantization, policy->engine);
    if (plan.reserved_gib + artifact.reservation_gib <= max_ram_gib + 1e-9) {
      plan.admitted.push_back(policy->id);
      plan.reserved_gib += artifact.reservation_gib;
    } else if (policy->residency == Residency::pinned) {
      const double deficit = plan.reserved_gib + artifact.reservation_gib - max_ram_gib;
      plan.error = policy->id + ": pinned startup model exceeds the profile memory limit by " +
                   std::to_string(std::ceil(deficit * 10.0) / 10.0) + " GiB";
      return plan;
    } else {
      plan.skipped.push_back(policy->id);
    }
  }
  return plan;
}

StartupPlan plan_profile_startup_resources(
    const Registry& registry, const Profile& profile, Backend backend,
    Quantization quantization, double max_ram_gib, double max_vram_gib,
    const HardwareInfo& hardware,
    const std::map<std::string, double>& device_limits_gib) {
  if (profile.schema < 3) {
    return plan_profile_startup(registry, profile, backend, quantization,
                                max_ram_gib);
  }
  StartupPlan plan;
  std::map<std::string, double> used_vram_by_device;
  std::vector<const ProfileModel*> selected;
  for (const auto& policy : profile.model_policies) {
    if (policy.backend == backend && policy.quantization == quantization) {
      selected.push_back(&policy);
    }
  }
  std::stable_sort(selected.begin(), selected.end(), [](const auto* left,
                                                        const auto* right) {
    if (left->startup != right->startup) return left->startup > right->startup;
    if (left->priority != right->priority) return left->priority > right->priority;
    return left->id < right->id;
  });
  for (const auto* policy : selected) {
    if (!policy->startup) {
      plan.skipped.push_back(policy->id);
      continue;
    }
    const auto& artifact = registry.model(policy->id)
                               .artifact_for(backend, quantization, policy->engine);
    const auto placement = resolve_model_placement(*policy, artifact, hardware);
    const auto ram = placement.ram_reservation_gib;
    const auto vram = placement.vram_reservation_gib;
    std::string memory_device;
    if (vram > 0) {
      const auto id = device_id(placement.device);
      const auto found = std::find_if(hardware.accelerators.begin(),
                                      hardware.accelerators.end(),
          [&](const auto& accelerator) { return accelerator.id == id; });
      memory_device = found == hardware.accelerators.end()
                          ? placement.device : found->runtime + ":" + id;
    }
    const bool fits_ram = plan.reserved_ram_gib + ram <= max_ram_gib + 1e-9;
    const bool fits_vram = fits_device_reservation(
        used_vram_by_device, device_limits_gib, memory_device, vram,
        max_vram_gib);
    if (fits_ram && fits_vram) {
      plan.admitted.push_back(policy->id);
      plan.reserved_ram_gib += ram;
      plan.reserved_vram_gib += vram;
      if (vram > 0) {
        used_vram_by_device[memory_device] += vram;
        plan.reserved_vram_by_device_gib[memory_device] += vram;
      }
      plan.reserved_gib += ram + vram;
    } else if (policy->residency == Residency::pinned) {
      plan.error = policy->id + ": pinned startup model exceeds the " +
                   (!fits_ram ? std::string("RAM") : std::string("VRAM")) +
                   " limit";
      return plan;
    } else {
      plan.skipped.push_back(policy->id);
    }
  }
  return plan;
}

std::vector<ResidentModel> rank_eviction_candidates(
    std::vector<ResidentModel> residents) {
  residents.erase(std::remove_if(residents.begin(), residents.end(),
                                 [](const auto& item) { return item.in_flight != 0; }),
                  residents.end());
  std::sort(residents.begin(), residents.end(), [](const auto& left, const auto& right) {
    if (left.ttl_expired != right.ttl_expired) return left.ttl_expired > right.ttl_expired;
    if (left.last_used_monotonic_ns != right.last_used_monotonic_ns) {
      return left.last_used_monotonic_ns < right.last_used_monotonic_ns;
    }
    if (left.reservation_gib != right.reservation_gib) {
      return left.reservation_gib > right.reservation_gib;
    }
    return left.id < right.id;
  });
  return residents;
}

std::optional<double> vllm_memory_utilization(
    VllmDevice device, double max_ram_gib, double max_vram_gib,
    double accelerator_memory_gib) {
  if (accelerator_memory_gib <= 0) return std::nullopt;

  double limit_gib = 0;
  if (device == VllmDevice::metal) {
    limit_gib = max_ram_gib;
  } else if (device == VllmDevice::cuda || device == VllmDevice::rocm ||
             device == VllmDevice::xpu) {
    limit_gib = max_vram_gib;
  } else {
    return std::nullopt;
  }
  if (limit_gib <= 0) return std::nullopt;
  return std::clamp(limit_gib / accelerator_memory_gib, 0.01, 0.95);
}

}  // namespace mica
