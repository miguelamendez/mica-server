#include <filesystem>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mica_server/config.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/server.hpp"
#include "mica_server/setup.hpp"

namespace {

using json = nlohmann::json;

void usage() {
  std::cout << R"(mica-server

Usage:
  mica-server detect [--config-dir PATH]
  mica-server plan  [setup options]
  mica-server setup [setup options]
  mica-server serve [--backend mlx|gguf|both] [--root PATH] [--host HOST] [--port PORT]

Setup options:
  --backends auto|mlx|gguf|mlx,gguf
                            Install one or both runtime stacks (default: auto)
  --profile all|core|quality
  --quant q4|q8|q4,q8       Cache one or both quantizations
  --ram-gib N               Hard model RAM admission budget (default 8)
  --vram-gib N              NVIDIA VRAM budget; zero forces Linux/WSL CPU
  --hf-repo OWNER/REPO      Catalog repository (model artifacts use per-model repos)
  --root PATH               Runtime/model root (default ~/models)
  --refresh                 Resolve current runtime/package versions again
  --dry-run                 Print actions without changing the machine

Serve options:
  --backend mlx|gguf|both   Select installed backend(s); required when both are installed

Configured models are downloaded on the first server launch for each active backend.
Downloaded backend/quantization variants are recorded in runtime.json.
)";
}

std::string value_after(const std::vector<std::string>& args, std::size_t& index) {
  if (index + 1 >= args.size()) throw std::invalid_argument("missing value after " + args[index]);
  return args[++index];
}

std::filesystem::path default_config() {
  if (std::filesystem::exists("config/models.lua")) return "config";
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "config";
}

mica::SetupOptions parse_setup(const std::vector<std::string>& args) {
  mica::SetupOptions options;
  options.config_directory = default_config();
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--backend" || args[i] == "--backends") {
      const auto value = value_after(args, i);
      if (value != "auto") {
        std::size_t start = 0;
        while (start <= value.size()) {
          const auto end = value.find(',', start);
          options.backends.push_back(
              mica::parse_backend(value.substr(start, end == std::string::npos
                                                          ? end : end - start)));
          if (end == std::string::npos) break;
          start = end + 1;
        }
      }
    } else if (args[i] == "--profile") options.profile = value_after(args, i);
    else if (args[i] == "--quant") {
      options.quantizations.clear();
      const auto value = value_after(args, i);
      std::size_t start = 0;
      while (start <= value.size()) {
        const auto end = value.find(',', start);
        options.quantizations.push_back(
            mica::parse_quantization(value.substr(start, end == std::string::npos
                                                             ? end : end - start)));
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
    else if (args[i] == "--ram-gib") options.max_ram_gib = std::stod(value_after(args, i));
    else if (args[i] == "--vram-gib") options.max_vram_gib = std::stod(value_after(args, i));
    else if (args[i] == "--hf-repo") options.hf_repo = value_after(args, i);
    else if (args[i] == "--root") options.root = value_after(args, i);
    else if (args[i] == "--config-dir") options.config_directory = value_after(args, i);
    else if (args[i] == "--refresh") options.refresh = true;
    else if (args[i] == "--dry-run") options.dry_run = true;
    else throw std::invalid_argument("unknown option: " + args[i]);
  }
  return options;
}

json hardware_json(const mica::HardwareInfo& hardware) {
  return {{"os", hardware.os}, {"arch", hardware.arch},
          {"apple_silicon", hardware.apple_silicon}, {"wsl", hardware.wsl},
          {"ram_gib", hardware.ram_gib}, {"nvidia_detected", hardware.nvidia_detected},
          {"nvidia_vram_gib", hardware.nvidia_vram_gib},
          {"supports_mlx", hardware.supports_mlx()},
          {"supports_gguf", hardware.supports_gguf()},
          {"recommended_backend", mica::to_string(hardware.recommended_backend())}};
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::vector<std::string> args(argv, argv + argc);
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h") {
      usage();
      return args.size() < 2 ? 1 : 0;
    }
    if (args[1] == "detect") {
      std::cout << std::setw(2) << hardware_json(mica::detect_hardware()) << '\n';
      return 0;
    }
    if (args[1] == "plan" || args[1] == "setup") {
      auto options = parse_setup(args);
      const auto registry = mica::load_registry(options.config_directory);
      auto resolved = mica::resolve_setup(registry, std::move(options));
      json backends = json::array();
      json startups = json::object();
      bool has_error = false;
      for (const auto backend : resolved.backends) {
        backends.push_back(mica::to_string(backend));
        for (const auto& [quantization, startup] : resolved.startups.at(backend)) {
          has_error = has_error || startup.error.has_value();
          startups[mica::to_string(backend)][mica::to_string(quantization)] = {
              {"admitted", startup.admitted}, {"skipped", startup.skipped},
              {"reserved_gib", startup.reserved_gib},
              {"error", startup.error.value_or("")}};
        }
      }
      json quantizations = json::array();
      for (const auto quantization : resolved.options.quantizations) {
        quantizations.push_back(mica::to_string(quantization));
      }
      json output = {{"hardware", hardware_json(resolved.hardware)},
                     {"installed_backends", backends},
                     {"quantizations", quantizations},
                     {"profile", resolved.options.profile},
                     {"ram_budget_gib", resolved.options.max_ram_gib},
                     {"vram_budget_gib", resolved.options.max_vram_gib},
                     {"startup_by_backend", startups},
                     {"model_download", "first_server_start"}};
      std::cout << std::setw(2) << output << '\n';
      if (args[1] == "plan") return has_error ? 2 : 0;
      mica::execute_setup(registry, resolved);
      return 0;
    }
    if (args[1] == "serve") {
      mica::ServerOptions options;
      options.config_directory = default_config();
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--root") options.root = value_after(args, i);
        else if (args[i] == "--config-dir") options.config_directory = value_after(args, i);
        else if (args[i] == "--host") options.host = value_after(args, i);
        else if (args[i] == "--port") options.port = std::stoi(value_after(args, i));
        else if (args[i] == "--backend") {
          const auto selected = value_after(args, i);
          if (selected == "both") {
            options.active_backends = {mica::Backend::mlx, mica::Backend::gguf};
          } else {
            options.active_backends = {mica::parse_backend(selected)};
          }
        }
        else throw std::invalid_argument("unknown option: " + args[i]);
      }
      if (options.root.empty()) {
        const char* home = std::getenv("HOME");
        if (!home) throw std::runtime_error("HOME is not set");
        options.root = std::filesystem::path(home) / "models";
      }
      const auto registry = mica::load_registry(options.config_directory);
      return mica::run_server(registry, options);
    }
    usage();
    throw std::invalid_argument("unknown command: " + args[1]);
  } catch (const std::exception& error) {
    std::cerr << "mica-server: " << error.what() << '\n';
    return 1;
  }
}
