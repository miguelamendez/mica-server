#include "mica_server/setup.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

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

// Native runtime builds can contain unusually large translation units. Keep
// setup deliberately conservative: two concurrent compiler processes stay
// comfortably below the 16 GiB compilation budget on supported machines, and
// avoid turning a 24 GiB laptop into an unresponsive build host.
constexpr int kNativeBuildParallelism = 2;
constexpr double kNativeBuildMemoryCeilingGib = 16.0;

std::filesystem::path home_directory() {
  const char* home = std::getenv("HOME");
  if (!home || std::string(home).empty()) throw std::runtime_error("HOME is not set");
  return home;
}

std::filesystem::path default_application_home() {
  const char* configured = std::getenv("MICA_HOME");
  if (configured && std::string(configured).empty() == false) {
    return configured;
  }
  return home_directory() / ".mica";
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

void install_mlx_environment(const Registry& registry, const ResolvedSetup& setup) {
  const auto environment = setup.options.root / "environments/mlx";
  const bool install_packages = setup.options.refresh ||
                                !std::filesystem::exists(environment / "bin/python");
  if (install_packages) {
    std::vector<std::string> command = {"uv", "venv", environment.string(), "--python", "3.11"};
    if (setup.options.refresh) command.emplace_back("--clear");
    execute_or_print(command, setup.options.dry_run);
  }
  if (install_packages) {
    std::vector<std::string> command = {
        "uv", "pip", "install", "--python", (environment / "bin/python").string(),
        "--upgrade", "huggingface_hub[hf_xet]"};
    const auto& profile = registry.profile(setup.options.profile);
    bool needs_lm = profile.schema < 2;
    bool needs_vlm = profile.schema < 2;
    bool needs_audio = profile.schema < 2;
    for (const auto& policy : profile.model_policies) {
      needs_lm = needs_lm || policy.engine == "mlx-lm";
      needs_vlm = needs_vlm || policy.engine == "mlx-vlm";
      needs_audio = needs_audio || policy.engine == "mlx-audio";
    }
    if (needs_lm) command.emplace_back("mlx-lm");
    if (needs_vlm) command.emplace_back("mlx-vlm");
    if (needs_audio) command.emplace_back("mlx-audio[all,server]>=0.5.1");
    execute_or_print(command, setup.options.dry_run);
  }
}

void install_download_environment(const ResolvedSetup& setup) {
  const auto environment = setup.options.root / "environments/tools";
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
         "--upgrade", "huggingface_hub[hf_xet]", "numpy", "safetensors"},
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

HardwareInfo resolve_hardware_profile(const SetupOptions& options) {
  if (!options.hardware_profile.empty()) {
    return load_hardware_profile(options.hardware_profile);
  }
  return detect_hardware();
}

void install_native_gguf_runtimes(const Registry& registry, const ResolvedSetup& setup) {
  if (setup.hardware.ram_gib > 0 && setup.hardware.ram_gib < 8.0) {
    throw std::runtime_error(
        "native GGUF runtime compilation requires at least 8 GiB of system RAM");
  }
  const int build_parallelism =
      std::clamp(setup.hardware.build_parallelism, 1, kNativeBuildParallelism);
  const double build_memory_limit =
      std::min(setup.hardware.build_memory_limit_gib, kNativeBuildMemoryCeilingGib);
  std::cout << "Native runtime build guard: at most " << build_parallelism
            << " compiler jobs within the " << build_memory_limit
            << " GiB compilation budget.\n";
  const auto runtime = setup.options.root / "runtimes";
  const auto llama_source = runtime / "llama.cpp";
  const auto llama_build = llama_source / "build-mica";
  const auto audio_source = runtime / "audio.cpp";
  const auto audio_build = audio_source / "build-mica";
  const auto& profile = registry.profile(setup.options.profile);
  bool needs_llama = profile.schema < 2;
  bool needs_audio = profile.schema < 2;
  std::vector<std::string> audio_families;
  for (const auto& policy : profile.model_policies) {
    needs_llama = needs_llama || policy.engine == "llama-cpp";
    if (policy.engine == "audio-cpp") {
      needs_audio = true;
      const auto& family = registry.model(policy.id).gguf_family;
      if (!family.empty()) audio_families.push_back(family);
    }
  }
  if (profile.schema < 2) audio_families = {"granite5asr", "audio8_tts"};
  std::vector<std::string> unique_audio_families;
  for (const auto& family : audio_families) {
    if (std::find(unique_audio_families.begin(), unique_audio_families.end(), family) ==
        unique_audio_families.end()) {
      unique_audio_families.push_back(family);
    }
  }
  audio_families = std::move(unique_audio_families);
  std::ostringstream audio_model_list;
  for (std::size_t i = 0; i < audio_families.size(); ++i) {
    if (i != 0) audio_model_list << ',';
    audio_model_list << audio_families[i];
  }
  const bool llama_ready = !needs_llama ||
      (std::filesystem::exists(llama_build / "bin/llama-server") &&
       std::filesystem::exists(llama_build / "bin/llama-quantize"));
  const bool audio_ready = !needs_audio ||
      (std::filesystem::exists(audio_build / "bin/audiocpp_server") &&
       std::filesystem::exists(audio_build / "bin/audiocpp_gguf"));
  if (!setup.options.refresh && !setup.options.dry_run &&
      llama_ready && audio_ready) {
    std::cout << "Native GGUF runtimes already exist; use --refresh to rebuild.\n";
    return;
  }
  if (needs_llama) {
    update_source_tree(llama_source, "https://github.com/ggml-org/llama.cpp.git",
                       registry.llama_cpp_revision, setup.options.refresh,
                       setup.options.dry_run);
    std::vector<std::string> configure = {
        "cmake", "-S", llama_source.string(), "-B", llama_build.string(),
        "-DCMAKE_BUILD_TYPE=Release", "-DGGML_NATIVE=ON"};
    if (setup.hardware.gguf_target == "metal") configure.emplace_back("-DGGML_METAL=ON");
    else if (setup.hardware.gguf_target == "cuda") configure.emplace_back("-DGGML_CUDA=ON");
    else if (setup.hardware.gguf_target == "hip") configure.emplace_back("-DGGML_HIP=ON");
    else if (setup.hardware.gguf_target == "sycl") configure.emplace_back("-DGGML_SYCL=ON");
    else if (setup.hardware.gguf_target == "vulkan") {
      configure.emplace_back("-DGGML_VULKAN=ON");
    }
    execute_or_print(configure, setup.options.dry_run);
    execute_or_print({"cmake", "--build", llama_build.string(), "--config", "Release",
                      "--target", "llama-server", "llama-quantize", "--parallel",
                      std::to_string(build_parallelism)},
                     setup.options.dry_run);
  }

  if (needs_audio) {
    update_source_tree(audio_source, "https://github.com/0xShug0/audio.cpp.git",
                       registry.audio_cpp_revision, setup.options.refresh,
                       setup.options.dry_run);
    std::vector<std::string> audio_configure = {
        "cmake", "-S", audio_source.string(), "-B", audio_build.string(),
        "-DCMAKE_BUILD_TYPE=Release", "-DAUDIOCPP_MODEL_SET=custom",
        "-DAUDIOCPP_MODELS=" + audio_model_list.str()};
    if (setup.hardware.audio_target == "metal") {
      audio_configure.emplace_back("-DENGINE_ENABLE_METAL=ON");
      // Homebrew keeps libomp keg-only on Apple Silicon. Passing its stable opt
      // prefix lets CMake's FindOpenMP locate both the headers and runtime without
      // requiring callers to mutate global CPPFLAGS/LDFLAGS.
      const auto homebrew_libomp = std::filesystem::path("/opt/homebrew/opt/libomp");
      if (std::filesystem::exists(homebrew_libomp)) {
        audio_configure.emplace_back("-DOpenMP_ROOT=" + homebrew_libomp.string());
      }
    }
    if (setup.hardware.audio_target == "cuda") {
      audio_configure.emplace_back("-DENGINE_ENABLE_CUDA=ON");
    } else if (setup.hardware.audio_target == "hip") {
      audio_configure.emplace_back("-DENGINE_ENABLE_HIP=ON");
    } else if (setup.hardware.audio_target == "vulkan") {
      audio_configure.emplace_back("-DENGINE_ENABLE_VULKAN=ON");
    }
    execute_or_print(audio_configure, setup.options.dry_run);
    execute_or_print({"cmake", "--build", audio_build.string(), "--config", "Release",
                      "--target", "audiocpp_server", "audiocpp_gguf", "--parallel",
                      std::to_string(build_parallelism)},
                     setup.options.dry_run);
  }
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
  const auto environment = setup.options.root / "environments/vllm";
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
  if (setup.vllm_device == VllmDevice::rocm) {
    execute_or_print({"uv", "pip", "install", "--python",
                      (environment / "bin/python").string(), "--upgrade", "vllm",
                      "--extra-index-url", "https://wheels.vllm.ai/rocm/"},
                     setup.options.dry_run);
    mark_installed();
    return;
  }
  if (setup.vllm_device == VllmDevice::xpu) {
    execute_or_print({"uv", "pip", "install", "--python",
                      (environment / "bin/python").string(), "--upgrade", "vllm",
                      "--extra-index-url", "https://wheels.vllm.ai/xpu",
                      "--index-strategy", "unsafe-best-match"},
                     setup.options.dry_run);
    mark_installed();
    return;
  }
  if (setup.vllm_device == VllmDevice::tpu) {
    execute_or_print({"uv", "pip", "install", "--python",
                      (environment / "bin/python").string(), "--upgrade",
                      "tpu-inference"},
                     setup.options.dry_run);
    mark_installed();
    return;
  }
  if (setup.hardware.os == "macos") {
    const auto source = setup.options.root / "runtimes/vllm";
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

json validate_command(const std::string& name, const std::vector<std::string>& command,
                      bool dry_run) {
  std::cout << (dry_run ? "[plan] validate " : "[test] validate ") << name << ": "
            << display_command(command) << '\n';
  if (dry_run) {
    return {{"status", "planned"}, {"command", command}};
  }
  const auto result = run_command(command, true);
  if (result.exit_code != 0) {
    throw std::runtime_error("setup acceptance failed for " + name + ":\n" +
                             result.output);
  }
  return {{"status", "passed"},
          {"command", command},
          {"output", trim(result.output)}};
}

json validate_loopback_bind(bool dry_run) {
  std::cout << (dry_run ? "[plan] validate " : "[test] validate ")
            << "native loopback bind\n";
  if (dry_run) return {{"status", "planned"}, {"implementation", "native-cpp"}};
  const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) throw std::runtime_error("setup acceptance cannot create a socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  const bool bound = bind(descriptor, reinterpret_cast<sockaddr*>(&address),
                          sizeof(address)) == 0;
  close(descriptor);
  if (!bound) throw std::runtime_error("setup acceptance cannot bind to loopback");
  return {{"status", "passed"}, {"implementation", "native-cpp"}};
}

json validate_setup(const Registry& registry, const ResolvedSetup& setup) {
  json acceptance = {
      {"hardware",
       {{"os", setup.hardware.os},
        {"os_version", setup.hardware.os_version},
        {"arch", setup.hardware.arch},
        {"ram_gib", setup.hardware.ram_gib},
        {"nvidia", setup.hardware.nvidia_detected},
        {"nvidia_vram_gib", setup.hardware.nvidia_vram_gib}}},
      {"backends", json::object()},
  };
  acceptance["loopback"] = validate_loopback_bind(setup.options.dry_run);
  for (const auto backend : setup.backends) {
    if (backend == Backend::mlx) {
      const auto python = (setup.options.root / "environments/mlx/bin/python").string();
      acceptance["backends"]["mlx"] = validate_command(
          "MLX imports and device",
          {python, "-c",
           "import mlx.core as mx; import mlx_lm, mlx_vlm, mlx_audio; "
           "print(mx.default_device())"},
          setup.options.dry_run);
    } else if (backend == Backend::gguf) {
      json gguf;
      const auto& profile = registry.profile(setup.options.profile);
      bool needs_llama = profile.schema < 2;
      bool needs_audio = profile.schema < 2;
      for (const auto& policy : profile.model_policies) {
        needs_llama = needs_llama || policy.engine == "llama-cpp";
        needs_audio = needs_audio || policy.engine == "audio-cpp";
      }
      if (needs_llama) {
        gguf["llama_cpp"] = validate_command(
            "llama.cpp runtime",
            {(setup.options.root / "runtimes/llama.cpp/build-mica/bin/llama-server").string(),
             "--version"},
            setup.options.dry_run);
      }
      if (needs_audio) {
        gguf["audio_cpp"] = validate_command(
            "audio.cpp runtime",
            {(setup.options.root / "runtimes/audio.cpp/build-mica/bin/audiocpp_server").string(),
             "--help"},
            setup.options.dry_run);
      }
      acceptance["backends"]["gguf"] = std::move(gguf);
    } else {
      const auto python = (setup.options.root / "environments/vllm/bin/python").string();
      acceptance["backends"]["vllm"] = {
          {"device", to_string(setup.vllm_device)},
          {"runtime", validate_command(
                          "vLLM runtime",
                          {python, "-c",
                           "import platform, vllm; print(vllm.__version__); "
                           "print(platform.machine())"},
                          setup.options.dry_run)}};
    }
  }
  return acceptance;
}

void write_setup_acceptance(const ResolvedSetup& setup, const json& acceptance) {
  if (setup.options.dry_run) return;
  const auto path = setup.options.root / "state/setup-acceptance.json";
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    throw std::runtime_error("cannot write setup acceptance evidence: " + path.string());
  }
  file << std::setw(2) << acceptance << '\n';
  if (!file) {
    throw std::runtime_error("failed writing setup acceptance evidence: " + path.string());
  }
}

void write_runtime_state(const Registry& registry, const ResolvedSetup& setup) {
  if (setup.options.dry_run) return;
  const auto state_dir = setup.options.root / "state";
  std::filesystem::create_directories(state_dir);
  const auto secrets_dir = setup.options.root / "secrets";
  std::filesystem::create_directories(secrets_dir);
  const auto key_path = secrets_dir / "api-key";
  if (!setup.options.api_key_file.empty()) {
    std::ifstream source(setup.options.api_key_file);
    if (!source) {
      throw std::runtime_error("cannot read --api-key-file: " +
                               setup.options.api_key_file.string());
    }
    std::ostringstream contents;
    contents << source.rdbuf();
    const auto key = trim(contents.str());
    const bool invalid_character = std::any_of(
        key.begin(), key.end(), [](unsigned char value) { return std::iscntrl(value); });
    if (key.size() < 16 || key.size() > 512 || invalid_character) {
      throw std::runtime_error("API token must contain 16 to 512 printable characters");
    }
    std::ofstream key_file(key_path, std::ios::trunc);
    if (!key_file) throw std::runtime_error("cannot write configured API-token file");
    key_file << key << '\n';
    key_file.close();
    chmod(key_path.c_str(), S_IRUSR | S_IWUSR);
    std::cout << "Imported the local API token into " << key_path
              << " (the token itself is not printed).\n";
  } else if (!std::filesystem::exists(key_path)) {
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
  const auto& profile = registry.profile(setup.options.profile);
  if (profile.schema >= 2) {
    for (const auto& policy : profile.model_policies) {
      configured_models[policy.id] = {
          {"enabled", true},
          {"variants", {{to_string(policy.backend),
                          json::array({to_string(policy.quantization)})}}},
          {"engine", policy.engine},
          {"execution", policy.execution},
          {"residency", to_string(policy.residency)},
          {"priority", policy.priority},
          {"startup", policy.startup},
          {"idle_seconds", policy.idle_seconds},
          {"max_input_tokens", policy.max_input_tokens},
          {"max_output_tokens", policy.max_output_tokens},
          {"max_total_tokens", policy.max_total_tokens},
          {"max_concurrent_requests", policy.max_concurrent_requests},
          {"kv_cache_precision", policy.kv_cache_precision}};
    }
  } else {
    for (const auto& id : profile.models) {
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
  }
  json state = {
      {"schema", 5},
      {"installed_backends", installed_backends},
      {"quantizations", selected_quantizations},
      {"default_quantization", to_string(setup.options.quantizations.front())},
      {"profile", setup.options.profile},
      {"profile_schema", profile.schema},
      {"profile_mode", profile.mode},
      {"profile_maximum_ram_gib", profile.maximum_ram_gib},
      {"profile_maximum_vram_gib", profile.maximum_vram_gib},
      {"profile_memory_safety_reserve_gib", profile.memory_safety_reserve_gib},
      {"profile_maximum_resident_workers", profile.maximum_resident_workers},
      {"configured_models", configured_models},
      {"download_policy", "first_server_start"},
      {"hf_repo", setup.options.hf_repo},
      {"max_ram_gib", setup.options.max_ram_gib},
      {"max_vram_gib", setup.options.max_vram_gib},
      {"vllm_device", to_string(setup.vllm_device)},
      {"api_key_file", key_path.string()},
      {"config_directory", setup.options.config_directory.string()},
      {"setup_acceptance_file",
       (setup.options.root / "state/setup-acceptance.json").string()},
      {"downloads", downloads},
      {"hardware", hardware_to_json(setup.hardware)},
      {"hardware_profile_file",
       (setup.options.root / "state/hardware-profile.json").string()},
      {"resolved", {{"llama_cpp", resolved_git_revision(setup.options.root / "runtimes/llama.cpp")},
                    {"audio_cpp", resolved_git_revision(setup.options.root / "runtimes/audio.cpp")},
                    {"mlx_packages", resolved_packages(setup.options.root / "environments/mlx")},
                    {"vllm_packages", resolved_packages(setup.options.root / "environments/vllm")},
                    {"download_packages", resolved_packages(setup.options.root /
                                                             "environments/tools")}}},
  };
  std::ofstream file(runtime_path, std::ios::trunc);
  file << std::setw(2) << state << '\n';
}

}  // namespace

ResolvedSetup resolve_setup(const Registry& registry, SetupOptions options) {
  ResolvedSetup resolved;
  resolved.hardware = resolve_hardware_profile(options);
  if (options.profile == "auto") {
    options.profile = resolved.hardware.supports_mlx()
                          ? "mica-assistant-mlx"
                          : "mica-assistant-gguf";
  }
  const auto& profile = registry.profile(options.profile);
  if (profile.schema >= 2) {
    if (options.quantizations_explicit) {
      throw std::runtime_error(
          "schema-2 profiles select quantization per model; omit --quant");
    }
    options.quantizations.clear();
    for (const auto& policy : profile.model_policies) {
      options.quantizations.push_back(policy.quantization);
    }
    if (options.backends.empty()) {
      for (const auto& policy : profile.model_policies) {
        options.backends.push_back(policy.backend);
      }
    }
  } else if (options.backends.empty()) {
    options.backends.push_back(resolved.hardware.recommended_backend());
  }
  std::sort(options.backends.begin(), options.backends.end());
  options.backends.erase(std::unique(options.backends.begin(), options.backends.end()),
                         options.backends.end());
  if (profile.schema >= 2 && options.backends_explicit) {
    std::vector<Backend> required;
    for (const auto& policy : profile.model_policies) required.push_back(policy.backend);
    std::sort(required.begin(), required.end());
    required.erase(std::unique(required.begin(), required.end()), required.end());
    if (required != options.backends) {
      throw std::runtime_error(
          "schema-2 profile backends are fixed by its model execution policies");
    }
  }
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
    std::string required_runtime;
    if (resolved.vllm_device == VllmDevice::cuda) required_runtime = "cuda";
    else if (resolved.vllm_device == VllmDevice::rocm) required_runtime = "rocm";
    else if (resolved.vllm_device == VllmDevice::xpu) required_runtime = "xpu";
    else if (resolved.vllm_device == VllmDevice::tpu) required_runtime = "tpu";
    if (!required_runtime.empty() && !resolved.hardware.has_runtime(required_runtime)) {
      throw std::runtime_error("vLLM " + to_string(resolved.vllm_device) +
                               " requires a detected " + required_runtime + " device");
    }
    const double device_memory = resolved.hardware.largest_memory_gib(required_runtime);
    if (!required_runtime.empty() && required_runtime != "tpu" &&
        options.max_vram_gib == 0 && device_memory > 0) {
      options.max_vram_gib = device_memory;
    }
  }
  if (options.max_ram_gib <= 0 ||
      (resolved.hardware.ram_gib > 0 && options.max_ram_gib > resolved.hardware.ram_gib)) {
    throw std::runtime_error("RAM budget must be positive and no larger than detected RAM");
  }
  if (profile.maximum_ram_gib > 0) {
    options.max_ram_gib = std::min(options.max_ram_gib, profile.maximum_ram_gib);
  }
  if (profile.maximum_vram_gib > 0 && options.max_vram_gib > 0) {
    options.max_vram_gib = std::min(options.max_vram_gib, profile.maximum_vram_gib);
  }
  if (options.max_vram_gib < 0) throw std::runtime_error("VRAM budget cannot be negative");
  if (options.quantizations.empty()) options.quantizations.push_back(Quantization::q4);
  std::sort(options.quantizations.begin(), options.quantizations.end());
  options.quantizations.erase(
      std::unique(options.quantizations.begin(), options.quantizations.end()),
      options.quantizations.end());
  if ((resolved.hardware.os == "linux" || resolved.hardware.os == "wsl") &&
      options.max_vram_gib > 0) {
    const double available = resolved.hardware.largest_memory_gib("");
    if (available <= 0) {
      throw std::runtime_error(
          "nonzero VRAM budget requires an accelerator with detected dedicated memory");
    }
    if (options.max_vram_gib > available + 1e-9) {
      throw std::runtime_error("VRAM budget exceeds the largest detected accelerator");
    }
  }
  if (options.root.empty()) options.root = default_application_home();
  if (options.hf_repo.empty()) options.hf_repo = registry.default_hf_repo;
  resolved.backends = options.backends;
  resolved.options = std::move(options);
  if (profile.backend &&
      (resolved.backends.size() != 1 || resolved.backends.front() != *profile.backend)) {
    throw std::runtime_error("profile " + profile.name + " requires backend " +
                             to_string(*profile.backend));
  }
  for (const auto backend : resolved.backends) {
    for (const auto quantization : resolved.options.quantizations) {
      double admission_budget = resolved.options.max_ram_gib;
      if (backend == Backend::vllm &&
          resolved.vllm_device != VllmDevice::cpu &&
          resolved.vllm_device != VllmDevice::metal &&
          resolved.vllm_device != VllmDevice::tpu &&
          resolved.options.max_vram_gib > 0) {
        admission_budget = std::min(admission_budget, resolved.options.max_vram_gib);
      }
      admission_budget = std::max(
          0.0, admission_budget - profile.memory_safety_reserve_gib);
      resolved.startups[backend][quantization] =
          plan_profile_startup(registry, profile, backend, quantization,
                               admission_budget);
    }
  }
  return resolved;
}

void execute_setup(const Registry& registry, const ResolvedSetup& setup) {
  // Only the default precision is prewarmed. Additional configured
  // precisions are cached for on-demand use and may evict default workers, so
  // they must not make setup fail merely because their required pair cannot
  // coexist under the warm-residency budget.
  for (const auto& [backend, quantizations] : setup.startups) {
    for (const auto& [quantization, startup] : quantizations) {
      if (startup.error) {
        throw std::runtime_error(to_string(backend) + "/" +
                                 to_string(quantization) + ": " +
                                 *startup.error);
      }
    }
  }
  if (setup.options.hf_repo.empty() || setup.options.hf_repo.find("YOUR_") != std::string::npos) {
    throw std::runtime_error("set --hf-repo to the destination repository before setup");
  }
  if (!setup.options.dry_run) {
    std::filesystem::create_directories(setup.options.root / "models");
    std::filesystem::create_directories(setup.options.root / "runtimes");
    write_hardware_profile(setup.hardware,
                           setup.options.root / "state/hardware-profile.json");
  } else {
    std::cout << "[plan] write hardware profile "
              << setup.options.root / "state/hardware-profile.json" << '\n';
  }
  const bool needs_python_tools = std::any_of(
      setup.backends.begin(), setup.backends.end(),
      [](const auto backend) { return backend != Backend::gguf; });
  if (needs_python_tools) install_download_environment(setup);
  for (const auto backend : setup.backends) {
    if (backend == Backend::mlx) install_mlx_environment(registry, setup);
    else if (backend == Backend::gguf) install_native_gguf_runtimes(registry, setup);
    else install_vllm_environment(setup);
  }
  const auto acceptance = validate_setup(registry, setup);
  write_setup_acceptance(setup, acceptance);
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
