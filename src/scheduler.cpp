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
  if (system_memory_only) {
    // A discrete profile may declare only its small host-side overhead. If
    // that same logical placement resolves to unified memory, the complete
    // model must still be charged to RAM rather than just that overhead.
    resolved.ram_reservation_gib = policy.ram_reservation_gib >= 0
                                       ? std::max(policy.ram_reservation_gib,
                                                  artifact.reservation_gib)
                                       : artifact.reservation_gib;
  } else {
    resolved.ram_reservation_gib = policy.ram_reservation_gib >= 0
                                       ? policy.ram_reservation_gib
                                       : 0.5;
    resolved.vram_reservation_gib = policy.vram_reservation_gib >= 0
                                        ? policy.vram_reservation_gib
                                        : artifact.reservation_gib;
  }
  resolved.gpu_layers = policy.gpu_layers >= 0
                            ? policy.gpu_layers
                            : system_memory_only ? 0 : 99;
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
    const auto& artifact = model.artifacts.at(backend).at(quantization);
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
    const HardwareInfo& hardware) {
  if (profile.schema < 3) {
    return plan_profile_startup(registry, profile, backend, quantization,
                                max_ram_gib);
  }
  StartupPlan plan;
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
                               .artifacts.at(backend).at(quantization);
    const auto placement = resolve_model_placement(*policy, artifact, hardware);
    const auto ram = placement.ram_reservation_gib;
    const auto vram = placement.vram_reservation_gib;
    const bool fits_ram = plan.reserved_ram_gib + ram <= max_ram_gib + 1e-9;
    const bool fits_vram = plan.reserved_vram_gib + vram <= max_vram_gib + 1e-9;
    if (fits_ram && fits_vram) {
      plan.admitted.push_back(policy->id);
      plan.reserved_ram_gib += ram;
      plan.reserved_vram_gib += vram;
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
