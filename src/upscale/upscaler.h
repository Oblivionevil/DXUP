#pragma once

// Temporal upscalers that run on DXUP's Direct3D 12 device in place of a
// game's own temporal anti-aliasing: NVIDIA DLSS Super Resolution, AMD FSR
// (FidelityFX API) and Intel XeSS. Their SDKs stay outside the repository;
// the build includes the ones it finds (meson options dlss_sdk, ffx_sdk,
// xess_sdk) and the runtime DLLs load from DXUP's own folder.

#ifdef _WIN32

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>

namespace dxup::upscale {

  enum class Kind : uint32_t {
    Off  = 0,
    Dlss = 1,
    Fsr  = 2,
    Xess = 3,
  };

  inline const char* kindName(Kind kind) {
    switch (kind) {
      case Kind::Dlss: return "DLSS";
      case Kind::Fsr:  return "FSR";
      case Kind::Xess: return "XeSS";
      default:         return "off";
    }
  }

  // One frame: the inputs at render resolution, the output at display
  // resolution. Inputs come in NON_PIXEL_SHADER_RESOURCE, the output in
  // UNORDERED_ACCESS, and the upscaler leaves them so.
  struct Frame {
    ID3D12Resource* color  = nullptr;  // linear HDR scene color
    ID3D12Resource* depth  = nullptr;  // R32_FLOAT device depth
    ID3D12Resource* motion = nullptr;  // R16G16_FLOAT, pixels from this frame to the previous one
    ID3D12Resource* output = nullptr;
    uint32_t renderWidth  = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth  = 0;
    uint32_t outputHeight = 0;
    // The projection's sub-pixel offset this frame, in render pixels.
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float sharpness   = 0.0f;
    float frameTimeMs = 16.7f;
    float cameraNear  = 10.0f;
    float cameraFar   = 100000.0f;
    float fovY        = 1.0f;
    bool  reset       = false;
  };

  struct Settings {
    bool depthInverted = true;   // 1 at the near plane, 0 at the far one
    bool hdr           = true;
  };

  class Upscaler {
  public:
    virtual ~Upscaler() = default;
    // Records the upscaling into the command list. The list's descriptor
    // heaps, root signatures and pipeline state are the upscaler's afterwards.
    virtual bool evaluate(ID3D12GraphicsCommandList* list, const Frame& frame) = 0;
    virtual Kind kind() const = 0;
  };

  struct CreateInfo {
    Kind          kind = Kind::Off;
    ID3D12Device* device = nullptr;
    std::wstring  runtimeDir;          // folder with the SDK runtime DLLs
    uint32_t      maxRenderWidth  = 0;
    uint32_t      maxRenderHeight = 0;
    uint32_t      outputWidth  = 0;
    uint32_t      outputHeight = 0;
    Settings      settings;
  };

  // Null with the reason in error when the kind is not built in, its runtime
  // is missing or the GPU does not support it.
  std::unique_ptr<Upscaler> create(const CreateInfo& info, std::string& error);

  // The kinds this build includes.
  bool built(Kind kind);

#ifdef DXUP_HAVE_DLSS
  std::unique_ptr<Upscaler> createDlss(const CreateInfo& info, std::string& error);
#endif
#ifdef DXUP_HAVE_FFX
  std::unique_ptr<Upscaler> createFsr(const CreateInfo& info, std::string& error);
#endif
#ifdef DXUP_HAVE_XESS
  std::unique_ptr<Upscaler> createXess(const CreateInfo& info, std::string& error);
#endif

}

#endif
