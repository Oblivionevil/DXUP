#pragma once

// ============================================================
// DXGI Stub for D3D12 Backend
//
// When using the D3D12 backend, we bypass any wrapper DXGI layer
// and use native Windows DXGI directly. This stub provides the
// minimal interface needed for D3D11-D3D12 integration.
// ============================================================

#include "../directx/directx_backend.h"

#include <vector>

#ifdef _WIN32
#include <dxgi1_4.h>
#include <d3d12.h>
#include "../util/com_ptr.h"
#endif

namespace dxup {

  // ============================================================
  // DXGI SwapChain Wrapper for D3D12
  //
  // Creates a native DXGI swap chain that works with the
  // D3D12 backend through native Windows DXGI.
  // ============================================================
  class DxgiSwapChainD3D12 {
  public:
#ifdef _WIN32
    struct SwapChainDesc {
      HWND          hwnd           = nullptr;
      uint32_t      width          = 0;
      uint32_t      height         = 0;
      DXGI_FORMAT   format         = DXGI_FORMAT_R8G8B8A8_UNORM;
      uint32_t      bufferCount    = 2;
      bool          windowed       = true;
      uint32_t      refreshRateNum = 60;
      uint32_t      refreshRateDen = 1;
      uint32_t      sampleCount    = 1;
      uint32_t      sampleQuality  = 0;
      bool          vsync          = true;
    };

    DxgiSwapChainD3D12(
      IDXGIFactory4*       factory,
      ID3D12CommandQueue*  commandQueue,
      const SwapChainDesc& desc);

    ~DxgiSwapChainD3D12();

    HRESULT present(uint32_t syncInterval, uint32_t flags);
    HRESULT resize(uint32_t width, uint32_t height);

    uint32_t getCurrentBackBufferIndex() const;
    ID3D12Resource* getBackBuffer(uint32_t index) const;
    uint32_t getBufferCount() const { return m_bufferCount; }

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

  private:
    ComPtr<IDXGISwapChain3>          m_swapChain;
    std::vector<ComPtr<ID3D12Resource>> m_backBuffers;
    uint32_t m_width       = 0;
    uint32_t m_height      = 0;
    uint32_t m_bufferCount = 0;

    void acquireBackBuffers();
#endif
  };

  // ============================================================
  // DXGI Factory Helper
  //
  // Provides utility functions for creating native DXGI objects
  // when using the D3D12 backend.
  // ============================================================
  class DxgiFactoryHelper {
  public:
#ifdef _WIN32
    static HRESULT createFactory(IDXGIFactory4** factory);
    static HRESULT enumerateAdapters(IDXGIFactory4* factory, uint32_t index, IDXGIAdapter1** adapter);
    static HRESULT getAdapterDesc(IDXGIAdapter1* adapter, DXGI_ADAPTER_DESC1* desc);
#endif
  };

}
