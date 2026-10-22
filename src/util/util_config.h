#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dxup {

  // User settings from dxup.conf next to the DLL, read once. One setting per
  // line as "key = value"; '#' starts a comment; keys ignore case. Settings
  // that are missing or do not parse keep the application's behaviour.
  //
  //   d3d11.samplerAnisotropy = 16   force anisotropic filtering (0 turns it off)
  //   d3d11.samplerLodBias = -0.5    add to every sampler's mip LOD bias
  //   d3d11.msaaSamples = 8          render multisampled targets with this many
  //                                  samples (raises, never lowers)
  //   d3d11.msaaForce = true         also multisample render targets and depth
  //                                  buffers of the swap chain's size that the
  //                                  application draws without (experimental)
  //   d3d11.msaaScope = geometry     which draws msaaForce multisamples: those
  //                                  with a depth buffer bound, and those onto
  //                                  multisampled contents (geometry, default),
  //                                  or every draw onto such a target (all)
  //   d3d11.sgssaa = true            supersample where targets are multisampled
  //                                  (pixel shaders run per sample); alpha
  //                                  only supersamples shaders that discard
  //                                  pixels (alpha-tested foliage, fences)
  //   d3d11.asyncShaders = true      compile shaders (pipelines) on background
  //                                  threads; draws skip while theirs compile
  //   d3d11.upscaler = dlss          replace the game's temporal anti-aliasing
  //                                  with DLSS, FSR or XeSS (dlss, fsr, xess,
  //                                  off); games DXUP knows the pass of
  //   d3d11.upscalerSharpness = 0.3  FSR's sharpening (0 turns it off)
  //   d3d11.upscalerShader = <hash>  another Unreal Engine 4 TAA shader to
  //                                  replace (hash as the dispatch trace shows)
  //   d3d11.upscalerAuto = true      find the game's TAA pass by itself (the
  //                                  pass whose output it reads back the next
  //                                  frame) when no known pass matches
  //   d3d11.upscalerInject = true    temporal anti-aliasing with the upscaler
  //                                  for games without TAA (DXUP jitters the
  //                                  scene and works out the camera motion);
  //                                  upscalerAuto turns to it when no TAA pass
  //                                  shows for half a minute
  //   dxgi.upscale = spatial         a back buffer smaller than the window is
  //                                  scaled up at Present by DXUP (Catmull-Rom
  //                                  and contrast-adaptive sharpening) instead
  //                                  of by the system's bilinear stretch
  //   dxgi.upscaleSharpness = 0.2    its sharpening (0 turns it off)
  //   dxgi.renderScale = 0.75        the application gets back buffers of this
  //                                  share of the size it asks for (0.33-1)
  //                                  and renders at that size; DXUP scales
  //                                  them up at Present (as dxgi.upscale)
  //   dxgi.syncInterval = 0          force VSync off (0) or on (1-4)
  //   dxgi.directBackBuffer = false  the application draws into a copy of
  //                                  the back buffer that DXUP copies at
  //                                  Present (default: into the real buffer)
  //   dxgi.maxFrameLatency = 1       force the number of pre-rendered frames (1-16)
  //   dxgi.spoofVendor = nvidia      the application sees a GPU of this maker
  //                                  (nvidia, amd, intel): its name, ids and
  //                                  a driver version of that maker; DXUP
  //                                  still renders on the real one
  //   dxgi.customVendorId = 10de     the ids (hex) and name the application
  //   dxgi.customDeviceId = 2503     sees, over those of spoofVendor
  //   dxgi.customDeviceDesc = ...
  //   d3d12.recordThread = true      experimental: a second worker thread
  //                                  makes the calls on the Direct3D 12
  //                                  command list that the first prepares
  //                                  (about 4% slower on NVIDIA in DXUP's
  //                                  benchmarks, see CommandListRecorder)
  class Config {
  public:
    static const Config& get();

    // Parses the text of a configuration file.
    explicit Config(const std::string& text);

    // The value of key, or fallback when unset or not of that type.
    int32_t getInt(const char* key, int32_t fallback) const;
    float   getFloat(const char* key, float fallback) const;
    // true/false, yes/no, on/off or 1/0.
    bool    getBool(const char* key, bool fallback) const;
    // The value in lower case.
    std::string getString(const char* key, const char* fallback) const;
    // The value as written.
    std::string getText(const char* key, const char* fallback) const;

  private:
    const std::string* find(const char* key) const;

    std::vector<std::pair<std::string, std::string>> m_entries;
  };

  // Where DXUP is installed (DXUP_INSTALL_DIR, else the setup's registry
  // entry); empty when it is not.
  std::wstring dxupInstallFolder();

  // Draws that forced multisampling (msaaForce) renders multisampled.
  enum class MsaaScope : uint8_t { Geometry, All };

  // Pixel shaders that run per sample on multisampled targets.
  enum class SgssaaMode : uint8_t { Off, Full, Alpha };

  // The settings DXUP applies, with -1 meaning "as the application asks".
  struct UserOverrides {
    int32_t samplerAnisotropy = -1;
    int32_t syncInterval      = -1;
    int32_t maxFrameLatency   = -1;
    // 0 when unset.
    int32_t msaaSamples       = 0;
    bool    msaaForce         = false;
    MsaaScope  msaaScope      = MsaaScope::Geometry;
    SgssaaMode sgssaa         = SgssaaMode::Off;
    // Pipelines compile on background threads; draws whose pipeline still compiles are skipped.
    bool    asyncShaders      = false;
    // Pipelines of earlier runs compile in the background once their shaders exist.
    bool    shaderPrecompile  = true;
    // Added to every sampler's LOD bias.
    float   samplerLodBias    = 0.0f;
    // The upscaler in place of the game's TAA: 0 off, 1 DLSS, 2 FSR, 3 XeSS
    // (upscale::Kind), its sharpening, and a further UE4 TAA shader hash.
    uint32_t upscaler         = 0;
    float    upscalerSharpness = 0.0f;
    uint64_t upscalerShader   = 0;
    // Multiplies the game's jitter (diagnostics: -1 flips an axis).
    float    upscalerJitterScale[2] = { 1.0f, 1.0f };
    // The game's TAA pass found by its feedback (d3d11.upscalerAuto).
    bool     upscalerAuto     = false;
    // DXUP's own temporal anti-aliasing for games without (d3d11.upscalerInject).
    bool     upscalerInject   = false;
    // The profiles DXUP brings (Kena); false lets upscalerAuto find the pass
    // there too (diagnostics: d3d11.upscalerBuiltin).
    bool     upscalerBuiltin  = true;
    // Present scales a smaller back buffer up to the window (dxgi.upscale).
    bool     spatialUpscale   = false;
    float    spatialSharpness = 0.2f;
    // The share of the asked size the application's back buffers get (1: all).
    float    renderScale      = 1.0f;
    // Draws and dispatches of the same pipeline in a row write the same
    // unordered access views without barriers between them, as in NVIDIA's
    // and AMD's UAV overlap extensions that games use natively (d3d11.
    // relaxedUavBarriers; on by default for God of War).
    bool     relaxedUavBarriers = false;
    // Barrier batches that wait for nothing move to an earlier batch
    // (d3d12.barrierHoisting, see D3D12BackendContext).
    bool     barrierHoisting  = true;
    // The CS worker's calls on the command list are made by a thread of
    // their own (d3d12.recordThread, see CommandListRecorder).
    bool     recordThread     = false;
    // Pixel shader writes no render target takes leave the shader, as
    // Direct3D 11 drivers compile it for the bound targets
    // (d3d12.pixelOutputMasks).
    bool     pixelOutputMasks = true;
    // Render targets get their memory when first written, as Direct3D 11
    // drivers do (d3d12.lazyTargets): -1 NVIDIA only, 0 off, 1 on.
    int32_t  lazyTargets      = -1;
    // 0 times infinity or NaN in Direct3D 9 shaders (d3d9.floatEmulation;
    // Mirror's Edge has NaN in its data): -1 auto, 0 in the dot products
    // (as NVIDIA's and Intel's drivers), mul and mad IEEE (as Intel's; DXVK
    // too where the GPU has no such multiply: emulating it in mul and mad
    // nearly doubles what shaders cost to compile, see d3d9_shader_test
    // --compile-records), and rcp, rsq, log of 0 finite, so the shader makes
    // no infinities itself; 1 "strict", 0 also in mul and mad (as NVIDIA's
    // driver); 0 IEEE (NaN) everywhere.
    int32_t  d3d9FloatEmulation = -1;
    // A vertex with NaN depth (d3d9.nanDepth): -1 auto, as NVIDIA's driver
    // its primitive is not drawn, as Intel's it is drawn at depth 0; 0 not
    // drawn; 1 drawn at depth 0.
    int32_t  d3d9NanDepth = -1;
    // Direct3D 7-9 shaders straight to DXBC by DXUP (d3d9.shaderCompiler =
    // dxup, default), a fraction of a millisecond per shader, or as HLSL
    // through Microsoft's compiler (fxc: 20-120 ms per large shader, cached
    // on disk). Shaders DXUP's compiler does not handle take fxc either way.
    bool     d3d9DxupCompiler = true;
    // Flip model applications draw into the real swap chain buffers, no
    // proxy copied at Present (dxgi.directBackBuffer).
    bool     directBackBuffer = true;
    // The GPU DXUP renders on, whatever the application asks for: part of
    // its name in lower case ("intel", "uhd", "rtx 4070"), or "warp" for
    // Microsoft's software renderer (dxgi.adapter). Empty: the
    // application's choice, else the first GPU.
    std::string adapter;
    // The GPU the application sees (dxgi.spoofVendor, dxgi.customVendorId,
    // dxgi.customDeviceId, dxgi.customDeviceDesc); 0 and empty keep the
    // real one's. Games choose paths, settings and workarounds by the maker,
    // and some refuse to start on one.
    uint32_t    spoofVendorId = 0;
    uint32_t    spoofDeviceId = 0;
    std::wstring spoofDescription;
    // The user mode driver version DXGI reports (CheckInterfaceSupport):
    // games read the maker's version number out of it. 0 keeps the real one.
    int64_t     spoofDriverVersion = 0;

    static UserOverrides fromConfig(const Config& config);
    static const UserOverrides& get();
  };

}
