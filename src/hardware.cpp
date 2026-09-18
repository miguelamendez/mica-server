#include "mica_server/hardware.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace mica {
namespace {

using json = nlohmann::json;

std::string capture(const char* command) {
  std::array<char, 512> buffer{};
  std::string output;
  FILE* pipe = popen(command, "r");
  if (pipe == nullptr) return output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  pclose(pipe);
  return output;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

#ifdef __linux__
std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
#endif

double total_ram_gib() {
#ifdef __APPLE__
  std::uint64_t bytes = 0;
  std::size_t size = sizeof(bytes);
  if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  }
#endif
  const auto pages = sysconf(_SC_PHYS_PAGES);
  const auto page_size = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0) {
    return static_cast<double>(pages) * static_cast<double>(page_size) /
           (1024.0 * 1024.0 * 1024.0);
  }
  return 0.0;
}

#ifdef __APPLE__
std::string sysctl_string(const char* name) {
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
  while (!value.empty() && value.back() == '\0') value.pop_back();
  return value;
}

int sysctl_integer(const char* name) {
  std::uint32_t value = 0;
  std::size_t size = sizeof(value);
  return sysctlbyname(name, &value, &size, nullptr, 0) == 0
             ? static_cast<int>(value)
             : 0;
}
#endif

#ifdef __linux__
bool detect_wsl() {
  std::ifstream version("/proc/version");
  std::stringstream contents;
  contents << version.rdbuf();
  return lower(contents.str()).find("microsoft") != std::string::npos;
}

std::string cpuinfo_value(const std::string& wanted) {
  std::ifstream input("/proc/cpuinfo");
  std::string line;
  while (std::getline(input, line)) {
    const auto separator = line.find(':');
    if (separator == std::string::npos) continue;
    if (trim(line.substr(0, separator)) == wanted) return trim(line.substr(separator + 1));
  }
  return {};
}
#endif

std::vector<HardwareInfo::Accelerator> detect_nvidia() {
  const auto output = capture(
      "nvidia-smi --query-gpu=index,name,memory.total,driver_version,compute_cap "
      "--format=csv,noheader,nounits 2>/dev/null");
  std::vector<HardwareInfo::Accelerator> devices;
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    std::vector<std::string> fields;
    std::istringstream values(line);
    std::string value;
    while (std::getline(values, value, ',')) fields.push_back(trim(value));
    if (fields.size() < 4) continue;
    HardwareInfo::Accelerator device;
    device.id = fields[0];
    device.type = "gpu";
    device.vendor = "nvidia";
    device.name = fields[1];
    device.runtime = "cuda";
    device.driver = fields[3];
    if (fields.size() > 4) device.architecture = "compute_" + fields[4];
    try {
      device.memory_gib = std::stod(fields[2]) / 1024.0;
    } catch (...) {
    }
    device.apis = {"cuda", "vulkan"};
    devices.push_back(std::move(device));
  }
  return devices;
}

void derive_targets(HardwareInfo* info) {
  info->mlx_target = info->supports_mlx() ? "metal" : "unsupported";
  info->gguf_target = "cpu";
  info->audio_target = "cpu";
  info->vllm_target = "cpu";
  const auto choose = [&](const std::string& runtime, const std::string& gguf,
                          const std::string& audio, const std::string& vllm) {
    if (!info->has_runtime(runtime)) return false;
    info->gguf_target = gguf;
    info->audio_target = audio;
    info->vllm_target = vllm;
    return true;
  };
  if (choose("metal", "metal", "metal", "metal")) return;
  if (choose("cuda", "cuda", "cuda", "cuda")) return;
  if (choose("rocm", "hip", "hip", "rocm")) return;
  if (choose("xpu", "sycl", "vulkan", "xpu")) return;
  if (info->has_runtime("tpu")) info->vllm_target = "tpu";
}

}  // namespace

bool HardwareInfo::has_runtime(const std::string& runtime) const {
  return std::any_of(accelerators.begin(), accelerators.end(), [&](const auto& device) {
    return device.runtime == runtime;
  });
}

double HardwareInfo::largest_memory_gib(const std::string& runtime) const {
  double largest = 0.0;
  for (const auto& device : accelerators) {
    if (runtime.empty() || device.runtime == runtime) {
      largest = std::max(largest, device.memory_gib);
    }
  }
  return largest;
}

bool HardwareInfo::supports_mlx() const {
  return os == "macos" && apple_silicon &&
         (mlx_target == "metal" || has_runtime("metal"));
}

bool HardwareInfo::supports_gguf() const {
  return os == "macos" || os == "linux" || os == "wsl";
}

bool HardwareInfo::supports_vllm() const {
  return (os == "macos" && apple_silicon) || os == "linux" || os == "wsl";
}

Backend HardwareInfo::recommended_backend() const {
  return supports_mlx() ? Backend::mlx : Backend::gguf;
}

VllmDevice HardwareInfo::recommended_vllm_device() const {
  if (vllm_target == "metal") return VllmDevice::metal;
  if (vllm_target == "cuda") return VllmDevice::cuda;
  if (vllm_target == "rocm") return VllmDevice::rocm;
  if (vllm_target == "xpu") return VllmDevice::xpu;
  if (vllm_target == "tpu") return VllmDevice::tpu;
  return VllmDevice::cpu;
}

HardwareInfo detect_hardware() {
  HardwareInfo info;
  utsname value{};
  if (uname(&value) == 0) info.arch = value.machine;
#ifdef __APPLE__
  info.os = "macos";
  info.os_version = trim(capture("sw_vers -productVersion 2>/dev/null"));
  info.cpu_vendor = "apple";
  info.cpu_model = sysctl_string("machdep.cpu.brand_string");
  if (info.cpu_model.empty()) info.cpu_model = sysctl_string("hw.model");
  info.physical_cpu_cores = sysctl_integer("hw.physicalcpu");
  info.logical_cpu_cores = sysctl_integer("hw.logicalcpu");
#elif defined(__linux__)
  info.wsl = detect_wsl();
  info.os = info.wsl ? "wsl" : "linux";
  info.os_version = trim(capture("uname -r 2>/dev/null"));
  info.cpu_vendor = cpuinfo_value("vendor_id");
  if (info.cpu_vendor.empty()) info.cpu_vendor = cpuinfo_value("CPU implementer");
  info.cpu_model = cpuinfo_value("model name");
  if (info.cpu_model.empty()) info.cpu_model = cpuinfo_value("Hardware");
  info.logical_cpu_cores = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
#else
  info.os = "unsupported";
#endif
  info.apple_silicon = info.os == "macos" &&
                       (info.arch == "arm64" || info.arch == "aarch64");
  info.ram_gib = total_ram_gib();
  if (info.apple_silicon) {
    info.unified_memory_gib = info.ram_gib;
    HardwareInfo::Accelerator apple;
    apple.id = "0";
    apple.type = "gpu";
    apple.vendor = "apple";
    apple.name = info.cpu_model.empty() ? "Apple Silicon GPU" : info.cpu_model + " GPU";
    apple.runtime = "metal";
    apple.memory_gib = info.ram_gib;
    apple.unified_memory = true;
    apple.apis = {"metal"};
    info.accelerators.push_back(std::move(apple));
  }
  auto nvidia = detect_nvidia();
  info.accelerators.insert(info.accelerators.end(), nvidia.begin(), nvidia.end());
  info.nvidia_detected = !nvidia.empty();
  for (const auto& device : nvidia) info.nvidia_vram_gib.push_back(device.memory_gib);
  derive_targets(&info);
  return info;
}

HardwareInfo hardware_from_json(const json& profile) {
  if (profile.value("schema", 0) != 1) {
    throw std::runtime_error("unsupported hardware profile schema");
  }
  HardwareInfo info;
  const auto& system = profile.at("system");
  info.os = system.at("os").get<std::string>();
  info.os_version = system.value("version", std::string());
  info.arch = system.at("arch").get<std::string>();
  info.wsl = system.value("wsl", false);
  info.apple_silicon = system.value("apple_silicon", false);
  const auto& cpu = profile.at("cpu");
  info.cpu_vendor = cpu.value("vendor", std::string());
  info.cpu_model = cpu.value("model", std::string());
  info.physical_cpu_cores = cpu.value("physical_cores", 0);
  info.logical_cpu_cores = cpu.value("logical_cores", 0);
  const auto& memory = profile.at("memory");
  info.ram_gib = memory.at("system_ram_gib").get<double>();
  info.unified_memory_gib = memory.value("unified_memory_gib", 0.0);
  for (const auto& value : profile.value("accelerators", json::array())) {
    HardwareInfo::Accelerator device;
    device.id = value.value("id", std::string());
    device.type = value.value("type", std::string());
    device.vendor = value.value("vendor", std::string());
    device.name = value.value("name", std::string());
    device.runtime = value.value("runtime", std::string());
    device.architecture = value.value("architecture", std::string());
    device.driver = value.value("driver", std::string());
    device.memory_gib = value.value("memory_gib", 0.0);
    device.unified_memory = value.value("unified_memory", false);
    device.apis = value.value("apis", std::vector<std::string>{});
    info.accelerators.push_back(std::move(device));
  }
  if (profile.contains("backend_targets")) {
    const auto& targets = profile.at("backend_targets");
    info.mlx_target = targets.value("mlx", json::object()).value("device", "unsupported");
    info.gguf_target = targets.value("gguf", json::object()).value("device", "cpu");
    info.audio_target = targets.value("audio", json::object()).value("device", "cpu");
    info.vllm_target = targets.value("vllm", json::object()).value("device", "cpu");
  } else {
    derive_targets(&info);
  }
  info.toolchains = profile.value("toolchains", std::map<std::string, bool>{});
  const auto build_policy = profile.value("build_policy", json::object());
  info.build_memory_limit_gib =
      std::clamp(build_policy.value("max_memory_gib", 16.0), 1.0, 16.0);
  info.build_parallelism =
      std::clamp(build_policy.value("max_parallel_jobs", 2), 1, 2);
  for (const auto& device : info.accelerators) {
    if (device.runtime == "cuda") {
      info.nvidia_detected = true;
      info.nvidia_vram_gib.push_back(device.memory_gib);
    }
  }
  return info;
}

json hardware_to_json(const HardwareInfo& hardware) {
  json accelerators = json::array();
  for (const auto& device : hardware.accelerators) {
    accelerators.push_back({{"id", device.id},
                            {"type", device.type},
                            {"vendor", device.vendor},
                            {"name", device.name},
                            {"runtime", device.runtime},
                            {"architecture", device.architecture},
                            {"driver", device.driver},
                            {"memory_gib", device.memory_gib},
                            {"unified_memory", device.unified_memory},
                            {"apis", device.apis}});
  }
  return {{"schema", 1},
          {"system", {{"os", hardware.os},
                      {"version", hardware.os_version},
                      {"arch", hardware.arch},
                      {"wsl", hardware.wsl},
                      {"apple_silicon", hardware.apple_silicon}}},
          {"cpu", {{"vendor", hardware.cpu_vendor},
                   {"model", hardware.cpu_model},
                   {"physical_cores", hardware.physical_cpu_cores},
                   {"logical_cores", hardware.logical_cpu_cores}}},
          {"memory", {{"system_ram_gib", hardware.ram_gib},
                      {"unified_memory_gib", hardware.unified_memory_gib}}},
          {"accelerators", accelerators},
          {"toolchains", hardware.toolchains},
          {"build_policy", {{"max_memory_gib", hardware.build_memory_limit_gib},
                            {"max_parallel_jobs", hardware.build_parallelism}}},
          {"backend_targets", {{"mlx", {{"device", hardware.mlx_target}}},
                               {"gguf", {{"device", hardware.gguf_target}}},
                               {"audio", {{"device", hardware.audio_target}}},
                               {"vllm", {{"device", hardware.vllm_target}}}}}};
}

HardwareInfo load_hardware_profile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read hardware profile: " + path.string());
  return hardware_from_json(json::parse(input));
}

void write_hardware_profile(const HardwareInfo& hardware,
                            const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::trunc);
  if (!output) throw std::runtime_error("cannot write hardware profile: " + path.string());
  output << std::setw(2) << hardware_to_json(hardware) << '\n';
}

}  // namespace mica
