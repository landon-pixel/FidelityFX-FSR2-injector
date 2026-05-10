# Universal Upscaling Injector Roadmap

The repository now has a terminal injector executable, but universal upscaling needs a second component: a graphics bridge DLL.

## Implemented

- `tools/fsr2_injector`: terminal process launcher/attacher.
- `LoadLibraryW` DLL injection for same-architecture Windows processes.
- Suspended launch mode so the bridge can load before the target starts rendering.
- Process listing, dry-run validation, and basic architecture checks.

## Next bridge DLL milestones

1. Create `fsr2_bridge.dll` with a tiny `DllMain`, config loading, and logging.
2. Hook DXGI swap-chain creation and `Present` for D3D11/D3D12 apps.
3. Add a Vulkan implicit layer path for Vulkan apps instead of generic DLL injection.
4. Capture the target backbuffer before presentation.
5. Implement a spatial upscaling fallback using FSR1/RCAS for apps where only color is available.
6. Add optional per-game adapters that can provide real FSR2 inputs such as motion vectors and depth.
7. Add an allowlist/blocklist and refuse known protected multiplayer/anti-cheat processes.

## Why FSR2 cannot be blindly forced everywhere

FSR2 is not just a final-image scaler. Its quality comes from temporal reconstruction, and that reconstruction depends on data the application normally owns internally. Without depth and motion vectors, a bridge can still upscale the final frame, but it cannot reliably recover disocclusion, camera motion, transparency, particle reactivity, or jitter history.

That means the practical universal design is hybrid:

- spatial upscale for generic apps and unsupported games,
- full or near-full FSR2 only for titles where a bridge can obtain the required render data,
- explicit user control so protected or unstable targets are not touched.
