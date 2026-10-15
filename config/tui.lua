-- Presentation only: C++ owns validated commands, secrets, installation and HTTP.
return {
  title = "Mica",
  sections = {
    {id = "server", title = "Server", hint = "Status, workload and models; o opens dashboard (same API port); Settings saves dashboard enable/disable"},
    {id = "workloads", title = "Workloads", hint = "Select a collection of models; install, start, edit or hot-swap"},
    {id = "models", title = "Models", hint = "Modalities, abilities, engines, quantizations, context and components"},
    {id = "engines", title = "Engines", hint = "Runtime manifests, hardware support, installed revision and endpoints"},
    {id = "endpoints", title = "Endpoints", hint = "Public routes; model-dependent operations require a compatible workload"},
    {id = "settings", title = "Settings", hint = "Listener, dashboard, workload, RAM/VRAM, keys and updates; changes require confirmation"},
    {id = "apps", title = "Apps", hint = "i installs, w prepares models, o opens UI, s starts background; apps use their own workload and do not restart Mica"},
  },
}
