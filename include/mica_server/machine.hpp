#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "mica_server/types.hpp"

namespace mica {

// User-owned limits. Missing fields inherit detected capacity and conservative
// build defaults; this is deliberately separate from detector-owned facts.
struct MachinePolicy {
  std::optional<std::vector<std::string>> allowed_devices;
  std::optional<double> inference_ram_gib;
  std::map<std::string, double> dedicated_memory_gib;
  std::optional<int> cpu_threads;
  std::optional<double> build_ram_gib;
  std::optional<int> build_parallel_jobs;
};

struct ResolvedMachinePolicy {
  std::set<std::string> allowed_devices;
  double inference_ram_gib{0.0};
  std::map<std::string, double> dedicated_memory_gib;
  int cpu_threads{0};
  double build_ram_gib{0.0};
  int build_parallel_jobs{0};
};

MachinePolicy machine_policy_from_document(const nlohmann::json& document);
MachinePolicy load_machine_policy(const std::filesystem::path& path);
ResolvedMachinePolicy resolve_machine_policy(const HardwareInfo& hardware,
                                             const MachinePolicy& policy);
// Imports the last resolved legacy limits once, when no user policy exists.
std::optional<MachinePolicy> legacy_machine_policy_from_runtime(
    const std::filesystem::path& path, const HardwareInfo& hardware);
// Creates a user-owned policy only when absent. Returns true if created.
bool ensure_machine_policy_if_absent(const std::filesystem::path& path,
                                     const MachinePolicy& policy = {});

}  // namespace mica
