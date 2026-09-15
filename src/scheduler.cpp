#include "mica_server/scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mica {

StartupPlan plan_startup(const Registry& registry, const Profile& profile,
                         Backend backend, Quantization quantization,
                         double max_ram_gib) {
  StartupPlan plan;
  std::vector<const ModelDefinition*> selected;
  selected.reserve(profile.models.size());
  for (const auto& id : profile.models) selected.push_back(&registry.model(id));
  std::stable_sort(selected.begin(), selected.end(), [](const auto* left, const auto* right) {
    if (left->required != right->required) return left->required > right->required;
    if (left->startup_priority != right->startup_priority) {
      return left->startup_priority < right->startup_priority;
    }
    return left->id < right->id;
  });

  for (const auto* model : selected) {
    const auto backend_it = model->artifacts.find(backend);
    if (backend_it == model->artifacts.end()) {
      if (model->required) plan.error = model->id + ": backend is not declared";
      else plan.skipped.push_back(model->id);
      if (plan.error) return plan;
      continue;
    }
    const auto artifact_it = backend_it->second.find(quantization);
    if (artifact_it == backend_it->second.end() || !artifact_it->second.supported) {
      const std::string reason = artifact_it == backend_it->second.end()
                                     ? "quantization is not declared"
                                     : artifact_it->second.reason;
      if (model->required) plan.error = model->id + ": " + reason;
      else plan.skipped.push_back(model->id);
      if (plan.error) return plan;
      continue;
    }
    const double reservation = artifact_it->second.reservation_gib;
    if (reservation <= 0) {
      if (model->required) plan.error = model->id + ": missing measured reservation";
      else plan.skipped.push_back(model->id);
      if (plan.error) return plan;
      continue;
    }
    if (plan.reserved_gib + reservation <= max_ram_gib + 1e-9) {
      plan.admitted.push_back(model->id);
      plan.reserved_gib += reservation;
    } else if (model->required) {
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

}  // namespace mica

