#include "upscaler.h"

#ifdef _WIN32

namespace dxup::upscale {

  bool built(Kind kind) {
#ifdef DXUP_HAVE_DLSS
    if (kind == Kind::Dlss)
      return true;
#endif
#ifdef DXUP_HAVE_FFX
    if (kind == Kind::Fsr)
      return true;
#endif
#ifdef DXUP_HAVE_XESS
    if (kind == Kind::Xess)
      return true;
#endif
    (void)kind;
    return false;
  }

  std::unique_ptr<Upscaler> create(const CreateInfo& info, std::string& error) {
    if (!info.device || !info.maxRenderWidth || !info.maxRenderHeight || !info.outputWidth || !info.outputHeight) {
      error = "invalid sizes";
      return nullptr;
    }
#ifdef DXUP_HAVE_DLSS
    if (info.kind == Kind::Dlss)
      return createDlss(info, error);
#endif
#ifdef DXUP_HAVE_FFX
    if (info.kind == Kind::Fsr)
      return createFsr(info, error);
#endif
#ifdef DXUP_HAVE_XESS
    if (info.kind == Kind::Xess)
      return createXess(info, error);
#endif
    error = std::string(kindName(info.kind)) + " is not part of this build";
    return nullptr;
  }

}

#endif
