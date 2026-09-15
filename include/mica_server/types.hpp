#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mica {

enum class Backend { mlx, gguf, vllm };
enum class Quantization { q4, q8, native };
enum class VllmDevice { automatic, cpu, cuda, metal };

std::string to_string(Backend value);
std::string to_string(Quantization value);
Backend parse_backend(const std::string& value);
Quantization parse_quantization(const std::string& value);
std::string to_string(VllmDevice value);
VllmDevice parse_vllm_device(const std::string& value);

struct HardwareInfo {
  std::string os;
  std::string os_version;
  std::string arch;
  bool apple_silicon{false};
  bool wsl{false};
  double ram_gib{0.0};
  bool nvidia_detected{false};
  std::vector<double> nvidia_vram_gib;

  [[nodiscard]] bool supports_mlx() const;
  [[nodiscard]] bool supports_gguf() const;
  [[nodiscard]] bool supports_vllm() const;
  [[nodiscard]] Backend recommended_backend() const;
  [[nodiscard]] VllmDevice recommended_vllm_device() const;
};

struct Artifact {
  bool supported{false};
  std::string pattern;
  std::string reason;
  double reservation_gib{0.0};
  std::string projector_pattern;
};

struct ModelDefinition {
  std::string id;
  std::string capability;
  std::string description;
  std::vector<std::string> tags;
  std::string source_repo;
  std::map<Backend, std::string> repositories;
  int startup_priority{100};
  bool required{false};
  std::map<Backend, bool> required_by_backend;
  std::map<Backend, std::map<Quantization, Artifact>> artifacts;

  [[nodiscard]] bool required_for(Backend backend) const {
    const auto found = required_by_backend.find(backend);
    return found == required_by_backend.end() ? required : found->second;
  }
};

struct Profile {
  std::string name;
  Quantization quantization{Quantization::q4};
  std::vector<std::string> models;
};

struct Policy {
  int idle_ttl_seconds{300};
  double safety_margin{1.15};
  int min_system_available_ram_gib{6};
  int load_timeout_seconds{180};
  int queue_timeout_seconds{60};
};

struct Registry {
  std::string default_hf_repo;
  std::string llama_cpp_revision;
  std::string audio_cpp_revision;
  std::vector<ModelDefinition> models;
  std::map<std::string, Profile> profiles;
  Policy policy;

  [[nodiscard]] const ModelDefinition& model(const std::string& id) const;
  [[nodiscard]] const Profile& profile(const std::string& name) const;
};

struct ResidentModel {
  std::string id;
  double reservation_gib{0.0};
  std::uint64_t last_used_monotonic_ns{0};
  bool ttl_expired{false};
  int in_flight{0};
};

struct StartupPlan {
  std::vector<std::string> admitted;
  std::vector<std::string> skipped;
  double reserved_gib{0.0};
  std::optional<std::string> error;
};

}  // namespace mica
