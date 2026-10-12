#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "mica_server/types.hpp"

namespace mica {

inline bool supports_tool_calling(const ModelDefinition& model,
                                  const EngineDefinition& engine,
                                  const EngineDefinition::EndpointContract& endpoint) {
  return endpoint.operation == "chat.generate" && endpoint.supports_tools &&
      std::find(model.abilities.begin(), model.abilities.end(), "tool_calling") != model.abilities.end() &&
      std::find(model.tool_call_formats.begin(), model.tool_call_formats.end(), "openai") != model.tool_call_formats.end() &&
      (engine.launcher != "vllm" || model.tool_call_parsers.contains(engine.id));
}

inline std::vector<std::string> tool_call_runtime_arguments(
    const ModelDefinition& model, const EngineDefinition& engine) {
  if (!std::any_of(engine.endpoint_contracts.begin(), engine.endpoint_contracts.end(),
                  [&](const auto& endpoint) { return supports_tool_calling(model, engine, endpoint); })) return {};
  if (engine.launcher == "llama-server") return {"--jinja"};
  if (engine.launcher == "vllm")
    return {"--enable-auto-tool-choice", "--tool-call-parser", model.tool_call_parsers.at(engine.id)};
  // MLX's OpenAI adapter infers its parser from the model's chat template.
  return {};
}

}  // namespace mica
