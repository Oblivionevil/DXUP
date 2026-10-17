// NVIDIA DLSS Super Resolution through NGX on Direct3D 12, with DXUP as the
// application (its own project ID). Needs the DLSS SDK at build time and
// nvngx_dlss.dll in DXUP's folder at run time.
//
// This software contains source code provided by NVIDIA Corporation.

#include "upscaler.h"

#if defined(_WIN32) && defined(DXUP_HAVE_DLSS)

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

#include <windows.h>

namespace dxup::upscale {

  namespace {

    // DXUP's NGX project ID (custom engine): a GUID of its own.
    constexpr const char* kProjectId = "8d2f6a41-3c7b-4e59-a1d0-5b9e7c3f2a64";

    // The folder NGX writes its logs to: %TEMP%\DXUP.
    std::wstring dataPath() {
      wchar_t temp[MAX_PATH] = {};
      const DWORD length = GetTempPathW(MAX_PATH, temp);
      std::wstring path = length ? std::wstring(temp, length) : std::wstring(L".\\");
      path += L"DXUP";
      CreateDirectoryW(path.c_str(), nullptr);
      return path;
    }

    std::string resultText(NVSDK_NGX_Result result) {
      char text[32];
      snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
      return text;
    }

    class DlssUpscaler final : public Upscaler {
    public:
      explicit DlssUpscaler(const CreateInfo& info) : m_info(info) { }

      ~DlssUpscaler() override {
        if (m_handle)
          NVSDK_NGX_D3D12_ReleaseFeature(m_handle);
        if (m_params)
          NVSDK_NGX_D3D12_DestroyParameters(m_params);
        if (m_initialized)
          NVSDK_NGX_D3D12_Shutdown1(m_info.device);
      }

      bool init(std::string& error) {
        const wchar_t* paths[] = { m_info.runtimeDir.c_str() };
        NVSDK_NGX_FeatureCommonInfo common = {};
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = 1;
        const std::wstring data = dataPath();
        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
          "0.7", data.c_str(), m_info.device, &common);
        if (NVSDK_NGX_FAILED(result)) {
          error = "NGX init failed " + resultText(result);
          return false;
        }
        m_initialized = true;
        result = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_params);
        if (NVSDK_NGX_FAILED(result) || !m_params) {
          error = "NGX capability parameters failed " + resultText(result);
          return false;
        }
        int available = 0;
        NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (!available) {
          int needsDriver = 0;
          NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
          error = needsDriver ? "DLSS needs a newer driver" : "DLSS is not available on this GPU";
          return false;
        }
        return true;
      }

      bool evaluate(ID3D12GraphicsCommandList* list, const Frame& frame) override {
        if (!m_handle) {
          NVSDK_NGX_DLSS_Create_Params create = {};
          create.Feature.InWidth = m_info.maxRenderWidth;
          create.Feature.InHeight = m_info.maxRenderHeight;
          create.Feature.InTargetWidth = m_info.outputWidth;
          create.Feature.InTargetHeight = m_info.outputHeight;
          // Rendered at the output's size (anti-aliasing only): DLAA.
          create.Feature.InPerfQualityValue = m_info.maxRenderWidth >= m_info.outputWidth
            ? NVSDK_NGX_PerfQuality_Value_DLAA : NVSDK_NGX_PerfQuality_Value_MaxQuality;
          create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
            | (m_info.settings.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0)
            | (m_info.settings.depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
          const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &m_handle, m_params, &create);
          if (NVSDK_NGX_FAILED(result) || !m_handle) {
            m_handle = nullptr;
            return false;
          }
        }
        NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
        eval.Feature.pInColor = frame.color;
        eval.Feature.pInOutput = frame.output;
        eval.pInDepth = frame.depth;
        eval.pInMotionVectors = frame.motion;
        eval.InJitterOffsetX = frame.jitterX;
        eval.InJitterOffsetY = frame.jitterY;
        eval.InRenderSubrectDimensions.Width = frame.renderWidth;
        eval.InRenderSubrectDimensions.Height = frame.renderHeight;
        eval.InReset = frame.reset ? 1 : 0;
        eval.InMVScaleX = 1.0f;
        eval.InMVScaleY = 1.0f;
        eval.InFrameTimeDeltaInMsec = frame.frameTimeMs;
        return NVSDK_NGX_SUCCEED(NGX_D3D12_EVALUATE_DLSS_EXT(list, m_handle, m_params, &eval));
      }

      Kind kind() const override { return Kind::Dlss; }

    private:
      CreateInfo            m_info;
      NVSDK_NGX_Parameter*  m_params = nullptr;
      NVSDK_NGX_Handle*     m_handle = nullptr;
      bool                  m_initialized = false;
    };

  }

  std::unique_ptr<Upscaler> createDlss(const CreateInfo& info, std::string& error) {
    auto upscaler = std::make_unique<DlssUpscaler>(info);
    if (!upscaler->init(error))
      return nullptr;
    return upscaler;
  }

}

#endif
