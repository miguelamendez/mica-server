#include "mica_server/tui.hpp"
#include "mica_server/service.hpp"
#include "mica_server/server.hpp"
#include "mica_server/catalog.hpp"
#include "mica_server/config.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/command.hpp"
#include "mica_server/inventory.hpp"
#include "mica_server/machine.hpp"
#include "mica_server/memory.hpp"
#include "mica_server/scheduler.hpp"
#include <nlohmann/json.hpp>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace mica {
namespace {
using json = nlohmann::json;
using namespace ftxui;
struct Section { std::string id, title, hint; };
std::vector<Section> sections_from_lua(const std::filesystem::path& path) {
  auto* lua = luaL_newstate();
  if (!lua) throw std::runtime_error("cannot initialize Lua TUI configuration");
  struct Close { lua_State* state; ~Close() { lua_close(state); } } close{lua};
  // No IO, OS or package libraries: the UI configuration is presentation data.
  if (luaL_loadfile(lua, path.c_str()) || lua_pcall(lua, 0, 1, 0))
    throw std::runtime_error(std::string("invalid TUI configuration: ") + lua_tostring(lua, -1));
  if (!lua_istable(lua, -1)) throw std::runtime_error("TUI configuration must return a table");
  lua_getfield(lua, -1, "sections");
  if (!lua_istable(lua, -1)) throw std::runtime_error("TUI sections missing");
  std::vector<Section> sections;
  const std::vector<std::string> allowed{"server", "workloads", "models", "engines", "endpoints", "settings"};
  const auto count = lua_rawlen(lua, -1);
  if (count != allowed.size()) throw std::runtime_error("TUI requires six sections");
  for (std::size_t i = 1; i <= count; ++i) {
    lua_rawgeti(lua, -1, i);
    const auto field = [&](const char* key) {
      lua_getfield(lua, -1, key);
      if (!lua_isstring(lua, -1)) throw std::runtime_error("TUI section field must be text");
      std::string value = lua_tostring(lua, -1); lua_pop(lua, 1); return value;
    };
    Section section{field("id"), field("title"), field("hint")};
    if (section.id != allowed[i - 1]) throw std::runtime_error("unsupported TUI section order");
    sections.push_back(std::move(section)); lua_pop(lua, 1);
  }
  return sections;
}

json read_optional(const std::filesystem::path& path) {
  if (!std::filesystem::is_regular_file(path)) return json::object();
  std::ifstream stream(path); return json::parse(stream);
}
std::string compact(const json& value) {
  if (value.is_null()) return "not verified";
  if (value.is_string()) return value.get<std::string>();
  if (value.is_array()) {
    std::string out;
    for (const auto& item : value) { if (!out.empty()) out += ", "; out += compact(item); }
    return out.empty() ? "none" : out;
  }
  if (value.is_object()) return value.dump();
  return value.dump();
}

struct ViewData {
  Registry registry;
  json hardware, machine, server, settings, models, runtime, engines, workloads, disk;
};
std::string model_group(const std::string& capability) {
  if (capability == "text") return "LLM";
  if (capability == "vision") return "VLM";
  if (capability == "embedding" || capability == "embeddings") return "Embeddings";
  if (capability == "asr") return "ASR";
  if (capability == "tts") return "TTS";
  if (capability == "diarization" || capability == "diar") return "Diarization";
  if (capability == "image-generation") return "Image generation";
  if (capability == "audio-generation") return "Audio generation";
  return capability;
}
std::string gib_text(std::uintmax_t bytes) {
  std::ostringstream out; out << std::fixed << std::setprecision(2) << bytes / 1073741824.0 << " GiB";
  return out.str();
}
json disk_usage(const std::filesystem::path& root) {
  json result = json::object();
  for (const auto* name : {"models", "runtimes", "environments"}) {
    std::uintmax_t bytes = 0;
    std::error_code error;
    auto it = std::filesystem::recursive_directory_iterator(root / name,
        std::filesystem::directory_options::skip_permission_denied, error);
    const auto end = std::filesystem::recursive_directory_iterator();
    while (!error && it != end) {
      if (it->is_regular_file(error) && !it->is_symlink(error)) {
        auto size = it->file_size(error); if (!error) bytes += size;
      }
      it.increment(error);
    }
    result[name] = bytes;
  }
  return result;
}
ViewData refresh_data(const std::filesystem::path& root, const std::filesystem::path& config) {
  ViewData data;
  data.registry = load_registry(config);
  data.registry.runtime_root = root.string();
  merge_custom_models(data.registry, root);
  merge_installed_profiles(data.registry, root);
  const auto hardware = detect_hardware();
  data.hardware = hardware_to_json(hardware);
  data.machine = std::filesystem::exists(root / "config/machine.yaml")
                     ? read_profile_file(root / "config/machine.yaml") : json::object();
  data.server = local_server_status(root);
  data.settings = redact_server_configuration(load_server_configuration(root / "config/server.json"));
  const bool configured = data.settings.value("api_key_configured", false) ||
                          std::filesystem::exists(root / "secrets/api-key");
  data.settings.erase("api_key");
  data.settings["api_key"] = configured ? "configured (hidden)" : "not configured";
  data.settings["host"] = data.settings.value("host", "127.0.0.1");
  data.settings["port"] = data.settings.value("port", ServerOptions{}.port);
  data.models = registry_catalog(data.registry, std::nullopt, std::nullopt, false);
  data.engines = json::object();
  for (const auto& [id, engine] : data.registry.engines)
    data.engines[id] = inspect_engine(engine, hardware, root);
  for (auto& entry : data.models["data"]) {
    const auto& model = data.registry.model(entry.at("id").get<std::string>());
    entry["group"] = model_group(model.capability);
    entry["compatible"] = false; entry["cached"] = false;
    entry["supported_engines"] = json::array();
    for (auto& family : entry["variants"].items()) for (auto& variant : family.value()) {
      const auto engine = variant.at("engine").get<std::string>();
      const auto backend = data.registry.engine(engine).backend;
      const auto quant = Quantization(variant.at("quantization").get<std::string>());
      variant["availability"] = inspect_artifact(model, model.artifact_for(backend, quant, engine), backend, quant, root);
      variant["compatible"] = data.engines.at(engine).at("compatible");
      entry["compatible"] = entry["compatible"].get<bool>() || variant["compatible"].get<bool>();
      entry["cached"] = entry["cached"].get<bool>() || variant["availability"]["cached"].get<bool>();
      if (std::find(entry["supported_engines"].begin(), entry["supported_engines"].end(), json(engine)) == entry["supported_engines"].end())
        entry["supported_engines"].push_back(engine);
    }
  }
  const auto active = data.server.value("profile", json::object()).value("name", "");
  const auto fallback = data.settings.value("default_workload", "");
  const auto allocation = resolve_machine_policy(hardware, data.machine.empty() ? MachinePolicy{} : machine_policy_from_document(data.machine));
  const auto policy_hardware = hardware_for_machine_policy(hardware, allocation);
  data.workloads = json::object();
  for (const auto& [id, profile] : data.registry.profiles) if (profile.schema >= 4) {
    auto availability = inspect_workload(data.registry, profile, hardware, root);
    availability["visible"] = profile.catalog_visible || id == active || id == fallback ||
        std::filesystem::exists(root / "config/profiles" / (id + ".yaml"));
    availability["description"] = profile.description;
    availability["active"] = id == active;
    double vram = 0;
    for (const auto& [device, limit] : allocation.dedicated_memory_gib) vram = std::max(vram, limit);
    availability["allocation_fits"] = profile.required_ram_gib <= allocation.inference_ram_gib && profile.required_vram_gib <= vram;
    availability["allocation_reason"] = availability["allocation_fits"].get<bool>() ?
        "Declared requirements fit; setup still checks per-device worker footprints" : "RAM/VRAM requirements exceed machine allocation";
    try {
      auto registry = data.registry; auto resolved = profile;
      resolve_profile_engines(registry, resolved, policy_hardware, root);
      validate_memory_strategy(registry, resolved, allocation.inference_ram_gib + vram);
      for (const auto& policy : resolved.model_policies) {
        const auto& artifact = registry.model(policy.id).artifact_for(policy.backend, policy.quantization, policy.engine);
        const auto placement = resolve_model_placement(policy, artifact, policy_hardware);
        auto device = placement.device;
        if (device != "cpu") {
          const auto colon = device.find(':');
          const auto id = colon == std::string::npos ? "0" : device.substr(colon + 1);
          for (const auto& accelerator : hardware.accelerators)
            if (accelerator.id == id) { device = accelerator.runtime + ":" + id; break; }
        }
        if (!allocation.allowed_devices.contains(device)) throw std::runtime_error("Machine policy disables " + device);
      }
    } catch (const std::exception& error) {
      availability["allocation_fits"] = false; availability["allocation_reason"] = error.what();
    }
    data.workloads[id] = std::move(availability);
  }
  data.disk = disk_usage(root);
  data.runtime = read_optional(root / "state/runtime.json");
  // Runtime is internal; retain only fields used in engine/workload status cards.
  json safe = json::object();
  for (const auto* key : {"resolved", "downloads", "profile", "max_ram_gib", "max_vram_gib"})
    if (data.runtime.contains(key)) safe[key] = data.runtime[key];
  data.runtime = std::move(safe);
  return data;
}
}

int run_tui(const std::filesystem::path& executable, const std::filesystem::path& root,
            const std::filesystem::path& config, bool snapshot) {
  const auto sections = sections_from_lua(config / "tui.lua");
  const auto endpoint_catalog = server_endpoints();
  auto data = refresh_data(root, config);
  if (snapshot) {
    json navigation = json::array();
    for (const auto& section : sections) navigation.push_back({{"id", section.id}, {"title", section.title}});
    json workloads = json::array();
    for (const auto& [id, profile] : data.registry.profiles)
      if (data.workloads.contains(id) && data.workloads[id]["visible"].get<bool>()) {
        auto entry = data.workloads[id]; entry["id"] = id; workloads.push_back(entry);
      }
    json engines = json::array();
    for (const auto& [id, engine] : data.registry.engines)
      engines.push_back({{"id", id}, {"description", engine.description}, {"availability", data.engines[id]}, {"hardware", engine.hardware}});
    std::cout << json{{"sections", navigation}, {"server", data.server}, {"machine", data.hardware},
                     {"settings", data.settings}, {"disk_usage_bytes", data.disk}, {"workloads", workloads},
                     {"server_subsections", {"Overview", "Choose workload", "Machine"}},
                     {"models", data.models}, {"engines", engines}, {"endpoints", server_endpoints()}}.dump(2) << '\n';
    return 0;
  }
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    throw std::runtime_error("tui needs an interactive terminal; use --snapshot for diagnostics");
  auto screen = App::Fullscreen();
  const auto accent = Color::RGB(180, 190, 254);
  const auto muted = Color::RGB(166, 173, 200);
  const auto good = Color::RGB(166, 227, 161);
  int section_index = 0, row_index = 0, scroll = 0;
  std::string search, message = "Choose a section. Tab opens its list; arrows select; ? shows keys.";
  bool search_open = false, confirm = false, busy = false, help = false, list_focus = false, show_all = false, server_picker = false;
  const std::vector<std::string> groups{"All", "LLM", "VLM", "ASR", "TTS", "Embeddings", "Diarization", "Image generation", "Audio generation"};
  int group_index = 0;
  std::vector<std::string> scope;
  struct Location { int section, row, group; bool picker; std::vector<std::string> scope; };
  std::vector<Location> history;
  std::string edit_option, edit_value;
  std::vector<std::string> pending;
  std::thread worker;
  auto search_input = Input(&search, "Filter entries...");
  auto edit_input = Input(&edit_value, "Enter a value...");

  const auto entries = [&]() {
    std::vector<std::string> values;
    const auto& section = sections[section_index].id;
    if (section == "server" && !server_picker) values = {"Overview", "Choose workload", "Machine"};
    else if (section == "workloads" || (section == "server" && server_picker)) {
      for (const auto& [id, profile] : data.registry.profiles)
        if (data.workloads.contains(id) && data.workloads[id]["visible"].get<bool>() &&
            (show_all || data.workloads[id]["compatible"].get<bool>() || data.workloads[id]["active"].get<bool>())) values.push_back(id);
    } else if (section == "models") {
      for (const auto& model : data.models["data"]) {
        const auto id = model.at("id").get<std::string>();
        if ((show_all || model["compatible"].get<bool>()) &&
            (group_index == 0 || model["group"] == groups[group_index]) &&
            (scope.empty() || std::find(scope.begin(), scope.end(), id) != scope.end())) values.push_back(id);
      }
      std::stable_sort(values.begin(), values.end(), [&](const auto& a, const auto& b) {
        return model_group(data.registry.model(a).capability) < model_group(data.registry.model(b).capability);
      });
    } else if (section == "engines") {
      for (const auto& [id, engine] : data.registry.engines)
        if ((show_all || data.engines[id]["compatible"].get<bool>()) &&
            (scope.empty() || std::find(scope.begin(), scope.end(), id) != scope.end())) values.push_back(id);
    } else if (section == "endpoints") {
      for (const auto& endpoint : endpoint_catalog["endpoints"])
        values.push_back(endpoint.at("method").get<std::string>() + " " + endpoint.at("path").get<std::string>());
    } else if (section == "settings") {
      values = {"Bind address", "Port", "Default workload", "RAM limit (GiB)",
                "Dedicated GPU limit (GiB)", "Import API key file", "Rotate API key"};
    }
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
      std::string searchable = value;
      if (section == "models") {
        const auto& model = data.registry.model(value);
        searchable += " " + model.description;
        for (const auto& task : model.supported_tasks) searchable += " " + task;
      } else if (section == "engines") searchable += " " + data.registry.engine(value).description;
      else if (section == "workloads" || (section == "server" && server_picker)) searchable += " " + data.registry.profile(value).description;
      std::string query = search;
      const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
      std::transform(searchable.begin(), searchable.end(), searchable.begin(), lower);
      std::transform(query.begin(), query.end(), query.begin(), lower);
      return !query.empty() && searchable.find(query) == std::string::npos;
    }), values.end());
    return values;
  };
  const auto queue = [&](std::vector<std::string> command) {
    pending = std::move(command); confirm = true; help = false;
  };
  const auto begin_job = [&]() {
    if (worker.joinable()) worker.join();
    busy = true; confirm = false;
    const auto command = pending;
    message = "Running " + display_command(command) + " — wait for completion";
    worker = std::thread([&, command] {
      CommandResult result;
      try { result = run_command(command, true); }
      catch (const std::exception& e) { result = {1, e.what()}; }
      screen.Post([&, result = std::move(result)] {
        busy = false;
        message = result.exit_code == 0 ? "Completed. Inventory refreshed." : "Failed. " + result.output.substr(0, 450);
        try { data = refresh_data(root, config); } catch (const std::exception& e) { message = e.what(); }
      });
      screen.PostEvent(Event::Custom);
    });
  };
  const auto detail = [&](const std::string& selected) {
    Elements lines;
    const auto heading = [&](const std::string& title) {
      lines.push_back(text(" " + title) | bold | color(accent));
    };
    const auto section_heading = [&](const std::string& title) {
      if (!lines.empty()) { lines.push_back(text("")); lines.push_back(separator()); }
      heading(title);
    };
    const auto row = [&](const std::string& label, const json& value) {
      auto readable = label;
      std::replace(readable.begin(), readable.end(), '_', ' ');
      if (!readable.empty()) readable[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(readable[0])));
      lines.push_back(paragraph(" " + readable + ": " + compact(value)));
    };
    std::function<void(const json&, std::string)> tree;
    tree = [&](const json& value, std::string prefix) {
      for (auto item = value.begin(); item != value.end(); ++item) {
        const auto label = prefix + item.key();
        if (item.value().is_object()) { heading(label); tree(item.value(), "  "); }
        else if (item.value().is_array() && !item.value().empty() && item.value()[0].is_object()) {
          heading(label);
          for (const auto& entry : item.value()) { tree(entry, "  "); lines.push_back(text("")); }
        } else row(label, item.value());
      }
    };
    const auto object = [&](const json& value) { tree(value, ""); };
    const auto& section = sections[section_index].id;
    if (section == "server" && !server_picker && selected != "Machine") {
      section_heading("Server · Overview");
      row("Status", data.server.value("status", "stopped"));
      row("Default workload", data.settings.value("default_workload", "not configured"));
      if (data.server.contains("profile")) {
        row("Workload", data.server["profile"]["name"]);
        row("Listening", compact(data.server["host"]) + ":" + compact(data.server["port"]));
        section_heading("Memory allocation (planning reservations)");
        row("RAM reserved / allocated (GiB)", compact(data.server["reserved_ram_gib"]) + " / " + compact(data.server["max_ram_gib"]));
        row("VRAM reserved / allocated (GiB)", compact(data.server["reserved_vram_gib"]) + " / " + compact(data.server["max_vram_gib"]));
        section_heading("Loaded models");
        for (const auto& w : data.server["workers"]) lines.push_back(text(" " + w.at("id").get<std::string>()));
        if (data.server["workers"].empty()) row("Workers", "none resident");
      } else if (data.server.contains("error")) row("Error", data.server["error"]);
      else row("Loaded models", "none — server stopped");
      section_heading("Stored data (logical file size; not RAM)");
      for (const auto& item : data.disk.items()) row(item.key(), gib_text(item.value().get<std::uintmax_t>()));
      section_heading("Actions"); row("Controls", "Choose workload + Enter to start or swap · s choose/start · x stop · r refresh");
      row("Memory accounting", "Reservations are planning estimates, not measurements or a universal hard RSS/VRAM cap.");
    } else if (section == "server" && !server_picker && selected == "Machine") {
      section_heading("Detected machine (facts)"); object(data.hardware);
      section_heading("User limits (machine.yaml)"); object(data.machine);
      row("Edit limits", "Use Settings. Changes require stopping and reconciling the workload.");
    } else if (section == "settings") {
      section_heading("Server configuration"); object(data.settings);
      section_heading("Machine policy"); object(data.machine);
      section_heading("Editing and application");
      row("Selected", selected);
      row("Actions", "Enter edits the selected setting. API keys stay hidden; import a file or generate a replacement.");
      row("Apply", "Stop first. Settings are saved for the next start; a workload is reconciled against the new limits.");
      row("Network", "0.0.0.0 listens on all IPv4 interfaces. Use a trusted LAN and a firewall; HTTP is not encrypted.");
    } else if (selected.empty()) row("Entries", "No matches");
    else if (section == "workloads" || (section == "server" && server_picker)) {
      const auto& profile = data.registry.profile(selected);
      section_heading("Workload · Overview"); row("ID", selected); row("Description", profile.description);
      section_heading("Memory and residency policy");
      row("RAM required (GiB)", profile.required_ram_gib); row("VRAM required (GiB)", profile.required_vram_gib);
      row("Resident worker limit", profile.maximum_resident_workers);
      row("Strategy", profile.residency_strategy.empty() ? "per-model residency policy" : profile.residency_strategy);
      if (profile.memory_limit_gib > 0) row("Workload ceiling (GiB)", profile.memory_limit_gib);
      section_heading("Availability");
      const auto active = data.server.value("profile", json::object()).value("name", "");
      row("Active", active == selected);
      row("Prepared selection", data.runtime.value("profile", "") == selected);
      row("Source", std::filesystem::exists(root / "config/profiles" / (selected + ".yaml")) ? "installed / user-owned" : "packaged example");
      const auto& availability = data.workloads[selected];
      row("Hardware compatible", availability["compatible"]); row("Compatibility", availability["reason"]);
      row("Engines installed", availability["engines_installed"]); row("Models cached", availability["models_cached"]);
      row("Memory/policy eligibility", availability["allocation_fits"]);
      row("Allocation check", availability["allocation_reason"]);
      section_heading("Models in this workload");
      for (const auto& policy : profile.model_policies) {
        lines.push_back(text("")); lines.push_back(separator());
        heading(policy.id);
        const auto& model = data.registry.model(policy.id);
        row("Description", model.description);
        row("Supported tasks", model.supported_tasks);
        row("Engine", policy.engine.empty() ? "automatic (compatible installed engine, then manifest order)" : policy.engine);
        row("Artifact / quantization", to_string(policy.quantization));
        row("Input / output / total", std::to_string(policy.max_input_tokens) + " / " + std::to_string(policy.max_output_tokens) + " / " + std::to_string(policy.max_total_tokens));
        row("KV cache", policy.kv_cache_precision); row("Residency", to_string(policy.residency));
        row("Priority / startup", std::to_string(policy.priority) + " / " + (policy.startup ? "yes" : "no"));
      }
      section_heading("Actions"); row("Controls", server_picker ? "Enter start/swap selected workload · Esc back" : "Enter browse models · i install · s start · a hot-swap · e edit YAML · c clone");
    } else if (section == "models") {
      for (const auto& model : data.models["data"]) if (model["id"] == selected) {
        section_heading("Model · Overview"); row("ID", selected);
        row("Description", model["description"]); row("Category", model["group"]);
        section_heading("Capabilities and intended tasks");
        for (const auto* key : {"supported_tasks", "input_modalities", "output_modalities", "abilities", "tool_call_formats"})
          if (model.contains(key)) row(key, model[key]);
        row("Task declarations", "Intended uses, not quality certification or endpoint permissions.");
        section_heading("Context and generation guidance");
        for (const auto* key : {"native_context_tokens", "recommended_context_tokens", "max_output_tokens", "thinking_modes", "thinking_budget_supported"})
          if (model.contains(key)) row(key, model[key]);
        section_heading("Availability and supported engines");
        row("Hardware compatible", model["compatible"]);
        row("Any complete artifact cached", model["cached"]); row("Supported engines", model["supported_engines"]);
        section_heading("Artifact and component records");
        if (model.contains("variants")) for (const auto& family : model["variants"].items())
          for (const auto& variant : family.value()) {
            lines.push_back(text("")); lines.push_back(separator());
            heading(compact(variant["engine"]) + " / " + compact(variant["quantization_type"]));
            row("Format", variant["format"]);
            row("Download (GiB)", variant["download_size_gib"]);
            row("Base reservation estimate (GiB)", variant["memory_reservation_gib"]);
            row("Engine compatible / installed", compact(variant["compatible"]) + " / " + compact(data.engines.at(variant["engine"].get<std::string>())["installed"]));
            row("Bundle cached", variant["availability"]["cached"]);
            row("Local artifact", variant["availability"]["path"]);
            row("Cache status", variant["availability"]["reason"]);
            for (const auto& file : variant.value("files", json::array()))
              row(compact(file["role"]), compact(file["repository"]) + "/" + compact(file["path"]) + " · " + gib_text(file.value("size_bytes", std::uint64_t{0})));
            if (variant.contains("memory_estimate") && variant["memory_estimate"].is_object()) {
              heading("Memory estimates (planning, not measured peaks)"); object(variant["memory_estimate"]);
            }
          }
        section_heading("Supported interactions (operation and input/output shape)");
        if (model.contains("supported_interactions")) object(json{{"supported_interactions", model["supported_interactions"]}});
        section_heading("Provenance and references");
        for (const auto* key : {"license", "source_repository"})
          if (model.contains(key)) row(key, model[key]);
        if (model.contains("references")) object(json{{"references", model["references"]}});
        const auto downloads = data.runtime.value("downloads", json::object());
        for (const auto& download : downloads.items())
          if (download.value().value("model", "") == selected) row("Cached " + download.key(), download.value().value("local_path", ""));
        section_heading("Actions"); row("Controls", "Enter browses supported engines · Esc back. Browsing never changes a workload engine pin.");
        row("Loading", "Choose a workload to install exact model/engine/quantization selections.");
      }
    } else if (section == "engines") {
      section_heading("Engine · Overview"); row("ID", selected);
      row("Description", data.registry.engine(selected).description);
      section_heading("Availability");
      row("Hardware compatible", data.engines[selected]["compatible"]);
      row("Compatibility", data.engines[selected]["reason"]);
      row("Installed", data.engines[selected]["installed"]);
      row("Executable / environment", data.engines[selected]["path"]);
      const auto path = config / "engines" / (selected + ".yaml");
      if (std::filesystem::exists(path)) {
        const auto manifest = read_profile_file(path);
        section_heading("Runtime and artifact compatibility");
        for (const auto* key : {"status", "backend", "device_target", "hardware", "artifact_formats", "compatibility_tags"})
          if (manifest.contains(key)) row(key, manifest[key]);
        section_heading("Implemented operation contracts (worker endpoints)");
        if (manifest.contains("endpoint_contracts")) object(json{{"endpoint_contracts", manifest["endpoint_contracts"]}});
        section_heading("Installation recipe");
        for (const auto* key : {"installer", "launcher"}) if (manifest.contains(key)) row(key, manifest[key]);
        for (const auto* key : {"source", "install"}) if (manifest.contains(key)) object(json{{key, manifest[key]}});
      }
      const auto resolved = data.runtime.value("resolved", json::object()).value("engines", json::object());
      if (resolved.contains(selected)) { section_heading("Installed runtime"); object(resolved[selected]); }
      section_heading("Actions");
      row("Installation", "Workload installation prepares missing engines. Installed does not mean inference-certified.");
    } else if (section == "endpoints") {
      for (const auto& endpoint : endpoint_catalog["endpoints"])
        if (selected == endpoint.at("method").get<std::string>() + " " + endpoint.at("path").get<std::string>()) {
          section_heading("Endpoint · Overview"); row("Route", selected);
          row("Description", endpoint["description"]);
          section_heading("Access and availability");
          row("Authentication required", endpoint["authentication_required"]);
          row("Availability", "Inference requires a compatible model interaction and the selected engine's implemented operation, not a task label alone.");
          section_heading("Usage example"); row("example", endpoint["example"]);
        }
      row("Availability", "Listed routes exist globally. Inference needs a compatible active model; see Models and Server.");
    }
    return lines;
  };

  auto renderer = Renderer([&] {
    auto values = entries();
    row_index = std::clamp(row_index, 0, std::max(0, static_cast<int>(values.size()) - 1));
    Elements navigation;
    for (std::size_t i = 0; i < sections.size(); ++i) {
      auto line = text(" " + std::to_string(i + 1) + "  " + sections[i].title + " ");
      if (static_cast<int>(i) == section_index) line = line | bold | color(accent) | inverted;
      navigation.push_back(line);
    }
    auto left = vbox(navigation) | size(WIDTH, EQUAL, 22) | border;
    Elements list;
    for (std::size_t i = 0; i < values.size(); ++i) {
      auto label = values[i];
      const auto& section = sections[section_index].id;
      if (section == "models") {
        for (const auto& model : data.models["data"]) if (model["id"] == values[i])
          label = "[" + compact(model["group"]) + "] " + label + (model["cached"].get<bool>() ? " ✓" : " ↓") + (model["compatible"].get<bool>() ? "" : " !");
      } else if (section == "engines") label += data.engines[values[i]]["installed"].get<bool>() ? " ✓" : " ↓";
      else if (section == "workloads" || (section == "server" && server_picker)) {
        const auto& available = data.workloads[values[i]];
        label += available["prepared"].get<bool>() ? " ✓" : " ↓";
        if (!available["compatible"].get<bool>()) label += " !";
        if (!available["allocation_fits"].get<bool>()) label += " $";
      }
      auto line = text(" " + label + " ");
      if (static_cast<int>(i) == row_index) line = line | color(accent) | focus | (list_focus ? inverted : bold);
      list.push_back(line);
    }
    auto rows = vbox(list) | vscroll_indicator | frame;
    auto details = detail(values.empty() ? "" : values[row_index]);
    scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(details.size()) - 1));
    if (scroll > 0) details.erase(details.begin(), details.begin() + scroll);
    auto right = vbox(details) | yframe | flex | border;
    const bool wide = Terminal::Size().dimx >= 120;
    auto middle = vbox({text(sections[section_index].id == "models" ? " Group: " + groups[group_index] + " (g)" : " Select / Enter to explore"), separator(), rows | flex}) | size(WIDTH, EQUAL, wide ? 38 : 28) | border;
    // Smaller terminals keep the list/details usable instead of squeezing
    // details between two fixed sidebars. Number keys still select views.
    auto body = wide ? hbox({left, middle, right}) : vbox({hbox(navigation), hbox({middle, right}) | flex});
    const auto state = data.server.value("status", "stopped");
    const auto active = data.server.value("profile", json::object()).value("name", "");
    const auto fallback = data.settings.value("default_workload", "not configured");
    auto header = hbox({text(" ◇ Mica ") | bold | color(accent), text(" " + (active.empty() ? "Default: " + fallback : "Active: " + active)) | color(muted),
                        filler(), text(" " + state + (busy ? " · working " : " ")) | color(state == "ready" ? good : muted)});
    const auto& selected_section = sections[section_index].id;
    const std::string actions = selected_section == "workloads" ? " Enter models · i install · s start · a swap · e edit · c clone " :
                                selected_section == "server" ? " s start · x stop · r refresh " :
                                selected_section == "settings" ? " Enter edit selected setting " : " r refresh · PgUp/PgDn details ";
    auto base = vbox({header, separator(), paragraph(" " + sections[section_index].hint) | color(muted), body | flex,
                      separator(), paragraph(" " + message.substr(0, 500)) | size(HEIGHT, LESS_THAN, 4),
                      text(actions) | color(accent),
                      text(std::string(" 1–6 views · Tab focus · ↑↓ select · Enter explore · Esc back · u ") + (show_all ? "compatible only" : "show all hardware") + " · / filter · ? help · q quit") | color(muted)});
    if (confirm) return dbox({base, vbox({text(" Confirm action ") | bold, separator(),
                             paragraph(display_command(pending)), text(""),
                             text(" Enter confirms · Esc cancels ")}) | border | size(WIDTH, LESS_THAN, 85) | clear_under | center});
    if (!edit_option.empty()) return dbox({base, vbox({text(" Set " + edit_option) | bold, separator(), edit_input->Render(),
                                           text(" Enter reviews · Esc cancels ")}) | border | size(WIDTH, LESS_THAN, 70) | clear_under | center});
    if (search_open) return dbox({base, vbox({text(" Filter "), search_input->Render(), text(" Enter closes · Esc clears ")}) | border | size(WIDTH, LESS_THAN, 60) | clear_under | center});
    if (help) return dbox({base, vbox({text(" Mica controls ") | bold, separator(),
      text(" Workloads: i install · s start · a activate · e edit · c clone "),
      text(" Server: Enter Choose workload · s choose/start · x stop · Machine subsection "),
      text(" Workload → Enter models → Enter engines · Esc returns · g model group "),
      text(" u shows incompatible hardware; hidden legacy examples stay hidden "),
      text(" ✓ cached/installed · ↓ needs installation/download · ! unsupported · $ exceeds allocation "),
      text(" Settings: select a field and press Enter "),
      text(" Changes are confirmed and use the same CLI as the terminal. "),
      text(" Closing the TUI leaves the inference server running. "), text(" Esc closes help ")}) | border | clear_under | center});
    return base;
  });
  auto ui = CatchEvent(renderer, [&](Event event) {
    if (busy) {
      if (event == Event::Character("q")) message = "Wait for the current operation to finish before closing.";
      return true;
    }
    if (confirm) {
      if (event == Event::Return) begin_job();
      if (event == Event::Escape) confirm = false;
      return true;
    }
    if (!edit_option.empty()) {
      if (event == Event::Escape) { edit_option.clear(); return true; }
      if (event == Event::Return) {
        if (edit_option == "new workload ID") {
          auto values = entries();
          queue({executable.string(), "workload", "create", edit_value, "--from", values.at(row_index),
                 "--output", (root / "config/profiles" / (edit_value + ".yaml")).string(), "--root", root.string(), "--config-dir", config.string()});
        } else queue({executable.string(), "config", "set", edit_option, edit_value, "--root", root.string()});
        edit_option.clear(); return true;
      }
      return edit_input->OnEvent(event);
    }
    if (search_open) {
      if (event == Event::Return) { search_open = false; return true; }
      if (event == Event::Escape) { search.clear(); search_open = false; return true; }
      row_index = 0; return search_input->OnEvent(event);
    }
    if (event == Event::Character("q")) { screen.ExitLoopClosure()(); return true; }
    if (event == Event::Character("?")) { help = !help; return true; }
    if (event == Event::Escape) {
      if (help) { help = false; return true; }
      if (!history.empty()) {
        auto previous = history.back(); history.pop_back();
        section_index = previous.section; row_index = previous.row; group_index = previous.group;
        server_picker = previous.picker; scope = previous.scope; search.clear(); scroll = 0;
      } else if (server_picker) { server_picker = false; row_index = 1; }
      else list_focus = false;
      return true;
    }
    if (event == Event::Character("u")) { show_all = !show_all; row_index = scroll = 0; return true; }
    if (event == Event::Character("g") && sections[section_index].id == "models") {
      group_index = (group_index + 1) % groups.size(); row_index = scroll = 0; return true;
    }
    if (event == Event::Character("/")) { search_open = true; return true; }
    if (event == Event::Tab) { list_focus = !list_focus; return true; }
    if (event == Event::PageDown) { scroll += 8; return true; }
    if (event == Event::PageUp) { scroll = std::max(0, scroll - 8); return true; }
    if (event == Event::ArrowDown || event == Event::ArrowUp) {
      const int delta = event == Event::ArrowDown ? 1 : -1;
      if (list_focus) row_index = std::max(0, row_index + delta);
      else { section_index = std::clamp(section_index + delta, 0, 5); row_index = 0; search.clear(); scope.clear(); history.clear(); server_picker = false; }
      scroll = 0; return true;
    }
    if (event.is_character() && event.character().size() == 1 && event.character()[0] >= '1' && event.character()[0] <= '6') {
      section_index = event.character()[0] - '1'; row_index = scroll = 0; search.clear(); scope.clear(); history.clear(); server_picker = false; list_focus = true; return true;
    }
    if (event == Event::Character("r")) {
      try { data = refresh_data(root, config); message = "Refreshed."; } catch (const std::exception& e) { message = e.what(); }
      return true;
    }
    const auto& section = sections[section_index].id;
    auto values = entries();
    const auto selected = values.empty() ? "" : values.at(std::min(row_index, static_cast<int>(values.size()) - 1));
    const auto command = [&](std::vector<std::string> args) {
      args.insert(args.begin(), executable.string()); args.insert(args.end(), {"--root", root.string()}); queue(std::move(args));
    };
    if (section == "server") {
      if (event == Event::Character("s") || event == Event::Return) {
        if (server_picker && !selected.empty()) {
          if (!data.workloads[selected]["compatible"].get<bool>()) {
            message = "Cannot start/swap: " + compact(data.workloads[selected]["reason"]); return true;
          }
          if (local_server_running(root)) command({"workload", "activate", selected});
          else command({"start", "--workload", selected, "--config-dir", config.string()});
        } else if (selected == "Choose workload" || event == Event::Character("s")) {
          server_picker = true; row_index = scroll = 0; list_focus = true;
          const auto current = data.server.value("profile", json::object()).value("name", data.settings.value("default_workload", ""));
          const auto choices = entries();
          const auto found = std::find(choices.begin(), choices.end(), current);
          if (found != choices.end()) row_index = static_cast<int>(found - choices.begin());
          message = "Choose a workload; Enter starts when stopped or hot-swaps when running. Install missing dependencies from Workloads first.";
        } else list_focus = true;
      }
      if (event == Event::Character("x")) command({"stop"});
    } else if (section == "workloads" && !selected.empty()) {
      if ((event == Event::Character("i") || event == Event::Character("s") || event == Event::Character("a")) &&
          !data.workloads[selected]["compatible"].get<bool>()) {
        message = "Cannot install/start/swap: " + compact(data.workloads[selected]["reason"]); return true;
      }
      if (event == Event::Return) {
        history.push_back({section_index, row_index, group_index, server_picker, scope});
        scope = data.registry.profile(selected).models; section_index = 2; row_index = scroll = group_index = 0; search.clear(); list_focus = true;
        message = "Models in " + selected + ". Enter inspects supported engines; Esc returns to workload.";
      }
      if (event == Event::Character("i")) command({"workload", "install", selected, "--config-dir", config.string()});
      if (event == Event::Character("s")) command({"start", "--workload", selected, "--config-dir", config.string()});
      if (event == Event::Character("a")) command({"workload", "activate", selected});
      if (event == Event::Character("c")) { edit_option = "new workload ID"; edit_value = selected + "-custom"; }
      if (event == Event::Character("e")) {
        screen.WithRestoredIO([&] {
          const auto result = run_command({executable.string(), "workload", "edit", selected, "--root", root.string(), "--config-dir", config.string()});
          message = result.exit_code == 0 ? "Edited YAML validated and installed." : "Edit rejected; draft retained for correction.";
        })();
        data = refresh_data(root, config);
      }
    } else if (section == "models" && event == Event::Return && !selected.empty()) {
      history.push_back({section_index, row_index, group_index, server_picker, scope});
      scope.clear();
      for (const auto& model : data.models["data"]) if (model["id"] == selected)
        scope = model["supported_engines"].get<std::vector<std::string>>();
      section_index = 3; row_index = scroll = 0; search.clear(); list_focus = true;
      message = "Supported engines for " + selected + ". View only; engine pins are edited in the workload YAML.";
    } else if (section == "settings" && event == Event::Return && !selected.empty()) {
      if (local_server_running(root)) { message = "Stop Mica first to change configuration safely."; return true; }
      const std::map<std::string, std::string> options{{"Bind address", "--host"}, {"Port", "--port"},
          {"Default workload", "--default-workload"}, {"RAM limit (GiB)", "--ram-gib"},
          {"Dedicated GPU limit (GiB)", "--vram-gib"}, {"Import API key file", "--api-key-file"}};
      if (selected == "Rotate API key") command({"config", "set", "--rotate-api-key"});
      else { edit_option = options.at(selected); edit_value.clear(); }
    } else if (event == Event::Return) list_focus = true;
    return true;
  });
  std::atomic<bool> done{false};
  std::thread ticker([&] {
    while (!done.load()) {
      for (int i = 0; i < 20 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (done.load()) break;
      auto state = local_server_status(root);
      screen.Post([&, state = std::move(state)] { data.server = state; });
      screen.PostEvent(Event::Custom);
    }
  });
  screen.Loop(ui);
  done.store(true);
  ticker.join();
  if (worker.joinable()) worker.join();
  return 0;
}
}
