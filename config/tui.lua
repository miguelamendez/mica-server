-- Presentation only: C++ owns validated commands, secrets, installation and HTTP.
return {
  title = "Mica",
  sections = {
    {id = "server", title = "Server", hint = "Status, default/active workload, loaded models, stored data; choose a workload or inspect Machine"},
    {id = "workloads", title = "Workloads", hint = "Select a collection of models; install, start, edit or hot-swap"},
    {id = "models", title = "Models", hint = "Modalities, abilities, engines, quantizations, context and components"},
    {id = "engines", title = "Engines", hint = "Runtime manifests, hardware support, installed revision and endpoints"},
    {id = "endpoints", title = "Endpoints", hint = "Public routes; model-dependent operations require a compatible workload"},
    {id = "settings", title = "Settings", hint = "Bind address, port, default workload, RAM/VRAM and API-key rotation"},
  },
}
