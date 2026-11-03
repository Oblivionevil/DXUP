#pragma once

// The legacy core without DXUP's d3d11.dll (d3d9.backend = d3d12): the calls
// it makes on Direct3D 11 objects go straight to DXUP's D3D12 backend, linked
// into this DLL. The objects are as thin as those calls allow: they translate
// and track what the backend needs, without the runtime's checks of what the
// core never does, without a second layer of bindings and without a separate
// module per call.

#include "../util/util_config.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <memory>
#include <string>

namespace dxup::legacy {

  // What devices of this module render on (default: DXUP's d3d11.dll). The
  // front ends that can do without it set their dxup.conf choice;
  // DXUP_D3D9_BACKEND overrides it (d3d11, d3d12, d3d12only).
  void setLegacyBackend(LegacyBackend backend);
  LegacyBackend legacyBackend();

  // What keeps the legacy core on DXUP's Direct3D 11 although d3d9.backend
  // asks for D3D12: settings only that front end implements. Null: nothing.
  const char* directBackendObstacle();

  // A device and its immediate context on the D3D12 backend. vendorId: the
  // PCI vendor of its GPU.
  HRESULT createDirectDevice(ID3D11Device** device, ID3D11DeviceContext** context, uint32_t* vendorId);

  // The private exports of DXUP's d3d11.dll for such a device.
  HRESULT WINAPI directCreateMappedView(IUnknown* device, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                        UINT mapping, ID3D11ShaderResourceView** view);
  HRESULT WINAPI directCreateSampler(IUnknown* device, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler);
  HRESULT WINAPI directWriteBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, UINT offset, const void* data, UINT size);
  HRESULT WINAPI directWriteBufferWith(ID3D11DeviceContext* context, ID3D11Buffer* buffer, UINT offset, UINT size,
                                       void (*fn)(void*, uint8_t*, size_t, const void*, size_t), void* user,
                                       void (*pack)(void*, uint8_t*), void* packUser, UINT dataSize);

  // A flip model swap chain on the device's queue. Its one buffer is always
  // the one Present shows next.
  class DirectSwapChain {
  public:
    virtual ~DirectSwapChain() = default;
    virtual HRESULT getBuffer(ID3D11Texture2D** texture) = 0;
    // Nothing may hold the buffer's views.
    virtual HRESULT resize(UINT width, UINT height) = 0;
    virtual HRESULT present(UINT syncInterval) = 0;
  };

  HRESULT createDirectSwapChain(ID3D11Device* device, HWND window, UINT width, UINT height,
                                std::unique_ptr<DirectSwapChain>& swapChain);

}
