#include "mica_server/config.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include "mica_server/profiles.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace mica {
namespace {

using json = nlohmann::json;

void reject_unknown_fields(const json& object,
                           std::initializer_list<std::string_view> allowed,
                           const std::string& label) {
  if (!object.is_object()) throw std::invalid_argument(label + " must be an object");
  for (auto item = object.begin(); item != object.end(); ++item) {
    if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
      throw std::invalid_argument(label + " has unknown field: " + item.key());
    }
  }
}

std::optional<ModelDefinition::TokenLimitClaim> token_claim(
    const json& document, const char* field) {
  if (!document.contains(field)) return std::nullopt;
  const auto& value = document.at(field);
  reject_unknown_fields(value, {"tokens", "source"}, field);
  ModelDefinition::TokenLimitClaim result;
  result.tokens = value.at("tokens").get<int>();
  result.source = value.at("source").get<std::string>();
  if (result.tokens < 1 || result.source.empty()) {
    throw std::invalid_argument(std::string(field) + " needs positive tokens and source");
  }
  return result;
}

bool pinned_revision(const std::string& value) {
  return (value.size() == 40 || value.size() == 64) &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isxdigit(character);
         });
}

bool safe_relative_path(const std::string& value) {
  if (value.empty()) return false;
  if (!std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '_' ||
               character == '.' || character == '/';
      })) return false;
  const std::filesystem::path path(value);
  if (path.is_absolute()) return false;
  return std::none_of(path.begin(), path.end(), [](const auto& part) {
    return part == ".." || part == ".";
  });
}

bool safe_repository(const std::string& value) {
  const auto slash = value.find('/');
  return slash != std::string::npos && slash > 0 && slash + 1 < value.size() &&
         value.find('/', slash + 1) == std::string::npos &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '-' ||
                  character == '_' || character == '.' || character == '/';
         });
}

bool valid_sha256(const std::string& value) {
  return value.empty() ||
         (value.size() == 64 &&
          std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isdigit(character) || (character >= 'a' && character <= 'f');
          }));
}

std::vector<std::filesystem::path> yaml_files(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::is_directory(directory)) return files;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (!item.is_regular_file()) continue;
    if (item.path().extension() == ".yaml" || item.path().extension() == ".yml") {
      files.push_back(item.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string string_field(lua_State* state, int index, const char* name,
                         const std::string& fallback = {}) {
  lua_getfield(state, index, name);
  std::string value = fallback;
  if (lua_isstring(state, -1)) value = lua_tostring(state, -1);
  lua_pop(state, 1);
  return value;
}

double number_field(lua_State* state, int index, const char* name, double fallback) {
  lua_getfield(state, index, name);
  const double value = lua_isnumber(state, -1) ? lua_tonumber(state, -1) : fallback;
  lua_pop(state, 1);
  return value;
}

bool bool_field(lua_State* state, int index, const char* name, bool fallback) {
  lua_getfield(state, index, name);
  const bool value = lua_isboolean(state, -1) ? lua_toboolean(state, -1) : fallback;
  lua_pop(state, 1);
  return value;
}

std::vector<std::string> string_array_field(lua_State* state, int index,
                                             const char* name) {
  std::vector<std::string> values;
  lua_getfield(state, index, name);
  if (lua_istable(state, -1)) {
    const auto length = lua_rawlen(state, -1);
    for (std::size_t i = 1; i <= length; ++i) {
      lua_rawgeti(state, -1, static_cast<lua_Integer>(i));
      if (lua_isstring(state, -1)) values.emplace_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  return values;
}

Registry* registry_from_upvalue(lua_State* state) {
  return static_cast<Registry*>(lua_touserdata(state, lua_upvalueindex(1)));
}

std::string default_engine(Backend backend, const std::string& capability) {
  if (backend == Backend::mlx) {
    if (capability == "text") return "mlx-lm";
    if (capability == "vision") return "mlx-vlm";
    return "mlx-audio";
  }
  if (backend == Backend::gguf) {
    return capability == "text" || capability == "vision"
               ? "llama-cpp"
               : "audio-cpp";
  }
  return "vllm";
}

std::string default_format(Backend backend) {
  if (backend == Backend::mlx) return "mlx";
  if (backend == Backend::gguf) return "gguf";
  return "compressed-tensors";
}

Artifact artifact_from(lua_State* state, int index, const char* prefix,
                       Backend backend, const std::string& capability,
                       bool backend_supported, const std::string& reason) {
  const std::string base(prefix);
  Artifact artifact;
  artifact.engine = string_field(
      state, index, (base + "_engine").c_str(), default_engine(backend, capability));
  artifact.format = string_field(
      state, index, (base + "_format").c_str(), default_format(backend));
  artifact.quantization_type = string_field(
      state, index, (base + "_quant_type").c_str(),
      base.ends_with("q4") ? "q4" : base.ends_with("q8") ? "q8" : "native");
  artifact.pattern = string_field(state, index, (base + "_path").c_str());
  artifact.repository_pattern =
      string_field(state, index, (base + "_repo_path").c_str(), artifact.pattern);
  artifact.projector_pattern =
      string_field(state, index, (base + "_projector").c_str());
  artifact.projector_repository_pattern = string_field(
      state, index, (base + "_projector_repo_path").c_str(),
      artifact.projector_pattern);
  artifact.reservation_gib =
      number_field(state, index, (base + "_ram_gib").c_str(), 0.0);
  artifact.size_bytes = static_cast<std::uint64_t>(
      number_field(state, index, (base + "_size_bytes").c_str(), 0.0));
  artifact.projector_size_bytes = static_cast<std::uint64_t>(number_field(
      state, index, (base + "_projector_size_bytes").c_str(), 0.0));
  artifact.size_source = string_field(
      state, index, (base + "_size_source").c_str(),
      artifact.size_bytes > 0 ? "measured-local-artifact" : "unknown");
  artifact.supported = backend_supported && !artifact.pattern.empty();
  artifact.reason = artifact.supported ? "" : reason;
  return artifact;
}

int register_settings(lua_State* state) {
  auto* registry = registry_from_upvalue(state);
  luaL_checktype(state, 1, LUA_TTABLE);
  registry->default_hf_repo = string_field(state, 1, "default_hf_repo");
  return 0;
}

int register_model(lua_State* state) {
  auto* registry = registry_from_upvalue(state);
  luaL_checktype(state, 1, LUA_TTABLE);
  ModelDefinition model;
  model.id = string_field(state, 1, "id");
  model.capability = string_field(state, 1, "capability");
  model.description = string_field(state, 1, "description");
  model.catalog_visible = bool_field(state, 1, "catalog_visible", true);
  model.tags = string_array_field(state, 1, "tags");
  model.source_repo = string_field(state, 1, "source_repo");
  model.mlx_converter = string_field(state, 1, "mlx_converter");
  model.mlx_quantization_profile =
      string_field(state, 1, "mlx_quantization_profile");
  model.gguf_family = string_field(state, 1, "gguf_family");
  model.gguf_context_tokens =
      static_cast<int>(number_field(state, 1, "gguf_context_tokens", 8192));
  model.gguf_parallel_slots =
      static_cast<int>(number_field(state, 1, "gguf_parallel_slots", 1));
  model.mlx_extract_mtp = bool_field(state, 1, "mlx_extract_mtp", false);
  model.repositories[Backend::mlx] = string_field(state, 1, "mlx_repo");
  model.repositories[Backend::gguf] = string_field(state, 1, "gguf_repo");
  model.repositories[Backend::vllm] = string_field(state, 1, "vllm_repo");
  for (const auto [backend, field] :
       {std::pair{Backend::mlx, "mlx_revision"},
        std::pair{Backend::gguf, "gguf_revision"},
        std::pair{Backend::vllm, "vllm_revision"}}) {
    const auto revision = string_field(state, 1, field);
    if (!revision.empty()) model.repository_revisions[backend] = revision;
  }
  model.startup_priority = static_cast<int>(number_field(state, 1, "priority", 100));
  model.required = bool_field(state, 1, "required", false);
  model.required_by_backend[Backend::mlx] =
      bool_field(state, 1, "mlx_required", model.required);
  model.required_by_backend[Backend::gguf] =
      bool_field(state, 1, "gguf_required", model.required);
  model.required_by_backend[Backend::vllm] =
      bool_field(state, 1, "vllm_required", false);
  if (model.id.empty()) return luaL_error(state, "model id is required");
  if (model.gguf_context_tokens < 512) {
    return luaL_error(state, "gguf_context_tokens must be at least 512");
  }
  if (model.gguf_parallel_slots < 1 || model.gguf_parallel_slots > 16) {
    return luaL_error(state, "gguf_parallel_slots must be between 1 and 16");
  }

  const bool mlx_supported = bool_field(state, 1, "mlx_supported", true);
  const bool gguf_supported = bool_field(state, 1, "gguf_supported", false);
  const bool vllm_supported = bool_field(state, 1, "vllm_supported", false);
  const auto mlx_reason = string_field(state, 1, "mlx_reason", "MLX artifact unavailable");
  const auto gguf_reason =
      string_field(state, 1, "gguf_reason", "llama.cpp compatibility not validated");
  const auto vllm_reason = string_field(
      state, 1, "vllm_reason", "vLLM-compatible artifact is not configured");
  model.artifacts[Backend::mlx][Quantization::q4] =
      artifact_from(state, 1, "mlx_q4", Backend::mlx, model.capability,
                    mlx_supported, mlx_reason);
  model.artifacts[Backend::mlx][Quantization::q8] =
      artifact_from(state, 1, "mlx_q8", Backend::mlx, model.capability,
                    mlx_supported, mlx_reason);
  model.artifacts[Backend::gguf][Quantization::q4] =
      artifact_from(state, 1, "gguf_q4", Backend::gguf, model.capability,
                    gguf_supported, gguf_reason);
  model.artifacts[Backend::gguf][Quantization::q8] =
      artifact_from(state, 1, "gguf_q8", Backend::gguf, model.capability,
                    gguf_supported, gguf_reason);
  model.artifacts[Backend::vllm][Quantization::q4] =
      artifact_from(state, 1, "vllm_q4", Backend::vllm, model.capability,
                    vllm_supported, vllm_reason);
  model.artifacts[Backend::vllm][Quantization::q8] =
      artifact_from(state, 1, "vllm_q8", Backend::vllm, model.capability,
                    vllm_supported, vllm_reason);
  model.artifacts[Backend::vllm][Quantization::native] =
      artifact_from(state, 1, "vllm_native", Backend::vllm, model.capability,
                    vllm_supported, vllm_reason);
  lua_getfield(state, 1, "artifacts");
  if (lua_istable(state, -1)) {
    const auto count = lua_rawlen(state, -1);
    for (std::size_t i = 1; i <= count; ++i) {
      lua_rawgeti(state, -1, static_cast<lua_Integer>(i));
      if (!lua_istable(state, -1)) {
        lua_pop(state, 2);
        return luaL_error(state, "model artifact declaration must be a table");
      }
      const auto backend = parse_backend(string_field(state, -1, "backend"));
      const auto variant = parse_quantization(
          string_field(state, -1, "variant"));
      Artifact artifact;
      artifact.supported = bool_field(state, -1, "supported", true);
      artifact.engine = string_field(state, -1, "engine");
      artifact.format = string_field(state, -1, "format", default_format(backend));
      artifact.quantization_type = string_field(
          state, -1, "quantization_type", to_string(variant));
      artifact.pattern = string_field(state, -1, "path");
      artifact.repository_pattern = string_field(
          state, -1, "repository_path", artifact.pattern);
      artifact.projector_pattern = string_field(state, -1, "projector");
      artifact.projector_repository_pattern = string_field(
          state, -1, "projector_repository_path", artifact.projector_pattern);
      artifact.size_bytes = static_cast<std::uint64_t>(
          number_field(state, -1, "size_bytes", 0));
      artifact.projector_size_bytes = static_cast<std::uint64_t>(
          number_field(state, -1, "projector_size_bytes", 0));
      artifact.reservation_gib = number_field(
          state, -1, "reservation_gib", 0.0);
      artifact.size_source = string_field(
          state, -1, "size_source", "upstream-metadata");
      artifact.sha256 = string_field(state, -1, "sha256");
      artifact.projector_sha256 = string_field(state, -1, "projector_sha256");
      artifact.reason = string_field(state, -1, "reason");
      if (artifact.supported &&
          (artifact.engine.empty() || artifact.pattern.empty() ||
           artifact.reservation_gib <= 0)) {
        lua_pop(state, 2);
        return luaL_error(state, "supported model artifact is incomplete");
      }
      model.artifacts[backend][variant] = std::move(artifact);
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  registry->models.push_back(std::move(model));
  return 0;
}

int register_profile(lua_State* state) {
  auto* registry = registry_from_upvalue(state);
  luaL_checktype(state, 1, LUA_TTABLE);
  Profile profile;
  profile.name = string_field(state, 1, "name");
  profile.catalog_visible = bool_field(state, 1, "catalog_visible", true);
  profile.quantization =
      parse_quantization(string_field(state, 1, "quantization", "q4"));
  profile.models = string_array_field(state, 1, "models");
  const auto backend = string_field(state, 1, "backend");
  if (!backend.empty()) profile.backend = parse_backend(backend);
  profile.max_input_tokens =
      static_cast<int>(number_field(state, 1, "max_input_tokens", 7168));
  profile.max_output_tokens =
      static_cast<int>(number_field(state, 1, "max_output_tokens", 1024));
  profile.max_total_tokens =
      static_cast<int>(number_field(state, 1, "max_total_tokens", 8192));
  profile.max_concurrent_requests =
      static_cast<int>(number_field(state, 1, "max_concurrent_requests", 1));
  profile.kv_cache_precision =
      string_field(state, 1, "kv_cache_precision", "q8");
  if (profile.name.empty()) return luaL_error(state, "profile name is required");
  if (profile.models.empty()) return luaL_error(state, "profile models are required");
  if (profile.max_input_tokens < 1 || profile.max_output_tokens < 1 ||
      profile.max_total_tokens < profile.max_input_tokens + profile.max_output_tokens) {
    return luaL_error(state, "profile token limits are inconsistent");
  }
  if (profile.max_concurrent_requests < 1 || profile.max_concurrent_requests > 64) {
    return luaL_error(state, "profile concurrency must be between 1 and 64");
  }
  if (profile.kv_cache_precision != "q4" && profile.kv_cache_precision != "q8" &&
      profile.kv_cache_precision != "auto") {
    return luaL_error(state, "profile KV cache precision must be q4, q8, or auto");
  }
  registry->profiles[profile.name] = std::move(profile);
  return 0;
}

int register_policy(lua_State* state) {
  auto* registry = registry_from_upvalue(state);
  luaL_checktype(state, 1, LUA_TTABLE);
  registry->policy.idle_ttl_seconds =
      static_cast<int>(number_field(state, 1, "idle_ttl_seconds", 300));
  registry->policy.safety_margin = number_field(state, 1, "safety_margin", 1.15);
  registry->policy.min_system_available_ram_gib =
      static_cast<int>(number_field(state, 1, "min_system_available_ram_gib", 6));
  registry->policy.load_timeout_seconds =
      static_cast<int>(number_field(state, 1, "load_timeout_seconds", 180));
  registry->policy.queue_timeout_seconds =
      static_cast<int>(number_field(state, 1, "queue_timeout_seconds", 60));
  return 0;
}

int register_vlm_tool(lua_State* state) {
  auto* registry = registry_from_upvalue(state);
  luaL_checktype(state, 1, LUA_TTABLE);
  auto& tool = registry->vlm_tool;
  tool.enabled = bool_field(state, 1, "enabled", true);
  tool.name = string_field(state, 1, "name", "vlm_tool");
  tool.description = string_field(state, 1, "description");
  tool.model_id = string_field(state, 1, "model_id", "minicpm-v46-thinking");
  tool.max_images_per_call =
      static_cast<int>(number_field(state, 1, "max_images_per_call", 8));
  tool.max_videos_per_call =
      static_cast<int>(number_field(state, 1, "max_videos_per_call", 1));
  tool.max_document_pages_per_call =
      static_cast<int>(number_field(state, 1, "max_document_pages_per_call", 8));
  tool.max_video_frames =
      static_cast<int>(number_field(state, 1, "max_video_frames", 32));
  tool.max_total_visual_items =
      static_cast<int>(number_field(state, 1, "max_total_visual_items", 8));
  tool.max_agent_steps =
      static_cast<int>(number_field(state, 1, "max_agent_steps", 4));
  tool.max_upload_bytes = static_cast<std::uint64_t>(
      number_field(state, 1, "max_upload_bytes", 50.0 * 1024.0 * 1024.0));
  if (tool.name.empty() || tool.model_id.empty()) {
    return luaL_error(state, "VLM tool name and model_id are required");
  }
  if (tool.max_images_per_call < 1 || tool.max_videos_per_call < 1 ||
      tool.max_document_pages_per_call < 1 || tool.max_video_frames < 1 ||
      tool.max_total_visual_items < 1 || tool.max_agent_steps < 1 ||
      tool.max_agent_steps > 16 || tool.max_upload_bytes < 1024) {
    return luaL_error(state, "VLM tool limits are invalid");
  }
  return 0;
}

void register_function(lua_State* state, Registry* registry, const char* name,
                       lua_CFunction function) {
  lua_pushlightuserdata(state, registry);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

void run_file(lua_State* state, const std::filesystem::path& path) {
  if (luaL_dofile(state, path.c_str()) != LUA_OK) {
    const std::string message = lua_tostring(state, -1);
    lua_pop(state, 1);
    throw std::runtime_error("Lua config failed: " + message);
  }
}

void load_engine_manifests(Registry& registry,
                           const std::filesystem::path& directory) {
  for (const auto& path : yaml_files(directory)) {
    const auto document = read_profile_file(path);
    if (!document.is_object() || document.value("schema", 0) != 1) {
      throw std::runtime_error("engine manifest must use schema 1: " + path.string());
    }
    reject_unknown_fields(document,
        {"schema", "id", "status", "backend", "installer", "launcher",
         "device_target", "hardware", "artifact_formats", "features",
         "source", "install"}, "engine manifest " + path.string());
    const auto id = document.at("id").get<std::string>();
    if (id.empty() || id.size() > 128 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '-' || c == '_';
        })) {
      throw std::runtime_error("invalid engine manifest id: " + id);
    }
    EngineDefinition engine;
    engine.id = id;
    engine.status = document.value("status", engine.status);
    engine.backend = parse_backend(document.at("backend").get<std::string>());
    engine.installer = document.at("installer").get<std::string>();
    engine.launcher = document.at("launcher").get<std::string>();
    const bool known_adapter =
        (engine.installer == "cmake-llama" && engine.launcher == "llama-server") ||
        (engine.installer == "cmake-audio" && engine.launcher == "audio-server") ||
        (engine.installer == "python-mlx" &&
         (engine.launcher == "mlx-lm" || engine.launcher == "mlx-vlm" ||
          engine.launcher == "mlx-audio")) ||
        (engine.installer == "python-vllm" && engine.launcher == "vllm");
    if (!known_adapter) {
      throw std::runtime_error("unsupported engine installer/launcher adapter: " + id);
    }
    engine.device_target = document.at("device_target").get<std::string>();
    engine.hardware = document.at("hardware").get<std::vector<std::string>>();
    engine.artifact_formats =
        document.at("artifact_formats").get<std::vector<std::string>>();
    engine.features = document.value("features", std::vector<std::string>{});
    if (document.contains("source")) {
      const auto& source = document.at("source");
      reject_unknown_fields(source, {"url", "revision"}, "engine source");
      engine.source_url = source.at("url").get<std::string>();
      engine.revision = source.at("revision").get<std::string>();
      if (!engine.source_url.starts_with("https://github.com/") ||
          engine.source_url.find_first_of(" \t\r\n?#") != std::string::npos ||
          (!pinned_revision(engine.revision) && engine.revision != "latest")) {
        throw std::runtime_error("engine revision is not pinned: " + id);
      }
    }
    if (document.contains("install")) {
      const auto& install = document.at("install");
      reject_unknown_fields(install,
          {"runtime_directory", "server_executable", "quantizer_executable",
           "build_targets", "version_arguments", "environment_group",
           "python_packages", "cmake_definitions"}, "engine install");
      engine.runtime_directory = install.value("runtime_directory", std::string());
      engine.server_executable = install.value("server_executable", std::string());
      engine.quantizer_executable =
          install.value("quantizer_executable", std::string());
      engine.build_targets =
          install.value("build_targets", std::vector<std::string>{});
      engine.version_arguments =
          install.value("version_arguments", std::vector<std::string>{});
      engine.environment_group =
          install.value("environment_group", std::string());
      engine.python_packages =
          install.value("python_packages", std::vector<std::string>{});
      engine.cmake_definitions.clear();
      if (install.contains("cmake_definitions")) {
        for (auto variant = install.at("cmake_definitions").begin();
             variant != install.at("cmake_definitions").end(); ++variant) {
          if (!variant.value().is_object()) {
            throw std::runtime_error("engine CMake definitions must be maps: " + id);
          }
          engine.cmake_definitions.try_emplace(variant.key());
          for (auto setting = variant.value().begin();
               setting != variant.value().end(); ++setting) {
            const auto key = setting.key();
            if (key.empty() || !std::all_of(key.begin(), key.end(),
                [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
              throw std::runtime_error("invalid CMake definition key in " + id);
            }
            if (!setting.value().is_string()) {
              throw std::runtime_error("CMake definition must be a string in " + id);
            }
            engine.cmake_definitions[variant.key()][key] =
                setting.value().get<std::string>();
          }
        }
      }
    }
    if (engine.installer.empty() || engine.launcher.empty() ||
        engine.device_target.empty() || engine.hardware.empty() ||
        engine.artifact_formats.empty()) {
      throw std::runtime_error("incomplete engine manifest: " + id);
    }
    if ((engine.installer == "cmake-llama" || engine.installer == "cmake-audio") &&
        (engine.source_url.empty() || engine.runtime_directory.empty() ||
         !safe_relative_path(engine.runtime_directory) ||
         engine.server_executable.empty() ||
         !safe_relative_path(engine.server_executable) ||
         engine.build_targets.empty())) {
      throw std::runtime_error("incomplete native engine manifest: " + id);
    }
    registry.engines[id] = std::move(engine);
  }
}

void load_model_manifests(Registry& registry,
                          const std::filesystem::path& directory) {
  for (const auto& path : yaml_files(directory)) {
    const auto document = read_profile_file(path);
    if (!document.is_object() || document.value("schema", 0) != 1) {
      throw std::runtime_error("model manifest must use schema 1: " + path.string());
    }
    reject_unknown_fields(
        document,
        {"schema", "id", "capability", "description", "source_repository",
         "declared_context_tokens", "tags", "thinking_modes",
         "thinking_budget_supported", "license", "artifacts", "mlx_converter",
         "mlx_quantization_profile", "gguf_family", "gguf_parallel_slots",
         "startup_priority", "required", "required_by_backend", "catalog_visible",
         "input_modalities", "output_modalities", "tool_call_formats",
         "trained_context_tokens", "useful_context_tokens",
         "supported_output_tokens", "trained_output_tokens"},
        "model manifest " + path.string());
    const auto id = document.at("id").get<std::string>();
    if (id.empty() || id.size() > 128 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '-' || c == '_' || c == '.';
        })) {
      throw std::runtime_error("invalid model manifest id: " + id);
    }
    auto model = std::find_if(registry.models.begin(), registry.models.end(),
        [&](const auto& item) { return item.id == id; });
    if (model == registry.models.end()) {
      ModelDefinition fresh;
      fresh.id = id;
      fresh.capability = document.at("capability").get<std::string>();
      fresh.description = document.at("description").get<std::string>();
      fresh.source_repo = document.at("source_repository").get<std::string>();
      fresh.gguf_context_tokens = document.value("declared_context_tokens", 0);
      fresh.tags = document.value("tags", std::vector<std::string>{});
      if (!safe_repository(fresh.source_repo) || fresh.description.empty() ||
          ((fresh.capability == "text" || fresh.capability == "vision") &&
           fresh.gguf_context_tokens < 512) ||
          (fresh.capability != "text" && fresh.capability != "vision" &&
           fresh.capability != "asr" && fresh.capability != "tts")) {
        throw std::runtime_error("incomplete logical model manifest: " + id);
      }
      registry.models.push_back(std::move(fresh));
      model = std::prev(registry.models.end());
    }
    if (document.at("capability").get<std::string>() != model->capability) {
      throw std::runtime_error("model capability mismatch: " + id);
    }
    model->description = document.at("description").get<std::string>();
    model->source_repo = document.at("source_repository").get<std::string>();
    model->gguf_context_tokens = document.value("declared_context_tokens", 0);
    model->tags = document.value("tags", std::vector<std::string>{});
    model->catalog_visible = document.value("catalog_visible", true);
    model->mlx_converter = document.value("mlx_converter", std::string());
    model->mlx_quantization_profile =
        document.value("mlx_quantization_profile", std::string());
    model->gguf_family = document.value("gguf_family", std::string());
    model->gguf_parallel_slots = document.value("gguf_parallel_slots", 1);
    model->input_modalities =
        document.value("input_modalities", std::vector<std::string>{});
    model->output_modalities =
        document.value("output_modalities", std::vector<std::string>{});
    model->tool_call_formats =
        document.value("tool_call_formats", std::vector<std::string>{});
    model->trained_context_tokens = token_claim(document, "trained_context_tokens");
    model->useful_context_tokens = token_claim(document, "useful_context_tokens");
    model->supported_output_tokens = token_claim(document, "supported_output_tokens");
    model->trained_output_tokens = token_claim(document, "trained_output_tokens");
    model->startup_priority = document.value("startup_priority", 100);
    model->required = document.value("required", false);
    model->required_by_backend.clear();
    if (document.contains("required_by_backend")) {
      for (auto item = document.at("required_by_backend").begin();
           item != document.at("required_by_backend").end(); ++item) {
        model->required_by_backend[parse_backend(item.key())] = item.value().get<bool>();
      }
    }
    if (!safe_repository(model->source_repo) || model->description.empty() ||
        ((model->capability == "text" || model->capability == "vision") &&
         model->gguf_context_tokens < 512) ||
        model->gguf_parallel_slots < 1 || model->gguf_parallel_slots > 16 ||
        model->startup_priority < 0) {
      throw std::runtime_error("invalid logical model manifest: " + id);
    }
    const std::set<std::string> known_modalities = {"text", "image", "video", "audio"};
    for (const auto& modalities : {model->input_modalities, model->output_modalities}) {
      if (modalities.empty()) {
        throw std::runtime_error("model manifest lacks input/output modalities: " + id);
      }
      std::set<std::string> unique;
      for (const auto& modality : modalities) {
        if (!known_modalities.contains(modality) || !unique.insert(modality).second) {
          throw std::runtime_error("invalid or duplicate model modality: " + id);
        }
      }
    }
    if (model->supported_output_tokens &&
        model->supported_output_tokens->tokens > model->gguf_context_tokens &&
        (model->capability == "text" || model->capability == "vision")) {
      throw std::runtime_error("supported output exceeds context for " + id);
    }
    model->artifacts.clear();
    model->repositories.clear();
    model->repository_revisions.clear();
    model->thinking_modes = document.value("thinking_modes", std::vector<std::string>{});
    model->thinking_budget_supported = document.value("thinking_budget_supported", false);
    const std::set<std::string> valid_thinking_modes = {
        "none", "on", "low", "medium", "xhigh"};
    std::set<std::string> unique_thinking_modes;
    for (const auto& mode : model->thinking_modes) {
      if (!valid_thinking_modes.contains(mode) ||
          !unique_thinking_modes.insert(mode).second) {
        throw std::runtime_error("invalid thinking mode in model manifest: " + id);
      }
    }
    if (model->thinking_budget_supported && model->thinking_modes.empty()) {
      throw std::runtime_error("thinking budget requires modes in model manifest: " + id);
    }
    static const std::set<std::string> allowed_licenses = {
        "apache-2.0", "mit", "bsd-2-clause", "bsd-3-clause", "isc"};
    const auto license = document.at("license").get<std::string>();
    if (!allowed_licenses.contains(license)) {
      throw std::runtime_error("model manifest requires an accepted commercial license: " + id);
    }
    for (const auto& tag : model->tags) {
      if (tag.starts_with("license:") && tag.substr(8) != license) {
        throw std::runtime_error("model YAML license conflicts with registered model: " + id);
      }
    }
    if (std::find(model->tags.begin(), model->tags.end(), "license:" + license) ==
        model->tags.end()) {
      model->tags.push_back("license:" + license);
    }
    for (const auto& variant : document.at("artifacts")) {
      reject_unknown_fields(variant,
          {"id", "backend", "engine", "format", "quantization_type",
           "repository", "revision", "reservation_gib", "required_features",
           "minimum_engine_commit", "files"}, "model artifact");
      const auto backend = parse_backend(variant.at("backend").get<std::string>());
      const auto quantization =
          parse_quantization(variant.at("id").get<std::string>());
      const auto engine_id = variant.at("engine").get<std::string>();
      const auto& engine = registry.engine(engine_id);
      const auto format = variant.at("format").get<std::string>();
      if (engine.backend != backend ||
          std::find(engine.artifact_formats.begin(), engine.artifact_formats.end(),
                    format) == engine.artifact_formats.end()) {
        throw std::runtime_error("artifact/engine format mismatch: " + id);
      }
      const auto repository = variant.at("repository").get<std::string>();
      const auto revision = variant.at("revision").get<std::string>();
      if (!safe_repository(repository) || !pinned_revision(revision)) {
        throw std::runtime_error("artifact revision must be an immutable SHA: " + id);
      }
      Artifact artifact;
      artifact.supported = true;
      artifact.engine = engine_id;
      artifact.format = format;
      artifact.quantization_type = variant.at("quantization_type").get<std::string>();
      artifact.reservation_gib = variant.at("reservation_gib").get<double>();
      artifact.size_source = "pinned-model-manifest";
      artifact.required_features =
          variant.value("required_features", std::vector<std::string>{});
      artifact.minimum_engine_commit =
          variant.value("minimum_engine_commit", std::string());
      if (!artifact.minimum_engine_commit.empty() &&
          (!pinned_revision(artifact.minimum_engine_commit) ||
           engine.runtime_directory.empty())) {
        throw std::runtime_error("invalid minimum engine commit for " + id);
      }
      for (const auto& feature : artifact.required_features) {
        if (std::find(engine.features.begin(), engine.features.end(), feature) ==
            engine.features.end()) {
          throw std::runtime_error("engine lacks artifact feature " + feature +
                                   " for " + id);
        }
      }
      std::set<std::string> roles;
      for (const auto& entry : variant.at("files")) {
        reject_unknown_fields(entry,
            {"role", "path", "repository_path", "source", "size_bytes", "sha256"},
            "model artifact file");
        Artifact::File file;
        file.role = entry.at("role").get<std::string>();
        file.path = entry.at("path").get<std::string>();
        file.repository_path = entry.value("repository_path", file.path);
        file.repository = repository;
        file.revision = revision;
        if (entry.contains("source")) {
          const auto& source = entry.at("source");
          reject_unknown_fields(source, {"repository", "revision", "license"},
                                "artifact component source");
          if (!allowed_licenses.contains(source.at("license").get<std::string>())) {
            throw std::runtime_error("artifact component license is not allowed: " + id);
          }
          file.repository = source.at("repository").get<std::string>();
          file.revision = source.at("revision").get<std::string>();
        }
        file.size_bytes = entry.at("size_bytes").get<std::uint64_t>();
        file.sha256 = entry.value("sha256", std::string());
        static const std::set<std::string> allowed_roles = {
            "model", "vision-projector", "mtp-drafter", "dflash-drafter",
            "tokenizer", "processor", "codec", "adapter"};
        if (!allowed_roles.contains(file.role) ||
            !roles.insert(file.role).second || !safe_relative_path(file.path) ||
            !safe_relative_path(file.repository_path) ||
            !safe_repository(file.repository) || !pinned_revision(file.revision) ||
            !valid_sha256(file.sha256)) {
          throw std::runtime_error("invalid artifact file in " + id + ": " + file.role);
        }
        if (file.role == "model") {
          artifact.pattern = file.path;
          artifact.repository_pattern = file.repository_path;
          artifact.size_bytes = file.size_bytes;
          artifact.sha256 = file.sha256;
        } else if (file.role == "vision-projector") {
          artifact.projector_pattern = file.path;
          artifact.projector_repository_pattern = file.repository_path;
          artifact.projector_size_bytes = file.size_bytes;
          artifact.projector_sha256 = file.sha256;
        }
        artifact.files.push_back(std::move(file));
      }
      if (artifact.pattern.empty() || artifact.reservation_gib <= 0 ||
          (model->capability == "vision" && backend == Backend::gguf &&
           artifact.projector_pattern.empty())) {
        throw std::runtime_error("incomplete model artifact: " + id);
      }
      model->repositories[backend] = repository;
      model->repository_revisions[backend] = revision;
      model->artifacts[backend][quantization] = std::move(artifact);
    }
  }
}

}  // namespace

std::string to_string(Backend value) {
  switch (value) {
    case Backend::mlx: return "mlx";
    case Backend::gguf: return "gguf";
    case Backend::vllm: return "vllm";
  }
  throw std::invalid_argument("invalid backend");
}

std::string to_string(Quantization value) {
  return value.id;
}

Backend parse_backend(const std::string& value) {
  if (value == "mlx") return Backend::mlx;
  if (value == "gguf") return Backend::gguf;
  if (value == "vllm") return Backend::vllm;
  throw std::invalid_argument("backend must be mlx, gguf, or vllm");
}

std::string to_string(VllmDevice value) {
  switch (value) {
    case VllmDevice::automatic: return "auto";
    case VllmDevice::cpu: return "cpu";
    case VllmDevice::cuda: return "cuda";
    case VllmDevice::metal: return "metal";
    case VllmDevice::rocm: return "rocm";
    case VllmDevice::xpu: return "xpu";
    case VllmDevice::tpu: return "tpu";
  }
  throw std::invalid_argument("invalid vLLM device");
}

VllmDevice parse_vllm_device(const std::string& value) {
  if (value == "auto") return VllmDevice::automatic;
  if (value == "cpu") return VllmDevice::cpu;
  if (value == "cuda") return VllmDevice::cuda;
  if (value == "metal") return VllmDevice::metal;
  if (value == "rocm") return VllmDevice::rocm;
  if (value == "xpu") return VllmDevice::xpu;
  if (value == "tpu") return VllmDevice::tpu;
  throw std::invalid_argument(
      "vLLM device must be auto, cpu, cuda, metal, rocm, xpu, or tpu");
}

std::string to_string(Residency value) {
  switch (value) {
    case Residency::pinned: return "pinned";
    case Residency::warm: return "warm";
    case Residency::on_demand: return "on-demand";
    case Residency::ephemeral: return "ephemeral";
  }
  throw std::invalid_argument("invalid residency");
}

Residency parse_residency(const std::string& value) {
  if (value == "pinned") return Residency::pinned;
  if (value == "warm") return Residency::warm;
  if (value == "on-demand") return Residency::on_demand;
  if (value == "ephemeral") return Residency::ephemeral;
  throw std::invalid_argument(
      "residency must be pinned, warm, on-demand, or ephemeral");
}

Quantization parse_quantization(const std::string& value) {
  if (value.empty() || value.size() > 64 ||
      !std::isalnum(static_cast<unsigned char>(value.front())) ||
      !std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::islower(character) || std::isdigit(character) ||
               character == '_' || character == '-';
      })) {
    throw std::invalid_argument(
        "artifact variant must be a lowercase identifier using letters, numbers, _ or -");
  }
  return Quantization(value);
}

const Quantization Quantization::q4{"q4"};
const Quantization Quantization::q8{"q8"};
const Quantization Quantization::native{"native"};

const ModelDefinition& Registry::model(const std::string& id) const {
  const auto found = std::find_if(models.begin(), models.end(),
                                  [&](const auto& item) { return item.id == id; });
  if (found == models.end()) throw std::out_of_range("unknown model: " + id);
  return *found;
}

const EngineDefinition& Registry::engine(const std::string& id) const {
  const auto found = engines.find(id);
  if (found == engines.end()) throw std::out_of_range("unknown engine: " + id);
  return found->second;
}

const Profile& Registry::profile(const std::string& name) const {
  const auto found = profiles.find(name);
  if (found == profiles.end()) throw std::out_of_range("unknown profile: " + name);
  return found->second;
}

Registry load_registry(const std::filesystem::path& config_directory) {
  Registry registry;
  load_engine_manifests(registry, config_directory / "engines");
  lua_State* state = luaL_newstate();
  if (!state) throw std::runtime_error("unable to create Lua state");
  luaL_openlibs(state);
  lua_newtable(state);
  register_function(state, &registry, "settings", register_settings);
  register_function(state, &registry, "model", register_model);
  register_function(state, &registry, "profile", register_profile);
  register_function(state, &registry, "policy", register_policy);
  register_function(state, &registry, "vlm_tool", register_vlm_tool);
  lua_setglobal(state, "mica");
  try {
    run_file(state, config_directory / "models.lua");
    load_model_manifests(registry, config_directory / "model-manifests");
    run_file(state, config_directory / "policy.lua");
    const auto tools = config_directory / "tools.lua";
    if (std::filesystem::exists(tools)) run_file(state, tools);
  } catch (...) {
    lua_close(state);
    throw;
  }
  lua_close(state);
  for (const auto& path : yaml_files(config_directory / "tasks")) {
    merge_profile_file(registry, path);
  }
  if (registry.models.empty()) throw std::runtime_error("registry has no models");
  if (registry.profiles.empty()) throw std::runtime_error("registry has no profiles");
  return registry;
}

}  // namespace mica
