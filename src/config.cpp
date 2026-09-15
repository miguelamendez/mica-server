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

Artifact artifact_from(lua_State* state, int index, const char* prefix,
                       bool backend_supported, const std::string& reason) {
  const std::string base(prefix);
  Artifact artifact;
  artifact.pattern = string_field(state, index, (base + "_path").c_str());
  artifact.projector_pattern =
      string_field(state, index, (base + "_projector").c_str());
  artifact.reservation_gib =
      number_field(state, index, (base + "_ram_gib").c_str(), 0.0);
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
  model.tags = string_array_field(state, 1, "tags");
  model.source_repo = string_field(state, 1, "source_repo");
  model.mlx_converter = string_field(state, 1, "mlx_converter");
  model.mlx_quantization_profile =
      string_field(state, 1, "mlx_quantization_profile");
  model.mlx_extract_mtp = bool_field(state, 1, "mlx_extract_mtp", false);
  model.repositories[Backend::mlx] = string_field(state, 1, "mlx_repo");
  model.repositories[Backend::gguf] = string_field(state, 1, "gguf_repo");
  model.repositories[Backend::vllm] = string_field(state, 1, "vllm_repo");
  model.startup_priority = static_cast<int>(number_field(state, 1, "priority", 100));
  model.required = bool_field(state, 1, "required", false);
  model.required_by_backend[Backend::mlx] =
      bool_field(state, 1, "mlx_required", model.required);
  model.required_by_backend[Backend::gguf] =
      bool_field(state, 1, "gguf_required", model.required);
  model.required_by_backend[Backend::vllm] =
      bool_field(state, 1, "vllm_required", false);
  if (model.id.empty()) return luaL_error(state, "model id is required");

  const bool mlx_supported = bool_field(state, 1, "mlx_supported", true);
  const bool gguf_supported = bool_field(state, 1, "gguf_supported", false);
  const bool vllm_supported = bool_field(state, 1, "vllm_supported", false);
  const auto mlx_reason = string_field(state, 1, "mlx_reason", "MLX artifact unavailable");
  const auto gguf_reason =
      string_field(state, 1, "gguf_reason", "llama.cpp compatibility not validated");
  const auto vllm_reason = string_field(
      state, 1, "vllm_reason", "vLLM-compatible artifact is not configured");
  model.artifacts[Backend::mlx][Quantization::q4] =
      artifact_from(state, 1, "mlx_q4", mlx_supported, mlx_reason);
  model.artifacts[Backend::mlx][Quantization::q8] =
      artifact_from(state, 1, "mlx_q8", mlx_supported, mlx_reason);
  model.artifacts[Backend::gguf][Quantization::q4] =
      artifact_from(state, 1, "gguf_q4", gguf_supported, gguf_reason);
  model.artifacts[Backend::gguf][Quantization::q8] =
      artifact_from(state, 1, "gguf_q8", gguf_supported, gguf_reason);
  model.artifacts[Backend::vllm][Quantization::q4] =
      artifact_from(state, 1, "vllm_q4", vllm_supported, vllm_reason);
  model.artifacts[Backend::vllm][Quantization::q8] =
      artifact_from(state, 1, "vllm_q8", vllm_supported, vllm_reason);
  model.artifacts[Backend::vllm][Quantization::native] =
      artifact_from(state, 1, "vllm_native", vllm_supported, vllm_reason);
  registry->models.push_back(std::move(model));
  return 0;
}

int register_profile(lua_State* state) {
  auto* registry = registry_from_upvalue(state);
  luaL_checktype(state, 1, LUA_TTABLE);
  Profile profile;
  profile.name = string_field(state, 1, "name");
  profile.quantization =
      parse_quantization(string_field(state, 1, "quantization", "q4"));
  profile.models = string_array_field(state, 1, "models");
  if (profile.name.empty()) return luaL_error(state, "profile name is required");
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
  switch (value) {
    case Quantization::q4: return "q4";
    case Quantization::q8: return "q8";
    case Quantization::native: return "native";
  }
  throw std::invalid_argument("invalid quantization");
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
  }
  throw std::invalid_argument("invalid vLLM device");
}

VllmDevice parse_vllm_device(const std::string& value) {
  if (value == "auto") return VllmDevice::automatic;
  if (value == "cpu") return VllmDevice::cpu;
  if (value == "cuda") return VllmDevice::cuda;
  if (value == "metal") return VllmDevice::metal;
  throw std::invalid_argument("vLLM device must be auto, cpu, cuda, or metal");
}

Quantization parse_quantization(const std::string& value) {
  if (value == "q4") return Quantization::q4;
  if (value == "q8") return Quantization::q8;
  if (value == "native") return Quantization::native;
  throw std::invalid_argument("quantization must be q4, q8, or native");
}

const ModelDefinition& Registry::model(const std::string& id) const {
  const auto found = std::find_if(models.begin(), models.end(),
                                  [&](const auto& item) { return item.id == id; });
  if (found == models.end()) throw std::out_of_range("unknown model: " + id);
  return *found;
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
  lua_setglobal(state, "mica");
  try {
    run_file(state, config_directory / "models.lua");
    run_file(state, config_directory / "policy.lua");
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
