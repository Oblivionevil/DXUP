// Intel XeSS Super Resolution on Direct3D 12 (runs on any GPU with shader
// model 6.4 and DP4a). The SDK headers are used at build time; Intel's
// libxess.dll loads from DXUP's folder at run time.

#include "upscaler.h"

#if defined(_WIN32) && defined(DXUP_HAVE_XESS)

#include <windows.h>

#include <algorithm>

#include <xess/xess.h>
#include <xess/xess_d3d12.h>

namespace dxup::upscale {

  namespace {

    using PfnCreateContext = xess_result_t (*)(ID3D12Device*, xess_context_handle_t*);
    using PfnInit = xess_result_t (*)(xess_context_handle_t, const xess_d3d12_init_params_t*);
    using PfnExecute = xess_result_t (*)(xess_context_handle_t, ID3D12GraphicsCommandList*, const xess_d3d12_execute_params_t*);
    using PfnDestroy = xess_result_t (*)(xess_context_handle_t);
    using PfnOptimalInput = xess_result_t (*)(xess_context_handle_t, const xess_2d_t*, xess_quality_settings_t,
                                              xess_2d_t*, xess_2d_t*, xess_2d_t*);

    // The quality mode whose render size covers the given one: XeSS takes
    // any input size up to its mode's.
    xess_quality_settings_t qualityFor(float ratio) {
      if (ratio >= 0.99f) return XESS_QUALITY_SETTING_AA;
      if (ratio >= 1.0f / 1.3f - 0.005f) return XESS_QUALITY_SETTING_ULTRA_QUALITY;
      if (ratio >= 1.0f / 1.5f - 0.005f) return XESS_QUALITY_SETTING_QUALITY;
      if (ratio >= 1.0f / 1.7f - 0.005f) return XESS_QUALITY_SETTING_BALANCED;
      if (ratio >= 1.0f / 2.0f - 0.005f) return XESS_QUALITY_SETTING_PERFORMANCE;
      return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    }

    class XessUpscaler final : public Upscaler {
    public:
      explicit XessUpscaler(const CreateInfo& info) : m_info(info) { }

      ~XessUpscaler() override {
        if (m_context && m_destroy)
          m_destroy(m_context);
        if (m_module)
          FreeLibrary(m_module);
      }

      bool init(std::string& error) {
        const std::wstring path = m_info.runtimeDir + L"\\libxess.dll";
        m_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module) {
          error = "libxess.dll not found";
          return false;
        }
        auto create = reinterpret_cast<PfnCreateContext>(GetProcAddress(m_module, "xessD3D12CreateContext"));
        auto init = reinterpret_cast<PfnInit>(GetProcAddress(m_module, "xessD3D12Init"));
        m_execute = reinterpret_cast<PfnExecute>(GetProcAddress(m_module, "xessD3D12Execute"));
        m_destroy = reinterpret_cast<PfnDestroy>(GetProcAddress(m_module, "xessDestroyContext"));
        if (!create || !init || !m_execute || !m_destroy) {
          error = "libxess.dll without its entry points";
          return false;
        }
        xess_result_t result = create(m_info.device, &m_context);
        if (result != XESS_RESULT_SUCCESS || !m_context) {
          error = "XeSS context creation failed (" + std::to_string(int(result)) + ")";
          m_context = nullptr;
          return false;
        }
        const float ratio = (std::min)(float(m_info.maxRenderWidth) / float(m_info.outputWidth),
                                       float(m_info.maxRenderHeight) / float(m_info.outputHeight));
        xess_d3d12_init_params_t params = {};
        params.outputResolution = { m_info.outputWidth, m_info.outputHeight };
        params.qualitySetting = qualityFor(ratio);
        // XeSS takes input sizes in its mode's range: the mode whose optimal
        // size is nearest to the game's among those that take it.
        auto optimal = reinterpret_cast<PfnOptimalInput>(GetProcAddress(m_module, "xessGetOptimalInputResolution"));
        uint32_t bestDistance = UINT32_MAX;
        for (int mode = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE; optimal && mode <= XESS_QUALITY_SETTING_AA; mode++) {
          xess_2d_t best = {}, low = {}, high = {};
          const auto setting = static_cast<xess_quality_settings_t>(mode);
          if (optimal(m_context, &params.outputResolution, setting, &best, &low, &high) != XESS_RESULT_SUCCESS)
            continue;
          if (m_info.maxRenderWidth < low.x || m_info.maxRenderHeight < low.y
           || m_info.maxRenderWidth > high.x || m_info.maxRenderHeight > high.y)
            continue;
          const uint32_t distance = best.x > m_info.maxRenderWidth ? best.x - m_info.maxRenderWidth : m_info.maxRenderWidth - best.x;
          if (distance < bestDistance) {
            bestDistance = distance;
            params.qualitySetting = setting;
          }
        }
        params.initFlags = XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE
          | (m_info.settings.depthInverted ? XESS_INIT_FLAG_INVERTED_DEPTH : 0)
          | (m_info.settings.hdr ? 0 : XESS_INIT_FLAG_LDR_INPUT_COLOR);
        result = init(m_context, &params);
        if (result != XESS_RESULT_SUCCESS) {
          error = "XeSS init failed (" + std::to_string(int(result)) + ")";
          return false;
        }
        return true;
      }

      bool evaluate(ID3D12GraphicsCommandList* list, const Frame& frame) override {
        xess_d3d12_execute_params_t params = {};
        params.pColorTexture = frame.color;
        params.pVelocityTexture = frame.motion;
        params.pDepthTexture = frame.depth;
        params.pOutputTexture = frame.output;
        params.jitterOffsetX = frame.jitterX;
        params.jitterOffsetY = frame.jitterY;
        params.exposureScale = 1.0f;
        params.resetHistory = frame.reset ? 1u : 0u;
        params.inputWidth = frame.renderWidth;
        params.inputHeight = frame.renderHeight;
        return m_execute(m_context, list, &params) == XESS_RESULT_SUCCESS;
      }

      Kind kind() const override { return Kind::Xess; }

    private:
      CreateInfo             m_info;
      HMODULE                m_module = nullptr;
      xess_context_handle_t  m_context = nullptr;
      PfnExecute             m_execute = nullptr;
      PfnDestroy             m_destroy = nullptr;
    };

  }

  std::unique_ptr<Upscaler> createXess(const CreateInfo& info, std::string& error) {
    auto upscaler = std::make_unique<XessUpscaler>(info);
    if (!upscaler->init(error))
      return nullptr;
    return upscaler;
  }

}

#endif
