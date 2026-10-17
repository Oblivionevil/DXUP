// AMD FSR upscaling through the FidelityFX API on Direct3D 12. The API
// headers (MIT) come from the FidelityFX SDK at build time; AMD's signed
// amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll load
// from DXUP's folder at run time.

#include "upscaler.h"

#if defined(_WIN32) && defined(DXUP_HAVE_FFX)

#include <windows.h>

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

namespace dxup::upscale {

  namespace {

    class FsrUpscaler final : public Upscaler {
    public:
      explicit FsrUpscaler(const CreateInfo& info) : m_info(info) { }

      ~FsrUpscaler() override {
        if (m_context && m_destroy)
          m_destroy(&m_context, nullptr);
        if (m_module)
          FreeLibrary(m_module);
        if (m_provider)
          FreeLibrary(m_provider);
      }

      bool init(std::string& error) {
        // The loader finds its providers among the loaded modules and on the
        // DLL search path, which does not have DXUP's folder: the upscaler
        // provider loads from there first.
        const std::wstring provider = m_info.runtimeDir + L"\\amd_fidelityfx_upscaler_dx12.dll";
        m_provider = LoadLibraryExW(provider.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_provider) {
          error = "amd_fidelityfx_upscaler_dx12.dll not found";
          return false;
        }
        const std::wstring path = m_info.runtimeDir + L"\\amd_fidelityfx_loader_dx12.dll";
        m_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module) {
          error = "amd_fidelityfx_loader_dx12.dll not found";
          return false;
        }
        auto create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m_module, "ffxCreateContext"));
        m_destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m_module, "ffxDestroyContext"));
        m_dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m_module, "ffxDispatch"));
        if (!create || !m_destroy || !m_dispatch) {
          error = "FidelityFX loader without its entry points";
          return false;
        }
        ffxCreateBackendDX12Desc backend = {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = m_info.device;
        ffxCreateContextDescUpscaleVersion version = {};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.version = FFX_UPSCALER_VERSION;
        backend.header.pNext = &version.header;
        ffxCreateContextDescUpscale desc = {};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION
          | (m_info.settings.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0)
          | (m_info.settings.depthInverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0);
        desc.maxRenderSize = { m_info.maxRenderWidth, m_info.maxRenderHeight };
        desc.maxUpscaleSize = { m_info.outputWidth, m_info.outputHeight };
        const ffxReturnCode_t result = create(&m_context, &desc.header, nullptr);
        if (result != FFX_API_RETURN_OK || !m_context) {
          char text[64];
          snprintf(text, sizeof(text), "FSR context creation failed (%u)", unsigned(result));
          error = text;
          m_context = nullptr;
          return false;
        }
        return true;
      }

      bool evaluate(ID3D12GraphicsCommandList* list, const Frame& frame) override {
        ffxDispatchDescUpscale desc = {};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.commandList = list;
        desc.color = ffxApiGetResourceDX12(frame.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth = ffxApiGetResourceDX12(frame.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors = ffxApiGetResourceDX12(frame.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.output = ffxApiGetResourceDX12(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.jitterOffset = { frame.jitterX, frame.jitterY };
        desc.motionVectorScale = { 1.0f, 1.0f };
        desc.renderSize = { frame.renderWidth, frame.renderHeight };
        desc.upscaleSize = { frame.outputWidth, frame.outputHeight };
        desc.enableSharpening = frame.sharpness > 0.0f;
        desc.sharpness = frame.sharpness;
        desc.frameTimeDelta = frame.frameTimeMs;
        desc.preExposure = 1.0f;
        desc.reset = frame.reset;
        desc.cameraNear = frame.cameraNear;
        desc.cameraFar = frame.cameraFar;
        desc.cameraFovAngleVertical = frame.fovY;
        desc.viewSpaceToMetersFactor = 0.01f;
        return m_dispatch(&m_context, &desc.header) == FFX_API_RETURN_OK;
      }

      Kind kind() const override { return Kind::Fsr; }

    private:
      CreateInfo            m_info;
      HMODULE               m_module = nullptr;
      HMODULE               m_provider = nullptr;
      ffxContext            m_context = nullptr;
      PfnFfxDestroyContext  m_destroy = nullptr;
      PfnFfxDispatch        m_dispatch = nullptr;
    };

  }

  std::unique_ptr<Upscaler> createFsr(const CreateInfo& info, std::string& error) {
    auto upscaler = std::make_unique<FsrUpscaler>(info);
    if (!upscaler->init(error))
      return nullptr;
    return upscaler;
  }

}

#endif
