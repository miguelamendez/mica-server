#pragma once

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "mica_server/types.hpp"

namespace mica {

// Discovery metadata, never a replacement for runtime interaction validation.
struct TaskRequirements {
  std::vector<std::string> abilities;
  std::vector<std::string> operations;  // Any one of these interactions.
  std::vector<std::string> inputs;      // Must occur in that same interaction.
};

inline const std::map<std::string, TaskRequirements>& task_requirements() {
  static const std::map<std::string, TaskRequirements> values = {
      {"chat", {{"text_generation"}, {"chat.generate"}, {"text"}}},
      {"coding", {{"text_generation"}, {"chat.generate", "text.generate"}, {"text"}}},
      {"ocr", {{"text_generation", "image_understanding"}, {"chat.generate"}, {"image"}}},
      {"visual_question_answering", {{"text_generation", "image_understanding"}, {"chat.generate"}, {"image"}}},
      {"video_question_answering", {{"text_generation", "video_understanding"}, {"chat.generate"}, {"video"}}},
      {"audio_question_answering", {{"text_generation", "audio_understanding"}, {"chat.generate"}, {"audio"}}},
      {"tool_calling", {{"tool_calling"}, {"chat.generate"}, {"text"}}},
      {"structured_extraction", {{"text_generation"}, {"chat.generate", "text.generate"}, {"text"}}},
      {"decision_scoring", {{"text_generation"}, {"decisions.score"}, {"text"}}},
      {"transcription", {{"speech_recognition"}, {"audio.transcribe", "chat.generate"}, {"audio"}}},
      {"speech_translation", {{"speech_translation"}, {"audio.translate", "chat.generate"}, {"audio"}}},
      {"speaker_diarization", {{"speaker_diarization"}, {"audio.diarize"}, {"audio"}}},
      {"text_to_speech", {{"speech_synthesis"}, {"audio.synthesize_speech"}, {"text"}}},
      {"voice_cloning", {{"speech_synthesis", "voice_conditioning"}, {"audio.synthesize_speech"}, {"text", "audio"}}},
      {"embedding", {{"embedding_generation"}, {"embedding.generate"}, {}}},
      {"retrieval", {{"embedding_generation"}, {"embedding.generate"}, {}}},
      {"image_generation", {{"image_generation"}, {"image.generate"}, {"text"}}},
      {"image_editing", {{"image_editing"}, {"image.edit"}, {"image"}}},
      {"video_generation", {{"video_generation"}, {"video.generate"}, {"text"}}},
      {"video_editing", {{"video_editing"}, {"video.edit"}, {"video"}}},
      {"audio_generation", {{"general_audio_generation"}, {"audio.generate"}, {"text"}}},
      {"music_generation", {{"music_generation"}, {"audio.generate"}, {"text"}}},
      {"audio_transformation", {{"audio_transformation"}, {"audio.transform"}, {"audio"}}},
  };
  return values;
}

inline void validate_model_tasks(const ModelDefinition& model) {
  for (const auto& task : model.supported_tasks) {
    const auto& rule = task_requirements().at(task);
    for (const auto& ability : rule.abilities)
      if (std::find(model.abilities.begin(), model.abilities.end(), ability) == model.abilities.end())
        throw std::invalid_argument("model " + model.id + " task " + task + " requires ability " + ability);
    const auto matching = std::any_of(model.supported_interactions.begin(), model.supported_interactions.end(), [&](const auto& interaction) {
      if (std::find(rule.operations.begin(), rule.operations.end(), interaction.operation) == rule.operations.end()) return false;
      return std::all_of(rule.inputs.begin(), rule.inputs.end(), [&](const auto& input) {
        return std::find(interaction.required_inputs.begin(), interaction.required_inputs.end(), input) != interaction.required_inputs.end() ||
               std::find(interaction.optional_inputs.begin(), interaction.optional_inputs.end(), input) != interaction.optional_inputs.end();
      });
    });
    if (!matching) throw std::invalid_argument("model " + model.id + " task " + task + " has no matching supported interaction");
  }
}

}  // namespace mica
