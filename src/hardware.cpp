#include "mica_server/hardware.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace mica {
namespace {

std::string capture(const char* command) {
  std::array<char, 256> buffer{};
  std::string output;
  FILE* pipe = popen(command, "r");
  if (pipe == nullptr) return output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  pclose(pipe);
  return output;
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

#ifdef __linux__
bool detect_wsl() {
  std::ifstream version("/proc/version");
  std::stringstream contents;
  contents << version.rdbuf();
  return lower(contents.str()).find("microsoft") != std::string::npos;
}
#endif

std::vector<double> detect_nvidia_vram() {
  const auto output = capture(
      "nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null");
  std::vector<double> result;
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    try {
      const double mib = std::stod(line);
      if (mib > 0) result.push_back(mib / 1024.0);
    } catch (...) {
    }
  }
  return result;
}

}  // namespace

bool HardwareInfo::supports_mlx() const { return os == "macos" && apple_silicon; }

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
  if (supports_mlx()) return VllmDevice::metal;
  if ((os == "linux" || os == "wsl") && nvidia_detected) return VllmDevice::cuda;
  return VllmDevice::cpu;
}

HardwareInfo detect_hardware() {
  HardwareInfo info;
  utsname value{};
  if (uname(&value) == 0) info.arch = value.machine;
#ifdef __APPLE__
  info.os = "macos";
  info.os_version = capture("sw_vers -productVersion 2>/dev/null");
  info.os_version.erase(
      std::remove(info.os_version.begin(), info.os_version.end(), '\n'),
      info.os_version.end());
#elif defined(__linux__)
  info.wsl = detect_wsl();
  info.os = info.wsl ? "wsl" : "linux";
#else
  info.os = "unsupported";
#endif
  info.apple_silicon = info.os == "macos" &&
                       (info.arch == "arm64" || info.arch == "aarch64");
  info.ram_gib = total_ram_gib();
  info.nvidia_vram_gib = detect_nvidia_vram();
  info.nvidia_detected = !info.nvidia_vram_gib.empty();
  return info;
}

}  // namespace mica
