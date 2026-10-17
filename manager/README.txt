DXUP - Direct3D 7 to 11 on Direct3D 12

DXUP Manager (Start menu) sets up games:
  - "Add game" / drag an EXE onto the window / "Autofind" (installed Steam and
    Battle.net games) add a game and copy DXUP's DLLs (d3d11.dll and
    dxgi.dll, for older games also d3d9.dll, d3d8.dll or ddraw.dll) next to
    its executable. DLLs of other programs found there are kept as
    *.dxup-backup and come back when DXUP is removed from the game.
  - The settings of each game (anisotropic filtering, LOD bias, MSAA, forced
    MSAA, supersampling, VSync, pre-rendered frames, render scale,
    precompiling the shaders of earlier runs, shader compilation in the
    background) are stored in games.cfg in this folder and apply the next
    time the game starts.
  - A dxup.conf next to a game's executable takes precedence over games.cfg.

DXUP supports 32- and 64-bit Direct3D 7 to 11 games. Using DXUP with online
games that run anti-cheat software is at your own risk.

DXUP is released under the MIT License (LICENSE.txt in this folder). It was
written by an AI (Anthropic's Claude); the maintainer directs the work and
tests it in games.

Uninstall DXUP in Windows Settings > Apps: it is removed from all games first.

https://github.com/Oblivionevil/DXUP
