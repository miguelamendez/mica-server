# Android runtime options for Mica applications

Draft decision, 2026-10-18. All five applications target a local core/server and the relevant client interfaces on Android. An Android app using remote inference still owns its own local state and workflow. Runtime choice affects installation and compatible engines, not that application boundary.

## Runtime choices

| Path | Intended use | Constraints |
| --- | --- | --- |
| Native Android NDK and Kotlin UI | Default app core/server, SDK, UI and supported native engines | Build Android ABI libraries, package them appropriately, and obey permissions/lifecycle |
| Native Termux build | Terminal CLI/TUI/server and compatible native engines without a Linux rootfs | Target Termux/Android dependencies; a desktop glibc executable is not interchangeable |
| PRoot Linux ARM64 environment | Compatibility for components requiring Linux userland | Extra syscall overhead, different lifecycle, and separately validated device access |
| Android virtualization framework | Optional experiment on supported devices | Optional device/platform facilities; not a universal deployment requirement |

Prefer native targets where supported, and offer a minimal PRoot runtime when required. Do not bundle a desktop environment, game runtime, x86 translator or VM merely to serve models. This is a footprint-oriented design choice, not a measured Mica Android speed claim.

## Lessons from DroidDeck

[DroidDeck](https://github.com/Droid-Deck/DroidDeck) packages Android controls around a downloadable Linux runtime, pinned native components and lifecycle integrations. Its README still identifies PRoot use and device-specific GPU limits. It is a useful installer/runtime reference, not evidence of a faster replacement for PRoot. Use the official repository; its README warns against purported standalone project websites.

For Mica, reuse the design ideas of verified component downloads, runtime preparation, explicit device compatibility and recoverable setup. Keep the runtime small and native ARM64. Do not inherit gaming dependencies or assume its supported GPUs cover all inference devices. Evaluate any copied components and their licenses separately.

## Native inference and PRoot limits

The [official llama.cpp Android guide](https://github.com/ggml-org/llama.cpp/blob/master/docs/android.md) documents Android bindings, Termux builds and NDK cross-compilation. This makes native GGUF CPU inference a practical first target without requiring a complete Linux distribution. Host compilation settings must not silently impose the build machine's instruction set on the phone.

[PRoot-Distro](https://github.com/termux/proot-distro#limitations) documents ptrace-based syscall interception, no normal init-system support and limits on kernel facilities. Keep long-running processes under the Android host lifecycle rather than expecting systemd inside PRoot. Hardware discovery must identify the real host and restricted or substituted statistics. PRoot is not a security boundary for untrusted apps.

[Android Virtualization Framework](https://source.android.com/docs/core/virtualization) provides a separate VM approach on supported ARM64 devices, with optional platform APIs. It is a future test option, not the fallback every phone must provide. GPU/NPU access is a separate engine/device test for every runtime.

## Interfaces and release artifacts

| Interface | Android packaging and use |
| --- | --- |
| Server/core | Android native library hosted by the app lifecycle; optional compatible terminal/Linux service build |
| Android UI | Kotlin/Compose plus app SDK via UniFFI |
| CLI and TUI | Android/Termux binaries, or matching Linux ARM64 binaries inside the selected PRoot environment |
| Web UI | Vanilla browser assets served by the owning app |
| MCP | Scoped local/authorized transport with the same core permissions |

macOS/Linux desktop UIs remain native desktop releases; the Android native UI is the corresponding phone interface. Record supported combinations per artifact: OS, ABI, libc/runtime, Android API requirements and engine/driver needs. The Linux host builds Linux/Android artifacts; the Mac builds macOS artifacts.

Package executable components using a supported Android installation mechanism. A downloaded desktop binary is not automatically executable in an app's writable directory. Verify native library loading, subprocess restrictions and package updates on the actual Android API/device. Background work follows [Android foreground-service requirements](https://developer.android.com/develop/background-work/services/fgs); never disable OS protections automatically.

## Acceptance before support claims

1. Build each app's core/server, SDK and every planned interface artifact; distinguish cross-build success from runtime validation.
2. Test native Android and the selected Termux/PRoot path on a real ARM64 device, including install/update/rollback and local connections.
3. Complete a small local CPU inference through Infer, then test remote inference from Assistant/Dictation/Search.
4. Verify memory headroom, cancellation, thermal behavior, sleep/resume and host process cleanup. Measure cold setup, startup, prefill/decode and idle memory separately.
5. Test microphone, storage grants, clipboard and lifecycle where used; report unavailable integrations explicitly.
6. Certify optional GPU acceleration by engine and device. Do not infer CUDA/MLX/vLLM portability from desktop or gaming results.

Android code migration and on-device tests remain pending. No phones, Linux runtimes or new development dependencies are installed by this documentation update.
