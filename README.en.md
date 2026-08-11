# Sluice

An audio routing/mixer application for Windows. It aims to provide Voicemeeter-like functionality while also supporting simultaneous use of multiple ASIO devices and an unlimited number of inputs and outputs.

## Current Status: Phase 1 (Mixer Engine In Progress), Phase 1.5/2 partially started

`engine/` contains the major building blocks from implementation guide section 5: device abstraction, engine graph, DSP, and IPC. `ui/` contains a minimal WPF control UI. Phase 1.5 (KS backend) and Phase 2 (virtual ASIO driver) have also been started.

- **device/**: `IAudioDevice` interface and implementations: `AsioDevice` for real ASIO passthrough from Phase 0, `WasapiDevice` for shared-mode capture/render, `ProcessLoopbackDevice` for per-process loopback capture, `KsDevice` for the DirectKS backend (Phase 1.5), and VB-CABLE/VAC virtual device detection.
- **graph/**: strip/bus N x M routing matrix and RCU-style topology replacement (`GraphHandle`).
- **dsp/**: clock drift correction (ASRC + PI control), 4-band EQ, gate, compressor, limiter, and metering.
- **ipc/**: JSON-RPC-like control API over named pipes. `get_devices` returns the device list (lane, effective latency, xrun, ASRC ratio), and a `devices_changed` push notification (`PipeServer::Notify()`) keeps it updated automatically (implementation guide section 5.6).
- **ui/SluiceUi**: system tray resident WPF settings UI. `SluiceUi.Core` connects to engine IPC through `EngineClient` and displays/auto-updates the device list.

**Not integrated yet**: `graph/` (engine graph, N x M routing) builds and tests independently, but `engine/main.cpp` is still the Phase 0 simple 1-to-1 ASIO passthrough demo — it has not yet been wired into a working mixer app using `EngineGraph`, since multi-device integration testing on real hardware is still needed. IPC (`get_devices`/push notifications) is already wired to this Phase-0 device pair (devIn/devOut); the JSON schema (a device array) is designed not to change once `EngineGraph` integration lands.

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
vasio/             virtual ASIO driver (vasio.dll, Phase 2): the COM driver
                   itself, the shared-memory protocol, and offline tests
                   (see vasio/README.md). The engine-side shared-memory
                   consumer is not implemented yet.
Dockerfile.engine.windows   Windows container image for engine build/test
Dockerfile.ui.windows       Windows container image for ui build/test (dotnet SDK)
scripts/           Windows Docker launcher scripts from the repository root
```

Of Phase 3 (kernel virtual audio device), the angle where a DAW reaches it via ASIO is already covered by `engine/device/ks_device.h` (the DirectKS backend). Per implementation guide section 1.2, once a PortCls miniport driver exists it becomes automatically visible from WASAPI, DirectSound, MME, and DirectKS alike, so ASIO hosts can reach it the same way ASIO4ALL/ASIO2KS do (via `KsDevice`) without any ASIO-specific work (implementation guide section 6.2). A future phase is still expected to add `kmdriver/` (the WDM/PortCls miniport driver itself), but that kernel-driver work is currently on hold and not yet started.

## Build and Test

The ASIO SDK is not required. Both `engine/` and `vasio/` build against
[`asio-abi/`](asio-abi/README.md), an independent clean-room ABI
implementation (same approach as wineasio) instead of Steinberg's SDK, so
`sluice-engine.exe`/`vasio.dll` build for real without fetching any
external SDK.

### engine/: real build + core regression tests (Windows)

See `engine/README.md`.

If Windows build tools are not available locally, such as when working from WSL, use Docker Desktop on the Windows host in Windows container mode:

```powershell
# From Windows PowerShell
.\scripts\build-engine-tests-in-windows-docker.ps1
```

`Dockerfile.engine.windows` builds an image with VS Build Tools, CMake, and vcpkg/libsamplerate, then `docker run` mounts the source and executes `engine\scripts\run-tests.ps1`. Both the real `sluice-engine.exe` build and the core tests always run.

### vasio/: virtual ASIO driver build check

```powershell
# From Windows PowerShell
.\scripts\build-vasio-in-windows-docker.ps1
```

Reuses `Dockerfile.engine.windows` and builds `vasio.dll` for real alongside the shared-memory-protocol offline test (`test_shared_protocol`). See [`vasio/README.md`](vasio/README.md) for how to register the driver with `regsvr32` and verify it loads in a real DAW (this cannot be verified inside Docker).

### ui/: WPF build check and IPC client tests

```powershell
# From Windows PowerShell
.\scripts\build-ui-in-windows-docker.ps1
```

`Dockerfile.ui.windows` (dotnet SDK) verifies that `SluiceUi` (WPF) compiles and runs `SluiceUi.Core.Tests` for named-pipe integration tests. Running and visually checking the WPF app itself requires a real Windows desktop environment because containers do not have a desktop.

### Packaging (installer)

`packaging/sluice.iss` (Inno Setup). See [`packaging/README.md`](packaging/README.md) for pending action items around build steps and signing with SignPath Foundation.

## License and Trademark Notes

- **The ASIO SDK is not required.** Neither `engine/` nor `vasio/` uses Steinberg's SDK; both build against [`asio-abi/`](asio-abi/README.md), an independent clean-room ABI implementation (same approach as wineasio).
- "ASIO is a trademark and software of Steinberg Media Technologies GmbH". `asio-abi/` is not provided, endorsed, or reviewed by Steinberg -- it is an independent, ABI-compatible implementation.
- **VB-CABLE** and **VAC (Virtual Audio Cable)** are not bundled or automatically downloaded. Users are only directed to install them from the official sites (implementation guide section 7.2/12).
- **SmartScreen**: `sluice-engine.exe`, `SluiceUi.exe`, and the installer are currently unsigned. SignPath Foundation signing has not started because it requires the repository to be public (CI is already in place, see `.github/workflows/ci.yml`). Windows SmartScreen may show an "unknown publisher" warning on first launch. Users can choose "More info" -> "Run anyway", but this is at their own risk and must be clearly communicated when distributing.
- Detailed design decisions and phase plans live under local `docs/`, but those documents are not included in the repository because they contain personal working notes and are ignored by `.gitignore`.
