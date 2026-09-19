#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mica_server/config.hpp"
#include "mica_server/catalog.hpp"
#include "mica_server/command.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/server.hpp"
#include "mica_server/setup.hpp"

namespace {

using json = nlohmann::json;

std::filesystem::path executable_directory;

void usage() {
  std::cout << R"(mica-server

Usage:
  mica-server detect [--output PATH]
  mica-server registry list|ping [--modality VALUE] [--engine VALUE] [--backend VALUE]
  mica-server profile list [--remote] [--root PATH] [--catalog-url URL]
  mica-server profile show ID [--root PATH]
  mica-server profile validate FILE [--root PATH]
  mica-server profile install ID [--root PATH] [--catalog-url URL]
  mica-server profile install-file FILE [--root PATH]
  mica-server profile create ID --from PROFILE [--output PATH]
  mica-server profile edit ID [--root PATH] [--editor EXECUTABLE]
  mica-server plan  [setup options]
  mica-server setup [setup options]
  mica-server add-model --url URL --modality MODALITY [--id ID] [quantize options]
  mica-server quantize --model ID --backend mlx|gguf|vllm --quant q4|q8 [options]
  mica-server serve [--backend mlx|gguf|vllm] [--root PATH] [--host HOST] [--port PORT]

Setup options:
  --backends auto|mlx|gguf|vllm|LIST
                            Install one or more runtime stacks (default: auto)
  --profile NAME            Task profile (default: hardware-selected auto)
  --profile-file PATH       Validate, install, and select a schema-2 JSON profile
  --quant q4|q8|q4,q8       Legacy profiles only; schema-2 profiles pin variants
  --ram-gib N               Hard model RAM admission budget (default 8)
  --vram-gib N              NVIDIA VRAM budget (vLLM CUDA auto-fills when zero)
  --vllm-device VALUE       auto|cpu|cuda|metal|rocm|xpu|tpu
  --hardware-profile PATH   Use a saved detector profile (JSON schema 1)
  --api-key-file PATH       Import an API token from a file (never passed inline)
  --hf-repo OWNER/REPO      Catalog repository (model artifacts use per-model repos)
  --root PATH               Application home (default $MICA_HOME or ~/.mica)
  --refresh                 Resolve current runtime/package versions again
  --dry-run                 Print actions without changing the machine

Serve options:
  --backend mlx|gguf|vllm   Legacy-profile backend selection; schema 2 pins engines
  --server-config PATH      Server JSON (default ~/.mica/config/server.json)
  --api-key TOKEN           Override API token (prefer a file; arguments are visible)
  --api-key-file PATH       Override the configured API-token file

Custom model options:
  --url URL                 Hugging Face model URL or OWNER/REPO
  --modality VALUE          tts|asr|text-to-text|image-text-to-text|video-text-to-text
  --id ID                   Optional stable local id (defaults from repo name)
  --description TEXT        Optional catalog description
  --backend mlx|gguf|vllm   Quantization backend (must be installed)
  --quant q4|q8             Quantization size
  --group-size N            MLX affine group size (default 64)
  --algorithm VALUE         vLLM auto_round or gptq (quality default by size)
  --scheme VALUE            vLLM W4A16 or W8A16 (derived from --quant)
  --quant-device VALUE      vLLM conversion device: auto|cpu|cuda|xpu
  --calibration-dataset ID  Override the vLLM calibration dataset
  --calibration-split NAME  Override its split (default train_sft)
  --calibration-samples N   Override the quality profile sample count
  --calibration-seqlen N    Override the quality profile token length
  --calibration-batch N     Override the calibration batch size
  --autoround-iters N       Override AutoRound optimization iterations
  --revision REVISION       Pin the original Hugging Face source revision
  --config-dir PATH         Lua model registry (required for built-in models)
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
  if (const char* configured = std::getenv("MICA_CONFIG_DIR");
      configured && *configured) {
    return configured;
  }
  const std::vector<std::filesystem::path> candidates = {
      std::filesystem::current_path() / "config",
      executable_directory / "config",
      executable_directory.parent_path() / "config",
      executable_directory.parent_path() / "share/mica-server/config"};
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate / "models.lua") &&
        std::filesystem::exists(candidate / "profiles.lua")) {
      return candidate.lexically_normal();
    }
  }
  throw std::runtime_error(
      "cannot locate Mica configuration; set MICA_CONFIG_DIR or use --config-dir");
}

std::filesystem::path locate_executable(const std::string& command) {
  std::error_code error;
  const auto resolved = [&](const std::filesystem::path& candidate) {
    error.clear();
    const auto canonical = std::filesystem::weakly_canonical(candidate, error);
    return error ? std::filesystem::absolute(candidate) : canonical;
  };
  const std::filesystem::path input(command);
  if (input.has_parent_path()) return resolved(input);
  if (const char* raw_path = std::getenv("PATH")) {
    std::istringstream paths(raw_path);
    std::string directory;
    while (std::getline(paths, directory, ':')) {
      const auto candidate = std::filesystem::path(directory) / input;
      if (std::filesystem::exists(candidate)) return resolved(candidate);
    }
  }
  return resolved(input);
}

std::filesystem::path default_root() {
  const char* configured = std::getenv("MICA_HOME");
  if (configured && std::string(configured).empty() == false) {
    return std::filesystem::path(configured);
  }
  const char* home = std::getenv("HOME");
  if (!home) throw std::runtime_error("HOME is not set");
  return std::filesystem::path(home) / ".mica";
}

std::filesystem::path expand_user_path(const std::filesystem::path& input) {
  const auto text = input.string();
  if (text == "~" || text.starts_with("~/")) {
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME is not set");
    return text == "~" ? std::filesystem::path(home)
                       : std::filesystem::path(home) / text.substr(2);
  }
  return input;
}

void apply_server_config(mica::ServerOptions& options,
                         const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return;
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("cannot read server config: " + path.string());
  json document;
  stream >> document;
  if (!document.is_object()) {
    throw std::runtime_error("server config must contain a JSON object: " + path.string());
  }
  if (document.contains("host")) options.host = document.at("host").get<std::string>();
  if (document.contains("port")) options.port = document.at("port").get<int>();
  if (document.contains("api_key")) {
    options.api_key = document.at("api_key").get<std::string>();
  }
  if (document.contains("api_key_file")) {
    options.api_key_file = expand_user_path(
        document.at("api_key_file").get<std::string>());
  }
}

mica::SetupOptions parse_setup(const std::vector<std::string>& args) {
  mica::SetupOptions options;
  options.config_directory = default_config();
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--backend" || args[i] == "--backends") {
      options.backends_explicit = true;
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
    else if (args[i] == "--profile-file") options.profile_file = value_after(args, i);
    else if (args[i] == "--quant") {
      options.quantizations_explicit = true;
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
    else if (args[i] == "--hardware-profile") {
      options.hardware_profile = value_after(args, i);
    }
    else if (args[i] == "--api-key-file") options.api_key_file = value_after(args, i);
    else if (args[i] == "--refresh") options.refresh = true;
    else if (args[i] == "--dry-run") options.dry_run = true;
    else throw std::invalid_argument("unknown option: " + args[i]);
  }
  if (options.root.empty()) options.root = default_root();
  return options;
}

std::filesystem::path installed_profile_path(const std::filesystem::path& root,
                                             const std::string& id) {
  return root / "config/profiles" / (id + ".json");
}

json local_profile_list(const mica::Registry& registry,
                        const std::filesystem::path& root) {
  json profiles = json::array();
  for (const auto& [id, profile] : registry.profiles) {
    if (!profile.catalog_visible) continue;
    const bool installed = std::filesystem::exists(installed_profile_path(root, id));
    json engines = json::array();
    json artifact_families = json::array();
    std::vector<std::string> unique_engines;
    std::vector<std::string> unique_families;
    for (const auto& policy : profile.model_policies) {
      if (std::find(unique_engines.begin(), unique_engines.end(), policy.engine) ==
          unique_engines.end()) {
        unique_engines.push_back(policy.engine);
        engines.push_back(policy.engine);
      }
      const auto family = mica::to_string(policy.backend);
      if (std::find(unique_families.begin(), unique_families.end(), family) ==
          unique_families.end()) {
        unique_families.push_back(family);
        artifact_families.push_back(family);
      }
    }
    profiles.push_back({{"id", id},
                        {"schema", profile.schema},
                        {"mode", profile.mode},
                        {"source", installed ? "installed" : "built-in"},
                        {"engines", engines},
                        {"artifact_families", artifact_families},
                        {"maximum_ram_gib", profile.maximum_ram_gib},
                        {"models", profile.models}});
  }
  return {{"profiles", profiles}};
}

std::string editor_for(const std::string& requested) {
  if (!requested.empty()) return requested;
  if (const char* visual = std::getenv("VISUAL"); visual && *visual) return visual;
  if (const char* editor = std::getenv("EDITOR"); editor && *editor) return editor;
  return "vi";
}

json hardware_json(const mica::HardwareInfo& hardware) {
  auto result = mica::hardware_to_json(hardware);
  result["capabilities"] = {{"supports_mlx", hardware.supports_mlx()},
                            {"supports_gguf", hardware.supports_gguf()},
                            {"supports_vllm", hardware.supports_vllm()},
                            {"recommended_vllm_device",
                             mica::to_string(hardware.recommended_vllm_device())},
                            {"recommended_backend",
                             mica::to_string(hardware.recommended_backend())}};
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    executable_directory = locate_executable(argv[0]).parent_path();
    std::vector<std::string> args(argv, argv + argc);
    if (args.size() == 2 && args[1] == "--version") {
      std::cout << "mica-server " << MICA_SERVER_VERSION << '\n';
      return 0;
    }
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h") {
      usage();
      return args.size() < 2 ? 1 : 0;
    }
    if (args[1] == "detect") {
      std::filesystem::path output_path;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--output") output_path = value_after(args, i);
        else throw std::invalid_argument("unknown detect option: " + args[i]);
      }
      const auto hardware = mica::detect_hardware();
      if (!output_path.empty()) mica::write_hardware_profile(hardware, output_path);
      std::cout << std::setw(2) << hardware_json(hardware) << '\n';
      return 0;
    }
    if (args[1] == "profile") {
      if (args.size() < 3) {
        throw std::invalid_argument(
            "profile requires list, show, validate, install, install-file, create, or edit");
      }
      const auto action = args[2];
      std::filesystem::path root = default_root();
      std::filesystem::path config_directory = default_config();
      std::filesystem::path output;
      std::string id;
      std::string from;
      std::string editor;
      std::string catalog_url = mica::kDefaultProfileCatalogUrl;
      bool remote = false;
      std::size_t start = 3;
      if (action == "show" || action == "install" || action == "edit" ||
          action == "create") {
        if (args.size() < 4 || args[3].starts_with("--")) {
          throw std::invalid_argument("profile " + action + " requires an id");
        }
        id = args[3];
        start = 4;
      } else if (action == "validate" || action == "install-file") {
        if (args.size() < 4 || args[3].starts_with("--")) {
          throw std::invalid_argument("profile " + action + " requires a file");
        }
        output = args[3];
        start = 4;
      } else if (action != "list") {
        throw std::invalid_argument("unknown profile action: " + action);
      }
      for (std::size_t i = start; i < args.size(); ++i) {
        if (args[i] == "--root") root = value_after(args, i);
        else if (args[i] == "--config-dir") config_directory = value_after(args, i);
        else if (args[i] == "--catalog-url") catalog_url = value_after(args, i);
        else if (args[i] == "--remote") remote = true;
        else if (args[i] == "--from") from = value_after(args, i);
        else if (args[i] == "--output") output = value_after(args, i);
        else if (args[i] == "--editor") editor = value_after(args, i);
        else throw std::invalid_argument("unknown profile option: " + args[i]);
      }
      if (action == "list" && remote) {
        std::cout << std::setw(2) << mica::fetch_profile_catalog(catalog_url) << '\n';
        return 0;
      }
      auto registry = mica::load_registry(config_directory);
      mica::merge_custom_models(registry, root);
      mica::merge_installed_profiles(registry, root);
      if (action == "list") {
        std::cout << std::setw(2) << local_profile_list(registry, root) << '\n';
        return 0;
      }
      if (action == "show") {
        const auto& profile = registry.profile(id);
        const auto path = installed_profile_path(root, id);
        std::cout << std::setw(2)
                  << (std::filesystem::exists(path)
                          ? mica::read_profile_file(path)
                          : mica::profile_to_json(profile))
                  << '\n';
        return 0;
      }
      if (action == "validate") {
        auto validation_registry = registry;
        const auto name = mica::merge_profile_file(validation_registry, output);
        const auto& profile = validation_registry.profile(name);
        std::cout << std::setw(2)
                  << json({{"valid", true}, {"id", name},
                           {"models", profile.models}})
                  << '\n';
        return 0;
      }
      if (action == "install") {
        const auto path = mica::install_profile_from_catalog(
            registry, root, id, catalog_url);
        std::cout << std::setw(2)
                  << json({{"installed", id}, {"path", path.string()}}) << '\n';
        return 0;
      }
      if (action == "install-file") {
        const auto name = mica::merge_profile_file(registry, output);
        const auto path = mica::install_profile_file(registry, root, output);
        std::cout << std::setw(2)
                  << json({{"installed", name}, {"path", path.string()}}) << '\n';
        return 0;
      }
      if (action == "create") {
        if (from.empty()) {
          throw std::invalid_argument("profile create requires --from PROFILE");
        }
        const auto& source = registry.profile(from);
        const auto source_path = installed_profile_path(root, from);
        auto document = std::filesystem::exists(source_path)
                            ? mica::read_profile_file(source_path)
                            : mica::profile_to_json(source);
        document["id"] = id;
        auto validation_registry = registry;
        mica::profile_from_json(validation_registry, document);
        if (output.empty()) output = id + ".json";
        if (std::filesystem::exists(output)) {
          throw std::runtime_error("refusing to overwrite existing profile: " +
                                   output.string());
        }
        mica::write_profile_file(output, document);
        std::cout << std::setw(2)
                  << json({{"created", id}, {"path", output.string()}}) << '\n';
        return 0;
      }
      if (action == "edit") {
        const auto& profile = registry.profile(id);
        const auto installed = installed_profile_path(root, id);
        const auto temporary = installed.parent_path() / ("." + id + ".edit.draft");
        const auto document = std::filesystem::exists(installed)
                                  ? mica::read_profile_file(installed)
                                  : mica::profile_to_json(profile);
        mica::write_profile_file(temporary, document);
        const auto selected_editor = editor_for(editor);
        if (selected_editor.find_first_of(" \t\r\n") != std::string::npos) {
          throw std::invalid_argument(
              "editor must be one executable path without arguments; use --editor");
        }
        const auto result = mica::run_command({selected_editor, temporary.string()});
        if (result.exit_code != 0) {
          throw std::runtime_error("editor exited unsuccessfully; draft retained at " +
                                   temporary.string());
        }
        auto validation_registry = registry;
        const auto edited_id = mica::merge_profile_file(validation_registry, temporary);
        if (edited_id != id) {
          throw std::runtime_error("edited profile id must remain " + id +
                                   "; draft retained at " + temporary.string());
        }
        mica::install_profile_file(registry, root, temporary);
        std::filesystem::remove(temporary);
        std::cout << std::setw(2)
                  << json({{"updated", id}, {"path", installed.string()}}) << '\n';
        return 0;
      }
    }
    if (args[1] == "registry") {
      if (args.size() < 3 || (args[2] != "list" && args[2] != "ping")) {
        throw std::invalid_argument("registry requires list or ping");
      }
      std::filesystem::path config_directory = default_config();
      std::filesystem::path root = default_root();
      std::optional<std::string> capability;
      std::optional<mica::Backend> backend;
      std::optional<std::string> engine;
      for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--modality") {
          capability = mica::modality_capability(
              mica::normalize_modality(value_after(args, i)));
        } else if (args[i] == "--capability") {
          capability = value_after(args, i);
        } else if (args[i] == "--backend") {
          backend = mica::parse_backend(value_after(args, i));
        } else if (args[i] == "--engine") {
          engine = value_after(args, i);
        } else if (args[i] == "--config-dir") {
          config_directory = value_after(args, i);
        } else if (args[i] == "--root") {
          root = value_after(args, i);
        } else {
          throw std::invalid_argument("unknown registry option: " + args[i]);
        }
      }
      auto registry = mica::load_registry(config_directory);
      mica::merge_custom_models(registry, root);
      mica::merge_installed_profiles(registry, root);
      std::cout << std::setw(2)
                << mica::registry_catalog(registry, capability, backend,
                                          args[2] == "ping", engine)
                << '\n';
      return 0;
    }
    if (args[1] == "plan" || args[1] == "setup") {
      auto options = parse_setup(args);
      auto registry = mica::load_registry(options.config_directory);
      mica::merge_custom_models(registry, options.root);
      mica::merge_installed_profiles(registry, options.root);
      if (!options.profile_file.empty()) {
        options.profile = mica::merge_profile_file(registry, options.profile_file);
        if (args[1] == "setup") {
          mica::install_profile_file(registry, options.root, options.profile_file);
        }
      }
      auto resolved = mica::resolve_setup(registry, std::move(options));
      json backends = json::array();
      json startups = json::object();
      bool has_error = false;
      const auto default_quantization = resolved.options.quantizations.front();
      const auto schema2 = registry.profile(resolved.options.profile).schema >= 2;
      for (const auto backend : resolved.backends) {
        backends.push_back(mica::to_string(backend));
        for (const auto& [quantization, startup] : resolved.startups.at(backend)) {
          // Alternate precisions are on-demand cache choices, not simultaneous
          // startup requirements. Only the default precision gates setup.
          if (schema2 || quantization == default_quantization) {
            has_error = has_error || startup.error.has_value();
          }
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
        mica::QuantizeModelOptions quantize;
        quantize.root = options.root;
        quantize.id = added_id;
        quantize.backend = *quant_backend;
        quantize.quantization = *quantization;
        quantize.group_size = group_size;
        mica::quantize_model(quantize);
      }
      return 0;
    }
    if (args[1] == "quantize") {
      mica::QuantizeModelOptions options;
      options.root = default_root();
      options.config_directory = default_config();
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
        } else if (args[i] == "--algorithm") {
          options.vllm_algorithm = value_after(args, i);
        } else if (args[i] == "--scheme") {
          options.vllm_scheme = value_after(args, i);
        } else if (args[i] == "--quant-device") {
          options.quantization_device = value_after(args, i);
        } else if (args[i] == "--calibration-dataset") {
          options.calibration_dataset = value_after(args, i);
        } else if (args[i] == "--calibration-split") {
          options.calibration_dataset_split = value_after(args, i);
        } else if (args[i] == "--calibration-samples") {
          options.calibration_samples = std::stoi(value_after(args, i));
        } else if (args[i] == "--calibration-seqlen") {
          options.calibration_sequence_length = std::stoi(value_after(args, i));
        } else if (args[i] == "--calibration-batch") {
          options.calibration_batch_size = std::stoi(value_after(args, i));
        } else if (args[i] == "--autoround-iters") {
          options.auto_round_iterations = std::stoi(value_after(args, i));
        } else if (args[i] == "--root") options.root = value_after(args, i);
        else if (args[i] == "--config-dir") options.config_directory = value_after(args, i);
        else if (args[i] == "--revision") options.source_revision = value_after(args, i);
        else if (args[i] == "--dry-run") options.dry_run = true;
        else throw std::invalid_argument("unknown option: " + args[i]);
      }
      if (options.id.empty() || !has_backend || !has_quantization) {
        throw std::invalid_argument("quantize requires --model, --backend, and --quant");
      }
      mica::quantize_model(options);
      return 0;
    }
    if (args[1] == "serve") {
      mica::ServerOptions options;
      options.config_directory = default_config();
      options.root = default_root();
      std::filesystem::path server_config;
      for (std::size_t i = 2; i + 1 < args.size(); ++i) {
        if (args[i] == "--root") options.root = value_after(args, i);
        else if (args[i] == "--server-config") server_config = value_after(args, i);
      }
      if (server_config.empty()) server_config = options.root / "config/server.json";
      apply_server_config(options, expand_user_path(server_config));
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--root") options.root = value_after(args, i);
        else if (args[i] == "--server-config") value_after(args, i);
        else if (args[i] == "--config-dir") options.config_directory = value_after(args, i);
        else if (args[i] == "--host") options.host = value_after(args, i);
        else if (args[i] == "--port") options.port = std::stoi(value_after(args, i));
        else if (args[i] == "--api-key") options.api_key = value_after(args, i);
        else if (args[i] == "--api-key-file") {
          options.api_key.clear();
          options.api_key_file = value_after(args, i);
        }
        else if (args[i] == "--backend") {
          const auto selected = value_after(args, i);
          options.active_backend = mica::parse_backend(selected);
        }
        else throw std::invalid_argument("unknown option: " + args[i]);
      }
      auto registry = mica::load_registry(options.config_directory);
      mica::merge_custom_models(registry, options.root);
      mica::merge_installed_profiles(registry, options.root);
      return mica::run_server(registry, options);
    }
    usage();
    throw std::invalid_argument("unknown command: " + args[1]);
  } catch (const std::exception& error) {
    std::cerr << "mica-server: " << error.what() << '\n';
    return 1;
  }
}
