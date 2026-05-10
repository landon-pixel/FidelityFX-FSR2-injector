# fsr2injector

`fsr2injector` is a terminal launcher/attacher for loading a future FSR bridge DLL into a Windows process.

It does not, by itself, make arbitrary applications use FSR2. FSR2 needs render-resolution color, depth, motion vectors, exposure, jitter, and frame timing from the application. Apps that do not expose those resources need a graphics hook layer that intercepts DXGI/Vulkan presentation and either:

- uses a spatial upscaler/sharpener path when only the backbuffer is available, or
- reconstructs enough frame data for a best-effort temporal path.

This utility is the first piece of that pipeline: it handles process launch/attach, architecture checks, and DLL loading.

## Build

Generate the normal FSR2 CMake project, then build the `fsr2injector` target.

```powershell
cmake -S . -B build -DGFX_API_DX12=ON -DGFX_API_VK=OFF
cmake --build build --config Release --target fsr2injector
```

The executable is written to `bin\fsr2injector.exe`.

## Usage

Launch an app and inject a bridge DLL:

```powershell
bin\fsr2injector.exe --dll C:\path\to\fsr2_bridge.dll --exe C:\path\to\game.exe -- --game-args
```

Attach to a running process:

```powershell
bin\fsr2injector.exe --dll C:\path\to\fsr2_bridge.dll --pid 1234
```

List candidate processes:

```powershell
bin\fsr2injector.exe --list
```

Dry-run without injecting:

```powershell
bin\fsr2injector.exe --dry-run --dll C:\path\to\fsr2_bridge.dll --pid 1234
```

## Guardrails

Only inject into software you own or have permission to modify. Do not use this with anti-cheat protected games, competitive multiplayer titles, DRM-protected processes, or software where DLL injection violates the EULA. The injector refuses to cross 32-bit/64-bit architecture boundaries because `LoadLibraryW` injection requires the injector, target, and DLL to match.
