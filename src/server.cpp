#include "mica_server/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mica_server/scheduler.hpp"
#include "mica_server/command.hpp"

namespace mica {
namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

struct RuntimeState {
  std::vector<Backend> installed_backends;
  std::vector<Quantization> quantizations;
  Quantization default_quantization{Quantization::q4};
  std::map<std::string, std::vector<Quantization>> configured_quantizations;
  std::string profile;
  double max_ram_gib{8};
  double max_vram_gib{0};
  double largest_nvidia_vram_gib{0};
  VllmDevice vllm_device{VllmDevice::automatic};
  std::filesystem::path api_key_file;
};

struct Worker {
  const ModelDefinition* model{nullptr};
  Backend backend{Backend::gguf};
  Quantization quantization{Quantization::q4};
  Artifact artifact;
  std::filesystem::path artifact_path;
  pid_t pid{-1};
  int port{0};
  int in_flight{0};
  std::uint64_t last_used_ns{0};
};

std::uint64_t monotonic_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string read_trimmed(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::stringstream contents;
  contents << file.rdbuf();
  auto value = contents.str();
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  return value;
}

RuntimeState load_runtime_state(const std::filesystem::path& root) {
  std::ifstream file(root / "mica-server/runtime.json");
  if (!file) throw std::runtime_error("runtime is not configured; run mica-server setup");
  const auto state = json::parse(file);
  RuntimeState result;
  if (state.contains("installed_backends")) {
    for (const auto& item : state.at("installed_backends")) {
      result.installed_backends.push_back(parse_backend(item.get<std::string>()));
    }
  } else if (state.contains("backend")) {
    result.installed_backends.push_back(parse_backend(state.at("backend").get<std::string>()));
  }
  if (result.installed_backends.empty()) {
    throw std::runtime_error("runtime config has no installed backends");
  }
  if (state.contains("quantizations")) {
    for (const auto& item : state.at("quantizations")) {
      result.quantizations.push_back(parse_quantization(item.get<std::string>()));
    }
  } else if (state.contains("quantization")) {
    result.quantizations.push_back(parse_quantization(state.at("quantization").get<std::string>()));
  }
  if (result.quantizations.empty()) result.quantizations.push_back(Quantization::q4);
  result.default_quantization = parse_quantization(
      state.value("default_quantization", to_string(result.quantizations.front())));
  if (state.contains("configured_models") && state["configured_models"].is_object()) {
    for (const auto& [id, model] : state["configured_models"].items()) {
      if (!model.value("enabled", true) || !model.contains("variants")) continue;
      for (const auto& [backend, quantizations] : model["variants"].items()) {
        auto& selected = result.configured_quantizations[id + "@" + backend];
        for (const auto& quantization : quantizations) {
          selected.push_back(parse_quantization(quantization.get<std::string>()));
        }
      }
    }
  }
  result.profile = state.at("profile").get<std::string>();
  result.max_ram_gib = state.at("max_ram_gib").get<double>();
  result.max_vram_gib = state.value("max_vram_gib", 0.0);
  if (state.contains("hardware") && state["hardware"].contains("nvidia_vram_gib")) {
    for (const auto& value : state["hardware"]["nvidia_vram_gib"]) {
      result.largest_nvidia_vram_gib =
          std::max(result.largest_nvidia_vram_gib, value.get<double>());
    }
  }
  result.vllm_device = parse_vllm_device(state.value("vllm_device", "auto"));
  result.api_key_file = state.at("api_key_file").get<std::string>();
  return result;
}

int allocate_port() {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) throw std::runtime_error("cannot allocate worker socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(socket_fd);
    throw std::runtime_error("cannot bind worker socket");
  }
  socklen_t length = sizeof(address);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    close(socket_fd);
    throw std::runtime_error("cannot inspect worker socket");
  }
  const int port = ntohs(address.sin_port);
  close(socket_fd);
  return port;
}

pid_t spawn_worker(const std::vector<std::string>& command,
                   const std::filesystem::path& log_path) {
  const pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("fork failed while starting worker");
  if (pid == 0) {
    const int fd = open(log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
    std::vector<char*> argv;
    for (const auto& value : command) argv.push_back(const_cast<char*>(value.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  return pid;
}

std::string audio_backend(Backend backend, const RuntimeState& state) {
  if (backend == Backend::mlx) return "mlx";
#ifdef __APPLE__
  return "metal";
#else
  return state.max_vram_gib > 0 ? "cuda" : "cpu";
#endif
}

void write_audio_config(const Worker& worker, const RuntimeState& state,
                        const std::filesystem::path& path) {
  const bool asr = worker.model->capability == "asr";
  json config = {
      {"host", "127.0.0.1"},
      {"port", worker.port},
      {"backend", audio_backend(worker.backend, state)},
      {"lazy_load", false},
      {"max_loaded_models", 1},
      {"models", json::array({{{"id", worker.model->id},
                                {"family", asr ? "granite5asr" : "audio8_tts"},
                                {"path", worker.artifact_path.string()},
                                {"task", asr ? "asr" : "tts"},
                                {"mode", "offline"}}})},
  };
  std::ofstream file(path, std::ios::trunc);
  file << std::setw(2) << config << '\n';
}

std::vector<std::string> worker_command(const Worker& worker, const RuntimeState& state,
                                        const std::filesystem::path& root) {
  const auto port = std::to_string(worker.port);
  if (worker.backend == Backend::mlx) {
    const auto python = (root / "environment-mlx/bin/python").string();
    if (worker.model->capability == "text") {
      const auto server = worker.model->mlx_converter == "mlx_vlm.convert"
                              ? "mlx_vlm.server"
                              : "mlx_lm.server";
      return {python, "-m", server, "--model", worker.artifact_path.string(),
              "--host", "127.0.0.1", "--port", port};
    }
    if (worker.model->capability == "vision") {
      return {python, "-m", "mlx_vlm.server", "--model", worker.artifact_path.string(),
              "--host", "127.0.0.1", "--port", port};
    }
    return {python, "-m", "mlx_audio.server", "--host", "127.0.0.1", "--port", port};
  }
  if (worker.backend == Backend::vllm) {
    std::vector<std::string> command = {
        (root / "environment-vllm/bin/vllm").string(), "serve",
        worker.artifact_path.string(), "--served-model-name", worker.model->id,
        "--host", "127.0.0.1", "--port", port};
    if (state.vllm_device == VllmDevice::cuda && state.max_vram_gib > 0 &&
        state.largest_nvidia_vram_gib > 0) {
      const auto utilization = std::min(
          0.95, std::min(worker.artifact.reservation_gib, state.max_vram_gib) /
                    state.largest_nvidia_vram_gib);
      std::ostringstream value;
      value << std::fixed << std::setprecision(3) << utilization;
      command.insert(command.end(), {"--gpu-memory-utilization", value.str()});
    }
    return command;
  }
  if (worker.model->capability == "text" || worker.model->capability == "vision") {
    std::vector<std::string> command = {
        (root / "runtime/llama.cpp/build-mica/bin/llama-server").string(),
        "-m", worker.artifact_path.string(), "--host", "127.0.0.1", "--port", port};
    if (!worker.artifact.projector_pattern.empty()) {
      command.emplace_back("--mmproj");
      command.emplace_back((worker.artifact_path.parent_path() /
                            worker.artifact.projector_pattern).string());
    }
#ifdef __APPLE__
    command.insert(command.end(), {"--n-gpu-layers", "99"});
#else
    command.insert(command.end(), {"--n-gpu-layers", state.max_vram_gib > 0 ? "99" : "0"});
#endif
    return command;
  }
  return {(root / "runtime/audio.cpp/build-mica/bin/audiocpp_server").string(),
          "--config", (root / "mica-server/workers" /
                       (worker.model->id + "-" + to_string(worker.backend) + "-" +
                        to_string(worker.quantization) + ".json")).string()};
}

class WorkerManager {
 public:
  WorkerManager(const Registry& registry, const RuntimeState& state,
                std::filesystem::path root, std::vector<Backend> active_backends)
      : registry_(registry), state_(state), root_(std::move(root)),
        active_backends_(std::move(active_backends)) {
    std::filesystem::create_directories(root_ / "mica-server/workers");
    std::filesystem::create_directories(root_ / "mica-server/logs");
  }

  ~WorkerManager() {
    stop_.store(true);
    if (sweeper_.joinable()) sweeper_.join();
    std::lock_guard lock(mutex_);
    for (auto& [_, worker] : workers_) stop_worker(worker);
  }

  void start_background() {
    sweeper_ = std::thread([this] {
      while (!stop_.load()) {
        for (int i = 0; i < 20 && !stop_.load(); ++i) std::this_thread::sleep_for(100ms);
        sweep_idle();
      }
    });
  }

  void prewarm() {
    try {
      const auto& profile = registry_.profile(state_.profile);
      // Setup may install both stacks, but one server activates one backend. Completion
      // markers make subsequent launches reuse that backend's configured artifacts.
      for (const auto backend : active_backends_) {
        for (const auto& id : profile.models) {
          const auto& model = registry_.model(id);
          for (const auto quantization : quantizations_for(id, backend)) {
            const auto artifact = model.artifacts.at(backend).at(quantization);
            if (!artifact.supported) continue;
            Worker candidate;
            candidate.model = &model;
            candidate.backend = backend;
            candidate.quantization = quantization;
            candidate.artifact = artifact;
            candidate.artifact_path = root_ / "checkpoints" / to_string(backend) /
                                      model.id / artifact.pattern;
            ensure_artifact(candidate);
          }
        }
      }
      double planned = 0;
      for (std::size_t backend_index = 0; backend_index < active_backends_.size();
           ++backend_index) {
        const auto backend = active_backends_[backend_index];
        std::vector<const ModelDefinition*> ordered;
        for (const auto& id : profile.models) ordered.push_back(&registry_.model(id));
        std::stable_sort(ordered.begin(), ordered.end(), [backend](const auto* left, const auto* right) {
          if (left->required_for(backend) != right->required_for(backend)) {
            return left->required_for(backend) > right->required_for(backend);
          }
          if (left->startup_priority != right->startup_priority) {
            return left->startup_priority < right->startup_priority;
          }
          return left->id < right->id;
        });
        for (const auto* model : ordered) {
          const auto selected_quantizations = quantizations_for(model->id, backend);
          if (selected_quantizations.empty()) continue;
          const auto default_found = std::find(selected_quantizations.begin(),
                                               selected_quantizations.end(),
                                               state_.default_quantization);
          const auto warm_quantization = default_found == selected_quantizations.end()
                                             ? selected_quantizations.front()
                                             : state_.default_quantization;
          const auto artifact = model->artifacts.at(backend).at(warm_quantization);
          const bool required_baseline =
              backend_index == 0 && model->required_for(backend);
          if (!artifact.supported || planned + artifact.reservation_gib > residency_limit()) {
            if (required_baseline) {
              throw std::runtime_error(model->id +
                                       ": required startup baseline exceeds RAM budget");
            }
            continue;
          }
          auto lease = acquire(model->id, backend, warm_quantization);
          release(lease);
          planned += artifact.reservation_gib;
        }
      }
      ready_.store(true);
    } catch (const std::exception& error) {
      std::lock_guard lock(error_mutex_);
      readiness_error_ = error.what();
    }
  }

  bool ready() const { return ready_.load(); }

  std::string readiness_error() const {
    std::lock_guard lock(error_mutex_);
    return readiness_error_;
  }

  std::shared_ptr<Worker> acquire(const std::string& id, Backend backend,
                                  Quantization quantization) {
    std::lock_guard lock(mutex_);
    if (std::find(active_backends_.begin(), active_backends_.end(), backend) ==
        active_backends_.end()) {
      throw std::runtime_error("backend is not active: " + to_string(backend));
    }
    const auto selected_quantizations = quantizations_for(id, backend);
    if (std::find(selected_quantizations.begin(), selected_quantizations.end(), quantization) ==
        selected_quantizations.end()) {
      throw std::runtime_error("quantization is not configured: " + to_string(quantization));
    }
    const auto key = worker_key(id, backend, quantization);
    if (auto found = workers_.find(key); found != workers_.end()) {
      ++found->second->in_flight;
      return found->second;
    }
    const auto& model = registry_.model(id);
    const auto& profile = registry_.profile(state_.profile);
    if (std::find(profile.models.begin(), profile.models.end(), id) == profile.models.end()) {
      throw std::runtime_error("model is not in active profile: " + id);
    }
    const auto artifact = model.artifacts.at(backend).at(quantization);
    if (!artifact.supported) throw std::runtime_error(artifact.reason);
    make_room(artifact.reservation_gib);
    auto worker = std::make_shared<Worker>();
    worker->model = &model;
    worker->backend = backend;
    worker->quantization = quantization;
    worker->artifact = artifact;
    worker->artifact_path = root_ / "checkpoints" / to_string(backend) / model.id /
                            artifact.pattern;
    worker->port = allocate_port();
    worker->in_flight = 1;
    ensure_artifact(*worker);
    if (backend == Backend::gguf &&
        (model.capability == "asr" || model.capability == "tts")) {
      write_audio_config(*worker, state_,
                         root_ / "mica-server/workers" /
                             (model.id + "-" + to_string(backend) + "-" +
                              to_string(quantization) + ".json"));
    }
    worker->pid = spawn_worker(worker_command(*worker, state_, root_),
                               root_ / "mica-server/logs" /
                                   (model.id + "-" + to_string(backend) + "-" +
                                    to_string(quantization) + ".log"));
    try {
      wait_for_health(*worker);
      warmup(*worker);
    } catch (...) {
      stop_worker(worker);
      throw;
    }
    worker->last_used_ns = monotonic_ns();
    reserved_gib_ += artifact.reservation_gib;
    workers_[key] = worker;
    return worker;
  }

  void release(const std::shared_ptr<Worker>& worker) {
    std::lock_guard lock(mutex_);
    if (worker->in_flight > 0) --worker->in_flight;
    worker->last_used_ns = monotonic_ns();
  }

  json models_json() const {
    std::lock_guard lock(mutex_);
    json data = json::array();
    const auto& profile = registry_.profile(state_.profile);
    for (const auto& id : profile.models) {
      const auto& definition = registry_.model(id);
      for (const auto backend : active_backends_) {
        const auto selected_quantizations = quantizations_for(id, backend);
        for (const auto quantization : selected_quantizations) {
          const auto backend_it = definition.artifacts.find(backend);
          if (backend_it == definition.artifacts.end()) continue;
          const auto artifact_it = backend_it->second.find(quantization);
          if (artifact_it == backend_it->second.end() || !artifact_it->second.supported) continue;
          const auto key = worker_key(id, backend, quantization);
          const auto loaded = workers_.find(key);
          const auto unambiguous = selected_quantizations.size() == 1;
          const auto public_id = unambiguous ? id : key;
          data.push_back({{"id", public_id}, {"object", "model"}, {"owned_by", "local"},
                          {"capability", definition.capability},
                          {"description", definition.description}, {"tags", definition.tags},
                          {"backend", to_string(backend)},
                          {"quantization", to_string(quantization)},
                          {"repository", definition.repositories.at(backend)},
                          {"state", loaded == workers_.end() ? "stopped" : "ready"}});
        }
      }
    }
    return {{"object", "list"}, {"data", data}};
  }

  json admin_json() const {
    std::lock_guard lock(mutex_);
    json workers = json::array();
    for (const auto& [id, worker] : workers_) {
      workers.push_back({{"id", id}, {"backend", to_string(worker->backend)},
                         {"quantization", to_string(worker->quantization)},
                         {"pid", worker->pid}, {"port", worker->port},
                         {"reservation_gib", worker->artifact.reservation_gib},
                         {"in_flight", worker->in_flight},
                         {"last_used_monotonic_ns", worker->last_used_ns}});
    }
    json active = json::array();
    for (const auto backend : active_backends_) active.push_back(to_string(backend));
    return {{"ready", ready()}, {"active_backends", active},
            {"reserved_ram_gib", reserved_gib_},
            {"max_ram_gib", state_.max_ram_gib},
            {"max_vram_gib", state_.max_vram_gib},
            {"effective_residency_limit_gib", residency_limit()},
            {"vllm_device", to_string(state_.vllm_device)}, {"workers", workers}};
  }

 private:
  std::vector<Quantization> quantizations_for(const std::string& id,
                                               Backend backend) const {
    const auto found = state_.configured_quantizations.find(id + "@" + to_string(backend));
    if (found != state_.configured_quantizations.end()) return found->second;
    return state_.quantizations;
  }

  static std::string worker_key(const std::string& id, Backend backend,
                                Quantization quantization) {
    return id + "@" + to_string(backend) + ":" + to_string(quantization);
  }

  void ensure_artifact(const Worker& worker) const {
    const auto base = worker.artifact_path.parent_path();
    const auto marker = base / (".mica-complete-" + to_string(worker.quantization));
    if (std::filesystem::exists(worker.artifact_path) && std::filesystem::exists(marker)) return;

    const auto repository_it = worker.model->repositories.find(worker.backend);
    if (repository_it == worker.model->repositories.end() || repository_it->second.empty()) {
      throw std::runtime_error("no curated repository configured for " + worker.model->id +
                               "@" + to_string(worker.backend));
    }
    std::filesystem::create_directories(base);
    std::vector<std::string> command = {
        (root_ / "environment-tools/bin/hf").string(), "download", repository_it->second};
    if (worker.backend == Backend::vllm &&
        worker.quantization == Quantization::native) {
      command.insert(command.end(), {"--local-dir", worker.artifact_path.string()});
    } else if (std::filesystem::path(worker.artifact.pattern).extension().empty()) {
      command.insert(command.end(), {"--include", worker.artifact.pattern + "/*"});
    } else {
      command.push_back(worker.artifact.pattern);
    }
    if (!worker.artifact.projector_pattern.empty()) {
      command.push_back(worker.artifact.projector_pattern);
    }
    if (!(worker.backend == Backend::vllm &&
          worker.quantization == Quantization::native)) {
      command.insert(command.end(), {"--local-dir", base.string()});
    }
    const auto result = run_command(command, true);
    if (result.exit_code != 0) {
      throw std::runtime_error("lazy Hugging Face download failed for " + worker.model->id +
                               "@" + to_string(worker.backend) + ": " + result.output);
    }
    if (!std::filesystem::exists(worker.artifact_path) ||
        (!worker.artifact.projector_pattern.empty() &&
         !std::filesystem::exists(base / worker.artifact.projector_pattern))) {
      throw std::runtime_error("download completed without the configured artifact: " +
                               worker.artifact_path.string());
    }
    std::ofstream marker_file(marker, std::ios::trunc);
    marker_file << repository_it->second << '\n';
    marker_file.close();
    record_download(worker, repository_it->second);
  }

  void record_download(const Worker& worker, const std::string& repository) const {
    const auto path = root_ / "mica-server/runtime.json";
    std::ifstream input(path);
    auto state = json::parse(input);
    const auto key = worker_key(worker.model->id, worker.backend, worker.quantization);
    state["downloads"][key] = {
        {"model", worker.model->id}, {"backend", to_string(worker.backend)},
        {"quantization", to_string(worker.quantization)}, {"repository", repository},
        {"artifact", worker.artifact.pattern}, {"local_path", worker.artifact_path.string()},
        {"downloaded", true}, {"smoke_validated", false}};
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << std::setw(2) << state << '\n';
    output.close();
    std::filesystem::rename(temporary, path);
  }

  void record_smoke_validation(const Worker& worker) const {
    const auto path = root_ / "mica-server/runtime.json";
    std::ifstream input(path);
    auto state = json::parse(input);
    const auto key = worker_key(worker.model->id, worker.backend, worker.quantization);
    state["downloads"][key]["smoke_validated"] = true;
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << std::setw(2) << state << '\n';
    output.close();
    std::filesystem::rename(temporary, path);
  }

  void wait_for_health(const Worker& worker) const {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(registry_.policy.load_timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
      if (waitpid(worker.pid, nullptr, WNOHANG) == worker.pid) {
        throw std::runtime_error("worker exited during startup: " + worker.model->id);
      }
      httplib::Client client("127.0.0.1", worker.port);
      client.set_connection_timeout(1, 0);
      if (const auto response = client.Get("/health"); response && response->status == 200) return;
      std::this_thread::sleep_for(250ms);
    }
    throw std::runtime_error("worker health timeout: " + worker.model->id);
  }

  void warmup(const Worker& worker) const {
    httplib::Client client("127.0.0.1", worker.port);
    client.set_read_timeout(registry_.policy.load_timeout_seconds, 0);
    const std::string worker_model = worker.backend == Backend::mlx
                                         ? worker.artifact_path.string()
                                         : worker.model->id;
    httplib::Result response;
    if (worker.model->capability == "asr") {
      const auto fixture = asr_fixture();
      httplib::UploadFormDataItems items = {
          {"model", worker_model, "", "text/plain"},
          {"file", read_binary(fixture), "warmup.wav", "audio/wav"}};
      response = client.Post("/v1/audio/transcriptions", items);
    } else if (worker.model->capability == "tts") {
      const json body = {{"model", worker_model}, {"input", "Warmup."},
                         {"voice", "default"}, {"response_format", "wav"}};
      response = client.Post("/v1/audio/speech", body.dump(), "application/json");
    } else {
      json content = "Reply with exactly MICA_OK_31415.";
      if (worker.model->capability == "vision") {
        content = json::array({{{"type", "text"},
                                {"text", "What single color fills this image? Answer one word."}},
                               {{"type", "image_url"},
                                {"image_url", {{"url", "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg=="}}}}});
      }
      const json body = {{"model", worker_model},
                         {"messages", json::array({{{"role", "user"}, {"content", content}}})},
                         {"max_tokens", 8}, {"stream", false}};
      response = client.Post("/v1/chat/completions", body.dump(), "application/json");
    }
    if (!response || response->status < 200 || response->status >= 300) {
      throw std::runtime_error("real inference warm-up failed: " + worker.model->id);
    }
    validate_inference(worker, response->body);
    record_smoke_validation(worker);
  }

  static std::string read_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read smoke fixture: " + path.string());
    std::ostringstream data;
    data << input.rdbuf();
    return data.str();
  }

  std::filesystem::path asr_fixture() const {
    const auto path = root_ / "mica-server/workers/asr-warmup.wav";
    if (std::filesystem::exists(path)) return path;
#ifdef __APPLE__
    const auto command = std::vector<std::string>{
        "/usr/bin/say", "-o", path.string(), "--file-format=WAVE",
        "--data-format=LEI16@16000", "Mica warmup transcription"};
#else
    const auto command = std::vector<std::string>{
        "/usr/bin/espeak", "-w", path.string(), "Mica warmup transcription"};
#endif
    const auto result = run_command(command, true);
    if (result.exit_code != 0 || !std::filesystem::exists(path)) {
      throw std::runtime_error(
          "cannot create ASR speech fixture; install espeak on Linux/WSL: " + result.output);
    }
    return path;
  }

  static std::string response_text(const std::string& body) {
    const auto parsed = json::parse(body);
    if (parsed.contains("text") && parsed["text"].is_string()) {
      return parsed["text"].get<std::string>();
    }
    if (parsed.contains("choices") && parsed["choices"].is_array() &&
        !parsed["choices"].empty()) {
      const auto& choice = parsed["choices"][0];
      if (choice.contains("text") && choice["text"].is_string()) {
        return choice["text"].get<std::string>();
      }
      if (choice.contains("message") && choice["message"].contains("content") &&
          choice["message"]["content"].is_string()) {
        return choice["message"]["content"].get<std::string>();
      }
    }
    return {};
  }

  static void validate_inference(const Worker& worker, const std::string& body) {
    if (worker.model->capability == "tts") {
      if (body.size() <= 44 || body.compare(0, 4, "RIFF") != 0 ||
          body.compare(8, 4, "WAVE") != 0) {
        throw std::runtime_error("TTS smoke output is not a nonempty WAV: " +
                                 worker.model->id);
      }
      return;
    }
    std::string text;
    try {
      text = response_text(body);
    } catch (const std::exception& error) {
      throw std::runtime_error("smoke response is not valid JSON for " + worker.model->id +
                               ": " + error.what());
    }
    if (text.empty()) {
      throw std::runtime_error("smoke response has no generated text: " + worker.model->id);
    }
    auto normalized = text;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (worker.model->capability == "text" && normalized.find("mica_ok_31415") == std::string::npos) {
      throw std::runtime_error("text smoke response did not follow the deterministic prompt: " +
                               worker.model->id);
    }
    if (worker.model->capability == "vision" && normalized.find("red") == std::string::npos) {
      throw std::runtime_error("vision smoke response was not grounded in the red fixture: " +
                               worker.model->id);
    }
  }

  void make_room(double requested) {
    if (reserved_gib_ + requested <= residency_limit() + 1e-9) return;
    std::vector<ResidentModel> candidates;
    const auto now = monotonic_ns();
    for (const auto& [id, worker] : workers_) {
      candidates.push_back({id, worker->artifact.reservation_gib, worker->last_used_ns,
                            now > worker->last_used_ns +
                                      static_cast<std::uint64_t>(registry_.policy.idle_ttl_seconds) *
                                          1000000000ULL,
                            worker->in_flight});
    }
    for (const auto& candidate : rank_eviction_candidates(std::move(candidates))) {
      auto found = workers_.find(candidate.id);
      if (found == workers_.end()) continue;
      reserved_gib_ -= found->second->artifact.reservation_gib;
      stop_worker(found->second);
      workers_.erase(found);
      if (reserved_gib_ + requested <= residency_limit() + 1e-9) return;
    }
    throw std::runtime_error("model_memory_budget_exceeded");
  }

  double residency_limit() const {
    if (active_backends_.size() == 1 && active_backends_.front() == Backend::vllm &&
        state_.vllm_device == VllmDevice::cuda && state_.max_vram_gib > 0) {
      return std::min(state_.max_ram_gib, state_.max_vram_gib);
    }
    return state_.max_ram_gib;
  }

  void sweep_idle() {
    std::lock_guard lock(mutex_);
    const auto now = monotonic_ns();
    for (auto it = workers_.begin(); it != workers_.end();) {
      const bool expired = it->second->in_flight == 0 &&
                           now > it->second->last_used_ns +
                                     static_cast<std::uint64_t>(registry_.policy.idle_ttl_seconds) *
                                         1000000000ULL;
      if (!expired) {
        ++it;
        continue;
      }
      reserved_gib_ -= it->second->artifact.reservation_gib;
      stop_worker(it->second);
      it = workers_.erase(it);
    }
  }

  static void stop_worker(const std::shared_ptr<Worker>& worker) {
    if (!worker || worker->pid <= 0) return;
    kill(worker->pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
      if (waitpid(worker->pid, nullptr, WNOHANG) == worker->pid) {
        worker->pid = -1;
        return;
      }
      std::this_thread::sleep_for(100ms);
    }
    kill(worker->pid, SIGKILL);
    waitpid(worker->pid, nullptr, 0);
    worker->pid = -1;
  }

  const Registry& registry_;
  RuntimeState state_;
  std::filesystem::path root_;
  std::vector<Backend> active_backends_;
  mutable std::mutex mutex_;
  mutable std::mutex error_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Worker>> workers_;
  double reserved_gib_{0};
  std::atomic<bool> ready_{false};
  std::atomic<bool> stop_{false};
  std::string readiness_error_;
  std::thread sweeper_;
};

bool authorized(const httplib::Request& request, const std::string& key) {
  if (key.empty()) return false;
  return request.get_header_value("Authorization") == "Bearer " + key;
}

void json_error(httplib::Response& response, int status, const std::string& code,
                const std::string& message) {
  response.status = status;
  response.set_content(json({{"error", {{"code", code}, {"message", message}}}}).dump(),
                       "application/json");
}

void copy_worker_response(const httplib::Response& source, httplib::Response& destination) {
  destination.status = source.status;
  for (const auto& [name, value] : source.headers) {
    if (name != "Content-Length" && name != "Connection" && name != "Transfer-Encoding") {
      destination.set_header(name, value);
    }
  }
  destination.body = source.body;
}

std::string rewrite_json_model(const std::string& body, const Worker& worker,
                               const RuntimeState&) {
  if (worker.backend != Backend::mlx) return body;
  auto parsed = json::parse(body);
  parsed["model"] = worker.artifact_path.string();
  return parsed.dump();
}

struct ResolvedModelRequest {
  std::string id;
  Backend backend;
  Quantization quantization;
};

ResolvedModelRequest resolve_model_request(
    const std::string& requested, const std::vector<Backend>& active_backends,
    const RuntimeState& state) {
  const auto separator = requested.rfind('@');
  auto default_for = [&](const std::string& id, Backend backend) {
    const auto found = state.configured_quantizations.find(id + "@" + to_string(backend));
    if (found == state.configured_quantizations.end() || found->second.empty()) {
      return state.default_quantization;
    }
    if (std::find(found->second.begin(), found->second.end(), state.default_quantization) !=
        found->second.end()) return state.default_quantization;
    return found->second.front();
  };
  if (separator != std::string::npos) {
    const auto selector = requested.substr(separator + 1);
    const auto quant_separator = selector.find(':');
    const auto backend = parse_backend(selector.substr(0, quant_separator));
    const auto model_id = requested.substr(0, separator);
    const auto quantization = quant_separator == std::string::npos
                                  ? default_for(model_id, backend)
                                  : parse_quantization(selector.substr(quant_separator + 1));
    if (std::find(active_backends.begin(), active_backends.end(), backend) ==
        active_backends.end()) {
      throw std::runtime_error("requested backend is not active: " + to_string(backend));
    }
    return {model_id, backend, quantization};
  }
  if (active_backends.empty()) throw std::runtime_error("no active backend");
  return {requested, active_backends.front(), default_for(requested, active_backends.front())};
}

}  // namespace

int run_server(const Registry& registry, const ServerOptions& options) {
  const auto state = load_runtime_state(options.root);
  std::vector<Backend> active_backends;
  if (!options.active_backend) {
    if (state.installed_backends.size() != 1) {
      throw std::runtime_error(
          "multiple backends are installed; start with --backend mlx, gguf, or vllm");
    }
    active_backends = state.installed_backends;
  } else {
    active_backends = {*options.active_backend};
  }
  for (const auto backend : active_backends) {
    if (std::find(state.installed_backends.begin(), state.installed_backends.end(), backend) ==
        state.installed_backends.end()) {
      throw std::runtime_error("backend is not installed: " + to_string(backend));
    }
  }
  const auto api_key = read_trimmed(state.api_key_file);
  auto manager = std::make_shared<WorkerManager>(registry, state, options.root,
                                                 active_backends);
  manager->start_background();
  std::thread prewarm([manager] { manager->prewarm(); });

  httplib::Server server;
  server.Get("/health", [](const auto&, auto& response) {
    response.set_content(json({{"status", "ok"}}).dump(), "application/json");
  });
  server.Get("/ready", [manager](const auto&, auto& response) {
    if (manager->ready()) {
      response.set_content(json({{"ready", true}}).dump(), "application/json");
    } else {
      json_error(response, 503, "not_ready", manager->readiness_error());
    }
  });

  auto require_auth = [api_key](const httplib::Request& request,
                                httplib::Response& response) {
    if (authorized(request, api_key)) return true;
    json_error(response, 401, "invalid_api_key", "Bearer API key required");
    return false;
  };

  server.Get("/v1/models", [manager, require_auth](const auto& request, auto& response) {
    if (!require_auth(request, response)) return;
    response.set_content(manager->models_json().dump(), "application/json");
  });
  server.Get("/admin/models", [manager, require_auth](const auto& request, auto& response) {
    if (!require_auth(request, response)) return;
    response.set_content(manager->admin_json().dump(), "application/json");
  });

  auto proxy = [manager, state, active_backends, require_auth](const httplib::Request& request,
                                               httplib::Response& response) {
    if (!require_auth(request, response)) return;
    if (!manager->ready()) {
      json_error(response, 503, "not_ready", manager->readiness_error());
      return;
    }
    try {
      std::string model_id;
      if (request.is_multipart_form_data()) {
        const auto form = request.form;
        if (form.has_field("model")) model_id = form.get_field("model");
      } else {
        model_id = json::parse(request.body).at("model").get<std::string>();
      }
      const auto selected = resolve_model_request(model_id, active_backends, state);
      auto worker = manager->acquire(selected.id, selected.backend, selected.quantization);
      httplib::Client client("127.0.0.1", worker->port);
      client.set_read_timeout(3600, 0);
      httplib::Headers headers;
      for (const auto& [name, value] : request.headers) {
        if (name != "Authorization" && name != "Host" && name != "Content-Length" &&
            !(request.is_multipart_form_data() && name == "Content-Type")) {
          headers.emplace(name, value);
        }
      }
      httplib::Result result;
      if (request.is_multipart_form_data()) {
        httplib::UploadFormDataItems items;
        for (const auto& [name, field] : request.form.fields) {
          items.push_back({name,
                           name == "model" && worker->backend == Backend::mlx
                               ? worker->artifact_path.string()
                               : field.content,
                           "", "text/plain"});
        }
        for (const auto& [name, file] : request.form.files) {
          items.push_back({name, file.content, file.filename, file.content_type});
        }
        result = client.Post(request.path, headers, items);
      } else {
        const auto body = rewrite_json_model(request.body, *worker, state);
        result = client.Post(request.path, headers, body,
                             request.get_header_value("Content-Type"));
      }
      manager->release(worker);
      if (!result) {
        json_error(response, 502, "worker_unavailable", "worker request failed");
        return;
      }
      copy_worker_response(*result, response);
    } catch (const std::exception& error) {
      json_error(response, 503, "model_unavailable", error.what());
    }
  };

  server.Post("/v1/chat/completions", proxy);
  server.Post("/v1/completions", proxy);
  server.Post("/v1/audio/transcriptions", proxy);
  server.Post("/v1/audio/speech", proxy);

  const bool listened = server.listen(options.host, options.port);
  if (prewarm.joinable()) prewarm.join();
  return listened ? 0 : 1;
}

}  // namespace mica
