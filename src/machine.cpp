#include "mica_server/machine.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "mica_server/profiles.hpp"

namespace mica {
namespace {

using json = nlohmann::json;

void check_fields(const json& object, std::initializer_list<const char*> allowed,
                  const std::string& label) {
  if (!object.is_object()) {
    throw std::invalid_argument(label + " must be an object");
  }
  for (auto item = object.begin(); item != object.end(); ++item) {
    if (std::none_of(allowed.begin(), allowed.end(), [&](const char* field) {
          return item.key() == field;
        })) {
      throw std::invalid_argument(label + " contains unknown field: " + item.key());
    }
  }
}

double number_limit(const json& object, const char* field, bool allow_zero) {
  const auto& value = object.at(field);
  if (!value.is_number() || !std::isfinite(value.get<double>()) ||
      value.get<double>() < 0 || (!allow_zero && value.get<double>() == 0)) {
    throw std::invalid_argument(std::string(field) + " must be a finite " +
                                (allow_zero ? "nonnegative" : "positive") + " number");
  }
  return value.get<double>();
}

int integer_limit(const json& object, const char* field) {
  const auto& value = object.at(field);
  if (!value.is_number_integer() || value.get<int>() < 1) {
    throw std::invalid_argument(std::string(field) + " must be a positive integer");
  }
  return value.get<int>();
}

std::string device_name(const HardwareInfo::Accelerator& accelerator) {
  return accelerator.runtime + ":" + accelerator.id;
}

}  // namespace

MachinePolicy machine_policy_from_document(const json& document) {
  check_fields(document, {"schema", "allowed_devices", "limits"}, "machine policy");
  if (!document.contains("schema") || !document.at("schema").is_number_integer() ||
      document.at("schema").get<int>() != 1) {
    throw std::invalid_argument("machine policy must use schema 1");
  }
  MachinePolicy policy;
  if (document.contains("allowed_devices")) {
    const auto& devices = document.at("allowed_devices");
    if (!devices.is_array() || devices.empty()) {
      throw std::invalid_argument("allowed_devices must be a nonempty array");
    }
    std::vector<std::string> parsed;
    for (const auto& item : devices) {
      if (!item.is_string()) {
        throw std::invalid_argument("allowed_devices must contain strings");
      }
      parsed.push_back(item.get<std::string>());
    }
    policy.allowed_devices = std::move(parsed);
  }
  if (!document.contains("limits")) return policy;
  const auto& limits = document.at("limits");
  check_fields(limits, {"inference", "build"}, "machine limits");
  if (limits.contains("inference")) {
    const auto& inference = limits.at("inference");
    check_fields(inference, {"ram_gib", "dedicated_memory_gib", "cpu_threads"},
                 "inference limits");
    if (inference.contains("ram_gib")) {
      policy.inference_ram_gib = number_limit(inference, "ram_gib", false);
    }
    if (inference.contains("cpu_threads")) {
      policy.cpu_threads = integer_limit(inference, "cpu_threads");
    }
    if (inference.contains("dedicated_memory_gib")) {
      const auto& memory = inference.at("dedicated_memory_gib");
      if (!memory.is_object()) {
        throw std::invalid_argument("dedicated_memory_gib must be an object");
      }
      for (auto item = memory.begin(); item != memory.end(); ++item) {
        policy.dedicated_memory_gib[item.key()] =
            number_limit(memory, item.key().c_str(), true);
      }
    }
  }
  if (limits.contains("build")) {
    const auto& build = limits.at("build");
    check_fields(build, {"ram_gib", "parallel_jobs"}, "build limits");
    if (build.contains("ram_gib")) {
      policy.build_ram_gib = number_limit(build, "ram_gib", false);
    }
    if (build.contains("parallel_jobs")) {
      policy.build_parallel_jobs = integer_limit(build, "parallel_jobs");
    }
  }
  return policy;
}

MachinePolicy load_machine_policy(const std::filesystem::path& path) {
  if (path.extension() != ".yaml" && path.extension() != ".yml") {
    throw std::invalid_argument("machine policy must be YAML: " + path.string());
  }
  return machine_policy_from_document(read_profile_file(path));
}

ResolvedMachinePolicy resolve_machine_policy(const HardwareInfo& hardware,
                                             const MachinePolicy& policy) {
  if (hardware.ram_gib <= 0) {
    throw std::invalid_argument("machine policy requires detected RAM capacity");
  }
  ResolvedMachinePolicy resolved;
  resolved.allowed_devices.insert("cpu");
  for (const auto& accelerator : hardware.accelerators) {
    resolved.allowed_devices.insert(device_name(accelerator));
    if (!accelerator.unified_memory && accelerator.memory_gib > 0) {
      resolved.dedicated_memory_gib[device_name(accelerator)] =
          accelerator.memory_gib;
    }
  }
  if (policy.allowed_devices) {
    std::set<std::string> requested(policy.allowed_devices->begin(),
                                    policy.allowed_devices->end());
    if (requested.size() != policy.allowed_devices->size()) {
      throw std::invalid_argument("allowed_devices contains a duplicate device");
    }
    for (const auto& device : requested) {
      if (!resolved.allowed_devices.contains(device)) {
        throw std::invalid_argument("machine policy device is not detected: " + device);
      }
    }
    resolved.allowed_devices = std::move(requested);
  }
  resolved.inference_ram_gib = policy.inference_ram_gib.value_or(hardware.ram_gib);
  if (resolved.inference_ram_gib > hardware.ram_gib) {
    throw std::invalid_argument("machine RAM limit exceeds detected RAM");
  }
  resolved.cpu_threads = policy.cpu_threads.value_or(
      hardware.logical_cpu_cores > 0 ? hardware.logical_cpu_cores : 1);
  if (hardware.logical_cpu_cores > 0 &&
      resolved.cpu_threads > hardware.logical_cpu_cores) {
    throw std::invalid_argument("machine CPU thread limit exceeds detected cores");
  }
  resolved.build_ram_gib =
      policy.build_ram_gib.value_or(std::min(16.0, hardware.ram_gib));
  if (resolved.build_ram_gib > hardware.ram_gib) {
    throw std::invalid_argument("machine build RAM limit exceeds detected RAM");
  }
  resolved.build_parallel_jobs = policy.build_parallel_jobs.value_or(
      std::min(2, std::max(1, hardware.logical_cpu_cores)));
  if (hardware.logical_cpu_cores > 0 &&
      resolved.build_parallel_jobs > hardware.logical_cpu_cores) {
    throw std::invalid_argument("machine build jobs exceed detected cores");
  }
  for (const auto& [device, limit] : policy.dedicated_memory_gib) {
    const auto found = resolved.dedicated_memory_gib.find(device);
    if (found == resolved.dedicated_memory_gib.end()) {
      throw std::invalid_argument("no detected dedicated memory for " + device);
    }
    if (limit > found->second) {
      throw std::invalid_argument("machine dedicated memory limit exceeds detected capacity: " +
                                  device);
    }
    if (limit == 0) {
      if (policy.allowed_devices && resolved.allowed_devices.contains(device)) {
        throw std::invalid_argument("zero memory conflicts with allowed device: " + device);
      }
      resolved.allowed_devices.erase(device);
    }
    found->second = limit;
  }
  for (auto item = resolved.dedicated_memory_gib.begin();
       item != resolved.dedicated_memory_gib.end();) {
    if (!resolved.allowed_devices.contains(item->first)) {
      item = resolved.dedicated_memory_gib.erase(item);
    } else {
      ++item;
    }
  }
  return resolved;
}

std::optional<MachinePolicy> legacy_machine_policy_from_runtime(
    const std::filesystem::path& path, const HardwareInfo& hardware) {
  if (!std::filesystem::exists(path)) return std::nullopt;
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read legacy runtime: " + path.string());
  const auto state = json::parse(input);
  if (!state.is_object()) {
    throw std::invalid_argument("legacy runtime must be an object");
  }
  MachinePolicy policy;
  if (state.contains("max_ram_gib") && !state.at("max_ram_gib").is_null()) {
    const auto limit = number_limit(state, "max_ram_gib", false);
    policy.inference_ram_gib = std::min(limit, hardware.ram_gib);
  }
  if (state.contains("max_vram_gib") && !state.at("max_vram_gib").is_null()) {
    const auto limit = number_limit(state, "max_vram_gib", true);
    std::vector<const HardwareInfo::Accelerator*> dedicated;
    for (const auto& accelerator : hardware.accelerators) {
      if (!accelerator.unified_memory && accelerator.memory_gib > 0) {
        dedicated.push_back(&accelerator);
      }
    }
    // A single aggregate legacy VRAM number cannot be assigned safely across
    // multiple devices. Keep only the unambiguous one-device case.
    if (dedicated.size() == 1) {
      policy.dedicated_memory_gib[device_name(*dedicated.front())] =
          std::min(limit, dedicated.front()->memory_gib);
    }
  }
  return policy;
}

bool ensure_machine_policy_if_absent(const std::filesystem::path& path,
                                     const MachinePolicy& policy) {
  if (path.extension() != ".yaml" && path.extension() != ".yml") {
    throw std::invalid_argument("machine policy must be YAML: " + path.string());
  }
  std::ostringstream output;
  output << std::setprecision(17) << "schema: 1\n";
  if (policy.allowed_devices) {
    output << "allowed_devices: [";
    for (std::size_t i = 0; i < policy.allowed_devices->size(); ++i) {
      if (i) output << ", ";
      output << '"' << policy.allowed_devices->at(i) << '"';
    }
    output << "]\n";
  }
  if (policy.inference_ram_gib || policy.cpu_threads ||
      !policy.dedicated_memory_gib.empty() || policy.build_ram_gib ||
      policy.build_parallel_jobs) {
    output << "limits:\n";
    if (policy.inference_ram_gib || policy.cpu_threads ||
        !policy.dedicated_memory_gib.empty()) {
      output << "  inference:\n";
      if (policy.inference_ram_gib) {
        output << "    ram_gib: " << *policy.inference_ram_gib << '\n';
      }
      if (policy.cpu_threads) {
        output << "    cpu_threads: " << *policy.cpu_threads << '\n';
      }
      if (!policy.dedicated_memory_gib.empty()) {
        output << "    dedicated_memory_gib:\n";
        for (const auto& [device, limit] : policy.dedicated_memory_gib) {
          output << "      \"" << device << "\": " << limit << '\n';
        }
      }
    }
    if (policy.build_ram_gib || policy.build_parallel_jobs) {
      output << "  build:\n";
      if (policy.build_ram_gib) output << "    ram_gib: " << *policy.build_ram_gib << '\n';
      if (policy.build_parallel_jobs) {
        output << "    parallel_jobs: " << *policy.build_parallel_jobs << '\n';
      }
    }
  }
  const auto contents = output.str();
  std::filesystem::create_directories(path.parent_path());
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    if (errno == EEXIST) return false;
    throw std::runtime_error("cannot create machine policy: " + path.string() +
                             ": " + std::strerror(errno));
  }
  std::size_t written = 0;
  while (written < contents.size()) {
    const auto count = ::write(descriptor, contents.data() + written,
                               contents.size() - written);
    if (count <= 0) {
      const auto error = errno;
      ::close(descriptor);
      std::filesystem::remove(path);
      throw std::runtime_error("cannot write machine policy: " +
                               std::string(std::strerror(error)));
    }
    written += static_cast<std::size_t>(count);
  }
  if (::close(descriptor) != 0) {
    const auto error = errno;
    std::filesystem::remove(path);
    throw std::runtime_error("cannot close machine policy: " +
                             std::string(std::strerror(error)));
  }
  return true;
}

}  // namespace mica
