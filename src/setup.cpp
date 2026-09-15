#include "mica_server/setup.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#ifdef __APPLE__
#include <Security/Security.h>
#else
#include <sys/random.h>
#endif

#include "mica_server/command.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/scheduler.hpp"

namespace mica {
namespace {

using json = nlohmann::json;

std::filesystem::path home_directory() {
  const char* home = std::getenv("HOME");
  if (!home || std::string(home).empty()) throw std::runtime_error("HOME is not set");
  return home;
}

void execute_or_print(const std::vector<std::string>& command, bool dry_run) {
  std::cout << (dry_run ? "[plan] " : "[run]  ") << display_command(command) << '\n';
  if (dry_run) return;
  const auto result = run_command(command, true);
  if (result.exit_code != 0) {
    throw std::runtime_error("command failed (" + std::to_string(result.exit_code) +
                             "): " + display_command(command) + "\n" + result.output);
  }
}

void install_mlx_environment(const ResolvedSetup& setup) {
  const auto environment = setup.options.root / "environment-mlx";
  const bool install_packages = setup.options.refresh ||
                                !std::filesystem::exists(environment / "bin/python");
  if (install_packages) {
    std::vector<std::string> command = {"uv", "venv", environment.string(), "--python", "3.11"};
    if (setup.options.refresh) command.emplace_back("--clear");
    execute_or_print(command, setup.options.dry_run);
  }
  if (install_packages) {
    execute_or_print(
        {"uv", "pip", "install", "--python", (environment / "bin/python").string(),
         "--upgrade", "mlx-audio[all,server]>=0.5.1", "mlx-vlm", "mlx-lm",
         "huggingface_hub[hf_xet]"},
        setup.options.dry_run);
  }
}

void install_download_environment(const ResolvedSetup& setup) {
  const auto environment = setup.options.root / "environment-tools";
  const bool install_packages = setup.options.refresh ||
                                !std::filesystem::exists(environment / "bin/python");
  if (install_packages) {
    std::vector<std::string> command = {"uv", "venv", environment.string(), "--python", "3.11"};
    if (setup.options.refresh) command.emplace_back("--clear");
    execute_or_print(command, setup.options.dry_run);
  }
  if (install_packages) {
    execute_or_print(
        {"uv", "pip", "install", "--python", (environment / "bin/python").string(),
         "--upgrade", "huggingface_hub[hf_xet]"},
        setup.options.dry_run);
  }
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

int version_major(const std::string& version) {
  try {
    return std::stoi(version.substr(0, version.find('.')));
  } catch (...) {
    return 0;
  }
}

std::string fetch_text(const std::string& url) {
  const auto result = run_command(
      {"curl", "--location", "--fail", "--silent", "--show-error", url}, true);
  if (result.exit_code != 0) {
    throw std::runtime_error("cannot resolve vLLM package metadata: " + result.output);
  }
  return result.output;
}

void update_source_tree(const std::filesystem::path& source, const std::string& url,
                        const std::string& revision, bool refresh, bool dry_run) {
  if (!std::filesystem::exists(source / ".git")) {
    execute_or_print({"git", "clone", url, source.string()}, dry_run);
  } else if (refresh) {
    execute_or_print({"git", "-C", source.string(), "fetch", "--tags", "origin"}, dry_run);
  }
  if (!revision.empty() && revision != "latest") {
    execute_or_print({"git", "-C", source.string(), "checkout", "--detach", revision}, dry_run);
  } else if (refresh) {
    execute_or_print({"git", "-C", source.string(), "checkout", "--detach", "origin/master"},
                     dry_run);
  }
}

void install_native_gguf_runtimes(const Registry& registry, const ResolvedSetup& setup) {
  const auto runtime = setup.options.root / "runtime";
  const auto llama_source = runtime / "llama.cpp";
  const auto llama_build = llama_source / "build-mica";
  update_source_tree(llama_source, "https://github.com/ggml-org/llama.cpp.git",
                     registry.llama_cpp_revision, setup.options.refresh,
                     setup.options.dry_run);
  std::vector<std::string> configure = {
      "cmake", "-S", llama_source.string(), "-B", llama_build.string(),
      "-DCMAKE_BUILD_TYPE=Release", "-DGGML_NATIVE=ON"};
  if (setup.hardware.os == "macos") configure.emplace_back("-DGGML_METAL=ON");
  if ((setup.hardware.os == "linux" || setup.hardware.os == "wsl") &&
      setup.options.max_vram_gib > 0) {
    configure.emplace_back("-DGGML_CUDA=ON");
  }
  execute_or_print(configure, setup.options.dry_run);
  execute_or_print({"cmake", "--build", llama_build.string(), "--config", "Release",
                    "--target", "llama-server", "llama-quantize", "-j"},
                   setup.options.dry_run);

  const auto audio_source = runtime / "audio.cpp";
  const auto audio_build = audio_source / "build-mica";
  update_source_tree(audio_source, "https://github.com/0xShug0/audio.cpp.git",
                     registry.audio_cpp_revision, setup.options.refresh,
                     setup.options.dry_run);
  std::vector<std::string> audio_configure = {
      "cmake", "-S", audio_source.string(), "-B", audio_build.string(),
      "-DCMAKE_BUILD_TYPE=Release"};
  if (setup.hardware.os == "macos") audio_configure.emplace_back("-DENGINE_ENABLE_METAL=ON");
  if ((setup.hardware.os == "linux" || setup.hardware.os == "wsl") &&
      setup.options.max_vram_gib > 0) {
    audio_configure.emplace_back("-DENGINE_ENABLE_CUDA=ON");
  }
  execute_or_print(audio_configure, setup.options.dry_run);
  execute_or_print({"cmake", "--build", audio_build.string(), "--config", "Release",
                    "--target", "audiocpp_server", "audiocpp_gguf", "-j"},
                   setup.options.dry_run);
}

void install_vllm_metal(const ResolvedSetup& setup,
                        const std::filesystem::path& environment) {
  if (setup.options.dry_run) {
    std::cout << "[plan] resolve and install the latest compatible stable vLLM + "
                 "vllm-metal release wheels\n";
    return;
  }
  const auto release = json::parse(fetch_text(
      "https://api.github.com/repos/vllm-project/vllm-metal/releases/latest"));
  const auto release_tag = release.value("tag_name", std::string());
  std::string metal_wheel;
  for (const auto& asset : release.value("assets", json::array())) {
    const auto name = asset.value("name", std::string());
    if (name.ends_with(".whl") && name.find("macosx_15_0_arm64") != std::string::npos) {
      metal_wheel = asset.value("browser_download_url", std::string());
      break;
    }
  }
  if (release_tag.empty() || metal_wheel.empty()) {
    throw std::runtime_error("latest vllm-metal release has no macOS arm64 wheel");
  }
  const auto vllm_tag = trim(fetch_text(
      "https://raw.githubusercontent.com/vllm-project/vllm-metal/" + release_tag +
      "/.github/vllm-release-tag.commit"));
  if (vllm_tag.size() < 2 || vllm_tag.front() != 'v') {
    throw std::runtime_error("vllm-metal release has invalid compatible vLLM metadata");
  }
  const auto version = vllm_tag.substr(1);
  const auto vllm_wheel =
      "https://github.com/vllm-project/vllm/releases/download/" + vllm_tag +
      "/vllm-" + version + "%2Bcpu-cp312-cp312-macosx_11_0_arm64.whl";
  execute_or_print({"uv", "pip", "install", "--python",
                    (environment / "bin/python").string(), "--upgrade", vllm_wheel,
                    "vllm-metal[stt] @ " + metal_wheel}, false);
}

void install_vllm_environment(const ResolvedSetup& setup) {
  const auto environment = setup.options.root / "environment-vllm";
  const auto device_marker = environment / ".mica-device";
  std::string installed_device;
  if (std::filesystem::exists(device_marker)) {
    std::ifstream marker_file(device_marker);
    std::getline(marker_file, installed_device);
  }
  const bool device_changed = installed_device != to_string(setup.vllm_device);
  const bool install_packages = setup.options.refresh || device_changed ||
                                !std::filesystem::exists(environment / "bin/python");
  if (install_packages) {
    std::vector<std::string> command = {"uv", "venv", environment.string(),
                                        "--python", "3.12"};
    if (setup.options.refresh || device_changed) command.emplace_back("--clear");
    execute_or_print(command, setup.options.dry_run);
  }
  if (!install_packages) return;

  const auto mark_installed = [&] {
    if (setup.options.dry_run) return;
    std::ofstream marker_file(device_marker, std::ios::trunc);
    marker_file << to_string(setup.vllm_device) << '\n';
  };

  if (setup.vllm_device == VllmDevice::metal) {
    install_vllm_metal(setup, environment);
    mark_installed();
    return;
  }
  if (setup.vllm_device == VllmDevice::cuda) {
    execute_or_print({"uv", "pip", "install", "--python",
                      (environment / "bin/python").string(), "--upgrade", "vllm",
                     "--torch-backend", "auto"},
                     setup.options.dry_run);
    mark_installed();
    return;
  }
  if (setup.hardware.os == "macos") {
    const auto source = setup.options.root / "runtime/vllm";
    update_source_tree(source, "https://github.com/vllm-project/vllm.git", "latest",
                       setup.options.refresh, setup.options.dry_run);
    execute_or_print({"uv", "pip", "install", "--python",
                      (environment / "bin/python").string(), "-r",
                      (source / "requirements/cpu.txt").string(), "--extra-index-url",
                      "https://download.pytorch.org/whl/cpu"},
                     setup.options.dry_run);
    execute_or_print({"env", "VLLM_TARGET_DEVICE=cpu", "uv", "pip", "install",
                      "--python", (environment / "bin/python").string(), source.string(),
                      "--no-build-isolation"},
                     setup.options.dry_run);
    mark_installed();
    return;
  }
  execute_or_print({"uv", "pip", "install", "--python",
                    (environment / "bin/python").string(), "--upgrade", "vllm",
                    "--extra-index-url", "https://wheels.vllm.ai/nightly/cpu",
                    "--index-strategy", "first-index", "--torch-backend", "cpu"},
                   setup.options.dry_run);
  mark_installed();
}

std::string resolved_git_revision(const std::filesystem::path& source) {
  if (!std::filesystem::exists(source / ".git")) return "not-installed";
  auto result = run_command({"git", "-C", source.string(), "rev-parse", "HEAD"}, true);
  if (result.exit_code != 0) return "unknown";
  result.output.erase(std::remove(result.output.begin(), result.output.end(), '\n'),
                      result.output.end());
  return result.output;
}

std::vector<std::string> resolved_packages(const std::filesystem::path& environment) {
  if (!std::filesystem::exists(environment / "bin/python")) return {};
  auto result = run_command({"uv", "pip", "freeze", "--python",
                             (environment / "bin/python").string()}, true);
  if (result.exit_code != 0) return {"resolution-unavailable"};
  std::vector<std::string> packages;
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty()) packages.push_back(line);
  }
  return packages;
}

void write_runtime_state(const Registry& registry, const ResolvedSetup& setup) {
  if (setup.options.dry_run) return;
  const auto state_dir = setup.options.root / "mica-server";
  std::filesystem::create_directories(state_dir);
  const auto key_path = state_dir / "api-key";
  if (!std::filesystem::exists(key_path)) {
    std::ofstream key_file(key_path, std::ios::trunc);
    key_file << generate_api_key() << '\n';
    key_file.close();
    chmod(key_path.c_str(), S_IRUSR | S_IWUSR);
    std::cout << "Generated local API key at " << key_path
              << " (the key itself is not printed).\n";
  }

  json downloads = json::object();
  const auto runtime_path = state_dir / "runtime.json";
  if (std::filesystem::exists(runtime_path)) {
    try {
      std::ifstream previous_file(runtime_path);
      const auto previous = json::parse(previous_file);
      downloads = previous.value("downloads", json::object());
    } catch (const std::exception&) {
      throw std::runtime_error("existing runtime.json is invalid; refusing to overwrite it");
    }
  }

  json installed_backends = json::array();
  for (const auto backend : setup.backends) installed_backends.push_back(to_string(backend));
  json selected_quantizations = json::array();
  for (const auto quantization : setup.options.quantizations) {
    selected_quantizations.push_back(to_string(quantization));
  }
  json configured_models = json::object();
  for (const auto& id : registry.profile(setup.options.profile).models) {
    json variants = json::object();
    for (const auto backend : setup.backends) {
      json quants = json::array();
      for (const auto quantization : setup.options.quantizations) {
        quants.push_back(to_string(quantization));
      }
      variants[to_string(backend)] = quants;
    }
    configured_models[id] = {{"enabled", true}, {"variants", variants}};
  }
  json state = {
      {"schema", 3},
      {"installed_backends", installed_backends},
      {"quantizations", selected_quantizations},
      {"default_quantization", to_string(setup.options.quantizations.front())},
      {"profile", setup.options.profile},
      {"configured_models", configured_models},
      {"download_policy", "first_server_start"},
      {"hf_repo", setup.options.hf_repo},
      {"max_ram_gib", setup.options.max_ram_gib},
      {"max_vram_gib", setup.options.max_vram_gib},
      {"vllm_device", to_string(setup.vllm_device)},
      {"api_key_file", key_path.string()},
      {"config_directory", setup.options.config_directory.string()},
      {"downloads", downloads},
      {"hardware", {{"os", setup.hardware.os},
                    {"arch", setup.hardware.arch},
                    {"ram_gib", setup.hardware.ram_gib},
                    {"nvidia", setup.hardware.nvidia_detected},
                    {"nvidia_vram_gib", setup.hardware.nvidia_vram_gib}}},
      {"resolved", {{"llama_cpp", resolved_git_revision(setup.options.root / "runtime/llama.cpp")},
                    {"audio_cpp", resolved_git_revision(setup.options.root / "runtime/audio.cpp")},
                    {"mlx_packages", resolved_packages(setup.options.root / "environment-mlx")},
                    {"vllm_packages", resolved_packages(setup.options.root / "environment-vllm")},
                    {"download_packages", resolved_packages(setup.options.root /
                                                             "environment-tools")}}},
  };
  std::ofstream file(runtime_path, std::ios::trunc);
  file << std::setw(2) << state << '\n';
}

}  // namespace

ResolvedSetup resolve_setup(const Registry& registry, SetupOptions options) {
  ResolvedSetup resolved;
  resolved.hardware = detect_hardware();
  if (options.backends.empty()) options.backends.push_back(resolved.hardware.recommended_backend());
  std::sort(options.backends.begin(), options.backends.end());
  options.backends.erase(std::unique(options.backends.begin(), options.backends.end()),
                         options.backends.end());
  for (const auto backend : options.backends) {
    if (backend == Backend::mlx && !resolved.hardware.supports_mlx()) {
      throw std::runtime_error("MLX is supported only on Apple Silicon macOS");
    }
    if (backend == Backend::gguf && !resolved.hardware.supports_gguf()) {
      throw std::runtime_error("GGUF runtime supports macOS, Linux, and Windows through WSL");
    }
    if (backend == Backend::vllm && !resolved.hardware.supports_vllm()) {
      throw std::runtime_error(
          "vLLM supports Linux/WSL and Apple Silicon macOS in this installer");
    }
  }
  resolved.vllm_device = options.vllm_device == VllmDevice::automatic
                              ? resolved.hardware.recommended_vllm_device()
                              : options.vllm_device;
  if (std::find(options.backends.begin(), options.backends.end(), Backend::vllm) !=
      options.backends.end()) {
    if (resolved.vllm_device == VllmDevice::metal && !resolved.hardware.supports_mlx()) {
      throw std::runtime_error("vLLM Metal requires Apple Silicon macOS");
    }
    if (resolved.vllm_device == VllmDevice::metal &&
        version_major(resolved.hardware.os_version) < 15) {
      throw std::runtime_error("vLLM Metal requires macOS 15 or newer");
    }
    if (resolved.vllm_device == VllmDevice::cpu && resolved.hardware.os == "macos" &&
        version_major(resolved.hardware.os_version) < 14) {
      throw std::runtime_error("native vLLM CPU requires macOS 14 or newer");
    }
    if (resolved.vllm_device == VllmDevice::cuda && !resolved.hardware.nvidia_detected) {
      throw std::runtime_error("vLLM CUDA requires a detected NVIDIA GPU");
    }
    if (resolved.vllm_device == VllmDevice::cuda && options.max_vram_gib == 0) {
      options.max_vram_gib = *std::max_element(resolved.hardware.nvidia_vram_gib.begin(),
                                               resolved.hardware.nvidia_vram_gib.end());
    }
  }
  if (options.max_ram_gib <= 0 ||
      (resolved.hardware.ram_gib > 0 && options.max_ram_gib > resolved.hardware.ram_gib)) {
    throw std::runtime_error("RAM budget must be positive and no larger than detected RAM");
  }
  if (options.max_vram_gib < 0) throw std::runtime_error("VRAM budget cannot be negative");
  if (options.quantizations.empty()) options.quantizations.push_back(Quantization::q4);
  std::sort(options.quantizations.begin(), options.quantizations.end());
  options.quantizations.erase(
      std::unique(options.quantizations.begin(), options.quantizations.end()),
      options.quantizations.end());
  if ((resolved.hardware.os == "linux" || resolved.hardware.os == "wsl") &&
      options.max_vram_gib > 0) {
    if (!resolved.hardware.nvidia_detected) {
      throw std::runtime_error("nonzero VRAM budget requires a detected NVIDIA GPU");
    }
    const double available = *std::max_element(resolved.hardware.nvidia_vram_gib.begin(),
                                               resolved.hardware.nvidia_vram_gib.end());
    if (options.max_vram_gib > available + 1e-9) {
      throw std::runtime_error("VRAM budget exceeds the largest detected NVIDIA device");
    }
  }
  if (options.root.empty()) options.root = home_directory() / "models";
  if (options.hf_repo.empty()) options.hf_repo = registry.default_hf_repo;
  resolved.backends = options.backends;
  resolved.options = std::move(options);
  const auto& profile = registry.profile(resolved.options.profile);
  for (const auto backend : resolved.backends) {
    for (const auto quantization : resolved.options.quantizations) {
      double admission_budget = resolved.options.max_ram_gib;
      if (backend == Backend::vllm && resolved.vllm_device == VllmDevice::cuda &&
          resolved.options.max_vram_gib > 0) {
        admission_budget = std::min(admission_budget, resolved.options.max_vram_gib);
      }
      resolved.startups[backend][quantization] =
          plan_startup(registry, profile, backend, quantization,
                       admission_budget);
    }
  }
  return resolved;
}

void execute_setup(const Registry& registry, const ResolvedSetup& setup) {
  for (const auto& [backend, quantizations] : setup.startups) {
    for (const auto& [quantization, startup] : quantizations) {
      if (startup.error) {
        throw std::runtime_error(to_string(backend) + "/" + to_string(quantization) +
                                 ": " + *startup.error);
      }
    }
  }
  if (setup.options.hf_repo.empty() || setup.options.hf_repo.find("YOUR_") != std::string::npos) {
    throw std::runtime_error("set --hf-repo to the destination repository before setup");
  }
  if (!setup.options.dry_run) {
    std::filesystem::create_directories(setup.options.root / "checkpoints");
    std::filesystem::create_directories(setup.options.root / "runtime");
  }
  install_download_environment(setup);
  for (const auto backend : setup.backends) {
    if (backend == Backend::mlx) install_mlx_environment(setup);
    else if (backend == Backend::gguf) install_native_gguf_runtimes(registry, setup);
    else install_vllm_environment(setup);
  }
  write_runtime_state(registry, setup);
}

std::string generate_api_key() {
  std::array<unsigned char, 32> bytes{};
#ifdef __APPLE__
  if (SecRandomCopyBytes(kSecRandomDefault, bytes.size(), bytes.data()) != errSecSuccess) {
    throw std::runtime_error("SecRandomCopyBytes failed");
  }
#else
  if (getrandom(bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size())) {
    throw std::runtime_error("getrandom failed");
  }
#endif
  std::ostringstream out;
  out << "mica_" << std::hex << std::setfill('0');
  for (const auto byte : bytes) out << std::setw(2) << static_cast<int>(byte);
  return out.str();
}

}  // namespace mica
