#include "dxgi_stub.h"

#ifdef _WIN32
#include <stdexcept>

namespace dxup {

  // ============================================================
  // DxgiSwapChainD3D12
  // ============================================================

  DxgiSwapChainD3D12::DxgiSwapChainD3D12(
    IDXGIFactory4*       factory,
    ID3D12CommandQueue*  commandQueue,
    const SwapChainDesc& desc)
  : m_width(desc.width)
  , m_height(desc.height)
  , m_bufferCount(desc.bufferCount) {

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width       = desc.width;
    swapChainDesc.Height      = desc.height;
    swapChainDesc.Format      = desc.format;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = desc.bufferCount;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count   = 1;
    swapChainDesc.SampleDesc.Quality = 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = factory->CreateSwapChainForHwnd(
      commandQueue,
      desc.hwnd,
      &swapChainDesc,
      nullptr,    // Fullscreen desc
      nullptr,    // Restrict to output
      swapChain1.ReleaseAndGetAddressOf());

    if (FAILED(hr))
      throw std::runtime_error("Failed to create swap chain");

    // Disable Alt+Enter fullscreen toggle
    factory->MakeWindowAssociation(desc.hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr))
      throw std::runtime_error("Failed to get IDXGISwapChain3");

    acquireBackBuffers();
  }

  DxgiSwapChainD3D12::~DxgiSwapChainD3D12() {
  }

  HRESULT DxgiSwapChainD3D12::present(uint32_t syncInterval, uint32_t flags) {
    return m_swapChain->Present(syncInterval, flags);
  }

  HRESULT DxgiSwapChainD3D12::resize(uint32_t width, uint32_t height) {
    m_backBuffers.clear();

    HRESULT hr = m_swapChain->ResizeBuffers(
      m_bufferCount, width, height,
      DXGI_FORMAT_UNKNOWN, 0);

    if (SUCCEEDED(hr)) {
      m_width  = width;
      m_height = height;
      acquireBackBuffers();
    }

    return hr;
  }

  uint32_t DxgiSwapChainD3D12::getCurrentBackBufferIndex() const {
    return m_swapChain->GetCurrentBackBufferIndex();
  }

  ID3D12Resource* DxgiSwapChainD3D12::getBackBuffer(uint32_t index) const {
    if (index >= m_backBuffers.size())
      return nullptr;
    return m_backBuffers[index].Get();
  }

  void DxgiSwapChainD3D12::acquireBackBuffers() {
    m_backBuffers.resize(m_bufferCount);
    for (uint32_t i = 0; i < m_bufferCount; i++) {
      m_swapChain->GetBuffer(i, __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(m_backBuffers[i].GetAddressOf()));
    }
  }

  // ============================================================
  // DxgiFactoryHelper
  // ============================================================

  HRESULT DxgiFactoryHelper::createFactory(IDXGIFactory4** factory) {
    UINT flags = 0;
#ifdef _DEBUG
    flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    return CreateDXGIFactory2(flags, IID_PPV_ARGS(factory));
  }

  HRESULT DxgiFactoryHelper::enumerateAdapters(IDXGIFactory4* factory, uint32_t index, IDXGIAdapter1** adapter) {
    return factory->EnumAdapters1(index, adapter);
  }

  HRESULT DxgiFactoryHelper::getAdapterDesc(IDXGIAdapter1* adapter, DXGI_ADAPTER_DESC1* desc) {
    return adapter->GetDesc1(desc);
  }

}

#endif // _WIN32
