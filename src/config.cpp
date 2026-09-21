#include "mica_server/config.hpp"

#include <algorithm>
#include <stdexcept>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace mica {
namespace {

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
  registry->llama_cpp_revision = string_field(state, 1, "llama_cpp_revision");
  registry->audio_cpp_revision = string_field(state, 1, "audio_cpp_revision");
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

const EngineDefinition& engine_for_profile(const Registry& registry,
                                           const std::string& engine) {
  const auto found = registry.engines.find(engine);
  if (found == registry.engines.end()) {
    throw std::invalid_argument("unsupported profile engine: " + engine);
  }
  if (found->second.status != "current" && found->second.status != "candidate") {
    throw std::invalid_argument("profile engine is not runnable: " + engine);
  }
  return found->second;
}

void validate_profile_model(const Registry& registry, const ProfileModel& policy,
                            const std::string& profile_name) {
  const auto& model = registry.model(policy.id);
  const auto backend = model.artifacts.find(policy.backend);
  if (backend == model.artifacts.end()) {
    throw std::runtime_error("profile " + profile_name + " selects an undeclared backend for " +
                             policy.id);
  }
  const auto artifact = backend->second.find(policy.quantization);
  if (artifact == backend->second.end() || !artifact->second.supported) {
    const auto reason = artifact == backend->second.end()
                            ? std::string("quantization is not declared")
                            : artifact->second.reason;
    throw std::runtime_error("profile " + profile_name + " cannot use " + policy.id +
                             "@" + to_string(policy.backend) + ":" +
                             to_string(policy.quantization) + ": " + reason);
  }
  if (artifact->second.engine != policy.engine) {
    throw std::runtime_error("profile " + profile_name + " selects engine " +
                             policy.engine + " but artifact " + policy.id + "@" +
                             to_string(policy.quantization) + " requires " +
                             artifact->second.engine);
  }
  if (policy.max_input_tokens < 1 || policy.max_output_tokens < 1 ||
      policy.max_total_tokens < policy.max_input_tokens + policy.max_output_tokens) {
    throw std::runtime_error("profile " + profile_name + " has inconsistent token limits for " +
                             policy.id);
  }
  if ((model.capability == "text" || model.capability == "vision") &&
      policy.max_total_tokens > model.gguf_context_tokens) {
    throw std::runtime_error("profile " + profile_name + " exceeds the declared context for " +
                             policy.id);
  }
  if (policy.max_concurrent_requests < 1 || policy.max_concurrent_requests > 64) {
    throw std::runtime_error("profile " + profile_name + " has invalid concurrency for " +
                             policy.id);
  }
  if (policy.kv_cache_precision != "q4" && policy.kv_cache_precision != "q8" &&
      policy.kv_cache_precision != "auto" &&
      policy.kv_cache_precision != "runtime-managed" &&
      policy.kv_cache_precision != "not-applicable") {
    throw std::runtime_error("profile " + profile_name + " has invalid KV cache precision for " +
                             policy.id);
  }
  if (policy.placement_mode != "auto" && policy.placement_mode != "fixed") {
    throw std::runtime_error("profile " + profile_name +
                             " has invalid placement mode for " + policy.id);
  }
  if (policy.placement_mode == "fixed" && policy.device == "auto") {
    throw std::runtime_error("profile " + profile_name +
                             " has fixed placement without a device for " + policy.id);
  }
  if (policy.device != "auto" && policy.device != "cpu" &&
      !policy.device.starts_with("accelerator:") &&
      !policy.device.starts_with("cuda:") &&
      !policy.device.starts_with("rocm:") &&
      !policy.device.starts_with("xpu:") &&
      !policy.device.starts_with("metal:")) {
    throw std::runtime_error("profile " + profile_name +
                             " has an unsupported placement device for " + policy.id);
  }
  if (policy.gpu_layers < -1 || policy.gpu_layers > 999 ||
      policy.ram_reservation_gib < -1 || policy.vram_reservation_gib < -1 ||
      (policy.device == "cpu" && policy.gpu_layers > 0)) {
    throw std::runtime_error("profile " + profile_name +
                             " has invalid placement resources for " + policy.id);
  }
}

void load_profiles_v3(lua_State* state, Registry& registry,
                      const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return;
  if (luaL_loadfile(state, path.c_str()) != LUA_OK || lua_pcall(state, 0, 1, 0) != LUA_OK) {
    const std::string message = lua_tostring(state, -1);
    lua_pop(state, 1);
    throw std::runtime_error("Lua profile config failed: " + message);
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    throw std::runtime_error("profiles.lua must return a table");
  }
  const int root = lua_absindex(state, -1);
  const auto schema = static_cast<int>(number_field(state, root, "schema", 0));
  if (schema != 3) {
    lua_pop(state, 1);
    throw std::runtime_error("profiles.lua must use schema 3");
  }
  lua_getfield(state, root, "engines");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 2);
    throw std::runtime_error("profiles.lua is missing engines");
  }
  const int engines = lua_absindex(state, -1);
  lua_pushnil(state);
  while (lua_next(state, engines) != 0) {
    const auto id = std::string(luaL_checkstring(state, -2));
    if (!lua_istable(state, -1)) {
      lua_pop(state, 3);
      throw std::runtime_error("engine descriptor must be a table: " + id);
    }
    EngineDefinition engine;
    engine.id = id;
    engine.status = string_field(state, -1, "status", "current");
    const auto backend = string_field(state, -1, "backend");
    if (engine.status == "current" || engine.status == "candidate") {
      engine.backend = parse_backend(backend);
    }
    engine.installer = string_field(state, -1, "installer");
    engine.launcher = string_field(state, -1, "launcher");
    engine.device_target = string_field(state, -1, "device_target");
    engine.source_url = string_field(state, -1, "source_url");
    engine.revision = string_field(state, -1, "revision");
    engine.runtime_directory = string_field(state, -1, "runtime_directory");
    engine.server_executable = string_field(state, -1, "server_executable");
    engine.quantizer_executable = string_field(state, -1, "quantizer_executable");
    engine.build_targets = string_array_field(state, -1, "build_targets");
    engine.version_arguments = string_array_field(state, -1, "version_arguments");
    engine.hardware = string_array_field(state, -1, "hardware");
    engine.artifact_formats = string_array_field(state, -1, "artifact_formats");
    const bool runnable = engine.status == "current" || engine.status == "candidate";
    if (runnable &&
        (engine.installer.empty() || engine.launcher.empty() ||
         engine.device_target.empty() || engine.artifact_formats.empty())) {
      lua_pop(state, 3);
      throw std::runtime_error("incomplete engine descriptor: " + id);
    }
    if (runnable && engine.installer == "cmake-llama" &&
        (engine.source_url.empty() || engine.revision.empty() ||
         engine.runtime_directory.empty() || engine.server_executable.empty() ||
         engine.build_targets.empty())) {
      lua_pop(state, 3);
      throw std::runtime_error("incomplete native llama engine descriptor: " + id);
    }
    registry.engines[id] = std::move(engine);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);  // engines
  lua_getfield(state, root, "execution_profiles");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 2);
    throw std::runtime_error("profiles.lua is missing execution_profiles");
  }
  const int executions = lua_absindex(state, -1);
  lua_getfield(state, root, "residency_profiles");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 3);
    throw std::runtime_error("profiles.lua is missing residency_profiles");
  }
  const int profiles = lua_absindex(state, -1);

  lua_pushnil(state);
  while (lua_next(state, profiles) != 0) {
    const auto profile_name = std::string(luaL_checkstring(state, -2));
    if (!lua_istable(state, -1)) {
      lua_pop(state, 4);
      throw std::runtime_error("residency profile must be a table: " + profile_name);
    }
    const int profile_table = lua_absindex(state, -1);
    Profile profile;
    profile.name = profile_name;
    profile.schema = 3;
    profile.mode = string_field(state, profile_table, "mode", "interactive");
    profile.catalog_visible = bool_field(
        state, profile_table, "catalog_visible", profile.mode != "validation");
    profile.maximum_ram_gib = number_field(state, profile_table, "maximum_ram_gib", 0.0);
    profile.maximum_vram_gib = number_field(state, profile_table, "maximum_vram_gib", 0.0);
    profile.memory_safety_reserve_gib =
        number_field(state, profile_table, "memory_safety_reserve_gib", 0.0);
    profile.maximum_resident_workers = static_cast<int>(
        number_field(state, profile_table, "maximum_resident_workers", 0));
    if (profile.maximum_ram_gib < 0 || profile.maximum_vram_gib < 0 ||
        profile.memory_safety_reserve_gib < 0 || profile.maximum_resident_workers < 0) {
      lua_pop(state, 4);
      throw std::runtime_error("profile memory limits cannot be negative: " + profile_name);
    }

    lua_getfield(state, profile_table, "models");
    if (!lua_istable(state, -1) || lua_rawlen(state, -1) == 0) {
      lua_pop(state, 5);
      throw std::runtime_error("profile has no models: " + profile_name);
    }
    const int models = lua_absindex(state, -1);
    const auto model_count = lua_rawlen(state, models);
    for (std::size_t i = 1; i <= model_count; ++i) {
      lua_rawgeti(state, models, static_cast<lua_Integer>(i));
      if (!lua_istable(state, -1)) {
        lua_pop(state, 6);
        throw std::runtime_error("profile model entry must be a table: " + profile_name);
      }
      const int entry = lua_absindex(state, -1);
      ProfileModel policy;
      policy.id = string_field(state, entry, "id");
      policy.execution = string_field(state, entry, "execution");
      policy.residency = parse_residency(
          string_field(state, entry, "residency", "on-demand"));
      policy.priority = static_cast<int>(number_field(state, entry, "priority", 50));
      policy.startup = bool_field(state, entry, "startup", false);
      policy.idle_seconds = static_cast<int>(number_field(
          state, entry, "idle_seconds",
          policy.residency == Residency::pinned ? 0 :
          policy.residency == Residency::ephemeral ? 0 : 300));
      lua_getfield(state, entry, "placement");
      if (lua_istable(state, -1)) {
        policy.placement_mode = string_field(state, -1, "mode", "auto");
        policy.device = string_field(state, -1, "device", "auto");
        policy.gpu_layers = static_cast<int>(
            number_field(state, -1, "gpu_layers", -1));
        policy.ram_reservation_gib =
            number_field(state, -1, "ram_reservation_gib", -1.0);
        policy.vram_reservation_gib =
            number_field(state, -1, "vram_reservation_gib", -1.0);
      }
      lua_pop(state, 1);
      if (policy.id.empty() || policy.execution.empty()) {
        lua_pop(state, 6);
        throw std::runtime_error("profile model requires id and execution: " + profile_name);
      }
      if (std::find(profile.models.begin(), profile.models.end(), policy.id) !=
          profile.models.end()) {
        lua_pop(state, 6);
        throw std::runtime_error("profile contains a duplicate model: " + policy.id);
      }

      lua_getfield(state, executions, policy.execution.c_str());
      if (!lua_istable(state, -1)) {
        lua_pop(state, 7);
        throw std::runtime_error("unknown execution profile " + policy.execution +
                                 " in " + profile_name);
      }
      const int execution = lua_absindex(state, -1);
      const auto execution_model = string_field(state, execution, "model");
      if (execution_model != policy.id) {
        lua_pop(state, 7);
        throw std::runtime_error("execution profile model mismatch for " + policy.id);
      }
      policy.engine = string_field(state, execution, "engine");
      const auto& engine = engine_for_profile(registry, policy.engine);
      policy.backend = engine.backend;
      policy.device_target = engine.device_target;

      lua_getfield(state, execution, "artifact");
      if (!lua_istable(state, -1)) {
        lua_pop(state, 8);
        throw std::runtime_error("execution profile has no artifact: " + policy.execution);
      }
      policy.quantization = parse_quantization(
          string_field(state, -1, "quantization", "q4"));
      lua_pop(state, 1);

      lua_getfield(state, execution, "context");
      if (lua_istable(state, -1)) {
        policy.max_input_tokens = static_cast<int>(
            number_field(state, -1, "max_input_tokens", policy.max_input_tokens));
        policy.max_output_tokens = static_cast<int>(
            number_field(state, -1, "max_output_tokens", policy.max_output_tokens));
        policy.max_total_tokens = static_cast<int>(
            number_field(state, -1, "max_total_tokens", policy.max_total_tokens));
      } else {
        policy.max_input_tokens = 1;
        policy.max_output_tokens = 1;
        policy.max_total_tokens = 2;
      }
      lua_pop(state, 1);

      lua_getfield(state, execution, "batching");
      if (lua_istable(state, -1)) {
        policy.max_concurrent_requests = static_cast<int>(number_field(
            state, -1, "max_concurrent_requests", policy.max_concurrent_requests));
      }
      lua_pop(state, 1);

      lua_getfield(state, execution, "kv_cache");
      if (lua_istable(state, -1)) {
        policy.kv_cache_precision =
            string_field(state, -1, "precision", policy.kv_cache_precision);
      }
      lua_pop(state, 1);
      lua_pop(state, 1);  // execution profile

      if (policy.priority < 0 || policy.priority > 1000 || policy.idle_seconds < 0) {
        lua_pop(state, 6);
        throw std::runtime_error("invalid residency policy for " + policy.id);
      }
      validate_profile_model(registry, policy, profile_name);
      profile.models.push_back(policy.id);
      profile.model_policies.push_back(std::move(policy));
      lua_pop(state, 1);  // model entry
    }
    lua_pop(state, 1);  // models
    const auto& first = profile.model_policies.front();
    profile.quantization = first.quantization;
    profile.max_input_tokens = first.max_input_tokens;
    profile.max_output_tokens = first.max_output_tokens;
    profile.max_total_tokens = first.max_total_tokens;
    profile.max_concurrent_requests = first.max_concurrent_requests;
    profile.kv_cache_precision = first.kv_cache_precision;
    const auto same_backend = std::all_of(
        profile.model_policies.begin(), profile.model_policies.end(),
        [&](const auto& item) { return item.backend == first.backend; });
    if (same_backend) profile.backend = first.backend;
    registry.profiles[profile.name] = std::move(profile);
    lua_pop(state, 1);  // residency profile value; keep key for lua_next
  }
  lua_pop(state, 3);  // residency profiles, execution profiles, root
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
    run_file(state, config_directory / "policy.lua");
    const auto tools = config_directory / "tools.lua";
    if (std::filesystem::exists(tools)) run_file(state, tools);
    load_profiles_v3(state, registry, config_directory / "profiles.lua");
  } catch (...) {
    lua_close(state);
    throw;
  }
  lua_close(state);
  if (registry.models.empty()) throw std::runtime_error("registry has no models");
  if (registry.profiles.empty()) throw std::runtime_error("registry has no profiles");
  return registry;
}

}  // namespace mica
