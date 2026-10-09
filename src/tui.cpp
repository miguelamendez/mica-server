#include "mica_server/tui.hpp"
#include "mica_server/service.hpp"
#include "mica_server/server.hpp"
#include "mica_server/catalog.hpp"
#include "mica_server/config.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/command.hpp"
#include <nlohmann/json.hpp>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
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
  const std::vector<std::string> allowed{"server", "workloads", "models", "engines", "machine", "endpoints", "settings"};
  const auto count = lua_rawlen(lua, -1);
  if (count != allowed.size()) throw std::runtime_error("TUI requires seven sections");
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
  json hardware, machine, server, settings, models, runtime;
};
ViewData refresh_data(const std::filesystem::path& root, const std::filesystem::path& config) {
  ViewData data;
  data.registry = load_registry(config);
  data.registry.runtime_root = root.string();
  merge_custom_models(data.registry, root);
  merge_installed_profiles(data.registry, root);
  data.hardware = hardware_to_json(detect_hardware());
  data.machine = std::filesystem::exists(root / "config/machine.yaml")
                     ? read_profile_file(root / "config/machine.yaml") : json::object();
  data.server = local_server_status(root);
  data.settings = read_optional(root / "config/server.json");
  const bool configured = data.settings.contains("api_key") ||
                          std::filesystem::exists(root / "secrets/api-key");
  data.settings.erase("api_key");
  data.settings["api_key"] = configured ? "configured (hidden)" : "not configured";
  data.settings["host"] = data.settings.value("host", "127.0.0.1");
  data.settings["port"] = data.settings.value("port", 8080);
  data.models = registry_catalog(data.registry, std::nullopt, std::nullopt, false);
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
      if (profile.schema >= 5) workloads.push_back({{"id", id}, {"description", profile.description}});
    json engines = json::array();
    for (const auto& [id, engine] : data.registry.engines)
      engines.push_back(read_profile_file(config / "engines" / (id + ".yaml")));
    std::cout << json{{"sections", navigation}, {"server", data.server}, {"machine", data.hardware},
                     {"settings", data.settings}, {"workloads", workloads},
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
  bool search_open = false, confirm = false, busy = false, help = false, list_focus = false;
  std::string edit_option, edit_value;
  std::vector<std::string> pending;
  std::thread worker;
  auto search_input = Input(&search, "Filter entries...");
  auto edit_input = Input(&edit_value, "Enter a value...");

  const auto entries = [&]() {
    std::vector<std::string> values;
    const auto& section = sections[section_index].id;
    if (section == "workloads") {
      for (const auto& [id, profile] : data.registry.profiles)
        if (profile.schema >= 5) values.push_back(id);
    } else if (section == "models") {
      for (const auto& model : data.models["data"]) values.push_back(model.at("id").get<std::string>());
    } else if (section == "engines") {
      for (const auto& [id, engine] : data.registry.engines) values.push_back(id);
    } else if (section == "endpoints") {
      for (const auto& endpoint : endpoint_catalog["endpoints"])
        values.push_back(endpoint.at("method").get<std::string>() + " " + endpoint.at("path").get<std::string>());
    } else if (section == "settings") {
      values = {"Bind address", "Port", "Default workload", "RAM limit (GiB)",
                "Dedicated GPU limit (GiB)", "Import API key file", "Rotate API key"};
    }
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
      return !search.empty() && value.find(search) == std::string::npos;
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
      auto result = run_command(command, true);
      screen.Post([&, result = std::move(result)] {
        busy = false;
        message = (result.exit_code == 0 ? "Completed. " : "Failed. ") + result.output;
        if (message.size() > 1800) message = message.substr(message.size() - 1800);
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
    const auto row = [&](const std::string& label, const json& value) {
      lines.push_back(paragraph(" " + label + ": " + compact(value)));
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
    if (section == "server") {
      heading("Local inference server");
      row("Status", data.server.value("status", "stopped"));
      if (data.server.contains("profile")) {
        row("Workload", data.server["profile"]["name"]);
        row("RAM reserved / allocated (GiB)", compact(data.server["reserved_ram_gib"]) + " / " + compact(data.server["max_ram_gib"]));
        row("VRAM reserved / allocated (GiB)", compact(data.server["reserved_vram_gib"]) + " / " + compact(data.server["max_vram_gib"]));
        row("Listening", compact(data.server["host"]) + ":" + compact(data.server["port"]));
        heading("Resident model workers");
        for (const auto& w : data.server["workers"]) {
          row(w.at("id").get<std::string>(), compact(w["device"]) + " · " + compact(w["in_flight"]) + " active requests");
        }
        if (data.server["workers"].empty()) row("Workers", "none resident");
      } else if (data.server.contains("error")) row("Error", data.server["error"]);
      lines.push_back(separator()); row("Actions", "s start · x stop · r refresh · 2 browse and swap workloads");
      row("Memory accounting", "Reservations are planning estimates, not measurements or a universal hard RSS/VRAM cap.");
    } else if (section == "machine") {
      heading("Detected machine (facts)"); object(data.hardware);
      heading("User limits (machine.yaml)"); object(data.machine);
      row("Edit limits", "Use Settings. Changes require stopping and reconciling the workload.");
    } else if (section == "settings") {
      heading("Server configuration"); object(data.settings);
      heading("Machine policy"); object(data.machine);
      row("Selected", selected);
      row("Actions", "Enter edits the selected setting. API keys stay hidden; import a file or generate a replacement.");
      row("Apply", "Stop first. Settings are saved for the next start; a workload is reconciled against the new limits.");
      row("Network", "0.0.0.0 listens on all IPv4 interfaces. Use a trusted LAN and a firewall; HTTP is not encrypted.");
    } else if (selected.empty()) row("Entries", "No matches");
    else if (section == "workloads") {
      const auto& profile = data.registry.profile(selected);
      heading(selected); row("Purpose", profile.description);
      row("RAM required (GiB)", profile.required_ram_gib); row("VRAM required (GiB)", profile.required_vram_gib);
      row("Active", data.runtime.value("profile", "") == selected);
      row("Source", std::filesystem::exists(root / "config/profiles" / (selected + ".yaml")) ? "installed / user-owned" : "packaged example");
      row("Resident worker limit", profile.maximum_resident_workers);
      const auto inference = data.machine.value("limits", json::object()).value("inference", json::object());
      const double ram = inference.value("ram_gib", 8.0);
      double vram = 0;
      for (const auto& value : inference.value("dedicated_memory_gib", json::object()))
        vram = std::max(vram, value.get<double>());
      row("Memory eligibility", profile.required_ram_gib <= ram && profile.required_vram_gib <= vram
                                  ? "Requirements fit configured allocation (engine/device checks run during install)"
                                  : "Blocked: workload requirements exceed configured allocation");
      for (const auto& policy : profile.model_policies) {
        heading(policy.id);
        row("Engine / artifact", policy.engine + " / " + to_string(policy.quantization));
        row("Input / output / total", std::to_string(policy.max_input_tokens) + " / " + std::to_string(policy.max_output_tokens) + " / " + std::to_string(policy.max_total_tokens));
        row("KV cache", policy.kv_cache_precision); row("Residency", to_string(policy.residency));
        row("Priority / startup", std::to_string(policy.priority) + " / " + (policy.startup ? "yes" : "no"));
      }
      lines.push_back(separator()); row("Actions", "i install engines + models · s install & start · a hot-swap · e edit YAML · c clone");
    } else if (section == "models") {
      for (const auto& model : data.models["data"]) if (model["id"] == selected) {
        heading(selected);
        for (const auto* key : {"description", "license", "input_modalities", "output_modalities", "abilities",
                               "native_context_tokens", "recommended_context_tokens", "max_output_tokens", "thinking_modes",
                               "thinking_budget_supported", "tool_call_formats", "source_repository", "quantizations"})
          if (model.contains(key)) row(key, model[key]);
        heading("Artifact and component records");
        if (model.contains("variants")) for (const auto& family : model["variants"].items())
          for (const auto& variant : family.value()) {
            heading(compact(variant["engine"]) + " / " + compact(variant["quantization_type"]));
            row("Format", variant["format"]);
            row("Download (GiB)", variant["download_size_gib"]);
            row("Base reservation estimate (GiB)", variant["memory_reservation_gib"]);
            for (const auto& file : variant.value("files", json::array()))
              row(compact(file["role"]), compact(file["repository"]) + "/" + compact(file["path"]));
          }
        const auto downloads = data.runtime.value("downloads", json::object());
        for (const auto& download : downloads.items())
          if (download.value().value("model", "") == selected) row("Cached " + download.key(), download.value().value("local_path", ""));
        row("Loading", "Choose a workload to install exact model/engine/quantization selections.");
      }
    } else if (section == "engines") {
      heading(selected);
      const auto path = config / "engines" / (selected + ".yaml");
      if (std::filesystem::exists(path)) object(read_profile_file(path));
      const auto resolved = data.runtime.value("resolved", json::object()).value("engines", json::object());
      if (resolved.contains(selected)) { heading("Installed runtime"); object(resolved[selected]); }
      else row("Installation", "Not selected in current setup; workloads install missing compatible engines.");
    } else if (section == "endpoints") {
      for (const auto& endpoint : endpoint_catalog["endpoints"])
        if (selected == endpoint.at("method").get<std::string>() + " " + endpoint.at("path").get<std::string>()) {
          heading(selected); object(endpoint);
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
    auto left = vbox(navigation) | size(WIDTH, EQUAL, 26) | border;
    Elements list;
    for (std::size_t i = 0; i < values.size(); ++i) {
      auto line = text(" " + values[i] + " ");
      if (static_cast<int>(i) == row_index) line = line | color(accent) | focus | (list_focus ? inverted : bold);
      list.push_back(line);
    }
    auto rows = vbox(list) | vscroll_indicator | frame;
    auto details = detail(values.empty() ? "" : values[row_index]);
    scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(details.size()) - 1));
    if (scroll > 0) details.erase(details.begin(), details.begin() + scroll);
    auto right = vbox(details) | flex | border;
    if (!values.empty())
      left = vbox({vbox(navigation), separator(), rows | flex}) | size(WIDTH, EQUAL, 26) | border;
    auto body = hbox({left, right});
    const auto state = data.server.value("status", "stopped");
    auto header = hbox({text(" ◇ Mica ") | bold | color(accent), text(" local model server ") | color(muted),
                        filler(), text(" " + state + (busy ? " · working " : " ")) | color(state == "ready" ? good : muted)});
    const auto& selected_section = sections[section_index].id;
    const std::string actions = selected_section == "workloads" ? " i install · s start · a swap · e edit · c clone " :
                                selected_section == "server" ? " s start · x stop · r refresh " :
                                selected_section == "settings" ? " Enter edit selected setting " : " r refresh · PgUp/PgDn details ";
    auto base = vbox({header, separator(), paragraph(" " + sections[section_index].hint) | color(muted), body | flex,
                      separator(), paragraph(" " + message.substr(0, 500)) | size(HEIGHT, LESS_THAN, 4),
                      text(actions) | color(accent),
                      text(" 1–7 views · Tab focus · ↑↓ select · / filter · ? help · q quit ") | color(muted)});
    if (confirm) return dbox({base, vbox({text(" Confirm action ") | bold, separator(),
                             paragraph(display_command(pending)), text(""),
                             text(" Enter confirms · Esc cancels ")}) | border | size(WIDTH, LESS_THAN, 85) | clear_under | center});
    if (!edit_option.empty()) return dbox({base, vbox({text(" Set " + edit_option) | bold, separator(), edit_input->Render(),
                                           text(" Enter reviews · Esc cancels ")}) | border | size(WIDTH, LESS_THAN, 70) | clear_under | center});
    if (search_open) return dbox({base, vbox({text(" Filter "), search_input->Render(), text(" Enter closes · Esc clears ")}) | border | size(WIDTH, LESS_THAN, 60) | clear_under | center});
    if (help) return dbox({base, vbox({text(" Mica controls ") | bold, separator(),
      text(" Workloads: i install · s start · a activate · e edit · c clone "),
      text(" Server: s start · x stop · r refresh "), text(" Settings: select a field and press Enter "),
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
    if (event == Event::Escape) { help = false; list_focus = false; return true; }
    if (event == Event::Character("/")) { search_open = true; return true; }
    if (event == Event::Tab) { list_focus = !list_focus; return true; }
    if (event == Event::PageDown) { scroll += 8; return true; }
    if (event == Event::PageUp) { scroll = std::max(0, scroll - 8); return true; }
    if (event == Event::ArrowDown || event == Event::ArrowUp) {
      const int delta = event == Event::ArrowDown ? 1 : -1;
      if (list_focus) row_index = std::max(0, row_index + delta);
      else { section_index = std::clamp(section_index + delta, 0, 6); row_index = 0; search.clear(); }
      scroll = 0; return true;
    }
    if (event.is_character() && event.character().size() == 1 && event.character()[0] >= '1' && event.character()[0] <= '7') {
      section_index = event.character()[0] - '1'; row_index = scroll = 0; search.clear(); list_focus = true; return true;
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
      if (event == Event::Character("s")) command({"start", "--config-dir", config.string()});
      if (event == Event::Character("x")) command({"stop"});
    } else if (section == "workloads" && !selected.empty()) {
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
