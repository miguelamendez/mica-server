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
#include "mica_server/catalog.hpp"
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
  mica-server add-model --url URL --modality MODALITY [--id ID] [quantize options]
  mica-server quantize --model ID --backend mlx|gguf --quant q4|q8 [options]
  mica-server serve --backend mlx|gguf|vllm [--root PATH] [--host HOST] [--port PORT]

Setup options:
  --backends auto|mlx|gguf|vllm|LIST
                            Install one or more runtime stacks (default: auto)
  --profile all|core|quality
  --quant q4|q8|q4,q8       Cache one or both quantizations
  --ram-gib N               Hard model RAM admission budget (default 8)
  --vram-gib N              NVIDIA VRAM budget (vLLM CUDA auto-fills when zero)
  --vllm-device VALUE       auto|cpu|cuda|metal (default: hardware-selected)
  --hf-repo OWNER/REPO      Catalog repository (model artifacts use per-model repos)
  --root PATH               Runtime/model root (default ~/models)
  --refresh                 Resolve current runtime/package versions again
  --dry-run                 Print actions without changing the machine

Serve options:
  --backend mlx|gguf|vllm   Serve exactly one installed backend

Custom model options:
  --url URL                 Hugging Face model URL or OWNER/REPO
  --modality VALUE          tts|asr|text-to-text|img-text-to-text
  --id ID                   Optional stable local id (defaults from repo name)
  --description TEXT        Optional catalog description
  --backend mlx|gguf        Quantization backend (must be installed)
  --quant q4|q8             Quantization size
  --group-size N            MLX affine group size (default 64)
  --backend vllm            Register the HF repository for lazy vLLM loading
  --model-memory-gib N      Required vLLM scheduler reservation for that model

Configured models are downloaded on the first server launch for the active backend.
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

std::filesystem::path default_root() {
  const char* home = std::getenv("HOME");
  if (!home) throw std::runtime_error("HOME is not set");
  return std::filesystem::path(home) / "models";
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
    else if (args[i] == "--vllm-device") {
      options.vllm_device = mica::parse_vllm_device(value_after(args, i));
    }
    else if (args[i] == "--hf-repo") options.hf_repo = value_after(args, i);
    else if (args[i] == "--root") options.root = value_after(args, i);
    else if (args[i] == "--config-dir") options.config_directory = value_after(args, i);
    else if (args[i] == "--refresh") options.refresh = true;
    else if (args[i] == "--dry-run") options.dry_run = true;
    else throw std::invalid_argument("unknown option: " + args[i]);
  }
  if (options.root.empty()) options.root = default_root();
  return options;
}

json hardware_json(const mica::HardwareInfo& hardware) {
  return {{"os", hardware.os}, {"os_version", hardware.os_version},
          {"arch", hardware.arch},
          {"apple_silicon", hardware.apple_silicon}, {"wsl", hardware.wsl},
          {"ram_gib", hardware.ram_gib}, {"nvidia_detected", hardware.nvidia_detected},
          {"nvidia_vram_gib", hardware.nvidia_vram_gib},
          {"supports_mlx", hardware.supports_mlx()},
          {"supports_gguf", hardware.supports_gguf()},
          {"supports_vllm", hardware.supports_vllm()},
          {"recommended_vllm_device", mica::to_string(hardware.recommended_vllm_device())},
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
      auto registry = mica::load_registry(options.config_directory);
      mica::merge_custom_models(registry, options.root);
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
                     {"vllm_device", mica::to_string(resolved.vllm_device)},
                     {"startup_by_backend", startups},
                     {"model_download", "first_server_start"}};
      std::cout << std::setw(2) << output << '\n';
      if (args[1] == "plan") return has_error ? 2 : 0;
      mica::execute_setup(registry, resolved);
      return 0;
    }
    if (args[1] == "add-model") {
      mica::AddModelOptions options;
      options.root = default_root();
      std::optional<mica::Backend> quant_backend;
      std::optional<mica::Quantization> quantization;
      int group_size = 64;
      double model_memory_gib = 0.0;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--url") options.url = value_after(args, i);
        else if (args[i] == "--modality") options.modality = value_after(args, i);
        else if (args[i] == "--id") options.id = value_after(args, i);
        else if (args[i] == "--description") options.description = value_after(args, i);
        else if (args[i] == "--root") options.root = value_after(args, i);
        else if (args[i] == "--backend") quant_backend = mica::parse_backend(value_after(args, i));
        else if (args[i] == "--quant") quantization = mica::parse_quantization(value_after(args, i));
        else if (args[i] == "--group-size") group_size = std::stoi(value_after(args, i));
        else if (args[i] == "--model-memory-gib") {
          model_memory_gib = std::stod(value_after(args, i));
        }
        else if (args[i] == "--dry-run") options.dry_run = true;
        else throw std::invalid_argument("unknown option: " + args[i]);
      }
      if (options.url.empty() || options.modality.empty()) {
        throw std::invalid_argument("add-model requires --url and --modality");
      }
      if (quant_backend == mica::Backend::vllm) {
        if (quantization) {
          throw std::invalid_argument(
              "add-model --backend vllm uses the repository's native format; omit --quant");
        }
        if (model_memory_gib <= 0) {
          throw std::invalid_argument(
              "add-model --backend vllm requires --model-memory-gib");
        }
      } else if (quant_backend.has_value() != quantization.has_value()) {
        throw std::invalid_argument("add-model quantization requires both --backend and --quant");
      }
      const auto added_id = mica::add_custom_model(options);
      if (quant_backend == mica::Backend::vllm && !options.dry_run) {
        mica::configure_vllm_custom_model(
            {options.root, added_id, model_memory_gib, false});
      } else if (quant_backend && !options.dry_run) {
        mica::quantize_custom_model({options.root, added_id,
                                     *quant_backend, *quantization, group_size, false});
      }
      return 0;
    }
    if (args[1] == "quantize") {
      mica::QuantizeModelOptions options;
      options.root = default_root();
      bool has_backend = false;
      bool has_quantization = false;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--model") options.id = value_after(args, i);
        else if (args[i] == "--backend") {
          options.backend = mica::parse_backend(value_after(args, i));
          has_backend = true;
        } else if (args[i] == "--quant") {
          options.quantization = mica::parse_quantization(value_after(args, i));
          has_quantization = true;
        } else if (args[i] == "--group-size") {
          options.group_size = std::stoi(value_after(args, i));
        } else if (args[i] == "--root") options.root = value_after(args, i);
        else if (args[i] == "--dry-run") options.dry_run = true;
        else throw std::invalid_argument("unknown option: " + args[i]);
      }
      if (options.id.empty() || !has_backend || !has_quantization) {
        throw std::invalid_argument("quantize requires --model, --backend, and --quant");
      }
      mica::quantize_custom_model(options);
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
          options.active_backend = mica::parse_backend(selected);
        }
        else throw std::invalid_argument("unknown option: " + args[i]);
      }
      if (options.root.empty()) options.root = default_root();
      auto registry = mica::load_registry(options.config_directory);
      mica::merge_custom_models(registry, options.root);
      return mica::run_server(registry, options);
    }
    usage();
    throw std::invalid_argument("unknown command: " + args[1]);
  } catch (const std::exception& error) {
    std::cerr << "mica-server: " << error.what() << '\n';
    return 1;
  }
}
