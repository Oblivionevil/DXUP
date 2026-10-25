#pragma once

// ============================================================
// DXGI COM Interface Implementations
//
// Full COM wrappers for DXGI interfaces needed by games.
// These wrap native DXGI objects and integrate with our
// D3D11 device layer, routing swap chain operations through
// the D3D12 backend's command queue.
// ============================================================

#ifdef _WIN32

#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cstring>
#include <string>
#include <new>

#include "../util/util_paths.h"
#include "d3d11_device.h"
#include "d3d11_context.h"
#include "../d3d12/d3d12_backend_device.h"
#include "../d3d12/d3d12_backend_context.h"
#include "../util/com_ptr.h"
#include "../util/util_config.h"
#include "../util/util_gpu_spoof.h"

namespace dxup {

  // D3D11Device and D3D11Texture2D available via d3d11_device.h

  inline bool dxupFpsOverlayEnabled() {
    static bool enabled = dxupSiblingExists("dxup_show_fps_overlay.txt");
    return enabled;
  }


  // Diagnostics: with dxup_frame_dump.txt next to the DLL, the back buffer
  // the application rendered is saved as dxup_frame_<n>.bmp at a few
  // presents.
  inline bool dxupFrameDumpEnabled() {
    static const bool enabled = [] {
      const DWORD attributes = GetFileAttributesA(dxupSiblingPath("dxup_frame_dump.txt").c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }();
    return enabled;
  }

  // With dxup_blocking_present.txt next to the DLL, the real swap chain
  // has no frame latency object of DXUP's and Present blocks for the next
  // buffer as before, for comparison.
  inline bool dxupBlockingPresent() {
    static const bool enabled = [] {
      const DWORD attributes = GetFileAttributesA(dxupSiblingPath("dxup_blocking_present.txt").c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }();
    return enabled;
  }

  // With dxup_sync_present.txt next to the DLL, presents run on the
  // application thread after the worker recorded the frame, for comparison.
  inline bool dxupSyncPresentEnabled() {
    static const bool enabled = [] {
      const DWORD attributes = GetFileAttributesA(dxupSiblingPath("dxup_sync_present.txt").c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }();
    return enabled;
  }

  // Forward declarations within this header
  class DXGIFactory;
  class DXGIAdapter;
  class DXGIOutput;
  inline ComPtr<IDXGIOutput> realOutputOf(IDXGIOutput* output);
  class DXGISwapChain;
  class DXGIDevice;

  // ============================================================
  // DXGIFactory : IDXGIFactory7
  //
  // Wraps a real IDXGIFactory4 and returns our custom adapter/
  // swap chain objects.
  // ============================================================
  class DXGIFactory : public IDXGIFactory7 {
  public:
    explicit DXGIFactory(ComPtr<IDXGIFactory4> realFactory)
    : m_refCount(1)
    , m_realFactory(std::move(realFactory)) {
    }

    ~DXGIFactory() = default;

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDXGIObject)
       || riid == __uuidof(IDXGIFactory)
       || riid == __uuidof(IDXGIFactory1)
       || riid == __uuidof(IDXGIFactory2)
       || riid == __uuidof(IDXGIFactory3)
       || riid == __uuidof(IDXGIFactory4)
       || riid == __uuidof(IDXGIFactory5)
       || riid == __uuidof(IDXGIFactory6)
       || riid == __uuidof(IDXGIFactory7)) {
        *ppvObject = static_cast<IDXGIFactory7*>(this);
        AddRef();
        return S_OK;
      }

      *ppvObject = nullptr;
      logMessage(LogLevel::Debug, "DXGIFactory::QueryInterface unsupported riid=%s",
        formatGuid(riid).c_str());
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      ULONG ref = --m_refCount;
      if (ref == 0 && !processExiting()) delete this;
      return ref;
    }

    // ---- IDXGIObject ----

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
      return m_realFactory->SetPrivateData(Name, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
      return m_realFactory->SetPrivateDataInterface(Name, pUnknown);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
      return m_realFactory->GetPrivateData(Name, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
      return m_realFactory->GetParent(riid, ppParent);
    }

    // ---- IDXGIFactory ----

    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override;

    // DXUP's swap chains belong to the factory of its D3D12 device: the
    // association goes there too (Alt+Enter turned off by the application).
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override {
      const HRESULT hr = m_realFactory->MakeWindowAssociation(WindowHandle, Flags);
      m_associatedWindow = WindowHandle;
      m_associationFlags = Flags;
      if (SUCCEEDED(hr) && m_backendFactory && m_backendFactory.Get() != m_realFactory.Get())
        m_backendFactory->MakeWindowAssociation(WindowHandle, Flags | (m_backendWindowed ? DXGI_MWA_NO_ALT_ENTER : 0u));
      return hr;
    }

    void associateBackendFactory(IDXGIFactory4* factory, HWND window, bool windowed) {
      if (!factory)
        return;
      m_backendFactory = factory;
      m_backendWindowed = windowed;
      UINT flags = windowed ? DXGI_MWA_NO_ALT_ENTER : 0u;
      if (m_associatedWindow == window)
        flags |= m_associationFlags;
      if (flags)
        factory->MakeWindowAssociation(window, flags);
    }

    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override {
      return m_realFactory->GetWindowAssociation(pWindowHandle);
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IUnknown*                 pDevice,
      DXGI_SWAP_CHAIN_DESC*     pDesc,
      IDXGISwapChain**          ppSwapChain) override;

    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override {
      return m_realFactory->CreateSoftwareAdapter(Module, ppAdapter);
    }

    // ---- IDXGIFactory1 ----

    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override;

    BOOL STDMETHODCALLTYPE IsCurrent() override {
      // Delegate to the real factory if it supports IDXGIFactory1
      ComPtr<IDXGIFactory1> factory1;
      if (SUCCEEDED(m_realFactory.As(&factory1)))
        return factory1->IsCurrent();
      return TRUE;
    }

    // ---- IDXGIFactory2 ----

    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override {
      ComPtr<IDXGIFactory2> factory2;
      return SUCCEEDED(m_realFactory.As(&factory2)) ? factory2->IsWindowedStereoEnabled() : FALSE;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
      IUnknown*                         pDevice,
      HWND                              hWnd,
      const DXGI_SWAP_CHAIN_DESC1*      pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
      IDXGIOutput*                      pRestrictToOutput,
      IDXGISwapChain1**                 ppSwapChain) override;

    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
      IUnknown*                         pDevice,
      IUnknown*                         pWindow,
      const DXGI_SWAP_CHAIN_DESC1*      pDesc,
      IDXGIOutput*                      pRestrictToOutput,
      IDXGISwapChain1**                 ppSwapChain) override {
      ComPtr<IDXGIFactory2> factory2;
      if (FAILED(m_realFactory.As(&factory2)))
        return E_NOINTERFACE;

      if (pDevice) {
        ComPtr<ID3D11Device5> d3d11Device;
        if (SUCCEEDED(pDevice->QueryInterface(kDxupD3D11DeviceIid, reinterpret_cast<void**>(d3d11Device.GetAddressOf())))) {
          logMessage(LogLevel::Warn, "DXGIFactory::CreateSwapChainForCoreWindow is not implemented for wrapped D3D11 devices");
          return E_NOTIMPL;
        }
      }

      return factory2->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, realOutputOf(pRestrictToOutput).Get(), ppSwapChain);
    }

    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override {
      ComPtr<IDXGIFactory2> factory2;
      return SUCCEEDED(m_realFactory.As(&factory2))
        ? factory2->GetSharedResourceAdapterLuid(hResource, pLuid)
        : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override {
      ComPtr<IDXGIFactory2> factory2;
      return SUCCEEDED(m_realFactory.As(&factory2))
        ? factory2->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie)
        : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override {
      ComPtr<IDXGIFactory2> factory2;
      return SUCCEEDED(m_realFactory.As(&factory2))
        ? factory2->RegisterStereoStatusEvent(hEvent, pdwCookie)
        : E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override {
      ComPtr<IDXGIFactory2> factory2;
      if (SUCCEEDED(m_realFactory.As(&factory2)))
        factory2->UnregisterStereoStatus(dwCookie);
    }

    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override {
      ComPtr<IDXGIFactory2> factory2;
      return SUCCEEDED(m_realFactory.As(&factory2))
        ? factory2->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie)
        : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override {
      ComPtr<IDXGIFactory2> factory2;
      return SUCCEEDED(m_realFactory.As(&factory2))
        ? factory2->RegisterOcclusionStatusEvent(hEvent, pdwCookie)
        : E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override {
      ComPtr<IDXGIFactory2> factory2;
      if (SUCCEEDED(m_realFactory.As(&factory2)))
        factory2->UnregisterOcclusionStatus(dwCookie);
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
      IUnknown*                    pDevice,
      const DXGI_SWAP_CHAIN_DESC1* pDesc,
      IDXGIOutput*                 pRestrictToOutput,
      IDXGISwapChain1**            ppSwapChain) override;

    // ---- IDXGIFactory3 ----

    UINT STDMETHODCALLTYPE GetCreationFlags() override {
      ComPtr<IDXGIFactory3> factory3;
      return SUCCEEDED(m_realFactory.As(&factory3)) ? factory3->GetCreationFlags() : 0;
    }

    // ---- IDXGIFactory4 ----

    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override;

    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override {
      return m_realFactory->EnumWarpAdapter(riid, ppvAdapter);
    }

    // ---- IDXGIFactory5 ----

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
      DXGI_FEATURE Feature,
      void* pFeatureSupportData,
      UINT FeatureSupportDataSize) override {
      ComPtr<IDXGIFactory5> factory5;
      HRESULT hr = m_realFactory.As(&factory5);
      return SUCCEEDED(hr)
        ? factory5->CheckFeatureSupport(
            Feature, pFeatureSupportData, FeatureSupportDataSize)
        : hr;
    }

    // ---- IDXGIFactory6 ----

    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(
      UINT Adapter,
      DXGI_GPU_PREFERENCE GpuPreference,
      REFIID riid,
      void** ppvAdapter) override;

    // ---- IDXGIFactory7 ----

    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(
      HANDLE hEvent,
      DWORD* pdwCookie) override {
      ComPtr<IDXGIFactory7> factory7;
      HRESULT hr = m_realFactory.As(&factory7);
      return SUCCEEDED(hr)
        ? factory7->RegisterAdaptersChangedEvent(hEvent, pdwCookie)
        : hr;
    }

    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(
      DWORD dwCookie) override {
      ComPtr<IDXGIFactory7> factory7;
      HRESULT hr = m_realFactory.As(&factory7);
      return SUCCEEDED(hr)
        ? factory7->UnregisterAdaptersChangedEvent(dwCookie)
        : hr;
    }

    // ---- Accessors ----

    IDXGIFactory4* getRealFactory() const { return m_realFactory.Get(); }

  private:
    std::atomic<ULONG>    m_refCount;
    ComPtr<IDXGIFactory4> m_realFactory;
    ComPtr<IDXGIFactory4> m_backendFactory;   // of the last swap chain created
    bool                  m_backendWindowed = false;
    HWND                  m_associatedWindow = nullptr;
    UINT                  m_associationFlags = 0;
  };


  // ============================================================
  // DXGIAdapter : IDXGIAdapter4
  //
  // Wraps a real IDXGIAdapter1 and returns our custom output
  // objects, and the spoofed GPU's description (dxgi.spoofVendor).
  // ============================================================
  class DXGIAdapter : public IDXGIAdapter4 {
  public:
    DXGIAdapter(ComPtr<IDXGIAdapter1> realAdapter, DXGIFactory* factory)
    : m_refCount(1)
    , m_realAdapter(std::move(realAdapter))
    , m_factory(factory) {
      if (m_factory) m_factory->AddRef();
      // With a spoofed GPU the later adapter interfaces are answered here
      // too, so that their descriptions show it.
      if (m_realAdapter && gpuSpoofed()) {
        m_realAdapter.As(&m_realAdapter2);
        m_realAdapter.As(&m_realAdapter3);
        m_realAdapter.As(&m_realAdapter4);
      }
    }

    ~DXGIAdapter() {
      if (m_factory) m_factory->Release();
    }

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDXGIObject)
       || riid == __uuidof(IDXGIAdapter)
       || riid == __uuidof(IDXGIAdapter1)
       || (riid == __uuidof(IDXGIAdapter2) && m_realAdapter2)
       || (riid == __uuidof(IDXGIAdapter3) && m_realAdapter3)
       || (riid == __uuidof(IDXGIAdapter4) && m_realAdapter4)) {
        *ppvObject = static_cast<IDXGIAdapter4*>(this);
        AddRef();
        return S_OK;
      }

      // Everything else is the real adapter's: newer adapter interfaces, and
      // the ones Windows' own runtime asks for when an application (NVAPI in
      // Just Cause 2) hands this adapter to the system's d3d10_1.dll, which
      // loads the system's d3d11.dll; it uses IDXGIAdapter4 without checking.
      if (!m_realAdapter) {
        *ppvObject = nullptr;
        return E_NOINTERFACE;
      }
      const HRESULT hr = m_realAdapter->QueryInterface(riid, ppvObject);
      if (FAILED(hr))
        logMessage(LogLevel::Debug, "DXGIAdapter::QueryInterface unsupported riid=%s", formatGuid(riid).c_str());
      return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      ULONG ref = --m_refCount;
      if (ref == 0 && !processExiting()) delete this;
      return ref;
    }

    // ---- IDXGIObject ----

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
      return m_realAdapter->SetPrivateData(Name, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
      return m_realAdapter->SetPrivateDataInterface(Name, pUnknown);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
      return m_realAdapter->GetPrivateData(Name, pDataSize, pData);
    }

    // The factory under any of its interfaces (God of War asks for
    // IDXGIFactory5 and does not check).
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
      if (!ppParent) return E_POINTER;
      if (m_factory)
        return m_factory->QueryInterface(riid, ppParent);
      *ppParent = nullptr;
      return E_NOINTERFACE;
    }

    // ---- IDXGIAdapter ----

    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override;

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override {
      if (!pDesc) return E_POINTER;
      const HRESULT hr = m_realAdapter->GetDesc(pDesc);
      if (SUCCEEDED(hr))
        spoofAdapterDesc(*pDesc);
      return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override {
      const HRESULT hr = m_realAdapter->CheckInterfaceSupport(InterfaceName, pUMDVersion);
      DXGI_ADAPTER_DESC1 real = {};
      if (SUCCEEDED(hr) && pUMDVersion && SUCCEEDED(m_realAdapter->GetDesc1(&real)))
        spoofDriverVersion(real.VendorId, pUMDVersion->QuadPart);
      return hr;
    }

    // ---- IDXGIAdapter1 ----

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override {
      if (!pDesc) return E_POINTER;
      const HRESULT hr = m_realAdapter->GetDesc1(pDesc);
      if (SUCCEEDED(hr))
        spoofAdapterDesc(*pDesc);
      return hr;
    }

    // ---- IDXGIAdapter2-4 (with a spoofed GPU only) ----

    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* pDesc) override {
      if (!pDesc) return E_POINTER;
      if (!m_realAdapter2) return E_NOINTERFACE;
      const HRESULT hr = m_realAdapter2->GetDesc2(pDesc);
      if (SUCCEEDED(hr))
        spoofAdapterDesc(*pDesc);
      return hr;
    }

    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override {
      return m_realAdapter3 ? m_realAdapter3->RegisterHardwareContentProtectionTeardownStatusEvent(hEvent, pdwCookie) : E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) override {
      if (m_realAdapter3)
        m_realAdapter3->UnregisterHardwareContentProtectionTeardownStatus(dwCookie);
    }

    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                   DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) override {
      return m_realAdapter3 ? m_realAdapter3->QueryVideoMemoryInfo(NodeIndex, MemorySegmentGroup, pVideoMemoryInfo) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                        UINT64 Reservation) override {
      return m_realAdapter3 ? m_realAdapter3->SetVideoMemoryReservation(NodeIndex, MemorySegmentGroup, Reservation) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent, DWORD* pdwCookie) override {
      return m_realAdapter3 ? m_realAdapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(hEvent, pdwCookie) : E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) override {
      if (m_realAdapter3)
        m_realAdapter3->UnregisterVideoMemoryBudgetChangeNotification(dwCookie);
    }

    HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3* pDesc) override {
      if (!pDesc) return E_POINTER;
      if (!m_realAdapter4) return E_NOINTERFACE;
      const HRESULT hr = m_realAdapter4->GetDesc3(pDesc);
      if (SUCCEEDED(hr))
        spoofAdapterDesc(*pDesc);
      return hr;
    }

    // ---- Accessors ----

    IDXGIAdapter1*  getRealAdapter() const { return m_realAdapter.Get(); }
    DXGIFactory*    getFactory()     const { return m_factory; }

  private:
    std::atomic<ULONG>    m_refCount;
    ComPtr<IDXGIAdapter1> m_realAdapter;
    // Set with a spoofed GPU only.
    ComPtr<IDXGIAdapter2> m_realAdapter2;
    ComPtr<IDXGIAdapter3> m_realAdapter3;
    ComPtr<IDXGIAdapter4> m_realAdapter4;
    DXGIFactory*          m_factory;
  };


  // ============================================================
  // DXGIOutput : IDXGIOutput
  //
  // Wraps a real IDXGIOutput, delegating most methods directly.
  // ============================================================
  // Private interface ID DXUP's DXGIOutput answers with itself.
  static constexpr GUID kDxupOutputIid =
    { 0x8f3d2a61, 0x1c4e, 0x4b7a, { 0xa5, 0x92, 0x6e, 0x0d, 0x31, 0xc8, 0x4f, 0x27 } };

  class DXGIOutput : public IDXGIOutput1 {
  public:
    DXGIOutput(ComPtr<IDXGIOutput> realOutput, DXGIAdapter* adapter)
    : m_refCount(1)
    , m_realOutput(std::move(realOutput))
    , m_adapter(adapter) {
      if (m_adapter) m_adapter->AddRef();
      if (m_realOutput)
        m_realOutput.As(&m_realOutput1);
    }

    ~DXGIOutput() {
      if (m_adapter) m_adapter->Release();
    }

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDXGIObject)
       || riid == __uuidof(IDXGIOutput)
       || (riid == __uuidof(IDXGIOutput1) && m_realOutput1)
       || riid == kDxupOutputIid) {
        *ppvObject = static_cast<IDXGIOutput1*>(this);
        AddRef();
        return S_OK;
      }

      // Newer output interfaces (IDXGIOutput6 for HDR) are the real output's.
      if (!m_realOutput) {
        *ppvObject = nullptr;
        return E_NOINTERFACE;
      }
      return m_realOutput->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      ULONG ref = --m_refCount;
      if (ref == 0 && !processExiting()) delete this;
      return ref;
    }

    // ---- IDXGIObject ----

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
      return m_realOutput->SetPrivateData(Name, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
      return m_realOutput->SetPrivateDataInterface(Name, pUnknown);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
      return m_realOutput->GetPrivateData(Name, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
      if (!ppParent) return E_POINTER;
      if (m_adapter)
        return m_adapter->QueryInterface(riid, ppParent);
      *ppParent = nullptr;
      return E_NOINTERFACE;
    }

    // ---- IDXGIOutput ----

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* pDesc) override {
      if (!pDesc) return E_POINTER;
      return m_realOutput->GetDesc(pDesc);
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeList(
      DXGI_FORMAT    EnumFormat,
      UINT           Flags,
      UINT*          pNumModes,
      DXGI_MODE_DESC* pDesc) override {
      return m_realOutput->GetDisplayModeList(EnumFormat, Flags, pNumModes, pDesc);
    }

    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(
      const DXGI_MODE_DESC* pModeToMatch,
      DXGI_MODE_DESC*       pClosestMatch,
      IUnknown*             pConcernedDevice) override {
      // The application's device is DXUP's, which DXGI does not know: it
      // refuses the call (The Witcher 3 quits then). Without a device DXGI
      // needs a format; the swap chains DXUP creates scan out 8-bit RGBA.
      if (!pConcernedDevice || !pModeToMatch)
        return m_realOutput->FindClosestMatchingMode(pModeToMatch, pClosestMatch, pConcernedDevice);
      DXGI_MODE_DESC mode = *pModeToMatch;
      if (mode.Format == DXGI_FORMAT_UNKNOWN)
        mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      return m_realOutput->FindClosestMatchingMode(&mode, pClosestMatch, nullptr);
    }

    HRESULT STDMETHODCALLTYPE WaitForVBlank() override {
      return m_realOutput->WaitForVBlank();
    }

    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown* pDevice, BOOL Exclusive) override {
      return m_realOutput->TakeOwnership(pDevice, Exclusive);
    }

    void STDMETHODCALLTYPE ReleaseOwnership() override {
      m_realOutput->ReleaseOwnership();
    }

    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCaps) override {
      return m_realOutput->GetGammaControlCapabilities(pGammaCaps);
    }

    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL* pArray) override {
      return m_realOutput->SetGammaControl(pArray);
    }

    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL* pArray) override {
      return m_realOutput->GetGammaControl(pArray);
    }

    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface* pScanoutSurface) override {
      return m_realOutput->SetDisplaySurface(pScanoutSurface);
    }

    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface* pDestination) override {
      return m_realOutput->GetDisplaySurfaceData(pDestination);
    }

    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
      return m_realOutput->GetFrameStatistics(pStats);
    }

    // ---- IDXGIOutput1 ----

    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes,
                                                  DXGI_MODE_DESC1* pDesc) override {
      return m_realOutput1->GetDisplayModeList1(EnumFormat, Flags, pNumModes, pDesc);
    }

    // As FindClosestMatchingMode: DXGI refuses DXUP's device.
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* pModeToMatch, DXGI_MODE_DESC1* pClosestMatch,
                                                       IUnknown* pConcernedDevice) override {
      if (!pConcernedDevice || !pModeToMatch)
        return m_realOutput1->FindClosestMatchingMode1(pModeToMatch, pClosestMatch, pConcernedDevice);
      DXGI_MODE_DESC1 mode = *pModeToMatch;
      if (mode.Format == DXGI_FORMAT_UNKNOWN)
        mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      return m_realOutput1->FindClosestMatchingMode1(&mode, pClosestMatch, nullptr);
    }

    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource* pDestination) override {
      return m_realOutput1->GetDisplaySurfaceData1(pDestination);
    }

    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown* pDevice, IDXGIOutputDuplication** ppOutputDuplication) override {
      return m_realOutput1->DuplicateOutput(pDevice, ppOutputDuplication);
    }

    // ---- Accessors ----

    IDXGIOutput* getRealOutput() const { return m_realOutput.Get(); }

  private:
    std::atomic<ULONG>   m_refCount;
    ComPtr<IDXGIOutput>  m_realOutput;
    ComPtr<IDXGIOutput1> m_realOutput1;
    DXGIAdapter*         m_adapter;
  };

  // The real output behind an output the application passes in. Real DXGI
  // calls private methods of the outputs it gets, so DXUP's wrappers must
  // never reach it.
  inline ComPtr<IDXGIOutput> realOutputOf(IDXGIOutput* output) {
    ComPtr<IDXGIOutput> real = output;
    ComPtr<IDXGIOutput> wrapper;
    if (output && SUCCEEDED(output->QueryInterface(kDxupOutputIid, reinterpret_cast<void**>(wrapper.GetAddressOf()))))
      real = static_cast<DXGIOutput*>(wrapper.Get())->getRealOutput();
    return real;
  }


  // ============================================================
  // DXGISwapChain : IDXGISwapChain4
  //
  // Wraps a real IDXGISwapChain3/4 created for the D3D12 command
  // queue. GetBuffer returns D3D11Texture2D COM objects that
  // wrap the underlying D3D12 back buffers.
  // ============================================================
  // Flip model swap chains cannot use sRGB formats; the application's
  // sRGB back buffer is the proxy it renders to, copied bit for bit into
  // the UNORM swap chain buffer, which scanout reads the same way.
  inline DXGI_FORMAT flipModelFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
      default:                              return format;
    }
  }

  inline bool isSrgbFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  }

  // dxup.conf dxgi.upscale and dxgi.renderScale: the size of the real
  // buffers for buffers of width x height that the application asks for,
  // and the size its own buffers get, which DXUP scales up into them at
  // Present. With a render scale below 1 the application's buffers are
  // that share of the size asked for (0: the window's). Else, in a window
  // with stretched buffers smaller than its client area in both directions,
  // the real buffers get the client area. Otherwise false, all unchanged.
  inline bool spatialUpscaleSize(HWND window, bool windowed, DXGI_SCALING scaling, UINT& width, UINT& height,
                                 UINT& appWidth, UINT& appHeight) {
    const UserOverrides& overrides = UserOverrides::get();
    RECT client = {};
    const bool haveClient = window && GetClientRect(window, &client);
    const UINT clientWidth = haveClient ? UINT(client.right - client.left) : 0;
    const UINT clientHeight = haveClient ? UINT(client.bottom - client.top) : 0;
    if (overrides.renderScale < 1.0f) {
      const UINT realWidth = width ? width : clientWidth, realHeight = height ? height : clientHeight;
      if (!realWidth || !realHeight)
        return false;
      appWidth = (std::max)(1u, UINT(float(realWidth) * overrides.renderScale + 0.5f));
      appHeight = (std::max)(1u, UINT(float(realHeight) * overrides.renderScale + 0.5f));
      width = realWidth;
      height = realHeight;
      logMessage(LogLevel::Info, "dxup.conf: back buffer %ux%u for %ux%u (render scale %.2f)", appWidth, appHeight,
        width, height, overrides.renderScale);
      return true;
    }
    if (!overrides.spatialUpscale || !windowed || scaling == DXGI_SCALING_NONE || !width || !height || !haveClient
     || clientWidth < width || clientHeight < height || (clientWidth == width && clientHeight == height))
      return false;
    logMessage(LogLevel::Info, "dxup.conf: back buffer %ux%u scaled up to %ux%u at Present", width, height,
      clientWidth, clientHeight);
    appWidth = width;
    appHeight = height;
    width = clientWidth;
    height = clientHeight;
    return true;
  }

  // Borderless full screen: the window's client area covers its monitor.
  inline bool windowCoversMonitor(HWND window) {
    if (!window || !IsWindowVisible(window) || IsIconic(window))
      return false;
    RECT client = {};
    if (!GetClientRect(window, &client))
      return false;
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
    MONITORINFO monitor = { sizeof(monitor) };
    HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    return handle && GetMonitorInfoW(handle, &monitor)
      && client.left <= monitor.rcMonitor.left && client.top <= monitor.rcMonitor.top
      && client.right >= monitor.rcMonitor.right && client.bottom >= monitor.rcMonitor.bottom;
  }

  inline std::atomic<bool>& windowGhostingOff() {
    static std::atomic<bool> off{ false };
    return off;
  }

  // Windows replaces a window whose thread has handled no input for 5 s with
  // a "not responding" ghost, and over a game that fills the screen the
  // taskbar comes up meanwhile (Heroes of the Storm while its menu loads).
  // DXGI turns ghosting off for the process when a swap chain enters
  // exclusive full screen; DXUP does so for a borderless one too.
  inline void disableGhostingForFullscreenWindow(HWND window) {
    if (windowGhostingOff().load(std::memory_order_relaxed) || !windowCoversMonitor(window)
     || windowGhostingOff().exchange(true))
      return;
    DisableProcessWindowsGhosting();
    logMessage(LogLevel::Info, "DXGI: window ghosting off (borderless full screen window 0x%p)", window);
  }

  class DXGISwapChain : public IDXGISwapChain4 {
  public:
    DXGISwapChain(
      D3D11Device*              d3d11Device,
      DXGIFactory*              factory,
      ComPtr<IDXGISwapChain3>   realSwapChain,
      ID3D12CommandQueue*       commandQueue)
    : m_refCount(1)
    , m_d3d11Device(d3d11Device)
    , m_factory(factory)
    , m_realSwapChain(std::move(realSwapChain))
    , m_commandQueue(commandQueue) {
      if (m_d3d11Device) {
        IUnknown* deviceUnknown = reinterpret_cast<IUnknown*>(m_d3d11Device);
        deviceUnknown->AddRef();

        ID3D11DeviceContext* immediateContext = nullptr;
        m_d3d11Device->GetImmediateContext(&immediateContext);
        m_immediateContext = static_cast<D3D11ImmediateContext*>(immediateContext);
      }
      if (m_factory) m_factory->AddRef();
      if (m_commandQueue) m_commandQueue->AddRef();
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      m_tearing = m_realSwapChain && SUCCEEDED(m_realSwapChain->GetDesc1(&desc))
        && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
      noteBackBufferSize();
      noteWindowSize();
    }

    // See disableGhostingForFullscreenWindow: when the swap chain is made or
    // resized, and every 128 presents (a window made full screen without
    // ResizeBuffers).
    void noteWindowSize() {
      HWND window = nullptr;
      if (!windowGhostingOff().load(std::memory_order_relaxed) && m_realSwapChain
       && SUCCEEDED(m_realSwapChain->GetHwnd(&window)))
        disableGhostingForFullscreenWindow(window);
    }

    // Forced multisampling covers targets of the swap chain's size; DXUP's
    // frame latency follows the buffer count.
    void noteBackBufferSize() {
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      if (!m_realSwapChain || FAILED(m_realSwapChain->GetDesc1(&desc)))
        return;
      if (m_d3d11Device)
        m_d3d11Device->NoteBackBufferSize(m_appWidth ? m_appWidth : desc.Width, m_appWidth ? m_appHeight : desc.Height);
      ComPtr<IDXGISwapChain2> swapChain2;
      if (m_latencyWaitable && SUCCEEDED(m_realSwapChain.As(&swapChain2)))
        swapChain2->SetMaximumFrameLatency((std::max)(desc.BufferCount, 2u) - 1);
    }

    ~DXGISwapChain() {
      detachDirectBackBuffer();
      // The back buffer images are not retired through the backend: let the
      // GPU finish with them before they go.
      releaseBackBufferReferences();
      m_proxyBackendImage = nullptr;
      m_resolveImage = nullptr;
      m_backBufferBackendImages.clear();
      m_proxyRenderTarget.Reset();
      m_backBufferTextures.clear();
      if (m_latencyWaitable) CloseHandle(m_latencyWaitable);
      if (m_immediateContext) m_immediateContext->Release();
      if (m_commandQueue) m_commandQueue->Release();
      if (m_factory) m_factory->Release();
      if (m_d3d11Device) {
        IUnknown* deviceUnknown = reinterpret_cast<IUnknown*>(m_d3d11Device);
        deviceUnknown->Release();
      }
    }

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDXGIObject)
       || riid == __uuidof(IDXGIDeviceSubObject)
       || riid == __uuidof(IDXGISwapChain)
       || riid == __uuidof(IDXGISwapChain1)
       || riid == __uuidof(IDXGISwapChain2)
       || riid == __uuidof(IDXGISwapChain3)
       || riid == __uuidof(IDXGISwapChain4)) {
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        AddRef();
        return S_OK;
      }

      *ppvObject = nullptr;
      logMessage(LogLevel::Debug, "DXGISwapChain::QueryInterface unsupported riid=%s",
        formatGuid(riid).c_str());
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      ULONG ref = --m_refCount;
      if (ref == 0 && !processExiting()) delete this;
      return ref;
    }

    // ---- IDXGIObject ----

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
      return m_realSwapChain->SetPrivateData(Name, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
      return m_realSwapChain->SetPrivateDataInterface(Name, pUnknown);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
      return m_realSwapChain->GetPrivateData(Name, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
      if (!ppParent) return E_POINTER;

      if (m_factory)
        return m_factory->QueryInterface(riid, ppParent);

      *ppParent = nullptr;
      return E_NOINTERFACE;
    }

    // ---- IDXGIDeviceSubObject ----

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override {
      if (!ppDevice) return E_POINTER;

      // Return the D3D11 device - callers should QI for ID3D11Device
      if (m_d3d11Device) {
        // Cast to IUnknown and QueryInterface to the requested interface
        IUnknown* deviceUnknown = reinterpret_cast<IUnknown*>(m_d3d11Device);
        return deviceUnknown->QueryInterface(riid, ppDevice);
      }

      *ppDevice = nullptr;
      return E_NOINTERFACE;
    }

    // ---- IDXGISwapChain ----

    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override {
      static int presentCount = 0;
      presentCount++;
      if (presentCount <= 3 || (presentCount % 300 == 0))
        logMessage(LogLevel::Info, "DXGISwapChain::Present #%d (sync=%u flags=0x%x) proxy=%p bbCount=%u",
          presentCount, SyncInterval, Flags,
          m_proxyRenderTarget.Get(), (uint32_t)m_backBufferTextures.size());
      return presentFrame(SyncInterval, Flags, nullptr, false);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override {
      if (!ppSurface) return E_POINTER;
      // Buffers the application does not have (the DISCARD effects give
      // the first only, as Windows' D3D11).
      DXGI_SWAP_CHAIN_DESC realDesc = {};
      if ((m_appDiscard && Buffer > 0)
       || (SUCCEEDED(m_realSwapChain->GetDesc(&realDesc)) && Buffer >= realDesc.BufferCount)) {
        *ppSurface = nullptr;
        return DXGI_ERROR_INVALID_CALL;
      }

      // Lazily create back buffer textures + proxy
      if (m_backBufferTextures.empty()) {
        DXGI_SWAP_CHAIN_DESC desc;
        m_realSwapChain->GetDesc(&desc);
        createBackBufferTextures(desc.BufferCount);
      }

      logMessage(LogLevel::Info, "DXGISwapChain::GetBuffer(Buffer=%u, riid=%s)",
        Buffer, formatGuid(riid).c_str());

      // If requesting ID3D11Texture2D:
      // Buffer 0 returns the proxy render target (game renders here).
      // Other buffer indices return the actual swap chain wrappers.
      if (riid == __uuidof(ID3D11Texture2D)) {
        // Scaled up at Present, the real buffers have another size; drawn
        // into directly, buffer 0 is always the next one: every buffer the
        // application asks for is the one it draws into.
        if ((Buffer == 0 || m_appWidth || m_direct) && m_proxyRenderTarget) {
          IUnknown* tex = m_proxyRenderTarget.Get();
          tex->AddRef();
          *ppSurface = tex;
          return S_OK;
        }

        UINT actualBuffer = Buffer;
        if (actualBuffer < m_backBufferTextures.size() && m_backBufferTextures[actualBuffer]) {
          IUnknown* tex = m_backBufferTextures[actualBuffer].Get();
          tex->AddRef();
          *ppSurface = tex;
          return S_OK;
        }

        *ppSurface = nullptr;
        return E_FAIL;
      }

      // Other interfaces (IDXGISurface, IDXGIResource) of the same buffers.
      if ((Buffer == 0 || m_appWidth || m_direct) && m_proxyRenderTarget)
        return m_proxyRenderTarget->QueryInterface(riid, ppSurface);
      if (Buffer < m_backBufferTextures.size() && m_backBufferTextures[Buffer])
        return m_backBufferTextures[Buffer]->QueryInterface(riid, ppSurface);
      *ppSurface = nullptr;
      return DXGI_ERROR_INVALID_CALL;
    }

    // The output of the swap chain's own adapter that drives the monitor of
    // the given output: the application's outputs come from DXUP's adapters,
    // the real swap chain lives on the D3D12 device's.
    ComPtr<IDXGIOutput> swapChainOutputFor(IDXGIOutput* target) {
      ComPtr<IDXGIOutput> real = realOutputOf(target);
      DXGI_OUTPUT_DESC wanted = {};
      ComPtr<IDXGIOutput> containing;
      ComPtr<IDXGIAdapter> adapter;
      if (!real || FAILED(real->GetDesc(&wanted))
       || FAILED(m_realSwapChain->GetContainingOutput(containing.GetAddressOf()))
       || FAILED(containing->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(adapter.GetAddressOf()))))
        return real;
      ComPtr<IDXGIOutput> output;
      for (UINT i = 0; SUCCEEDED(adapter->EnumOutputs(i, output.ReleaseAndGetAddressOf())); i++) {
        DXGI_OUTPUT_DESC desc = {};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == wanted.Monitor)
          return output;
      }
      return real;
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override {
      waitForPresents();
      m_fullscreenKnown = false;
      ComPtr<IDXGIOutput> target = swapChainOutputFor(pTarget);
      const HRESULT hr = m_realSwapChain->SetFullscreenState(Fullscreen, target.Get());
      if (SUCCEEDED(hr)) {
        renewRealBuffers();
        noteWindowSize();
      }
      return hr;
    }

    // A flip model swap chain presents again only after ResizeBuffers once
    // it went in or out of full screen; Direct3D 11's swap chains of the
    // application did without. The real buffers are made again at their
    // size, the application's proxy stays. Not while the application holds
    // one of the real buffers (then its own ResizeBuffers is needed anyway).
    void renewRealBuffers() {
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      if (FAILED(m_realSwapChain->GetDesc1(&desc)))
        return;
      for (const auto& texture : m_backBufferTextures) {
        if (texture && (texture.Get()->AddRef(), texture.Get()->Release()) > 1)
          return;
      }
      m_backBufferTextures.clear();
      m_backBufferBackendImages.clear();
      // Drawn into directly, the application's buffer lets go of the real
      // ones meanwhile and takes the new one Present shows next.
      const bool direct = m_direct;
      if (direct) {
        retargetNow(m_proxyBackendImage, nullptr);
        m_directBuffers.clear();
      }
      releaseBackBufferReferences();
      const HRESULT hr = m_realSwapChain->ResizeBuffers(desc.BufferCount, desc.Width, desc.Height, desc.Format, desc.Flags);
      if (FAILED(hr))
        logMessage(LogLevel::Warn, "SetFullscreenState: renewing the buffers failed (hr=0x%08x)", static_cast<unsigned>(hr));
      if (m_proxyRenderTarget)
        createBackBufferTextures(desc.BufferCount);
      if (direct) {
        m_direct = fetchDirectBuffers(desc.BufferCount);
        if (m_direct)
          retargetNow(m_proxyBackendImage, m_directBuffers[m_realSwapChain->GetCurrentBackBufferIndex() % m_directBuffers.size()].Get());
      }
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override {
      if (!ppTarget)
        return m_realSwapChain->GetFullscreenState(pFullscreen, nullptr);
      ComPtr<IDXGIOutput> target;
      HRESULT hr = m_realSwapChain->GetFullscreenState(pFullscreen, target.GetAddressOf());
      *ppTarget = SUCCEEDED(hr) && target ? applicationOutput(std::move(target)) : nullptr;
      return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override {
      if (!pDesc) return E_POINTER;
      HRESULT hr = m_realSwapChain->GetDesc(pDesc);
      if (SUCCEEDED(hr)) {
        // Report the buffers the application asked for.
        if (m_appFormat != DXGI_FORMAT_UNKNOWN)
          pDesc->BufferDesc.Format = m_appFormat;
        pDesc->SampleDesc = m_appSampleDesc;
        pDesc->Flags = applicationFlags(pDesc->Flags);
        if (m_appWidth) {
          pDesc->BufferDesc.Width = m_appWidth;
          pDesc->BufferDesc.Height = m_appHeight;
        }
      }
      return hr;
    }

    // The size the real buffers get for the application's (ResizeBuffers):
    // remembers the application's when DXUP scales up at Present.
    void chooseRealSize(UINT& width, UINT& height) {
      m_appWidth = m_appHeight = 0;
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      HWND window = nullptr;
      BOOL fullscreen = FALSE;
      if (FAILED(m_realSwapChain->GetDesc1(&desc)) || FAILED(m_realSwapChain->GetHwnd(&window)))
        return;
      m_realSwapChain->GetFullscreenState(&fullscreen, nullptr);
      UINT appWidth = 0, appHeight = 0;
      if (spatialUpscaleSize(window, !fullscreen, desc.Scaling, width, height, appWidth, appHeight)) {
        m_appWidth = appWidth;
        m_appHeight = appHeight;
      }
    }

    void setApplicationSize(UINT width, UINT height) {
      m_appWidth = width;
      m_appHeight = height;
      noteBackBufferSize();
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(
      UINT        BufferCount,
      UINT        Width,
      UINT        Height,
      DXGI_FORMAT NewFormat,
      UINT        SwapChainFlags) override {
      // Presents queued on the worker still use the buffers.
      waitForPresents();
      detachDirectBackBuffer();
      m_backBufferTextures.clear();
      m_backBufferBackendImages.clear();
      m_proxyRenderTarget.Reset();
      m_proxyBackendImage = nullptr;
      m_resolveImage = nullptr;
      releaseBackBufferReferences();

      if (NewFormat != DXGI_FORMAT_UNKNOWN)
        m_appFormat = NewFormat;
      chooseRealSize(Width, Height);
      HRESULT hr = m_realSwapChain->ResizeBuffers(
        flipModelBufferCount(BufferCount), Width, Height, flipModelFormat(NewFormat), resizeFlags(SwapChainFlags));
      if (SUCCEEDED(hr)) {
        noteBackBufferSize();
        noteWindowSize();
      }

      return hr;
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override {
      waitForPresents();
      m_fullscreenKnown = false;
      return m_realSwapChain->ResizeTarget(pNewTargetParameters);
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override {
      if (!ppOutput)
        return E_POINTER;
      ComPtr<IDXGIOutput> output;
      HRESULT hr = m_realSwapChain->GetContainingOutput(output.GetAddressOf());
      *ppOutput = SUCCEEDED(hr) && output ? applicationOutput(std::move(output)) : nullptr;
      return hr;
    }

    // Outputs reach the application wrapped, as from its adapter: it hands
    // them its device (FindClosestMatchingMode), which real DXGI refuses.
    IDXGIOutput* applicationOutput(ComPtr<IDXGIOutput> real) {
      ComPtr<IDXGIDevice> dxgiDevice;
      ComPtr<IDXGIAdapter> adapter;
      if (m_d3d11Device)
        reinterpret_cast<IUnknown*>(m_d3d11Device)->QueryInterface(__uuidof(IDXGIDevice),
          reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
      if (dxgiDevice)
        dxgiDevice->GetAdapter(adapter.GetAddressOf());
      return new DXGIOutput(std::move(real), static_cast<DXGIAdapter*>(adapter.Get()));
    }

    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
      return m_realSwapChain->GetFrameStatistics(pStats);
    }

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override {
      return m_realSwapChain->GetLastPresentCount(pLastPresentCount);
    }

    // ---- IDXGISwapChain1 ----

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override {
      HRESULT hr = m_realSwapChain->GetDesc1(pDesc);
      if (SUCCEEDED(hr) && pDesc) {
        if (m_appFormat != DXGI_FORMAT_UNKNOWN)
          pDesc->Format = m_appFormat;
        pDesc->SampleDesc = m_appSampleDesc;
        pDesc->Flags = applicationFlags(pDesc->Flags);
        if (m_appWidth) {
          pDesc->Width = m_appWidth;
          pDesc->Height = m_appHeight;
        }
      }
      return hr;
    }

    // The format and sample count the application requested; the real swap
    // chain uses a flip model compatible format and one sample.
    void setApplicationBufferDesc(DXGI_FORMAT format, DXGI_SAMPLE_DESC sampleDesc) {
      m_appFormat = format;
      m_appSampleDesc = sampleDesc.Count ? sampleDesc : DXGI_SAMPLE_DESC{ 1, 0 };
    }

    // The swap effect and buffer usage the application asked for.
    void setApplicationSwapModel(DXGI_SWAP_EFFECT effect, DXGI_USAGE usage) {
      m_appFlipModel = effect == DXGI_SWAP_EFFECT_FLIP_DISCARD || effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      m_appDiscard = effect == DXGI_SWAP_EFFECT_DISCARD || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
      m_appUnorderedAccess = (usage & DXGI_USAGE_UNORDERED_ACCESS) != 0;
    }

    // The real swap chain has a frame latency object the application did
    // not ask for: Present then never blocks, and the worker waits on the
    // object before it writes the next back buffer. A blocking Present
    // waited for the buffer the GPU last presented on the worker (Trine 4:
    // 2.1 of 3.3 ms per frame, spinning in the driver), which then recorded
    // nothing and submitted the next frame late. As many frames may queue
    // as a blocking Present allowed: one less than the buffers.
    void setApplicationFlags(UINT flags) {
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      ComPtr<IDXGISwapChain2> swapChain2;
      if ((flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
       || FAILED(m_realSwapChain->GetDesc1(&desc))
       || !(desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
       || FAILED(m_realSwapChain.As(&swapChain2)))
        return;
      m_latencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
      noteBackBufferSize();
    }

    // Before the worker writes the next back buffer. What the frame
    // recorded so far goes to the GPU first, so that it does not run dry
    // while the worker waits.
    void waitForBackBuffer(IBackendContext* backendContext) {
      if (!m_latencyWaitable || WaitForSingleObjectEx(m_latencyWaitable, 0, TRUE) != WAIT_TIMEOUT)
        return;
      if (backendContext)
        backendContext->flush();
      if (WaitForSingleObjectEx(m_latencyWaitable, 1000, TRUE) == WAIT_TIMEOUT)
        logMessage(LogLevel::Warn, "DXGISwapChain: no back buffer after 1 s");
    }

    // What the application sees of the real swap chain's flags.
    UINT applicationFlags(UINT flags) const {
      return m_latencyWaitable ? flags & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : flags;
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override {
      return m_realSwapChain->GetFullscreenDesc(pDesc);
    }

    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override {
      return m_realSwapChain->GetHwnd(pHwnd);
    }

    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override {
      return m_realSwapChain->GetCoreWindow(refiid, ppUnk);
    }

    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) override {
      return presentFrame(SyncInterval, PresentFlags, pPresentParameters, true);
    }

    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {
      ComPtr<IDXGISwapChain1> swapChain1;
      return SUCCEEDED(m_realSwapChain.As(&swapChain1)) ? swapChain1->IsTemporaryMonoSupported() : FALSE;
    }

    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override {
      if (!ppRestrictToOutput)
        return E_POINTER;
      ComPtr<IDXGIOutput> output;
      HRESULT hr = m_realSwapChain->GetRestrictToOutput(output.GetAddressOf());
      *ppRestrictToOutput = SUCCEEDED(hr) && output ? applicationOutput(std::move(output)) : nullptr;
      return hr;
    }

    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override {
      waitForPresents();
      ComPtr<IDXGISwapChain1> swapChain1;
      return SUCCEEDED(m_realSwapChain.As(&swapChain1)) ? swapChain1->SetBackgroundColor(pColor) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override {
      ComPtr<IDXGISwapChain1> swapChain1;
      return SUCCEEDED(m_realSwapChain.As(&swapChain1)) ? swapChain1->GetBackgroundColor(pColor) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override {
      waitForPresents();
      ComPtr<IDXGISwapChain1> swapChain1;
      return SUCCEEDED(m_realSwapChain.As(&swapChain1)) ? swapChain1->SetRotation(Rotation) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override {
      ComPtr<IDXGISwapChain1> swapChain1;
      return SUCCEEDED(m_realSwapChain.As(&swapChain1)) ? swapChain1->GetRotation(pRotation) : E_NOINTERFACE;
    }

    // ---- IDXGISwapChain2 ----

    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override {
      waitForPresents();
      ComPtr<IDXGISwapChain2> swapChain2;
      return SUCCEEDED(m_realSwapChain.As(&swapChain2)) ? swapChain2->SetSourceSize(Width, Height) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override {
      ComPtr<IDXGISwapChain2> swapChain2;
      return SUCCEEDED(m_realSwapChain.As(&swapChain2)) ? swapChain2->GetSourceSize(pWidth, pHeight) : E_NOINTERFACE;
    }

    // Without the flag the application's swap chain has no latency object.
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
      if (m_latencyWaitable)
        return DXGI_ERROR_INVALID_CALL;
      ComPtr<IDXGISwapChain2> swapChain2;
      return SUCCEEDED(m_realSwapChain.As(&swapChain2)) ? swapChain2->SetMaximumFrameLatency(MaxLatency) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override {
      if (m_latencyWaitable)
        return DXGI_ERROR_INVALID_CALL;
      ComPtr<IDXGISwapChain2> swapChain2;
      return SUCCEEDED(m_realSwapChain.As(&swapChain2)) ? swapChain2->GetMaximumFrameLatency(pMaxLatency) : E_NOINTERFACE;
    }

    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
      if (m_latencyWaitable)
        return nullptr;
      ComPtr<IDXGISwapChain2> swapChain2;
      return SUCCEEDED(m_realSwapChain.As(&swapChain2)) ? swapChain2->GetFrameLatencyWaitableObject() : nullptr;
    }

    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override {
      waitForPresents();
      ComPtr<IDXGISwapChain2> swapChain2;
      return SUCCEEDED(m_realSwapChain.As(&swapChain2)) ? swapChain2->SetMatrixTransform(pMatrix) : E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override {
      ComPtr<IDXGISwapChain2> swapChain2;
      return SUCCEEDED(m_realSwapChain.As(&swapChain2)) ? swapChain2->GetMatrixTransform(pMatrix) : E_NOINTERFACE;
    }

    // ---- IDXGISwapChain3 ----

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
      return m_realSwapChain->GetCurrentBackBufferIndex();
    }

    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) override {
      return m_realSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
    }

    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {
      waitForPresents();
      return m_realSwapChain->SetColorSpace1(ColorSpace);
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers1(
      UINT              BufferCount,
      UINT              Width,
      UINT              Height,
      DXGI_FORMAT       Format,
      UINT              SwapChainFlags,
      const UINT*       pCreationNodeMask,
      IUnknown* const*  ppPresentQueue) override {
      waitForPresents();
      detachDirectBackBuffer();
      m_backBufferTextures.clear();
      m_backBufferBackendImages.clear();
      m_proxyRenderTarget.Reset();
      m_proxyBackendImage = nullptr;
      m_resolveImage = nullptr;
      releaseBackBufferReferences();
      if (Format != DXGI_FORMAT_UNKNOWN)
        m_appFormat = Format;
      chooseRealSize(Width, Height);
      // A single buffer becomes two, each node and queue named twice.
      UINT nodeMasks[2] = {};
      IUnknown* presentQueues[2] = {};
      if (BufferCount == 1) {
        if (pCreationNodeMask) {
          nodeMasks[0] = nodeMasks[1] = pCreationNodeMask[0];
          pCreationNodeMask = nodeMasks;
        }
        if (ppPresentQueue) {
          presentQueues[0] = presentQueues[1] = ppPresentQueue[0];
          ppPresentQueue = presentQueues;
        }
      }
      const HRESULT hr = m_realSwapChain->ResizeBuffers1(
        flipModelBufferCount(BufferCount),
        Width,
        Height,
        flipModelFormat(Format),
        resizeFlags(SwapChainFlags),
        pCreationNodeMask,
        ppPresentQueue);
      if (SUCCEEDED(hr))
        noteBackBufferSize();
      return hr;
    }

    // ---- IDXGISwapChain4 ----

    HRESULT STDMETHODCALLTYPE SetHDRMetaData(
      DXGI_HDR_METADATA_TYPE Type,
      UINT Size,
      void* pMetaData) override {
      waitForPresents();
      ComPtr<IDXGISwapChain4> swapChain4;
      HRESULT hr = m_realSwapChain.As(&swapChain4);
      return SUCCEEDED(hr)
        ? swapChain4->SetHDRMetaData(Type, Size, pMetaData)
        : hr;
    }

    // ---- Accessors ----

    IDXGISwapChain3*   getRealSwapChain()  const { return m_realSwapChain.Get(); }
    D3D11Device*       getD3D11Device()    const { return m_d3d11Device; }
    ID3D12CommandQueue* getCommandQueue()  const { return m_commandQueue; }

  private:
    std::atomic<ULONG>           m_refCount;
    D3D11Device*                 m_d3d11Device;
    DXGIFactory*                 m_factory;
    ComPtr<IDXGISwapChain3>      m_realSwapChain;
    ID3D12CommandQueue*          m_commandQueue;
    D3D11ImmediateContext*       m_immediateContext = nullptr;
    std::vector<ComPtr<IUnknown>> m_backBufferTextures;
    std::vector<IBackendImage*>  m_backBufferBackendImages;
    ComPtr<IUnknown>             m_proxyRenderTarget;  // Game renders to this instead of swap chain buffers
    uint32_t                     m_dumpPresentCount = 0;
    uint64_t                     m_lastPeriodicDump = 0;   // GetTickCount64
    std::vector<uint32_t>        m_periodicDumps;           // the frames still on disk
    // Glitch detection (detectGlitches).
    static constexpr uint32_t    kGlitchSmall = 4;          // grids on their way back
    static constexpr uint32_t    kGlitchGrids = 6;          // grids kept
    static constexpr uint32_t    kGlitchFrames = 6;         // full frames kept
    struct GlitchGrid {
      uint32_t             present = 0;
      std::vector<uint8_t> luma;
    };
    ComPtr<ID3D11Texture2D>      m_glitchMips;
    ComPtr<ID3D11ShaderResourceView> m_glitchMipsView;
    ComPtr<ID3D11Texture2D>      m_glitchSmall[kGlitchSmall];
    uint32_t                     m_glitchSmallOf[kGlitchSmall] = {};
    ComPtr<ID3D11Texture2D>      m_glitchFrames[kGlitchFrames];
    uint32_t                     m_glitchFrameOf[kGlitchFrames] = {};
    GlitchGrid                   m_glitchGrids[kGlitchGrids];
    uint32_t                     m_glitchLevel = 0, m_glitchColumns = 0, m_glitchRows = 0;
    uint32_t                     m_glitchWidth = 0, m_glitchHeight = 0;
    bool                         m_glitchBgra = false;
    bool                         m_glitchOff = false;
    uint32_t                     m_glitchEvents = 0;
    uint32_t                     m_glitchLastEvent = 0;
    // A flicker seen at a present, reported once the next frames show it
    // was not a dark object moving across (that is seen frame after frame).
    struct GlitchCandidate {
      uint32_t glitch = 0, frames = 0, dark = 0, bright = 0, tiles = 0;
      uint32_t box[4] = {};
      uint32_t luma[3] = {};
    };
    GlitchCandidate              m_glitchPending;
    // The pending candidate's frame and the one before, copied aside when it
    // is seen (the kept frames roll on before it is reported).
    ComPtr<ID3D11Texture2D>      m_glitchAside[2];
    bool                         m_glitchAsideValid[2] = {};
    uint32_t                     m_glitchLastSeen = 0;     // the last glitch frame any candidate had
    uint32_t                     m_glitchSuppressed = 0;
    uint64_t                     m_traceGoLastCheck = 0;   // GetTickCount64

    bool traceGoDue() {
      const uint64_t now = GetTickCount64();
      if (now - m_traceGoLastCheck < 2000)
        return false;
      m_traceGoLastCheck = now;
      static const std::string path = dxupSiblingPath("dxup_trace_go.txt");
      return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    uint32_t                     m_traceGoFrames = 0;
    bool                         m_traceGoDone = false;
    uint32_t                     m_windowSizePresents = 0;
    IBackendImage*               m_proxyBackendImage = nullptr;
    Rc<IBackendImage>            m_resolveImage;
    // Drawn into directly (dxgi.directBackBuffer): the proxy's image is the
    // real buffer Present shows next, one of these (createDirectBackBuffer).
    std::atomic<bool>            m_direct{ false };
    std::vector<ComPtr<ID3D12Resource>> m_directBuffers;
    // Textures of their own for direct buffers the application kept past
    // ResizeBuffers.
    // Stand-ins for direct back buffers the application still held when they
    // were detached: kept as long as its texture (the owner) lives.
    struct DetachedImage { std::weak_ptr<d3d12::D3D12BackendImage> owner; Rc<IBackendImage> copy; };
    std::vector<DetachedImage> m_detachedImages;
    bool                         m_appFlipModel = false;
    bool                         m_appDiscard = false;
    bool                         m_appUnorderedAccess = false;
    DXGI_FORMAT                  m_appFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC             m_appSampleDesc = { 1, 0 };
    // The application's buffer size while DXUP scales it up at Present
    // (dxgi.upscale), else 0.
    UINT                         m_appWidth = 0;
    UINT                         m_appHeight = 0;
    // Presents run on the worker unless the swap chain is in exclusive
    // fullscreen, where a present can wait for the window's thread.
    bool                         m_fullscreenKnown = false;
    bool                         m_exclusiveFullscreen = false;
    // The real swap chain was created to tear (VSync forced off).
    bool                         m_tearing = false;
    HANDLE                       m_latencyWaitable = nullptr;

    // Flip model swap chains have at least two buffers (as created); bitblt
    // model applications resize with one (The Witcher 3), which DXGI
    // refuses with E_INVALIDARG. 0 keeps the count.
    static UINT flipModelBufferCount(UINT count) {
      return count == 1 ? 2u : count;
    }

    // DXGI keeps the tearing and frame latency object flags of a swap chain
    // for its lifetime.
    UINT resizeFlags(UINT flags) const {
      flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
      if (m_latencyWaitable)
        flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
      return m_tearing ? flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : flags;
    }

    bool exclusiveFullscreen() {
      if (!m_fullscreenKnown) {
        BOOL fullscreen = FALSE;
        m_exclusiveFullscreen = SUCCEEDED(m_realSwapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen;
        m_fullscreenKnown = true;
      }
      return m_exclusiveFullscreen;
    }

    // VSync forced by dxup.conf: the interval, and tearing where VSync is off
    // and the swap chain allows it. Tearing needs interval 0 and a window.
    void overrideSyncInterval(UINT& syncInterval, UINT& flags) {
      const int32_t forced = UserOverrides::get().syncInterval;
      if (forced < 0)
        return;
      syncInterval = static_cast<UINT>(forced);
      if (syncInterval || !m_tearing || exclusiveFullscreen())
        flags &= ~DXGI_PRESENT_ALLOW_TEARING;
      else
        flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    // What the last present the worker made returned.
    std::atomic<HRESULT>         m_lastPresentResult{ S_OK };

    // The swap chain changes only once no present is queued on the worker
    // (and the submit thread handed its command lists over).
    void waitForPresents() {
      if (IBackendContext* ctx = backendContext())
        ctx->waitForQueue();
    }

    bool presentOnWorker() {
      return !exclusiveFullscreen() && !dxupSyncPresentEnabled();
    }

    // A Present1 call's parameters, copied for the worker.
    struct PresentParameters {
      std::vector<RECT> dirtyRects;
      RECT              scrollRect = {};
      POINT             scrollOffset = {};
      bool              hasScrollRect = false;
      bool              hasScrollOffset = false;

      explicit PresentParameters(const DXGI_PRESENT_PARAMETERS* params) {
        if (!params)
          return;
        if (params->pDirtyRects && params->DirtyRectsCount)
          dirtyRects.assign(params->pDirtyRects, params->pDirtyRects + params->DirtyRectsCount);
        if ((hasScrollRect = params->pScrollRect != nullptr))
          scrollRect = *params->pScrollRect;
        if ((hasScrollOffset = params->pScrollOffset != nullptr))
          scrollOffset = *params->pScrollOffset;
      }

      DXGI_PRESENT_PARAMETERS get() {
        DXGI_PRESENT_PARAMETERS params = {};
        params.DirtyRectsCount = static_cast<UINT>(dirtyRects.size());
        params.pDirtyRects = dirtyRects.empty() ? nullptr : dirtyRects.data();
        params.pScrollRect = hasScrollRect ? &scrollRect : nullptr;
        params.pScrollOffset = hasScrollOffset ? &scrollOffset : nullptr;
        return params;
      }
    };

    // FPS counter
    uint32_t m_fpsFrameCount = 0;
    double   m_fpsLastTime   = 0.0;
    float    m_currentFps    = 0.0f;
    bool     m_fpsTimerInit  = false;
    LARGE_INTEGER m_fpsFreq  = {};

    // Hitch trace: how long the real Present takes on the worker, every
    // 1000 presents.
    void notePresentTime(int64_t start) {
      static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
      const double ms = double(hitchTimestamp() - start) * 1000.0 / double(frequency.QuadPart);
      m_presentStatMs += ms;
      m_presentStatMax = (std::max)(m_presentStatMax, ms);
      if (++m_presentStatCount < 1000)
        return;
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      m_realSwapChain->GetDesc1(&desc);
      logMessage(LogLevel::Warn, "present stats: %u presents, %.3f ms average in Present, max %.2f ms (%u buffers, flags 0x%x)",
        m_presentStatCount, m_presentStatMs / m_presentStatCount, m_presentStatMax, desc.BufferCount, desc.Flags);
      m_presentStatCount = 0;
      m_presentStatMs = 0.0;
      m_presentStatMax = 0.0;
    }
    uint32_t m_presentStatCount = 0;
    double   m_presentStatMs = 0.0;
    double   m_presentStatMax = 0.0;

    // Like a native D3D11 Present, block while more frames than the
    // device's maximum frame latency are queued for rendering.
    void applyFrameLatency() {
      if (!m_immediateContext || !m_d3d11Device)
        return;
      if (IBackendContext* backendContext = m_immediateContext->getBackendCtx())
        backendContext->waitForFrameLatency(m_d3d11Device->GetEffectiveFrameLatency());
    }

    IBackendContext* backendContext() const {
      return m_immediateContext ? m_immediateContext->getBackendCtx() : nullptr;
    }

    // ResizeBuffers fails while the GPU or a retired backend image still
    // references a back buffer.
    void releaseBackBufferReferences() {
      if (IBackendContext* ctx = backendContext())
        ctx->synchronize();
    }

    HRESULT presentFrame(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params, bool present1) {
      HitchScope hitch("present", syncInterval);
      // A device DXUP lost without the system knowing (IBackendDevice::
      // lostReason): the application recreates it as after a GPU fault.
      if (IBackendDevice* backend = m_d3d11Device ? m_d3d11Device->GetBackend() : nullptr) {
        if (FAILED(backend->lostReason()))
          return DXGI_ERROR_DEVICE_REMOVED;
      }
      overrideSyncInterval(syncInterval, flags);
      if (hitchTraceEnabled()) {
        // Frames that took long, to line the operations above up with.
        static int64_t s_lastPresent = 0;
        static uint64_t s_frame = 0;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const int64_t now = hitchTimestamp();
        const double ms = s_lastPresent ? double(now - s_lastPresent) * 1000.0 / double(frequency.QuadPart) : 0.0;
        if (ms >= 25.0)
          logMessage(LogLevel::Warn, "hitch: frame %llu took %.1f ms", static_cast<unsigned long long>(s_frame), ms);
        // The frame rate the trace saw.
        static double s_windowMs = 0.0, s_windowMax = 0.0;
        s_windowMs += ms;
        s_windowMax = (std::max)(s_windowMax, ms);
        if (s_frame && s_frame % 500 == 0) {
          logMessage(LogLevel::Warn, "frames: %llu, last 500 average %.2f ms (%.1f fps), max %.1f ms",
            static_cast<unsigned long long>(s_frame), s_windowMs / 500.0, 500000.0 / s_windowMs, s_windowMax);
          s_windowMs = 0.0;
          s_windowMax = 0.0;
        }
        s_lastPresent = now;
        s_frame++;
      }
      // Present uses the immediate context.
      D3D11ContextMtGuard mtGuard(m_d3d11Device, false);
      if (dxupFpsOverlayEnabled())
        updateFps();

      // Copy the proxy and submit on the thread that records the commands,
      // then present here once all of them are recorded, so the frame queue
      // and the driver's pacing see the application thread as they would
      // with a native device.
      IBackendContext* ctx = backendContext();
      // With dxup_trace_go.txt next to the DLL (looked for every two
      // seconds), the calls of the two frames after it appears go into
      // the log.
      if (!m_traceGoDone && !(flags & DXGI_PRESENT_TEST)) {
        if (m_traceGoFrames) {
          if (--m_traceGoFrames == 0) {
            m_d3d11Device->diagTraceDraws.store(false, std::memory_order_relaxed);
            m_traceGoDone = true;
          }
        } else if (traceGoDue()) {
          m_traceGoFrames = 2;
          m_d3d11Device->diagTraceDraws.store(true, std::memory_order_relaxed);
          m_d3d11Device->diagTraceIndex.store(0, std::memory_order_relaxed);
          logMessage(LogLevel::Warn, "draw trace: go");
        }
      }
      if (dxupFrameDumpEnabled() && !(flags & DXGI_PRESENT_TEST)) {
        const uint32_t frame = ++m_dumpPresentCount;
        detectGlitches(frame);
        // Presents 60, 300 and 301 (whose draws are traced), then one every
        // 15 seconds, so that a bug report has what was on screen when the
        // problem showed; of those only the last four stay (33 MB each at 4K).
        const uint64_t now = GetTickCount64();
        if (frame == 60 || frame == 300 || frame == 301) {
          dumpApplicationBackBuffer(frame);
          m_lastPeriodicDump = now;
        } else if (frame > 301 && now - m_lastPeriodicDump >= 15000) {
          dumpApplicationBackBuffer(frame);
          m_lastPeriodicDump = now;
          m_periodicDumps.push_back(frame);
          if (m_periodicDumps.size() > 4) {
            char old[64];
            sprintf_s(old, "dxup_frame_%u.bmp", m_periodicDumps.front());
            DeleteFileA(dxupSiblingPath(old).c_str());
            m_periodicDumps.erase(m_periodicDumps.begin());
          }
        }
        // The draws of the frame after present 300.
        // The draws of the frame after present 300, and of the one after
        // the dump of present 9000.
        const bool trace = (frame >= 300 && frame < 302) || frame == 9000;
        m_d3d11Device->diagTraceDraws.store(trace, std::memory_order_relaxed);
        m_d3d11Device->diagTraceIndex.store(0, std::memory_order_relaxed);
        if ((frame >= 300 && frame <= 302) || frame == 9000 || frame == 9001)
          logMessage(LogLevel::Warn, "draw trace: Present %u", frame);
      }
      // What was drawn into a forced multisampled copy of the back buffer
      // goes into it before the worker copies it.
      if (m_immediateContext && m_proxyRenderTarget && !(flags & DXGI_PRESENT_TEST) && m_d3d11Device->HasForcedMsaa()) {
        ComPtr<ID3D11Resource> proxy;
        if (SUCCEEDED(m_proxyRenderTarget.As(&proxy)))
          m_immediateContext->ReadForcedMsaa(proxy.Get());
      }
      if (m_immediateContext && !(flags & DXGI_PRESENT_TEST))
        m_immediateContext->NoteFramePresented();
      if (!(++m_windowSizePresents & 127))
        noteWindowSize();
      // Like a native driver thread, the worker copies, submits and
      // presents the frame while the application goes on with the next;
      // applyFrameLatency keeps it at most a frame ahead of the worker.
      if (ctx && !(flags & DXGI_PRESENT_TEST) && presentOnWorker()) {
        auto parameters = std::make_shared<PresentParameters>(params);
        const bool hasParameters = params != nullptr;
        // The worker may wait for the display (the application does not):
        // DO_NOT_WAIT there would drop the frame queued as presented.
        const UINT workerFlags = flags & ~UINT(DXGI_PRESENT_DO_NOT_WAIT);
        ctx->executeOnBackend([this, syncInterval, flags = workerFlags, present1, parameters, hasParameters](IBackendContext* backend) {
          preparePresent(backend);
          // With the submit thread (d3d12.submitThread) the frame's lists
          // reach the queue first. Present stays here: on the submit thread
          // NVIDIA's Present spun and held the next frame's lists back.
          backend->waitForQueue();
          DXGI_PRESENT_PARAMETERS copy = parameters->get();
          const int64_t start = hitchTraceEnabled() ? hitchTimestamp() : 0;
          const HRESULT hr = present1
            ? m_realSwapChain->Present1(syncInterval, flags, hasParameters ? &copy : nullptr)
            : m_realSwapChain->Present(syncInterval, flags);
          if (start)
            notePresentTime(start);
          if (FAILED(hr))
            logMessage(LogLevel::Error, "DXGI Present FAILED: hr=0x%08x", static_cast<unsigned>(hr));
          retargetDirectBackBuffer(backend);
          m_lastPresentResult.store(hr, std::memory_order_release);
        });
        ctx->endFrame();
        applyFrameLatency();
        return m_lastPresentResult.load(std::memory_order_acquire);
      }

      if (ctx) {
        if (!(flags & DXGI_PRESENT_TEST))
          ctx->executeOnBackend([this](IBackendContext* backend) { preparePresent(backend); });
        ctx->waitForQueue();
      }

      HRESULT hr = present1
        ? m_realSwapChain->Present1(syncInterval, flags, params)
        : m_realSwapChain->Present(syncInterval, flags);
      if (FAILED(hr))
        logMessage(LogLevel::Error, "DXGI Present FAILED: hr=0x%08x", static_cast<unsigned>(hr));
      else if (!(flags & DXGI_PRESENT_TEST)) {
        if (ctx && m_direct)
          ctx->executeOnBackend([this](IBackendContext* backend) { retargetDirectBackBuffer(backend); });
        if (ctx)
          ctx->endFrame();
        applyFrameLatency();
      }
      return hr;
    }

    // After Present: the application's buffer is the real buffer Present
    // shows next. On the thread that records the commands, in their order.
    void retargetDirectBackBuffer(IBackendContext* backend) {
      if (!m_direct || !backend || !m_proxyBackendImage || m_directBuffers.empty())
        return;
      const UINT index = m_realSwapChain->GetCurrentBackBufferIndex();
      if (index < m_directBuffers.size())
        backend->retargetImage(m_proxyBackendImage, m_directBuffers[index].Get());
    }

    // Retargets now, from the application's thread (the swap chain changes).
    void retargetNow(IBackendImage* image, IUnknown* resource) {
      IBackendContext* ctx = backendContext();
      if (!ctx || !image)
        return;
      ComPtr<IUnknown> held = resource;
      ctx->executeOnBackend([image, held](IBackendContext* backend) { backend->retargetImage(image, held.Get()); });
      ctx->waitForWorker();
    }

    bool fetchDirectBuffers(UINT bufferCount) {
      m_directBuffers.clear();
      for (UINT i = 0; i < bufferCount; i++) {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(m_realSwapChain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(buffer.GetAddressOf())))
         || !buffer) {
          m_directBuffers.clear();
          return false;
        }
        m_directBuffers.push_back(std::move(buffer));
      }
      return !m_directBuffers.empty();
    }

    // Before the real buffers go (ResizeBuffers, release): the application's
    // buffer lets go of them. One the application still holds (which
    // Direct3D 11 refuses for ResizeBuffers) draws into a texture of its own.
    void detachDirectBackBuffer() {
      if (!m_direct)
        return;
      // Presents still queued on the worker present this buffer.
      waitForPresents();
      m_direct = false;
      IBackendImage* image = m_proxyBackendImage;
      m_detachedImages.erase(std::remove_if(m_detachedImages.begin(), m_detachedImages.end(),
        [](const DetachedImage& detached) { return detached.owner.expired(); }), m_detachedImages.end());
      ComPtr<IUnknown> standIn;
      if (image && m_proxyRenderTarget && m_d3d11Device
       && (m_proxyRenderTarget.Get()->AddRef(), m_proxyRenderTarget.Get()->Release()) > 1) {
        if (Rc<IBackendImage> copy = m_d3d11Device->GetBackend()->createImage(image->getDesc())) {
          standIn = static_cast<d3d12::D3D12BackendImage*>(copy.get())->getResource();
          m_detachedImages.push_back({ static_cast<d3d12::D3D12BackendImage*>(image)->weak_from_this(), std::move(copy) });
        }
      }
      retargetNow(image, standIn.Get());
      m_directBuffers.clear();
    }

    // dxgi.directBackBuffer: the application draws into the real buffer
    // Present shows next, and Present has nothing to copy (4K: 33 MB and
    // 0.04 ms a frame). Direct3D 11 renames flip model buffers the same way:
    // the texture's resource changes at every Present. Flip model
    // applications with single-sampled buffers in the real buffers' format,
    // without unordered access; sRGB buffers, multisampling (also forced),
    // dxgi.upscale and renderScale keep the proxy, as do bitblt model
    // applications, which may count on the buffer keeping its contents.
    bool createDirectBackBuffer(UINT bufferCount) {
      if (!UserOverrides::get().directBackBuffer || !m_appFlipModel || m_appUnorderedAccess || m_appWidth
       || m_appSampleDesc.Count > 1 || bufferCount < 2 || !fetchDirectBuffers(bufferCount))
        return false;
      const D3D12_RESOURCE_DESC real = m_directBuffers[0]->GetDesc();
      const DXGI_FORMAT format = m_appFormat != DXGI_FORMAT_UNKNOWN ? m_appFormat : real.Format;
      D3D11_TEXTURE2D_DESC texDesc = {};
      texDesc.Width            = static_cast<UINT>(real.Width);
      texDesc.Height           = real.Height;
      texDesc.MipLevels        = 1;
      texDesc.ArraySize        = 1;
      texDesc.Format           = format;
      texDesc.SampleDesc       = { 1, 0 };
      texDesc.Usage            = D3D11_USAGE_DEFAULT;
      texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET;
      if (!(real.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        texDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
      ForcedMsaaFormats forcedMsaa;
      if (format != real.Format || real.SampleDesc.Count > 1 || real.DepthOrArraySize > 1
       || m_d3d11Device->EnhancedSampleCount(format, 1) > 1 || m_d3d11Device->ShouldForceMsaa(texDesc, forcedMsaa)) {
        m_directBuffers.clear();
        return false;
      }
      ImageDesc imageDesc = {};
      imageDesc.type      = ImageType::Tex2D;
      imageDesc.width     = texDesc.Width;
      imageDesc.height    = texDesc.Height;
      imageDesc.format    = format;
      imageDesc.bindFlags = (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        ? BindFlags::RenderTarget | BindFlags::ShaderResource : BindFlags::RenderTarget;
      // Swap chain buffers take sRGB views of their UNORM format.
      imageDesc.miscFlags = kResourceMiscCastable;
      const UINT current = m_realSwapChain->GetCurrentBackBufferIndex() % bufferCount;
      auto image = std::make_shared<d3d12::D3D12BackendImage>(m_directBuffers[current], imageDesc);
      image->setCurrentState(D3D12_RESOURCE_STATE_PRESENT);
      image->setRetargetable();
      m_proxyBackendImage = image.get();
      auto* texture = new D3D11Texture2D(m_d3d11Device, texDesc, image);
      const UINT swapChainMarker = 1;
      texture->SetPrivateData(kDxupSwapChainBufferGuid, sizeof(swapChainMarker), &swapChainMarker);
      ComPtr<IUnknown> unknown;
      texture->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(unknown.GetAddressOf()));
      m_proxyRenderTarget = unknown;
      texture->Release();
      m_direct = true;
      logMessage(LogLevel::Info, "Back buffer drawn into directly: %ux%u fmt=%u, %u buffers", texDesc.Width,
        texDesc.Height, static_cast<uint32_t>(format), bufferCount);
      return true;
    }

    void dumpApplicationBackBuffer(uint32_t frame) {
      ComPtr<ID3D11Texture2D> backBuffer;
      if (!m_proxyRenderTarget || FAILED(m_proxyRenderTarget.As(&backBuffer)))
        return;
      char name[64];
      sprintf_s(name, "dxup_frame_%u.bmp", frame);
      if (writeBitmap(backBuffer.Get(), name))
        logMessage(LogLevel::Warn, "frame dump: wrote %s", name);
    }

    // An 8-bit RGBA or BGRA texture as a BMP file next to the DLL.
    bool writeBitmap(ID3D11Texture2D* source, const char* name) {
      D3D11_TEXTURE2D_DESC desc = {};
      source->GetDesc(&desc);
      const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                     || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
      const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                     || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
      if ((!rgba && !bgra) || desc.SampleDesc.Count > 1) {
        logMessage(LogLevel::Warn, "frame dump: format %d samples %u not supported",
          static_cast<int>(desc.Format), desc.SampleDesc.Count);
        return false;
      }
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      ComPtr<ID3D11Texture2D> staging;
      ComPtr<ID3D11DeviceContext> context;
      if (FAILED(m_d3d11Device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())))
        return false;
      m_d3d11Device->GetImmediateContext(context.GetAddressOf());
      context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, nullptr);
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

      FILE* file = fopen(dxupSiblingPath(name).c_str(), "wb");
      if (file) {
        const uint32_t imageSize = desc.Width * desc.Height * 4;
        BITMAPFILEHEADER fileHeader = {};
        BITMAPINFOHEADER infoHeader = {};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + imageSize;
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = LONG(desc.Width);
        infoHeader.biHeight = -LONG(desc.Height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biSizeImage = imageSize;
        fwrite(&fileHeader, sizeof(fileHeader), 1, file);
        fwrite(&infoHeader, sizeof(infoHeader), 1, file);
        std::vector<uint8_t> row(desc.Width * 4);
        for (UINT y = 0; y < desc.Height; y++) {
          const auto* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
          for (UINT x = 0; x < desc.Width; x++) {
            row[x * 4 + 0] = src[x * 4 + (rgba ? 2 : 0)];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + (rgba ? 0 : 2)];
            row[x * 4 + 3] = 255;
          }
          fwrite(row.data(), row.size(), 1, file);
        }
        fclose(file);
      }
      context->Unmap(staging.Get(), 0);
      return file != nullptr;
    }

    // ---- Glitch detection (with saved frames) ----
    // Each presented frame goes down to a grid of about 60 x 34 tiles on the
    // GPU (the last levels of a mip chain); the grid comes back a few frames
    // later without waiting. Tiles that go dark (or bright) for one or two
    // frames while the frames around them agree are a flicker, not a camera
    // cut or a fade: the swap chain logs where, saves the glitch frame and
    // the one before as dxup_glitch_<present>.bmp, and the immediate context
    // logs the calls of both frames against each other (LogDrawHistory).
    void detectGlitches(uint32_t present) {
      ComPtr<ID3D11Texture2D> backBuffer;
      if (m_glitchOff || !m_proxyRenderTarget || FAILED(m_proxyRenderTarget.As(&backBuffer)))
        return;
      if (!m_glitchMips && !prepareGlitchDetection(backBuffer.Get()))
        return;
      D3D11_TEXTURE2D_DESC desc = {};
      backBuffer->GetDesc(&desc);
      if (desc.Width != m_glitchWidth || desc.Height != m_glitchHeight) {
        // A new size: start over.
        m_glitchAside[0].Reset();
        m_glitchAside[1].Reset();
        m_glitchAsideValid[0] = m_glitchAsideValid[1] = false;
        m_glitchPending = GlitchCandidate();
        m_glitchMips.Reset();
        m_glitchMipsView.Reset();
        for (auto& grid : m_glitchGrids)
          grid.present = 0;
        if (!prepareGlitchDetection(backBuffer.Get()))
          return;
      }
      ComPtr<ID3D11DeviceContext> context;
      m_d3d11Device->GetImmediateContext(context.GetAddressOf());
      const uint32_t frameSlot = present % kGlitchFrames;
      if (m_glitchFrames[frameSlot]) {
        context->CopySubresourceRegion(m_glitchFrames[frameSlot].Get(), 0, 0, 0, 0, backBuffer.Get(), 0, nullptr);
        m_glitchFrameOf[frameSlot] = present;
      }
      context->CopySubresourceRegion(m_glitchMips.Get(), 0, 0, 0, 0, backBuffer.Get(), 0, nullptr);
      context->GenerateMips(m_glitchMipsView.Get());
      const uint32_t smallSlot = present % kGlitchSmall;
      context->CopySubresourceRegion(m_glitchSmall[smallSlot].Get(), 0, 0, 0, 0, m_glitchMips.Get(), m_glitchLevel, nullptr);
      m_glitchSmallOf[smallSlot] = present;
      // The grids of earlier presents the GPU is done with, oldest first.
      for (uint32_t back = kGlitchSmall - 1; back >= 1; back--) {
        const uint32_t q = present - back;
        const uint32_t slot = q % kGlitchSmall;
        if (q == 0 || q > present || m_glitchSmallOf[slot] != q)
          continue;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map(m_glitchSmall[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
          break;
        m_glitchSmallOf[slot] = 0;
        if (FAILED(hr))
          continue;
        GlitchGrid& grid = m_glitchGrids[q % kGlitchGrids];
        grid.present = q;
        grid.luma.resize(size_t(m_glitchColumns) * m_glitchRows);
        for (uint32_t y = 0; y < m_glitchRows; y++) {
          const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
          for (uint32_t x = 0; x < m_glitchColumns; x++) {
            const uint8_t* p = row + x * 4;
            const uint32_t r = m_glitchBgra ? p[2] : p[0], g = p[1], b = m_glitchBgra ? p[0] : p[2];
            grid.luma[size_t(y) * m_glitchColumns + x] = uint8_t((r * 77 + g * 150 + b * 29) >> 8);
          }
        }
        context->Unmap(m_glitchSmall[slot].Get(), 0);
        checkFlicker(q, present);
      }
    }

    bool prepareGlitchDetection(ID3D11Texture2D* backBuffer) {
      D3D11_TEXTURE2D_DESC desc = {};
      backBuffer->GetDesc(&desc);
      DXGI_FORMAT format = desc.Format;
      switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
          break;
        default:
          format = DXGI_FORMAT_UNKNOWN;
          break;
      }
      if (format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count > 1 || desc.Width < 64 || desc.Height < 32) {
        logMessage(LogLevel::Warn, "glitch detection: back buffer %ux%u format %d samples %u not supported",
          desc.Width, desc.Height, static_cast<int>(desc.Format), desc.SampleDesc.Count);
        m_glitchOff = true;
        return false;
      }
      m_glitchBgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
      m_glitchLevel = 0;
      while ((desc.Width >> m_glitchLevel) > 64 && m_glitchLevel < 12)
        m_glitchLevel++;
      m_glitchColumns = (std::max)(desc.Width >> m_glitchLevel, 1u);
      m_glitchRows = (std::max)(desc.Height >> m_glitchLevel, 1u);
      m_glitchWidth = desc.Width;
      m_glitchHeight = desc.Height;

      D3D11_TEXTURE2D_DESC mips = {};
      mips.Width = desc.Width;
      mips.Height = desc.Height;
      mips.MipLevels = m_glitchLevel + 1;
      mips.ArraySize = 1;
      mips.Format = format;
      mips.SampleDesc.Count = 1;
      mips.Usage = D3D11_USAGE_DEFAULT;
      mips.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      mips.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
      D3D11_TEXTURE2D_DESC gridDesc = {};
      gridDesc.Width = m_glitchColumns;
      gridDesc.Height = m_glitchRows;
      gridDesc.MipLevels = 1;
      gridDesc.ArraySize = 1;
      gridDesc.Format = format;
      gridDesc.SampleDesc.Count = 1;
      gridDesc.Usage = D3D11_USAGE_STAGING;
      gridDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(m_d3d11Device->CreateTexture2D(&mips, nullptr, m_glitchMips.ReleaseAndGetAddressOf()))
       || FAILED(m_d3d11Device->CreateShaderResourceView(m_glitchMips.Get(), nullptr, m_glitchMipsView.ReleaseAndGetAddressOf()))) {
        logMessage(LogLevel::Warn, "glitch detection: cannot create its textures");
        m_glitchMips.Reset();
        m_glitchOff = true;
        return false;
      }
      for (uint32_t i = 0; i < kGlitchSmall; i++) {
        m_glitchSmallOf[i] = 0;
        if (FAILED(m_d3d11Device->CreateTexture2D(&gridDesc, nullptr, m_glitchSmall[i].ReleaseAndGetAddressOf()))) {
          m_glitchMips.Reset();
          m_glitchOff = true;
          return false;
        }
      }
      // The last frames in full, to save the glitch frame and the one before
      // (without them only the log tells).
      D3D11_TEXTURE2D_DESC frame = {};
      frame.Width = desc.Width;
      frame.Height = desc.Height;
      frame.MipLevels = 1;
      frame.ArraySize = 1;
      frame.Format = desc.Format;
      frame.SampleDesc.Count = 1;
      frame.Usage = D3D11_USAGE_DEFAULT;
      for (uint32_t i = 0; i < kGlitchFrames; i++) {
        m_glitchFrameOf[i] = 0;
        if (FAILED(m_d3d11Device->CreateTexture2D(&frame, nullptr, m_glitchFrames[i].ReleaseAndGetAddressOf())))
          m_glitchFrames[i].Reset();
      }
      logMessage(LogLevel::Warn, "glitch detection: on, %ux%u tiles of %ux%u pixels", m_glitchColumns, m_glitchRows,
        desc.Width / m_glitchColumns, desc.Height / m_glitchRows);
      return true;
    }

    const GlitchGrid* glitchGrid(uint32_t present) const {
      const GlitchGrid& grid = m_glitchGrids[present % kGlitchGrids];
      return present && grid.present == present ? &grid : nullptr;
    }

    // With the grid of present q in: does present q-1 (or q-2 and q-1)
    // differ from the frames around it where those agree?
    void checkFlicker(uint32_t q, uint32_t present) {
      for (uint32_t frames = 1; frames <= 2; frames++) {
        if (q < frames + 2)
          return;
        const GlitchGrid* after = glitchGrid(q);
        const GlitchGrid* before = glitchGrid(q - frames - 1);
        const GlitchGrid* middle[2] = { glitchGrid(q - frames), frames == 2 ? glitchGrid(q - 1) : nullptr };
        if (!after || !before || !middle[0] || (frames == 2 && !middle[1]))
          continue;
        const size_t tiles = after->luma.size();
        // How much the picture moves: the mean difference of the frames
        // around (0 for a still camera).
        uint64_t motionSum = 0;
        for (size_t t = 0; t < tiles; t++)
          motionSum += uint32_t(std::abs(int(before->luma[t]) - int(after->luma[t])));
        const uint32_t motion = uint32_t(motionSum / (std::max)(tiles, size_t(1)));
        uint32_t dark = 0, bright = 0;
        uint32_t box[4] = { UINT32_MAX, UINT32_MAX, 0, 0 };
        uint32_t lumaBefore = 0, lumaGlitch = 0, lumaAfter = 0;
        for (size_t t = 0; t < tiles; t++) {
          const int a = before->luma[t], b = after->luma[t];
          // The frames around agree here.
          if (std::abs(a - b) > (std::max)(a, b) / 4 + 6)
            continue;
          const int low = (std::min)(a, b), high = (std::max)(a, b);
          bool isDark = low >= 32, isBright = true;
          for (uint32_t m = 0; m < frames; m++) {
            const int v = middle[m]->luma[t];
            isDark = isDark && v * 5 < low * 2;
            isBright = isBright && v > high * 2 + 40;
          }
          if (!isDark && !isBright)
            continue;
          (isDark ? dark : bright)++;
          const uint32_t x = uint32_t(t % m_glitchColumns), y = uint32_t(t / m_glitchColumns);
          box[0] = (std::min)(box[0], x);
          box[1] = (std::min)(box[1], y);
          box[2] = (std::max)(box[2], x);
          box[3] = (std::max)(box[3], y);
          lumaBefore += uint32_t(a);
          lumaGlitch += middle[0]->luma[t];
          lumaAfter += uint32_t(b);
        }
        const uint32_t count = dark + bright;
        // A patch, not tiles strewn over the picture (edges of a moving
        // camera): at least 6 tiles filling a fifth of their bounding box.
        const uint32_t area = count ? (box[2] - box[0] + 1) * (box[3] - box[1] + 1) : 0;
        if (count < 6 || count * 5 < area)
          continue;
        // With the camera moving, near objects cross the picture within a
        // frame: then only a larger patch (2 % of the picture) counts.
        if (motion > 4 && count * 50 < tiles) {
          m_glitchSuppressed++;
          continue;
        }
        const uint32_t glitch = q - frames;
        // One flicker is seen once (as one frame, not again as two).
        if (glitch <= m_glitchLastEvent)
          return;
        m_glitchLastEvent = q - 1;
        // A candidate right after another is a dark object moving across
        // the picture: neither counts.
        const bool afterOther = m_glitchLastSeen && glitch <= m_glitchLastSeen + 2;
        m_glitchLastSeen = q - 1;
        if (afterOther) {
          if (m_glitchPending.glitch)
            m_glitchSuppressed++;
          m_glitchPending = GlitchCandidate();
          m_glitchSuppressed++;
          return;
        }
        flushGlitchCandidate(present);
        keepGlitchFrames(glitch);
        GlitchCandidate& candidate = m_glitchPending;
        candidate.glitch = glitch;
        candidate.frames = frames;
        candidate.dark = dark;
        candidate.bright = bright;
        candidate.tiles = uint32_t(tiles);
        std::memcpy(candidate.box, box, sizeof(box));
        candidate.luma[0] = lumaBefore / count;
        candidate.luma[1] = lumaGlitch / count;
        candidate.luma[2] = lumaAfter / count;
        return;
      }
      // Two frames on without another candidate: report the pending one.
      if (m_glitchPending.glitch && q >= m_glitchPending.glitch + m_glitchPending.frames + 2)
        flushGlitchCandidate(present);
    }

    void keepGlitchFrames(uint32_t glitch) {
      ComPtr<ID3D11DeviceContext> context;
      m_d3d11Device->GetImmediateContext(context.GetAddressOf());
      for (uint32_t which = 0; which < 2; which++) {
        m_glitchAsideValid[which] = false;
        const uint32_t frame = glitch - which;
        const uint32_t slot = frame % kGlitchFrames;
        if (!m_glitchFrames[slot] || m_glitchFrameOf[slot] != frame || m_glitchEvents >= 4)
          continue;
        if (!m_glitchAside[which]) {
          D3D11_TEXTURE2D_DESC desc = {};
          m_glitchFrames[slot]->GetDesc(&desc);
          if (FAILED(m_d3d11Device->CreateTexture2D(&desc, nullptr, m_glitchAside[which].GetAddressOf())))
            continue;
        }
        context->CopyResource(m_glitchAside[which].Get(), m_glitchFrames[slot].Get());
        m_glitchAsideValid[which] = true;
      }
    }

    void flushGlitchCandidate(uint32_t present) {
      GlitchCandidate candidate = m_glitchPending;
      m_glitchPending = GlitchCandidate();
      if (!candidate.glitch)
        return;
      // Video and loading screens (a few calls a frame) change as they like.
      const size_t calls = m_immediateContext ? m_immediateContext->HistoryCalls(present - candidate.glitch) : SIZE_MAX;
      if (calls < 10) {
        m_glitchSuppressed++;
        return;
      }
      reportGlitch(candidate.glitch, candidate.frames, candidate.dark, candidate.bright, candidate.tiles, candidate.box,
        candidate.luma[0], candidate.luma[1], candidate.luma[2], present);
    }

    void reportGlitch(uint32_t glitch, uint32_t frames, uint32_t dark, uint32_t bright, uint32_t tiles,
                      const uint32_t box[4], uint32_t lumaBefore, uint32_t lumaGlitch, uint32_t lumaAfter,
                      uint32_t present) {
      const uint32_t event = ++m_glitchEvents;
      if (event > 32) {
        if (event == 33)
          logMessage(LogLevel::Warn, "glitch: more than 32, the rest are not logged");
        return;
      }
      const uint32_t tileWidth = m_glitchWidth / m_glitchColumns, tileHeight = m_glitchHeight / m_glitchRows;
      logMessage(LogLevel::Warn,
        "glitch %u: present %u, %u frame%s: %u tiles darker, %u brighter of %u (%.1f%%), pixels %u,%u to %u,%u; brightness %u -> %u -> %u (%u moving objects and video frames skipped so far)",
        event, glitch, frames, frames > 1 ? "s" : "", dark, bright, tiles, 100.0 * (dark + bright) / tiles,
        box[0] * tileWidth, box[1] * tileHeight, (box[2] + 1) * tileWidth, (box[3] + 1) * tileHeight,
        lumaBefore, lumaGlitch, lumaAfter, m_glitchSuppressed);
      // The glitch frame and the one before, copied aside when the glitch
      // was seen (images of the first 4 glitches: 8 MB each at 1080p).
      if (event <= 4) {
        for (uint32_t which = 0; which < 2; which++) {
          if (!m_glitchAsideValid[which])
            continue;
          m_glitchAsideValid[which] = false;
          char name[64];
          sprintf_s(name, which ? "dxup_glitch_%u_before.bmp" : "dxup_glitch_%u.bmp", glitch);
          if (writeBitmap(m_glitchAside[which].Get(), name))
            logMessage(LogLevel::Warn, "glitch %u: wrote %s", event, name);
        }
      }
      // The calls: the glitch frame is `present - glitch` presents back.
      if (m_immediateContext && event <= 8)
        m_immediateContext->LogDrawHistory(present - glitch, glitch, event, event == 1);
    }

    void preparePresent(IBackendContext* backendContext) {
      waitForBackBuffer(backendContext);
      UINT backBufferIndex = m_realSwapChain->GetCurrentBackBufferIndex();
      IBackendImage* presentImage = backBufferIndex < m_backBufferBackendImages.size()
        ? m_backBufferBackendImages[backBufferIndex]
        : nullptr;
      if (!backendContext)
        return;

      // Present is not predicated in D3D11.
      backendContext->suspendPredication(true);
      if (m_direct && m_proxyBackendImage) {
        // The application drew into the buffer Present shows.
        retargetDirectBackBuffer(backendContext);
        presentImage = m_proxyBackendImage;
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        if (dxupFpsOverlayEnabled() && SUCCEEDED(m_realSwapChain->GetDesc(&swapChainDesc)))
          renderFpsOverlay(static_cast<d3d12::D3D12BackendContext*>(backendContext), presentImage,
            swapChainDesc.BufferDesc.Width, swapChainDesc.BufferDesc.Height);
      } else if (m_proxyBackendImage && presentImage && m_appWidth) {
        // dxgi.upscale: the application's smaller buffer scaled up.
        const auto& proxyDesc = m_proxyBackendImage->getDesc();
        IBackendImage* source = m_proxyBackendImage;
        if (proxyDesc.sampleCount > 1 && m_resolveImage) {
          backendContext->resolveSubresource(m_resolveImage.get(), 0, m_proxyBackendImage, 0, proxyDesc.format);
          source = m_resolveImage.get();
        }
        auto* d3d12Context = static_cast<d3d12::D3D12BackendContext*>(backendContext);
        if (!d3d12Context->scaleImage(presentImage, source, UserOverrides::get().spatialSharpness)) {
          static bool warned = false;
          if (!warned)
            logMessage(LogLevel::Warn, "dxgi.upscale: scaling the back buffer failed");
          warned = true;
        }
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        if (dxupFpsOverlayEnabled() && SUCCEEDED(m_realSwapChain->GetDesc(&swapChainDesc)))
          renderFpsOverlay(d3d12Context, presentImage, swapChainDesc.BufferDesc.Width, swapChainDesc.BufferDesc.Height);
      } else if (m_proxyBackendImage && presentImage) {
        const auto& proxyDesc = m_proxyBackendImage->getDesc();
        if (proxyDesc.sampleCount > 1) {
          if (m_resolveImage) {
            backendContext->resolveSubresource(m_resolveImage.get(), 0, m_proxyBackendImage, 0, proxyDesc.format);
            backendContext->copyImage(presentImage, {}, m_resolveImage.get(), {});
          } else {
            backendContext->resolveSubresource(presentImage, 0, m_proxyBackendImage, 0, proxyDesc.format);
          }
        } else {
          backendContext->copyImage(presentImage, {}, m_proxyBackendImage, {});
        }

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        if (dxupFpsOverlayEnabled() && SUCCEEDED(m_realSwapChain->GetDesc(&swapChainDesc))) {
          auto* d3d12Context = static_cast<d3d12::D3D12BackendContext*>(backendContext);
          renderFpsOverlay(
            d3d12Context,
            presentImage,
            swapChainDesc.BufferDesc.Width,
            swapChainDesc.BufferDesc.Height);
        }
      }

      backendContext->presentImage(presentImage);
      backendContext->suspendPredication(false);
    }

    void updateFps() {
      if (!m_fpsTimerInit) {
        QueryPerformanceFrequency(&m_fpsFreq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        m_fpsLastTime = static_cast<double>(now.QuadPart) / m_fpsFreq.QuadPart;
        m_fpsTimerInit = true;
      }
      m_fpsFrameCount++;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      double t = static_cast<double>(now.QuadPart) / m_fpsFreq.QuadPart;
      double elapsed = t - m_fpsLastTime;
      if (elapsed >= 1.0) {
        m_currentFps = static_cast<float>(m_fpsFrameCount / elapsed);
        logMessage(LogLevel::Info, "FPS: %.1f", m_currentFps);
        m_fpsFrameCount = 0;
        m_fpsLastTime = t;
      }
    }

    void renderFpsOverlay(d3d12::D3D12BackendContext* ctx, IBackendImage* target,
                          uint32_t targetW, uint32_t targetH) {
      // 5x7 bitmap font: 0-9, space, F, P, S, period
      static const uint8_t font[][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
        {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 2
        {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space (10)
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F (11)
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P (12)
        {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // S (13)
        {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // . (14)
      };
      auto ci = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c == 'F') return 11; if (c == 'P') return 12;
        if (c == 'S') return 13; if (c == '.') return 14;
        return 10;
      };

      char fpsStr[16];
      snprintf(fpsStr, sizeof(fpsStr), "%.1f FPS", m_currentFps);
      uint32_t textLen = static_cast<uint32_t>(strlen(fpsStr));

      constexpr uint32_t S = 2, CW = 5*S, CH = 7*S, GAP = S, PAD = 4;
      uint32_t overlayW = (textLen * (CW + GAP) + PAD * 2 + 3) & ~3u;
      uint32_t overlayH = CH + PAD * 2;

      std::vector<uint32_t> px(overlayW * overlayH, 0xFF1A1A1A); // dark bg

      for (uint32_t i = 0; i < textLen; i++) {
        int fi = ci(fpsStr[i]);
        uint32_t bx = PAD + i * (CW + GAP);
        for (uint32_t cy = 0; cy < 7; cy++) {
          uint8_t row = font[fi][cy];
          for (uint32_t cx = 0; cx < 5; cx++) {
            if (row & (0x10 >> cx)) {
              for (uint32_t sy = 0; sy < S; sy++)
                for (uint32_t sx = 0; sx < S; sx++) {
                  uint32_t x = bx + cx*S + sx, y = PAD + cy*S + sy;
                  if (x < overlayW && y < overlayH)
                    px[y * overlayW + x] = 0xFF00FF00; // green
                }
            }
          }
        }
      }

      uint32_t dstX = (targetW > overlayW + 8) ? targetW - overlayW - 8 : 0;
      ctx->blitPixels(target, dstX, 8, overlayW, overlayH, px.data());
    }

    void createBackBufferTextures(UINT bufferCount) {
      m_backBufferTextures.resize(bufferCount);
      m_backBufferBackendImages.assign(bufferCount, nullptr);

      for (UINT i = 0; i < bufferCount; i++) {
        ComPtr<ID3D12Resource> d3d12Resource;
        HRESULT hr = m_realSwapChain->GetBuffer(i,
          __uuidof(ID3D12Resource),
          reinterpret_cast<void**>(d3d12Resource.GetAddressOf()));

        if (FAILED(hr) || !d3d12Resource)
          continue;
        if (IBackendContext* ctx = backendContext()) {
          ComPtr<IUnknown> held = d3d12Resource.Get();
          ctx->executeOnBackend([held](IBackendContext* backend) { backend->trackSwapChainBuffer(held.Get()); });
        }

        D3D12_RESOURCE_DESC resDesc = d3d12Resource->GetDesc();

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width              = static_cast<UINT>(resDesc.Width);
        texDesc.Height             = resDesc.Height;
        texDesc.MipLevels          = resDesc.MipLevels;
        texDesc.ArraySize          = resDesc.DepthOrArraySize;
        texDesc.Format             = resDesc.Format;
        texDesc.SampleDesc.Count   = resDesc.SampleDesc.Count;
        texDesc.SampleDesc.Quality = resDesc.SampleDesc.Quality;
        texDesc.Usage              = D3D11_USAGE_DEFAULT;
        // The bindings the swap chain's buffers allow.
        texDesc.BindFlags          = D3D11_BIND_RENDER_TARGET;
        if (!(resDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
          texDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        if (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
          texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        texDesc.CPUAccessFlags     = 0;
        texDesc.MiscFlags          = 0;

        ImageDesc imgDesc = {};
        imgDesc.type        = ImageType::Tex2D;
        imgDesc.width       = texDesc.Width;
        imgDesc.height      = texDesc.Height;
        imgDesc.depth       = 1;
        imgDesc.mipLevels   = texDesc.MipLevels;
        imgDesc.arraySize   = texDesc.ArraySize;
        imgDesc.format      = texDesc.Format;
        imgDesc.sampleCount = texDesc.SampleDesc.Count;
        imgDesc.sampleQuality = texDesc.SampleDesc.Quality;
        imgDesc.bindFlags   = BindFlags::RenderTarget;
        if (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
          imgDesc.bindFlags = imgDesc.bindFlags | BindFlags::ShaderResource;
        if (texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
          imgDesc.bindFlags = imgDesc.bindFlags | BindFlags::UnorderedAccess;

        auto backendImage = std::make_shared<d3d12::D3D12BackendImage>(
          std::move(d3d12Resource), imgDesc);
        backendImage->setCurrentState(D3D12_RESOURCE_STATE_PRESENT);
        m_backBufferBackendImages[i] = backendImage.get();

        auto* texture = new D3D11Texture2D(m_d3d11Device, texDesc, backendImage);
        const UINT swapChainMarker = 1;
        texture->SetPrivateData(kDxupSwapChainBufferGuid, sizeof(swapChainMarker), &swapChainMarker);
        ComPtr<IUnknown> unknown;
        texture->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(unknown.GetAddressOf()));
        m_backBufferTextures[i] = unknown;
        texture->Release();
      }

      // Create a proxy render target that the game renders to instead of
      // directly using swap chain buffers.  This proxy has ALLOW_RENDER_TARGET
      // and ALLOW_UNORDERED_ACCESS so the game can use it for both RT and
      // compute UAV operations.  At Present time we copy proxy → current BB.
      if (!m_proxyRenderTarget && bufferCount > 0 && m_d3d11Device && !createDirectBackBuffer(bufferCount)) {
        ComPtr<ID3D12Resource> d3d12Res;
        HRESULT hr = m_realSwapChain->GetBuffer(0,
          __uuidof(ID3D12Resource),
          reinterpret_cast<void**>(d3d12Res.GetAddressOf()));
        if (SUCCEEDED(hr) && d3d12Res) {
          D3D12_RESOURCE_DESC resDesc = d3d12Res->GetDesc();
          d3d12Res.Reset();  // Don't hold onto swap chain buffer

          // The proxy has the format and sample count the application
          // asked for. UAVs exist for single-sampled non-sRGB formats only.
          const DXGI_FORMAT proxyFormat = m_appFormat != DXGI_FORMAT_UNKNOWN ? m_appFormat : resDesc.Format;
          const bool multisampled = m_appSampleDesc.Count > 1;
          ImageDesc proxyDesc = {};
          proxyDesc.type          = ImageType::Tex2D;
          proxyDesc.width         = m_appWidth ? m_appWidth : static_cast<uint32_t>(resDesc.Width);
          proxyDesc.height        = m_appWidth ? m_appHeight : resDesc.Height;
          proxyDesc.depth         = 1;
          proxyDesc.mipLevels     = resDesc.MipLevels;
          proxyDesc.arraySize     = resDesc.DepthOrArraySize;
          proxyDesc.format        = proxyFormat;
          proxyDesc.sampleCount   = m_d3d11Device->EnhancedSampleCount(proxyFormat, m_appSampleDesc.Count);
          proxyDesc.sampleQuality = proxyDesc.sampleCount == m_appSampleDesc.Count ? m_appSampleDesc.Quality : 0;
          proxyDesc.bindFlags     = BindFlags::RenderTarget | BindFlags::ShaderResource;
          if (!multisampled && !isSrgbFormat(proxyFormat))
            proxyDesc.bindFlags = proxyDesc.bindFlags | BindFlags::UnorderedAccess;
          // Games render to UNORM buffers through sRGB views.
          proxyDesc.miscFlags = kResourceMiscCastable;

          auto* backend = m_d3d11Device->GetBackend();
          auto proxyImage = backend->createImage(proxyDesc);

          // A multisampled sRGB proxy resolves in its own format, which the
          // UNORM swap chain buffer cannot take directly.
          if (proxyImage && multisampled && (proxyFormat != resDesc.Format || m_appWidth)) {
            ImageDesc resolveDesc = proxyDesc;
            resolveDesc.sampleCount = 1;
            resolveDesc.sampleQuality = 0;
            resolveDesc.bindFlags = BindFlags::ShaderResource;
            m_resolveImage = backend->createImage(resolveDesc);
          }
          if (proxyImage) {
            m_proxyBackendImage = proxyImage.get();
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width            = proxyDesc.width;
            texDesc.Height           = proxyDesc.height;
            texDesc.MipLevels        = proxyDesc.mipLevels;
            texDesc.ArraySize        = proxyDesc.arraySize;
            texDesc.Format           = proxyDesc.format;
            // The sample count the application asked for, whatever the
            // proxy renders with.
            texDesc.SampleDesc       = m_appSampleDesc;
            texDesc.Usage            = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (static_cast<uint32_t>(proxyDesc.bindFlags & BindFlags::UnorderedAccess))
              texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            texDesc.CPUAccessFlags   = 0;
            texDesc.MiscFlags        = 0;

            auto* proxyTex = new D3D11Texture2D(m_d3d11Device, texDesc, proxyImage);
            ForcedMsaaFormats forcedMsaa;
            if (m_d3d11Device->ShouldForceMsaa(texDesc, forcedMsaa))
              m_d3d11Device->AttachMsaaShadow(proxyTex, forcedMsaa);
            const UINT swapChainMarker = 1;
            proxyTex->SetPrivateData(kDxupSwapChainBufferGuid, sizeof(swapChainMarker), &swapChainMarker);
            ComPtr<IUnknown> unknown;
            proxyTex->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(unknown.GetAddressOf()));
            m_proxyRenderTarget = unknown;
            proxyTex->Release();
            logMessage(LogLevel::Info, "Created proxy render target: %ux%u fmt=%u (RT+UAV+SRV)",
              proxyDesc.width, proxyDesc.height, static_cast<uint32_t>(proxyDesc.format));
          } else {
            logMessage(LogLevel::Error, "Failed to create proxy render target!");
          }
        }
      }
    }
  };


  // ============================================================
  // DXGIDevice : IDXGIDevice4
  //
  // Wraps our D3D11Device as a DXGI device. Games use this to
  // navigate from ID3D11Device -> IDXGIDevice -> IDXGIAdapter.
  // ============================================================
  class DXGIDevice : public IDXGIDevice4 {
  public:
    DXGIDevice(D3D11Device* d3d11Device, DXGIAdapter* adapter)
    : m_d3d11Device(d3d11Device)
    , m_adapter(adapter)
    , m_gpuThreadPriority(0) {
      if (m_adapter) m_adapter->AddRef();
    }

    ~DXGIDevice() {
      if (m_adapter) m_adapter->Release();
    }

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;

      if (riid == __uuidof(IDXGIObject)
       || riid == __uuidof(IDXGIDevice)
       || riid == __uuidof(IDXGIDevice1)
       || riid == __uuidof(IDXGIDevice2)
       || riid == __uuidof(IDXGIDevice3)
       || riid == __uuidof(IDXGIDevice4)) {
        *ppvObject = static_cast<IDXGIDevice4*>(this);
        AddRef();
        return S_OK;
      }

      // IDXGIDevice and the D3D11 device are one COM object natively, so
      // IUnknown and every D3D11 interface resolve through the device.
      if (m_d3d11Device)
        return reinterpret_cast<IUnknown*>(m_d3d11Device)->QueryInterface(riid, ppvObject);

      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return m_d3d11Device
        ? reinterpret_cast<IUnknown*>(m_d3d11Device)->AddRef()
        : 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return m_d3d11Device
        ? reinterpret_cast<IUnknown*>(m_d3d11Device)->Release()
        : 0;
    }

    // ---- IDXGIObject ----

    // IDXGIDevice and ID3D11Device are the same object natively, so
    // private data set through either interface is visible to both.

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
      return m_d3d11Device
        ? m_d3d11Device->SetPrivateData(Name, DataSize, pData)
        : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
      return m_d3d11Device
        ? m_d3d11Device->SetPrivateDataInterface(Name, pUnknown)
        : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
      return m_d3d11Device
        ? m_d3d11Device->GetPrivateData(Name, pDataSize, pData)
        : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
      if (!ppParent) return E_POINTER;

      if (m_adapter)
        return m_adapter->QueryInterface(riid, ppParent);

      *ppParent = nullptr;
      return E_NOINTERFACE;
    }

    // ---- IDXGIDevice ----

    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** pAdapter) override {
      if (!pAdapter) return E_POINTER;

      if (m_adapter) {
        *pAdapter = static_cast<IDXGIAdapter*>(m_adapter);
        m_adapter->AddRef();
        return S_OK;
      }

      *pAdapter = nullptr;
      return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE CreateSurface(
      const DXGI_SURFACE_DESC*  pDesc,
      UINT                      NumSurfaces,
      DXGI_USAGE                Usage,
      const DXGI_SHARED_RESOURCE* pSharedResource,
      IDXGISurface**            ppSurface) override {
      (void)pDesc; (void)NumSurfaces; (void)Usage;
      (void)pSharedResource; (void)ppSurface;
      // Surface creation not needed for typical game usage
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryResourceResidency(
      IUnknown* const*    ppResources,
      DXGI_RESIDENCY*     pResidencyStatus,
      UINT                NumResources) override {
      if (!ppResources || !pResidencyStatus)
        return E_POINTER;

      // Report all resources as fully resident
      for (UINT i = 0; i < NumResources; i++)
        pResidencyStatus[i] = DXGI_RESIDENCY_FULLY_RESIDENT;

      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override {
      if (Priority < -7 || Priority > 7)
        return E_INVALIDARG;
      m_gpuThreadPriority = Priority;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override {
      if (!pPriority) return E_POINTER;
      *pPriority = m_gpuThreadPriority;
      return S_OK;
    }

    // ---- IDXGIDevice1 ----

    // Stored on the device, where the swap chain Present enforces it.
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
      constexpr UINT kMaxFrameLatency = 16; // documented IDXGIDevice1 maximum
      if (MaxLatency > kMaxFrameLatency)
        return DXGI_ERROR_INVALID_CALL;
      if (m_d3d11Device)
        m_d3d11Device->SetMaximumFrameLatency(MaxLatency);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override {
      if (!pMaxLatency) return DXGI_ERROR_INVALID_CALL;
      *pMaxLatency = m_d3d11Device ? m_d3d11Device->GetMaximumFrameLatency() : 3u;
      return S_OK;
    }

    // ---- IDXGIDevice2 ----

    HRESULT STDMETHODCALLTYPE OfferResources(
        UINT                 NumResources,
        IDXGIResource* const* ppResources,
        DXGI_OFFER_RESOURCE_PRIORITY Priority) override {
      (void)NumResources;
      (void)ppResources;
      (void)Priority;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ReclaimResources(
        UINT                 NumResources,
        IDXGIResource* const* ppResources,
        BOOL*                pDiscarded) override {
      (void)NumResources;
      (void)ppResources;
      if (pDiscarded) {
        for (UINT i = 0; i < NumResources; i++)
          pDiscarded[i] = FALSE;
      }
      return S_OK;
    }

    // Signalled once the GPU has finished all work submitted so far.
    HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE hEvent) override {
      if (!hEvent) return E_INVALIDARG;
      ComPtr<ID3D11DeviceContext> context;
      ComPtr<ID3D11DeviceContext3> context3;
      if (m_d3d11Device)
        m_d3d11Device->GetImmediateContext(context.GetAddressOf());
      if (context && SUCCEEDED(context.As(&context3)))
        context3->Flush1(D3D11_CONTEXT_TYPE_ALL, hEvent);
      else
        SetEvent(hEvent);
      return S_OK;
    }

    // ---- IDXGIDevice3 ----

    // Driver-internal memory: nothing to give back.
    void STDMETHODCALLTYPE Trim() override {}

    // ---- IDXGIDevice4 ----

    HRESULT STDMETHODCALLTYPE OfferResources1(
        UINT                 NumResources,
        IDXGIResource* const* ppResources,
        DXGI_OFFER_RESOURCE_PRIORITY Priority,
        UINT                 Flags) override {
      (void)Flags;
      return OfferResources(NumResources, ppResources, Priority);
    }

    // Offered resources keep their contents.
    HRESULT STDMETHODCALLTYPE ReclaimResources1(
        UINT                 NumResources,
        IDXGIResource* const* ppResources,
        DXGI_RECLAIM_RESOURCE_RESULTS* pResults) override {
      (void)ppResources;
      if (pResults) {
        for (UINT i = 0; i < NumResources; i++)
          pResults[i] = DXGI_RECLAIM_RESOURCE_RESULT_OK;
      }
      return S_OK;
    }

    // ---- Accessors ----

    D3D11Device*  getD3D11Device() const { return m_d3d11Device; }
    DXGIAdapter*  getAdapter()     const { return m_adapter; }

  private:
    D3D11Device*        m_d3d11Device;
    DXGIAdapter*        m_adapter;
    INT                 m_gpuThreadPriority;
  };


  // ============================================================
  // DXGIFactory out-of-class inline method definitions
  //
  // These are defined here because they depend on DXGIAdapter
  // and DXGISwapChain being fully declared.
  // ============================================================

  inline HRESULT STDMETHODCALLTYPE DXGIFactory::EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) {
    if (!ppAdapter) return E_POINTER;

    if (!m_realFactory) {
      *ppAdapter = nullptr;
      logMessage(LogLevel::Error,
        "DXGIFactory::EnumAdapters rejected null realFactory this=%p ref=%lu",
        this, static_cast<unsigned long>(m_refCount.load()));
      return E_FAIL;
    }

    ComPtr<IDXGIAdapter1> realAdapter;
    HRESULT hr = m_realFactory->EnumAdapters1(Adapter, realAdapter.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      *ppAdapter = nullptr;
      return hr;
    }

    *ppAdapter = new DXGIAdapter(realAdapter, this);
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE DXGIFactory::EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) {
    if (!ppAdapter) return E_POINTER;

    if (!m_realFactory) {
      *ppAdapter = nullptr;
      logMessage(LogLevel::Error,
        "DXGIFactory::EnumAdapters1 rejected null realFactory this=%p ref=%lu",
        this, static_cast<unsigned long>(m_refCount.load()));
      return E_FAIL;
    }

    ComPtr<IDXGIAdapter1> realAdapter;
    HRESULT hr = m_realFactory->EnumAdapters1(Adapter, realAdapter.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      *ppAdapter = nullptr;
      return hr;
    }

    *ppAdapter = new DXGIAdapter(realAdapter, this);
    return S_OK;
  }

  inline HRESULT getDxgiSwapChainDeviceAndQueue(
    IUnknown*             pDevice,
    D3D11Device**         ppOurDevice,
    ID3D12CommandQueue**  ppCommandQueue,
    IDXGIFactory4**       ppBackendFactory = nullptr) {
    if (ppOurDevice)
      *ppOurDevice = nullptr;
    if (ppCommandQueue)
      *ppCommandQueue = nullptr;
    if (ppBackendFactory)
      *ppBackendFactory = nullptr;
    if (!pDevice)
      return E_INVALIDARG;

    // Anything but a DXUP device (a D3D12 command queue, a native device)
    // goes to the system factory unchanged: S_FALSE.
    ComPtr<ID3D11Device5> dxupDevice;
    if (FAILED(pDevice->QueryInterface(kDxupD3D11DeviceIid, reinterpret_cast<void**>(dxupDevice.GetAddressOf()))))
      return S_FALSE;

    auto* ourDevice = static_cast<D3D11Device*>(dxupDevice.Get());
    auto* backend = ourDevice->GetBackend();
    if (!backend || backend->getBackendType() != BackendType::D3D12)
      return E_NOINTERFACE;

    auto* commandQueue = static_cast<ID3D12CommandQueue*>(backend->getNativeCommandQueue());
    if (!commandQueue)
      return E_FAIL;

    if (ppOurDevice) {
      *ppOurDevice = ourDevice;
      ourDevice->AddRef();
    }
    if (ppCommandQueue)
      *ppCommandQueue = commandQueue;
    if (ppBackendFactory) {
      auto* d3d12Backend = static_cast<d3d12::D3D12BackendDevice*>(backend);
      *ppBackendFactory = d3d12Backend->getDxgiFactory();
    }
    return S_OK;
  }

  // Whether presents with interval 0 may tear, for VSync off in a window.
  inline bool presentTearingSupported(IUnknown* factory) {
    ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    return factory && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))
      && SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)))
      && allowTearing;
  }

  // The flags of the real flip model swap chain: it cannot switch modes, and
  // VSync forced off by dxup.conf tears where the system allows it. Unless
  // the application waits for frames itself, DXUP does with the swap chain's
  // frame latency object (see DXGISwapChain::setApplicationFlags).
  inline UINT realSwapChainFlags(IUnknown* factory, UINT flags) {
    flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (!dxupBlockingPresent())
      flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (UserOverrides::get().syncInterval == 0 && presentTearingSupported(factory)) {
      flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
      logMessage(LogLevel::Info, "dxup.conf: VSync off, the swap chain presents with tearing");
    }
    return flags;
  }

  inline HRESULT wrapDxgiSwapChain(
    DXGIFactory*            factory,
    D3D11Device*            ourDevice,
    ComPtr<IDXGISwapChain1> realSwapChain1,
    ID3D12CommandQueue*     commandQueue,
    void**                  ppSwapChain,
    REFIID                  riid,
    DXGI_FORMAT             appFormat,
    DXGI_SAMPLE_DESC        appSampleDesc,
    UINT                    appFlags,
    DXGI_SWAP_EFFECT        appSwapEffect,
    DXGI_USAGE              appUsage,
    UINT                    appWidth = 0,
    UINT                    appHeight = 0) {
    if (!factory || !ourDevice || !ppSwapChain)
      return E_INVALIDARG;

    *ppSwapChain = nullptr;

    ComPtr<IDXGISwapChain3> swapChain3;
    HRESULT hr = realSwapChain1.As(&swapChain3);
    if (FAILED(hr))
      return hr;

    auto* wrappedSwapChain = new DXGISwapChain(ourDevice, factory, swapChain3, commandQueue);
    wrappedSwapChain->setApplicationBufferDesc(appFormat, appSampleDesc);
    wrappedSwapChain->setApplicationSwapModel(appSwapEffect, appUsage);
    wrappedSwapChain->setApplicationFlags(appFlags);
    if (appWidth)
      wrappedSwapChain->setApplicationSize(appWidth, appHeight);
    hr = wrappedSwapChain->QueryInterface(riid, ppSwapChain);
    wrappedSwapChain->Release();
    return hr;
  }

  inline HRESULT STDMETHODCALLTYPE DXGIFactory::CreateSwapChain(
    IUnknown*                 pDevice,
    DXGI_SWAP_CHAIN_DESC*     pDesc,
    IDXGISwapChain**          ppSwapChain) {
    if (!pDevice || !pDesc || !ppSwapChain)
      return E_POINTER;

    *ppSwapChain = nullptr;

    logMessage(LogLevel::Info, "DXGIFactory::CreateSwapChain(BufferCount=%u, Format=%d, HWND=0x%p, device=%p)",
      pDesc->BufferCount, pDesc->BufferDesc.Format, pDesc->OutputWindow, static_cast<void*>(pDevice));

    // The pDevice should be our D3D11Device. We need to get
    // the D3D12 command queue from the backend to create a
    // real swap chain that D3D12 can present with.

    // Build a DXGI_SWAP_CHAIN_DESC1 from the legacy desc
    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    desc1.Width       = pDesc->BufferDesc.Width;
    desc1.Height      = pDesc->BufferDesc.Height;
    desc1.Format      = flipModelFormat(pDesc->BufferDesc.Format);
    desc1.Stereo      = FALSE;
    desc1.SampleDesc  = pDesc->SampleDesc;
    desc1.BufferUsage = pDesc->BufferUsage;
    desc1.BufferCount = pDesc->BufferCount;
    desc1.Scaling     = DXGI_SCALING_STRETCH;
    desc1.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc1.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Flags       = realSwapChainFlags(m_realFactory.Get(), pDesc->Flags);
    // UNORDERED_ACCESS is not supported with flip model swap chains
    desc1.BufferUsage &= ~DXGI_USAGE_UNORDERED_ACCESS;

    // Force buffer count to at least 2 for flip model
    if (desc1.BufferCount < 2)
      desc1.BufferCount = 2;

    // Force sample count to 1 for flip model swap chains
    desc1.SampleDesc.Count   = 1;
    desc1.SampleDesc.Quality = 0;
    UINT appWidth = 0, appHeight = 0;
    const bool scaled = spatialUpscaleSize(pDesc->OutputWindow, pDesc->Windowed != FALSE, desc1.Scaling,
      desc1.Width, desc1.Height, appWidth, appHeight);

    D3D11Device* ourDevice = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    IDXGIFactory4* backendFactory = nullptr;
    HRESULT hr = getDxgiSwapChainDeviceAndQueue(pDevice, &ourDevice, &commandQueue, &backendFactory);
    if (FAILED(hr))
      return hr;
    if (hr == S_FALSE)
      return m_realFactory->CreateSwapChain(pDevice, pDesc, ppSwapChain);

    // Use the backend's factory (same one that created the D3D12 device)
    // D3D12 requires the swap chain factory to match the device's factory
    IDXGIFactory4* factoryForSwapChain = backendFactory ? backendFactory : m_realFactory.Get();

    // Create the real swap chain using the D3D12 command queue. A swap
    // chain asked for in full screen is created so, as DXGI does for
    // CreateSwapChain: switching it afterwards would need ResizeBuffers
    // before its first Present, which failed (Just Cause 2).
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
    fullscreenDesc.RefreshRate      = pDesc->BufferDesc.RefreshRate;
    fullscreenDesc.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    fullscreenDesc.Scaling          = pDesc->BufferDesc.Scaling;
    fullscreenDesc.Windowed         = pDesc->Windowed;
    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factoryForSwapChain->CreateSwapChainForHwnd(
      commandQueue,
      pDesc->OutputWindow,
      &desc1,
      &fullscreenDesc,
      nullptr,   // restrict to output
      swapChain1.ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
      ourDevice->Release();
      return hr;
    }

    // Alt+Enter off in a window; what the application associated through
    // this factory goes to the one of the swap chain too.
    associateBackendFactory(factoryForSwapChain, pDesc->OutputWindow, pDesc->Windowed != FALSE);

    hr = wrapDxgiSwapChain(this, ourDevice, swapChain1, commandQueue, reinterpret_cast<void**>(ppSwapChain), __uuidof(IDXGISwapChain),
      pDesc->BufferDesc.Format, pDesc->SampleDesc, pDesc->Flags, pDesc->SwapEffect, pDesc->BufferUsage,
      scaled ? appWidth : 0, scaled ? appHeight : 0);
    ourDevice->Release();
    return hr;
  }

  inline HRESULT STDMETHODCALLTYPE DXGIFactory::CreateSwapChainForHwnd(
    IUnknown*                         pDevice,
    HWND                              hWnd,
    const DXGI_SWAP_CHAIN_DESC1*      pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput*                      pRestrictToOutput,
    IDXGISwapChain1**                 ppSwapChain) {
    if (!pDevice || !pDesc || !ppSwapChain)
      return E_POINTER;

    *ppSwapChain = nullptr;

    logMessage(LogLevel::Info, "DXGIFactory::CreateSwapChainForHwnd(HWND=0x%p, %ux%u, Format=%d, BufferCount=%u, SwapEffect=%d, SampleCount=%u, Flags=0x%x, BufferUsage=0x%x, Stereo=%d, Scaling=%d, AlphaMode=%d)",
      hWnd, pDesc->Width, pDesc->Height, pDesc->Format, pDesc->BufferCount, pDesc->SwapEffect, pDesc->SampleDesc.Count, pDesc->Flags, pDesc->BufferUsage, pDesc->Stereo, pDesc->Scaling, pDesc->AlphaMode);

    logMessage(LogLevel::Info, "  HWND valid=%d, pDevice=0x%p", IsWindow(hWnd), pDevice);

    D3D11Device* ourDevice = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    IDXGIFactory4* backendFactory = nullptr;
    HRESULT hr = getDxgiSwapChainDeviceAndQueue(pDevice, &ourDevice, &commandQueue, &backendFactory);
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "DXGIFactory::CreateSwapChainForHwnd: getDxgiSwapChainDeviceAndQueue failed hr=0x%08X", hr);
      return hr;
    }
    if (hr == S_FALSE)
      return m_realFactory->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, realOutputOf(pRestrictToOutput).Get(), ppSwapChain);

    // Apply D3D12-compatible fixups: flip model requires specific settings
    DXGI_SWAP_CHAIN_DESC1 fixedDesc = *pDesc;
    fixedDesc.Format = flipModelFormat(pDesc->Format);
    fixedDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (fixedDesc.BufferCount < 2)
      fixedDesc.BufferCount = 2;
    fixedDesc.SampleDesc.Count   = 1;
    fixedDesc.SampleDesc.Quality = 0;
    fixedDesc.Flags = realSwapChainFlags(m_realFactory.Get(), fixedDesc.Flags);
    // Ensure valid buffer usage for D3D12 flip model
    if (fixedDesc.BufferUsage == 0)
      fixedDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // UNORDERED_ACCESS is not supported with flip model swap chains
    fixedDesc.BufferUsage &= ~DXGI_USAGE_UNORDERED_ACCESS;
    UINT appWidth = 0, appHeight = 0;
    const bool scaled = spatialUpscaleSize(hWnd, !pFullscreenDesc || pFullscreenDesc->Windowed, fixedDesc.Scaling,
      fixedDesc.Width, fixedDesc.Height, appWidth, appHeight);

    logMessage(LogLevel::Info, "DXGIFactory::CreateSwapChainForHwnd: fixed desc: BufferCount=%u, SwapEffect=%d, SampleCount=%u, Flags=0x%x, BufferUsage=0x%x",
      fixedDesc.BufferCount, fixedDesc.SwapEffect, fixedDesc.SampleDesc.Count, fixedDesc.Flags, fixedDesc.BufferUsage);

    // Use the backend's factory (same one that created the D3D12 device)
    IDXGIFactory4* factoryForSwapChain = backendFactory ? backendFactory : m_realFactory.Get();
    logMessage(LogLevel::Info, "  Using factory: backendFactory=0x%p, m_realFactory=0x%p, chosen=0x%p, commandQueue=0x%p",
      backendFactory, m_realFactory.Get(), factoryForSwapChain, commandQueue);

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factoryForSwapChain->CreateSwapChainForHwnd(
      commandQueue,
      hWnd,
      &fixedDesc,
      pFullscreenDesc,
      realOutputOf(pRestrictToOutput).Get(),
      swapChain1.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "DXGIFactory::CreateSwapChainForHwnd: real CreateSwapChainForHwnd failed hr=0x%08X", hr);
      ourDevice->Release();
      return hr;
    }
    // (No Alt+Enter default here: DXGI keeps handling it, as before.)
    associateBackendFactory(factoryForSwapChain, hWnd, false);

    logMessage(LogLevel::Info, "DXGIFactory::CreateSwapChainForHwnd: swap chain created successfully");

    hr = wrapDxgiSwapChain(this, ourDevice, swapChain1, commandQueue, reinterpret_cast<void**>(ppSwapChain), __uuidof(IDXGISwapChain1),
      pDesc->Format, pDesc->SampleDesc, pDesc->Flags, pDesc->SwapEffect, pDesc->BufferUsage,
      scaled ? appWidth : 0, scaled ? appHeight : 0);
    ourDevice->Release();
    return hr;
  }

  inline HRESULT STDMETHODCALLTYPE DXGIFactory::CreateSwapChainForComposition(
    IUnknown*                    pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput*                 pRestrictToOutput,
    IDXGISwapChain1**            ppSwapChain) {
    if (!pDevice || !pDesc || !ppSwapChain)
      return E_POINTER;

    *ppSwapChain = nullptr;

    logMessage(LogLevel::Info, "DXGIFactory::CreateSwapChainForComposition(%ux%u, Format=%d, BufferCount=%u, SwapEffect=%d)",
      pDesc->Width, pDesc->Height, pDesc->Format, pDesc->BufferCount, pDesc->SwapEffect);

    D3D11Device* ourDevice = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    IDXGIFactory4* backendFactory = nullptr;
    HRESULT hr = getDxgiSwapChainDeviceAndQueue(pDevice, &ourDevice, &commandQueue, &backendFactory);
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "DXGIFactory::CreateSwapChainForComposition: getDxgiSwapChainDeviceAndQueue failed hr=0x%08X", hr);
      return hr;
    }
    if (hr == S_FALSE)
      return m_realFactory->CreateSwapChainForComposition(pDevice, pDesc, realOutputOf(pRestrictToOutput).Get(), ppSwapChain);

    // Apply D3D12-compatible fixups
    DXGI_SWAP_CHAIN_DESC1 fixedDesc = *pDesc;
    fixedDesc.Format = flipModelFormat(pDesc->Format);
    fixedDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (fixedDesc.BufferCount < 2)
      fixedDesc.BufferCount = 2;
    fixedDesc.SampleDesc.Count   = 1;
    fixedDesc.SampleDesc.Quality = 0;
    fixedDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    // UNORDERED_ACCESS is not supported with flip model swap chains
    fixedDesc.BufferUsage &= ~DXGI_USAGE_UNORDERED_ACCESS;

    // Use the backend's factory (same one that created the D3D12 device)
    IDXGIFactory4* factoryForSwapChain = backendFactory ? backendFactory : m_realFactory.Get();

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factoryForSwapChain->CreateSwapChainForComposition(
      commandQueue,
      &fixedDesc,
      realOutputOf(pRestrictToOutput).Get(),
      swapChain1.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "DXGIFactory::CreateSwapChainForComposition: failed hr=0x%08X", hr);
      ourDevice->Release();
      return hr;
    }

    hr = wrapDxgiSwapChain(this, ourDevice, swapChain1, commandQueue, reinterpret_cast<void**>(ppSwapChain), __uuidof(IDXGISwapChain1),
      pDesc->Format, pDesc->SampleDesc, pDesc->Flags, pDesc->SwapEffect, pDesc->BufferUsage);
    ourDevice->Release();
    return hr;
  }

  inline HRESULT STDMETHODCALLTYPE DXGIFactory::EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) {
    if (!ppvAdapter)
      return E_POINTER;

    *ppvAdapter = nullptr;

    ComPtr<IDXGIAdapter1> realAdapter;
    HRESULT hr = m_realFactory->EnumAdapterByLuid(AdapterLuid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(realAdapter.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
      return hr;

    auto* adapter = new DXGIAdapter(realAdapter, this);
    hr = adapter->QueryInterface(riid, ppvAdapter);
    adapter->Release();
    return hr;
  }

  inline HRESULT STDMETHODCALLTYPE DXGIFactory::EnumAdapterByGpuPreference(
      UINT Adapter,
      DXGI_GPU_PREFERENCE GpuPreference,
      REFIID riid,
      void** ppvAdapter) {
    if (!ppvAdapter)
      return E_POINTER;

    *ppvAdapter = nullptr;

    ComPtr<IDXGIFactory6> factory6;
    HRESULT hr = m_realFactory.As(&factory6);
    if (FAILED(hr))
      return hr;

    ComPtr<IDXGIAdapter1> realAdapter;
    hr = factory6->EnumAdapterByGpuPreference(
      Adapter,
      GpuPreference,
      __uuidof(IDXGIAdapter1),
      reinterpret_cast<void**>(realAdapter.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
      return hr;

    auto* adapter = new (std::nothrow) DXGIAdapter(realAdapter, this);
    if (!adapter)
      return E_OUTOFMEMORY;

    hr = adapter->QueryInterface(riid, ppvAdapter);
    adapter->Release();
    return hr;
  }


  // ============================================================
  // DXGIAdapter out-of-class inline method definitions
  // ============================================================

  inline HRESULT STDMETHODCALLTYPE DXGIAdapter::EnumOutputs(UINT Output, IDXGIOutput** ppOutput) {
    if (!ppOutput) return E_POINTER;

    ComPtr<IDXGIOutput> realOutput;
    HRESULT hr = m_realAdapter->EnumOutputs(Output, realOutput.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      *ppOutput = nullptr;
      return hr;
    }

    *ppOutput = new DXGIOutput(realOutput, this);
    return S_OK;
  }

} // namespace dxup

#endif // _WIN32
