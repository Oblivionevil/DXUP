#pragma once

#include <cstdint>
#include <cwchar>

#include "util_config.h"

namespace dxup {

  // Microsoft's adapters (Basic Render Driver, WARP) are not spoofed.
  constexpr uint32_t kMicrosoftVendorId = 0x1414;

  inline bool gpuSpoofed() {
    const UserOverrides& overrides = UserOverrides::get();
    return overrides.spoofVendorId || overrides.spoofDeviceId || !overrides.spoofDescription.empty()
        || overrides.spoofDriverVersion;
  }

  // The adapter description the application sees with a spoofed GPU
  // (dxgi.spoofVendor, dxgi.custom*): DXGI_ADAPTER_DESC, DESC1, DESC2 and
  // DESC3 and the Direct3D 9 identifier's fields alike. Applying it again
  // changes nothing.
  template <typename Desc>
  void spoofAdapterDesc(Desc& desc) {
    const UserOverrides& overrides = UserOverrides::get();
    if (desc.VendorId == kMicrosoftVendorId)
      return;
    if (overrides.spoofVendorId)
      desc.VendorId = overrides.spoofVendorId;
    if (overrides.spoofDeviceId)
      desc.DeviceId = overrides.spoofDeviceId;
    if (!overrides.spoofDescription.empty())
      wcsncpy_s(desc.Description, overrides.spoofDescription.c_str(), _TRUNCATE);
  }

  // The user mode driver version (CheckInterfaceSupport) of the spoofed
  // GPU's maker, for an adapter of the given vendor.
  inline void spoofDriverVersion(uint32_t realVendorId, int64_t& version) {
    const int64_t spoofed = UserOverrides::get().spoofDriverVersion;
    if (spoofed && realVendorId != kMicrosoftVendorId)
      version = spoofed;
  }

}
