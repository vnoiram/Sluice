# Sluice

An audio routing/mixer application for Windows. It aims to provide Voicemeeter-like functionality while also supporting simultaneous use of multiple ASIO devices and an unlimited number of inputs and outputs.

## Current Status: Phase 1 (Mixer Engine In Progress)

`engine/` contains the major building blocks from implementation guide section 5: device abstraction, engine graph, DSP, and IPC. `ui/` contains a minimal WPF control UI.

- **device/**: `IAudioDevice` interface and implementations: `AsioDevice` for real ASIO passthrough from Phase 0, `WasapiDevice` for shared-mode capture/render, `ProcessLoopbackDevice` for per-process loopback capture, and VB-CABLE virtual device detection.
- **graph/**: strip/bus N x M routing matrix and RCU-style topology replacement (`GraphHandle`).
- **dsp/**: clock drift correction (ASRC + PI control), 4-band EQ, gate, compressor, limiter, and metering.
- **ipc/**: JSON-RPC-like control API over named pipes.
- **ui/SluiceUi**: system tray resident WPF settings UI. `SluiceUi.Core` connects to engine IPC through `EngineClient`.

**Not integrated yet**: these parts build and test independently, but `engine/main.cpp` is still the Phase 0 simple 1-to-1 ASIO passthrough demo. It has not yet been wired into a working mixer app using graph/ipc because multi-device integration testing on real hardware is still needed.

See [`engine/README.md`](engine/README.md) for details.

## Repository Layout

```text
engine/
  device/          device abstraction (IAudioDevice) + ASIO/WASAPI/ProcessLoopback/VB-CABLE
  graph/           strips/buses/NxM routing/RCU graph replacement
  dsp/             EQ, gate, compressor, limiter, meter, ASRC/drift correction
  ipc/             named-pipe JSON-RPC server
  rt/              lock-free foundation for RT thread communication, such as SPSC rings
  tests/           regression tests for platform-independent core parts (no ASIO SDK required)
  scripts/         build/test scripts for Windows Docker containers
ui/
  SluiceUi/        WPF app (system tray + settings window)
  SluiceUi.Core/   WPF-independent IPC client (EngineClient)
  SluiceUi.Core.Tests/  EngineClient regression tests
  scripts/         build/test scripts for Windows Docker containers
Dockerfile.engine.windows   Windows container image for engine build/test
Dockerfile.ui.windows       Windows container image for ui build/test (dotnet SDK)
scripts/           Windows Docker launcher scripts from the repository root
```

Future phases are expected to add `vasio/` (virtual ASIO driver, Phase 2) and `kmdriver/` (kernel virtual audio device, Phase 3).

## Build and Test

### engine/: real build with ASIO SDK (Windows, physical machine)

See `engine/README.md`. The ASIO SDK cannot be committed to the repository because of Steinberg's license, so each developer must place it under `engine/thirdparty/asiosdk`.

### engine/: core regression tests only (no ASIO SDK)

If Windows build tools are not available locally, such as when working from WSL, use Docker Desktop on the Windows host in Windows container mode:

```powershell
# From Windows PowerShell
.\scripts\build-engine-tests-in-windows-docker.ps1
```

`Dockerfile.engine.windows` builds an image with VS Build Tools, CMake, and vcpkg/libsamplerate, then `docker run` mounts the source and executes `engine\scripts\run-tests.ps1`. If `engine/thirdparty/asiosdk` exists it also performs the real ASIO build; otherwise it automatically falls back to core tests only.

### ui/: WPF build check and IPC client tests

```powershell
# From Windows PowerShell
.\scripts\build-ui-in-windows-docker.ps1
```

`Dockerfile.ui.windows` (dotnet SDK) verifies that `SluiceUi` (WPF) compiles and runs `SluiceUi.Core.Tests` for named-pipe integration tests. Running and visually checking the WPF app itself requires a real Windows desktop environment because containers do not have a desktop.

### Packaging (installer)

`packaging/sluice.iss` (Inno Setup). See [`packaging/README.md`](packaging/README.md) for pending action items around build steps and signing with SignPath Foundation.

## License and Trademark Notes

- **ASIO SDK** must be obtained by each developer after accepting Steinberg's license. Redistributing SDK source or committing it to this repository is prohibited.
- "ASIO is a trademark and software of Steinberg Media Technologies GmbH".
- **VB-CABLE** is not bundled or automatically downloaded. Users are only directed to install it from the official site (implementation guide section 5.6).
- **SmartScreen**: `sluice-engine.exe`, `SluiceUi.exe`, and the installer are currently unsigned. SignPath Foundation signing has not started because it requires a public repository and CI setup. Windows SmartScreen may show an "unknown publisher" warning on first launch. Users can choose "More info" -> "Run anyway", but this is at their own risk and must be clearly communicated when distributing.
- Detailed design decisions and phase plans live under local `docs/`, but those documents are not included in the repository because they contain personal working notes and are ignored by `.gitignore`.
