# DXUP

[![Downloads](https://img.shields.io/github/downloads/Oblivionevil/DXUP/total)](https://github.com/Oblivionevil/DXUP/releases)

> **Note: this project was written by an AI.** The code, the tests and the
> documentation were written by Anthropic's Claude (Claude Code). The
> maintainer directs the work and tests it in games.

DXUP runs Direct3D 7, 8, 9, 10 and 11 games on Direct3D 12 on Windows. It
replaces the game's Direct3D DLLs with its own, which translate every call to
Direct3D 12.

| API | DLLs next to the game | |
|---|---|---|
| DirectDraw 7 / Direct3D 7 | `ddraw.dll`, `d3d11.dll`, `dxgi.dll` | 32-bit |
| Direct3D 8 | `d3d8.dll`, `d3d11.dll`, `dxgi.dll` | 32-bit |
| Direct3D 9 | `d3d9.dll`, `d3d11.dll`, `dxgi.dll` | 32- and 64-bit |
| Direct3D 10 / 11 | `d3d11.dll`, `dxgi.dll` | 32- and 64-bit |

Besides running the game on Direct3D 12, DXUP can override some of the
game's settings per game, like a driver control panel: anisotropic filtering,
mipmap LOD bias, forced multisampling (MSAA) and supersampling (SGSSAA),
VSync, pre-rendered frames, and rendering at a lower resolution with
DXUP's own upscaling.

## Installation

Download a release from the [Releases](../../releases) page.

- **Setup (`DXUP-Setup-<version>.exe`):** installs DXUP Manager to
  `C:\Program Files\DXUP`. The manager finds Steam and Battle.net games
  (or any `.exe` you pick), copies the right DLLs into each game folder
  (32- or 64-bit, by the game's executable) and keeps the per-game settings.
  Running a newer setup updates every game's copy.
- **Portable (`DXUP-<version>-portable.zip`):** copy the DLLs of the game's
  API and bitness (`x64` or `x86`, see the table above) next to the game's
  executable. Delete them again to go back to Windows' own Direct3D.

Requirements: 64-bit Windows 10 or 11, and a GPU with a Direct3D 12 driver.
The setup and the manager need .NET Framework 4.8.1. The setup is not
signed, so Windows SmartScreen may ask for confirmation.

## Settings

The manager writes each game's settings to `C:\Program Files\DXUP\games.cfg`.
Without the manager, put a `dxup.conf` next to the DLLs (it takes precedence
over `games.cfg`). One `key = value` per line, `#` starts a comment:

```ini
d3d11.samplerAnisotropy = 16   # force anisotropic filtering (0 turns it off)
d3d11.samplerLodBias = -0.5    # added to every sampler's mipmap LOD bias
d3d11.msaaSamples = 8          # samples for multisampled targets (raises only)
d3d11.msaaForce = true         # multisample games that draw without MSAA
d3d11.msaaScope = geometry     # geometry (default) or all draws
d3d11.sgssaa = true            # supersampling where targets are multisampled (or: alpha)
d3d11.asyncShaders = true      # compile pipelines in the background
dxgi.syncInterval = 0          # force VSync off (0) or on (1-4)
dxgi.maxFrameLatency = 1       # pre-rendered frames (1-16)
dxgi.renderScale = 0.75        # render at this share of the resolution, scaled up by DXUP
dxgi.upscale = spatial         # scale a back buffer smaller than the window with DXUP's filter
```

The full list with explanations is in `dxup.conf.example` in the portable
zip and in [`src/util/util_config.h`](src/util/util_config.h).

## Status and known issues

- DXUP is tested on one PC with an NVIDIA GeForce RTX 5090. Other NVIDIA,
  AMD and Intel GPUs are hardly tested yet; bug reports are welcome.
- Tested games include Trine 4, Kena: Bridge of Spirits, Far Cry New Dawn,
  Just Cause 3, God of War, The Witcher 3, Orcs Must Die! 3, Heroes of the
  Storm, Just Cause 2, Gothic II and Mirror's Edge.
- Unreal Engine 4 games and The Witcher 3 take 3-10% more GPU time than
  native Direct3D 11. Unreal's own Direct3D 12 renderer is about as slow in
  the same scenes.
- `dxgi.renderScale` speeds up only games that size their rendering by the
  swap chain.
- **Online games with anti-cheat:** replacing a game's DLLs can look like
  tampering to anti-cheat software. Use DXUP with such games at your own
  risk.

## Building

DXUP builds with Meson, Ninja and the MSVC toolchain (Visual Studio with the
C++ workload and a Windows SDK), from a developer command prompt:

```bat
meson setup build_rel --buildtype=release
ninja -C build_rel
```

For 32-bit games, configure a second build directory from an x86 developer
prompt (`build_rel32`). `build_package.ps1` builds the setup from
`build_rel` and `build_rel32` (it needs the .NET SDK for the manager).

Tests: `src/tests` builds `dx11_test_app`, `d3d9_test_app`,
`d3d8_test_app` and `dx7_test_app`. Each runs its checks on Windows' own
Direct3D and on DXUP when DXUP's DLLs lie next to it.

## License

DXUP is released under the [MIT License](LICENSE).
