#pragma once

#include <cmath>
#include <nlohmann/json.hpp>

namespace mica {
// Preserve decode timing supplied by the engine. Do not infer tokens from text
// or divide by an end-to-end duration containing prefill, tools, or TTS.
inline nlohmann::json generation_metrics(const nlohmann::json& response) {
  using nlohmann::json;
  json metrics = {{"available", false}, {"scope", "final_answer_decode"},
                  {"includes_reasoning_tokens", true}};
  if (!response.is_object()) return metrics;
  const auto timing = response.find("timings");
  if (timing == response.end() || !timing->is_object()) return metrics;
  const auto finite_number = [&](const char* field) -> double {
    const auto value = timing->find(field);
    if (value == timing->end() || !value->is_number()) return 0;
    const double number = value->get<double>();
    return std::isfinite(number) && number > 0 ? number : 0;
  };
  const double tokens = finite_number("predicted_n");
  const double milliseconds = finite_number("predicted_ms");
  double speed = finite_number("predicted_per_second");
  if (speed == 0 && tokens > 0 && milliseconds > 0)
    speed = tokens * 1000 / milliseconds;
  if (speed <= 0 || !std::isfinite(speed)) return metrics;
  metrics["available"] = true;
  metrics["source"] = "engine_timings";
  metrics["decode_tokens_per_second"] = speed;
  if (tokens > 0) metrics["completion_tokens"] = tokens;
  if (milliseconds > 0) metrics["decode_seconds"] = milliseconds / 1000;
  return metrics;
}
}
