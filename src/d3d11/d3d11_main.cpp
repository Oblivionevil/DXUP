#ifdef _WIN32

#include "d3d11_device.h"
#include "d3d11_context.h"
#include "dxgi_interfaces.h"
#include "../d3d12/d3d12_backend_device.h"
#include "../util/com_ptr.h"
#include "../util/util_types.h"
#include "../util/util_paths.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <string>
#include <exception>
#include <intrin.h>
#include <cstring>

using namespace dxup;

// Our DLL's HINSTANCE, set in DllMain
static HINSTANCE s_ourDllInstance = nullptr;

namespace {

  // The upscalers' runtime DLLs: the dxup_upscalers folder next to this
  // DLL, the DXUP installation's bin\upscalers, or this DLL's own folder.
  std::wstring upscalerRuntimeDir() {
    auto isFolder = [](const std::wstring& path) {
      const DWORD attributes = GetFileAttributesW(path.c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    };
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(s_ourDllInstance, path, MAX_PATH);
    std::wstring dir(path, length);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
    const std::wstring local = dir + L"\\dxup_upscalers";
    if (isFolder(local))
      return local;
    const std::wstring install = dxupInstallFolder();
    if (!install.empty() && isFolder(install + L"\\bin\\upscalers"))
      return install + L"\\bin\\upscalers";
    return dir;
  }

  std::unique_ptr<upscale::Upscaler> createUpscaler(const upscale::CreateInfo& info, std::string& error) {
    upscale::CreateInfo withRuntime = info;
    withRuntime.runtimeDir = upscalerRuntimeDir();
    return upscale::create(withRuntime, error);
  }

  // Load the REAL system d3d11.dll from System32
  HMODULE loadSystemD3D11Module() {
    static HMODULE s_module = []() -> HMODULE {
      char sysDir[MAX_PATH] = {};
      if (!GetSystemDirectoryA(sysDir, MAX_PATH))
        return nullptr;
      std::string path = std::string(sysDir) + "\\d3d11.dll";
      return LoadLibraryA(path.c_str());
    }();
    return s_module;
  }


  DXGIAdapter* createDxgiAdapterWrapper(D3D11Device* device) {
    if (!device)
      return nullptr;

    auto* backend = device->GetBackend();
    if (!backend || backend->getBackendType() != BackendType::D3D12)
      return nullptr;

    auto* d3d12Backend = static_cast<d3d12::D3D12BackendDevice*>(backend);
    if (!d3d12Backend || !d3d12Backend->getAdapter())
      return nullptr;

    ComPtr<IDXGIFactory4> realFactory = d3d12Backend->getDxgiFactory();
    ComPtr<IDXGIAdapter1> realAdapter = d3d12Backend->getAdapter();

    // Keep device-parent factories in the same DLL as exported factories.
    // Overlays hook one factory vtable; a second copy in d3d11.dll would
    // bypass their CreateSwapChain hook when the game uses GetAdapter().
    wchar_t dxgiPath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(s_ourDllInstance, dxgiPath, MAX_PATH);
    wchar_t* name = length && length < MAX_PATH ? wcsrchr(dxgiPath, L'\\') : nullptr;
    if (name) {
      wcscpy_s(name + 1, MAX_PATH - size_t(name + 1 - dxgiPath), L"dxgi.dll");
      HMODULE dxgiModule = GetModuleHandleW(dxgiPath);
      if (!dxgiModule)
        dxgiModule = LoadLibraryW(dxgiPath);
      using CreateAdapter = HRESULT (WINAPI*)(IDXGIAdapter1*, IDXGIFactory4*, IDXGIAdapter1**);
      auto createAdapter = dxgiModule ? reinterpret_cast<CreateAdapter>(
        GetProcAddress(dxgiModule, "DXUPCreateDXGIAdapter")) : nullptr;
      ComPtr<IDXGIAdapter1> sharedAdapter;
      if (createAdapter && SUCCEEDED(createAdapter(realAdapter.Get(), realFactory.Get(),
          sharedAdapter.GetAddressOf())) && sharedAdapter)
        return static_cast<DXGIAdapter*>(sharedAdapter.Detach());
    }

    // Also support an older dxgi.dll, or a standalone d3d11.dll.
    DXGIFactory* factoryWrapper = realFactory ? new DXGIFactory(realFactory) : nullptr;
    DXGIAdapter* adapterWrapper = new DXGIAdapter(realAdapter, factoryWrapper);

    if (factoryWrapper)
      factoryWrapper->Release();

    return adapterWrapper;
  }

  // 12_0 and 12_1 only when the D3D12 device has them (maxLevel).
  HRESULT chooseFeatureLevel(const D3D_FEATURE_LEVEL* requestedLevels,
                             UINT requestedLevelCount,
                             D3D_FEATURE_LEVEL maxLevel,
                             D3D_FEATURE_LEVEL* chosenLevel) {
    if (!chosenLevel)
      return E_POINTER;

    // Match native D3D11's conservative default: 11_1 is only considered when
    // the app explicitly includes it in the requested feature-level array.
    if (!requestedLevels || requestedLevelCount == 0) {
      *chosenLevel = D3D_FEATURE_LEVEL_11_0;
      return S_OK;
    }

    for (UINT i = 0; i < requestedLevelCount; i++) {
      switch (requestedLevels[i]) {
        case D3D_FEATURE_LEVEL_12_1:
        case D3D_FEATURE_LEVEL_12_0:
          if (requestedLevels[i] <= maxLevel) {
            *chosenLevel = requestedLevels[i];
            return S_OK;
          }
          break;
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
        // As on every D3D11 GPU (God of War asks for each level in turn).
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
          *chosenLevel = requestedLevels[i];
          return S_OK;
        default:
          break;
      }
    }

    return DXGI_ERROR_UNSUPPORTED;
  }

}

// D3D11Device methods defined here after D3D11ImmediateContext is complete
HRESULT STDMETHODCALLTYPE D3D11Device::QueryInterface(REFIID riid, void** ppvObject) {
  if (!ppvObject)
    return E_POINTER;

  *ppvObject = nullptr;

  if (riid == kDxupD3D11DeviceIid) {
    *ppvObject = static_cast<ID3D11Device5*>(this);
    AddRef();
    return S_OK;
  }

  if (riid == __uuidof(IUnknown)
   || riid == __uuidof(ID3D11Device)
   || riid == __uuidof(ID3D11Device1)
   || riid == __uuidof(ID3D11Device2)
   || riid == __uuidof(ID3D11Device3)
   || riid == __uuidof(ID3D11Device4)
   || riid == __uuidof(ID3D11Device5)) {
    // One inheritance chain: every version is the same pointer.
    *ppvObject = static_cast<ID3D11Device5*>(this);
    AddRef();
    return S_OK;
  }

  if (riid == __uuidof(ID3D11Multithread) || riid == __uuidof(ID3D10Multithread)) {
    // Both interfaces declare the same methods in the same order.
    *ppvObject = static_cast<ID3D11Multithread*>(&m_multithread);
    AddRef();
    return S_OK;
  }

  if ((riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1)) && m_d3d10) {
    *ppvObject = static_cast<ID3D10Device1*>(&m_d3d10Device);
    AddRef();
    return S_OK;
  }

  // ID3D11On12Device: not on devices of D3D11CreateDevice (Windows' only
  // answers it on devices of D3D11On12CreateDevice, which go to Windows).

  if (riid == __uuidof(IDXGIObject)
   || riid == __uuidof(IDXGIDevice)
    || riid == __uuidof(IDXGIDevice1)
    || riid == __uuidof(IDXGIDevice2)
    || riid == __uuidof(IDXGIDevice3)
    || riid == __uuidof(IDXGIDevice4)) {
    DXGIDevice* dxgiDevice = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_dxgiDeviceMutex);
      if (!m_dxgiDevice) {
        DXGIAdapter* adapterWrapper = createDxgiAdapterWrapper(this);
        if (!adapterWrapper) {
          logMessage(LogLevel::Error,
            "D3D11Device::QueryInterface failed to create DXGI adapter wrapper for riid=%s",
            formatGuid(riid).c_str());
          return E_NOINTERFACE;
        }

        m_dxgiDevice = new (std::nothrow) DXGIDevice(this, adapterWrapper);
        adapterWrapper->Release();
        if (!m_dxgiDevice)
          return E_OUTOFMEMORY;
      }
      dxgiDevice = m_dxgiDevice;
    }

    return dxgiDevice->QueryInterface(riid, ppvObject);
  }

  // Unknown interfaces must not be forwarded to the D3D12 device: that
  // breaks COM identity, and vendor libraries probing for private driver
  // interfaces (NvAPI via NGX/DLSS) would get D3D12 driver objects and treat
  // them as D3D11 ones.
  logMessage(LogLevel::Debug, "D3D11Device::QueryInterface unsupported riid=%s",
    formatGuid(riid).c_str());
  return E_NOINTERFACE;
}

D3D11Device::~D3D11Device() {
  logMessage(LogLevel::Debug, "D3D11Device::~D3D11Device()");
  delete m_dxgiDevice;
  m_dxgiDevice = nullptr;

  // The immediate context shares this device's reference count, so it is
  // destroyed directly. Its destructor drops the bindings, which may be the
  // last references to some objects; the backend is still alive for them.
  delete m_immediateContext;
  m_immediateContext = nullptr;
}

void D3D11Device::GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) {
  if (!ppImmediateContext)
    return;
  // Threads asking at once get the same context.
  std::lock_guard<std::mutex> lock(m_immediateContextMutex);
  if (!m_immediateContext) {
    logMessage(LogLevel::Info, "Creating immediate context...");
    try {
      auto backendCtx = m_backendDevice.createThreadedImmediateContext();
      if (!backendCtx) {
        logMessage(LogLevel::Error, "Backend returned null immediate context");
        *ppImmediateContext = nullptr;
        return;
      }
      m_immediateContext = new D3D11ImmediateContext(this, std::move(backendCtx));
      logMessage(LogLevel::Info, "Immediate context created successfully");
    } catch (const std::exception& e) {
      logMessage(LogLevel::Error, "Failed to create immediate context: %s", e.what());
      *ppImmediateContext = nullptr;
      return;
    } catch (...) {
      logMessage(LogLevel::Error, "Failed to create immediate context: unknown exception");
      *ppImmediateContext = nullptr;
      return;
    }
  }
  m_immediateContext->AddRef();
  *ppImmediateContext = m_immediateContext;
}

HRESULT D3D11Device::CreateDeferredContext(
    UINT                    ContextFlags,
    ID3D11DeviceContext**   ppDeferredContext) {
  if (!ppDeferredContext) return E_INVALIDARG;

  try {
    auto backendCtx = m_backendDevice.createDeferredContext();
    if (!backendCtx) {
      logMessage(LogLevel::Error, "Failed to create backend context for deferred context");
      *ppDeferredContext = nullptr;
      return E_OUTOFMEMORY;
    }
    *ppDeferredContext = new D3D11ImmediateContext(this, std::move(backendCtx), true);
    logMessage(LogLevel::Info, "Created deferred context %p", *ppDeferredContext);
    return S_OK;
  } catch (...) {
    logMessage(LogLevel::Error, "Failed to create deferred context");
    *ppDeferredContext = nullptr;
    return E_OUTOFMEMORY;
  }
}

static bool dxupExceptionTraceEnabled() {
  static const bool enabled = dxupSiblingExists("dxup_exception_trace.txt");
  return enabled;
}

static void formatModuleAddress(const void* address, char* out, size_t outSize) {
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module)) {
    GetModuleFileNameA(module, path, MAX_PATH);
  }
  const char* name = strrchr(path, '\\');
  name = name ? name + 1 : path;
  sprintf_s(out, outSize, "%s+0x%llx", name,
    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
}

// Only observes: faults in DXUP itself are logged with their location, and
// with dxup_exception_trace.txt next to the DLL every first-chance exception
// of the process is logged with a short call stack (diagnostics for crashes
// that happen in the application after a DXUP call went wrong). Handling is
// always left to the application.
static bool s_exceptionTrace = false;   // dxup_exception_trace.txt, read at load

static __declspec(noinline) void traceException(PEXCEPTION_POINTERS pExInfo) {
  const DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
  void* addr = pExInfo->ExceptionRecord->ExceptionAddress;
  // Thread naming and debug output are exceptions by design.
  if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A)
    return;
  static volatile LONG s_traced = 0;
  if (InterlockedIncrement(&s_traced) > 500)
    return;
  char where[MAX_PATH + 32];
  formatModuleAddress(addr, where, sizeof(where));
  std::string stack;
  void* frames[24] = {};
  const USHORT frameCount = CaptureStackBackTrace(0, 24, frames, nullptr);
  for (USHORT i = 0; i < frameCount; i++) {
    char frame[MAX_PATH + 32];
    formatModuleAddress(frames[i], frame, sizeof(frame));
    stack += "\n    ";
    stack += frame;
  }
  ULONG_PTR info0 = pExInfo->ExceptionRecord->NumberParameters > 0 ? pExInfo->ExceptionRecord->ExceptionInformation[0] : 0;
  ULONG_PTR info1 = pExInfo->ExceptionRecord->NumberParameters > 1 ? pExInfo->ExceptionRecord->ExceptionInformation[1] : 0;
  // The top of the stack: a call through a bad pointer leaves the caller's
  // arguments (the object of a COM call among them) there.
  char top[256] = {};
  if (pExInfo->ContextRecord) {
#if defined(_M_IX86)
    const auto* sp = reinterpret_cast<const ULONG_PTR*>(pExInfo->ContextRecord->Esp);
#else
    const auto* sp = reinterpret_cast<const ULONG_PTR*>(pExInfo->ContextRecord->Rsp);
#endif
    ULONG_PTR words[4] = {};
    if (!IsBadReadPtr(sp, sizeof(words)))
      std::memcpy(words, sp, sizeof(words));
    ULONG_PTR object[4] = {};
    const auto* objectPtr = reinterpret_cast<const ULONG_PTR*>(words[1]);
    if (objectPtr && !IsBadReadPtr(objectPtr, sizeof(object)))
      std::memcpy(object, objectPtr, sizeof(object));
    snprintf(top, sizeof(top), "\n    stack %llx %llx %llx %llx, [sp+1] -> %llx %llx %llx %llx",
      static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]),
      static_cast<unsigned long long>(words[2]), static_cast<unsigned long long>(words[3]),
      static_cast<unsigned long long>(object[0]), static_cast<unsigned long long>(object[1]),
      static_cast<unsigned long long>(object[2]), static_cast<unsigned long long>(object[3]));
  }
  logMessage(LogLevel::Error, "exception trace: 0x%08X at %s tid=%lu info=%llx,%llx%s%s",
    code, where, static_cast<unsigned long>(GetCurrentThreadId()),
    static_cast<unsigned long long>(info0), static_cast<unsigned long long>(info1), top, stack.c_str());
}

static __declspec(noinline) void logOwnFault(PEXCEPTION_POINTERS pExInfo) {
  const DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
  void* addr = pExInfo->ExceptionRecord->ExceptionAddress;
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(addr), &module)
   || module != reinterpret_cast<HMODULE>(s_ourDllInstance))
    return;
  char where[MAX_PATH + 32];
  formatModuleAddress(addr, where, sizeof(where));
  logMessage(LogLevel::Error, "DXUP: exception 0x%08X at %s tid=%lu", code, where,
    static_cast<unsigned long>(GetCurrentThreadId()));
}

// The handler itself keeps a small frame, and the logging (a few KB of
// stack) runs only where the stack has room: games raise exceptions on
// threads that are nearly out of it (Just Cause 2 at start), and a fault in
// a vectored handler ends the process.
static LONG WINAPI dxupVectoredExceptionHandler(PEXCEPTION_POINTERS pExInfo) {
  if (!pExInfo || !pExInfo->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_STACK_OVERFLOW || code == STATUS_GUARD_PAGE_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  const ULONG_PTR here = reinterpret_cast<ULONG_PTR>(&low);
  if (here <= low || here - low < 64 * 1024)
    return EXCEPTION_CONTINUE_SEARCH;
  if (s_exceptionTrace)
    traceException(pExInfo);
  else if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
        || code == EXCEPTION_ILLEGAL_INSTRUCTION)
    logOwnFault(pExInfo);
  return EXCEPTION_CONTINUE_SEARCH;
}

static PVOID s_exceptionHandler = nullptr;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)lpvReserved;

  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      s_ourDllInstance = hinstDLL;
      logMessage(LogLevel::Info, "DXUP d3d11.dll loaded (DLL_PROCESS_ATTACH)");
      d3d12::setUpscalerFactory(&createUpscaler);
      s_exceptionTrace = dxupExceptionTraceEnabled();
      s_exceptionHandler = AddVectoredExceptionHandler(1, dxupVectoredExceptionHandler);
      break;
    case DLL_PROCESS_DETACH:
      logMessage(LogLevel::Info, "DXUP d3d11.dll unloading (DLL_PROCESS_DETACH)");
      // Unloaded with FreeLibrary: the handler's code goes with the DLL.
      if (s_exceptionHandler) {
        RemoveVectoredExceptionHandler(s_exceptionHandler);
        s_exceptionHandler = nullptr;
      }
      logShutdown();
      break;
  }
  return TRUE;
}

extern "C" {

HRESULT WINAPI D3D11On12CreateDevice(
    IUnknown*                   pDevice,
    UINT                        Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
    UINT                        FeatureLevels,
    IUnknown* const*            ppCommandQueues,
    UINT                        NumQueues,
    UINT                        NodeMask,
    ID3D11Device**              ppDevice,
    ID3D11DeviceContext**       ppImmediateContext,
    D3D_FEATURE_LEVEL*          pChosenFeatureLevel) {
  HMODULE sysD3D11 = loadSystemD3D11Module();
  if (!sysD3D11)
    return E_FAIL;

  using PFN_D3D11On12CreateDevice = HRESULT (WINAPI*)(
    IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT,
    UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
  auto systemCreate = reinterpret_cast<PFN_D3D11On12CreateDevice>(
    GetProcAddress(sysD3D11, "D3D11On12CreateDevice"));
  if (!systemCreate)
    return E_NOINTERFACE;

  return systemCreate(
    pDevice, Flags, pFeatureLevels, FeatureLevels, ppCommandQueues, NumQueues,
    NodeMask, ppDevice, ppImmediateContext, pChosenFeatureLevel);
}

// Without a Direct3D 12 device (a GPU without a Direct3D 12 driver, a
// virtual machine) the game gets Windows' own Direct3D 11 device: it runs
// as it would without DXUP. An adapter from DXUP's DXGI is handed over as
// the real one.
static HRESULT createNativeDevice(
    IDXGIAdapter*            pAdapter,
    D3D_DRIVER_TYPE          DriverType,
    HMODULE                  Software,
    UINT                     Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT                     FeatureLevels,
    UINT                     SDKVersion,
    ID3D11Device**           ppDevice,
    D3D_FEATURE_LEVEL*       pFeatureLevel,
    ID3D11DeviceContext**    ppImmediateContext) {
  HMODULE sysD3D11 = loadSystemD3D11Module();
  auto systemCreate = sysD3D11
    ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(sysD3D11, "D3D11CreateDevice")) : nullptr;
  if (!systemCreate)
    return E_FAIL;
  IDXGIAdapter* adapter = pAdapter;
  ComPtr<IDXGIAdapter2> real;
  if (pAdapter && SUCCEEDED(pAdapter->QueryInterface(IID_PPV_ARGS(&real))))
    adapter = real.Get();
  const HRESULT hr = systemCreate(adapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
    SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
  logMessage(LogLevel::Warn, "D3D11CreateDevice: no Direct3D 12 device, Windows' Direct3D 11 instead (hr=0x%08x)",
    static_cast<unsigned>(hr));
  return hr;
}

// The LUID of WARP, from the system's DXGI (not DXUP's dxgi.dll).
static bool warpAdapterLuid(LUID& luid) {
  char path[MAX_PATH];
  GetSystemDirectoryA(path, MAX_PATH);
  strncat_s(path, "\\dxgi.dll", _TRUNCATE);
  HMODULE dxgi = LoadLibraryA(path);
  using CreateFactory = HRESULT (WINAPI*)(REFIID, void**);
  auto create = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc = {};
  const bool found = create && SUCCEEDED(create(IID_PPV_ARGS(&factory)))
    && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->GetDesc(&desc));
  if (found)
    luid = desc.AdapterLuid;
  return found;
}

HRESULT __stdcall D3D11CreateDevice(
    IDXGIAdapter*            pAdapter,
    D3D_DRIVER_TYPE          DriverType,
    HMODULE                  Software,
    UINT                     Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT                     FeatureLevels,
    UINT                     SDKVersion,
    ID3D11Device**           ppDevice,
    D3D_FEATURE_LEVEL*       pFeatureLevel,
    ID3D11DeviceContext**    ppImmediateContext) {

  logMessage(LogLevel::Info, "D3D11CreateDevice called (DriverType=%d, Flags=0x%x, SDKVersion=%u)",
    static_cast<int>(DriverType), Flags, SDKVersion);

  if (pFeatureLevels && FeatureLevels > 0) {
    for (UINT i = 0; i < FeatureLevels; i++) {
      logMessage(LogLevel::Debug, "  Requested feature level[%u] = 0x%x", i, pFeatureLevels[i]);
    }
  }

  (void)Software;
  (void)SDKVersion;

  // As in D3D11: an explicit adapter goes with D3D_DRIVER_TYPE_UNKNOWN only.
  if ((pAdapter != nullptr) != (DriverType == D3D_DRIVER_TYPE_UNKNOWN))
    return E_INVALIDARG;

  LUID adapterLuid = {};
  const LUID* requestedAdapter = nullptr;
  if (pAdapter) {
    DXGI_ADAPTER_DESC adapterDesc = {};
    if (FAILED(pAdapter->GetDesc(&adapterDesc)))
      return E_INVALIDARG;
    adapterLuid = adapterDesc.AdapterLuid;
    requestedAdapter = &adapterLuid;
  } else if (DriverType == D3D_DRIVER_TYPE_WARP) {
    // Microsoft's software renderer, as D3D11 gives it.
    if (!warpAdapterLuid(adapterLuid))
      return DXGI_ERROR_UNSUPPORTED;
    requestedAdapter = &adapterLuid;
  }

  auto createDevice = [&](D3D_FEATURE_LEVEL level) -> D3D11Device* {
    logMessage(LogLevel::Info, "Creating D3D11Device with D3D12 backend (FeatureLevel=0x%x)...", level);
    try {
      return new D3D11Device(BackendType::D3D12, level, requestedAdapter);
    } catch (const std::exception& e) {
      logMessage(LogLevel::Error, "D3D11CreateDevice failed: %s", e.what());
    } catch (...) {
      logMessage(LogLevel::Error, "D3D11CreateDevice failed: unknown exception");
    }
    return nullptr;
  };

  // Levels 12_0 and 12_1 depend on the GPU: the device first, then the
  // level (some games ask for 12_x only).
  D3D11Device* device = nullptr;
  D3D_FEATURE_LEVEL maxLevel = D3D_FEATURE_LEVEL_11_1;
  for (UINT i = 0; pFeatureLevels && i < FeatureLevels; i++) {
    if (pFeatureLevels[i] >= D3D_FEATURE_LEVEL_12_0 && !device) {
      device = createDevice(D3D_FEATURE_LEVEL_11_0);
      if (!device)
        return createNativeDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
          SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
      maxLevel = device->maxFeatureLevel();
    }
  }

  D3D_FEATURE_LEVEL chosenFeatureLevel = D3D_FEATURE_LEVEL_11_0;
  HRESULT featureHr = chooseFeatureLevel(pFeatureLevels, FeatureLevels, maxLevel, &chosenFeatureLevel);
  if (FAILED(featureHr)) {
    char levels[128] = {};
    for (UINT i = 0; i < FeatureLevels && i < 8; i++) {
      char level[16];
      sprintf_s(level, " 0x%x", pFeatureLevels[i]);
      strcat_s(levels, level);
    }
    logMessage(LogLevel::Error, "D3D11CreateDevice: no supported requested feature level (hr=0x%08x, requested%s, max 0x%x)",
      featureHr, levels, maxLevel);
    if (device)
      device->Release();
    return featureHr;
  }

  // Without output pointers the call only asks for the feature level.
  if (!ppDevice && !ppImmediateContext) {
    if (device)
      device->Release();
    if (pFeatureLevel)
      *pFeatureLevel = chosenFeatureLevel;
    return S_FALSE;
  }

  if (device)
    device->setFeatureLevel(chosenFeatureLevel);
  else
    device = createDevice(chosenFeatureLevel);
  if (!device)
    return createNativeDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);

  logMessage(LogLevel::Info, "D3D11Device created successfully");
  // Windows' d3d10.dll (0x40000000) and d3d10_1.dll (0x80000000) create
  // their devices with these flags and then ask for ID3D10Device(1).
  constexpr UINT kCreateForD3D10 = 0xC0000000u;
  device->setCreationFlags(Flags & ~kCreateForD3D10);
  if (Flags & kCreateForD3D10)
    device->enableD3D10(Flags & ~kCreateForD3D10);

  if (pFeatureLevel)
    *pFeatureLevel = chosenFeatureLevel;

  // The context reference returned by GetImmediateContext is a device
  // reference. The creation reference goes to the caller, or is dropped.
  if (ppImmediateContext) {
    device->GetImmediateContext(ppImmediateContext);
    // Out of memory for its command list or descriptor heaps.
    if (!*ppImmediateContext) {
      device->Release();
      if (ppDevice) *ppDevice = nullptr;
      return E_OUTOFMEMORY;
    }
  }

  if (ppDevice)
    *ppDevice = device;
  else
    device->Release();

  logMessage(LogLevel::Info, "D3D11CreateDevice completed successfully device=%p", static_cast<void*>(device));
  return S_OK;
}

HRESULT __stdcall D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter*               pAdapter,
    D3D_DRIVER_TYPE             DriverType,
    HMODULE                     Software,
    UINT                        Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
    UINT                        FeatureLevels,
    UINT                        SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain**            ppSwapChain,
    ID3D11Device**              ppDevice,
    D3D_FEATURE_LEVEL*          pFeatureLevel,
    ID3D11DeviceContext**       ppImmediateContext) {

  logMessage(LogLevel::Info, "D3D11CreateDeviceAndSwapChain called");

  // First create the device
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  HRESULT hr = D3D11CreateDevice(
    pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion,
    &device, pFeatureLevel, &context);

  if (FAILED(hr)) {
    logMessage(LogLevel::Error, "D3D11CreateDeviceAndSwapChain: device creation failed (hr=0x%08x)", hr);
    return hr;
  }

  // The swap chain comes from the factory of the device's adapter, as in
  // D3D11 (Windows' d3d10.dll creates its swap chains here too).
  if (ppSwapChain && pSwapChainDesc) {
    logMessage(LogLevel::Info, "Creating swap chain: %ux%u, Format=%d, BufferCount=%u, HWND=0x%p",
      pSwapChainDesc->BufferDesc.Width, pSwapChainDesc->BufferDesc.Height,
      pSwapChainDesc->BufferDesc.Format, pSwapChainDesc->BufferCount,
      pSwapChainDesc->OutputWindow);
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    DXGI_SWAP_CHAIN_DESC desc = *pSwapChainDesc;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (SUCCEEDED(hr))
      hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (SUCCEEDED(hr))
      hr = adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (SUCCEEDED(hr))
      hr = factory->CreateSwapChain(device, &desc, ppSwapChain);
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "D3D11CreateDeviceAndSwapChain: swap chain creation failed (hr=0x%08x)", hr);
      *ppSwapChain = nullptr;
      device->Release();
      if (context) context->Release();
      return hr;
    }
  }

  if (ppDevice)
    *ppDevice = device;
  else
    device->Release();

  if (ppImmediateContext)
    *ppImmediateContext = context;
  else if (context)
    context->Release();

  logMessage(LogLevel::Info, "D3D11CreateDeviceAndSwapChain completed successfully");
  return S_OK;
}

// DXUP's own Direct3D 7-9 layers draw into back buffers of their own and
// present them through a swap chain made at the first Present. Forced
// multisampling covers targets of a swap chain's size, decided when they
// are made: the layers name the size of a back buffer before they make it.
void WINAPI DXUPNoteBackBufferSize(IUnknown* pDevice, UINT Width, UINT Height) {
  ComPtr<ID3D11Device5> dxupDevice;
  if (!pDevice || !Width || !Height
   || FAILED(pDevice->QueryInterface(kDxupD3D11DeviceIid, reinterpret_cast<void**>(dxupDevice.GetAddressOf()))))
    return;
  static_cast<D3D11Device*>(dxupDevice.Get())->NoteBackBufferSize(Width, Height);
}

// The PCI vendor of the GPU DXUP renders on, also when games are shown
// another (dxgi.spoofVendor): DXUP's Direct3D 7-9 layers do as that
// maker's driver where Direct3D 9 drivers differ. 0 for a device that is
// not DXUP's.
UINT WINAPI DXUPGetVendorId(IUnknown* pDevice) {
  ComPtr<ID3D11Device5> dxupDevice;
  if (!pDevice
   || FAILED(pDevice->QueryInterface(kDxupD3D11DeviceIid, reinterpret_cast<void**>(dxupDevice.GetAddressOf()))))
    return 0;
  auto* backend = static_cast<D3D11Device*>(dxupDevice.Get())->GetBackend();
  return backend ? static_cast<d3d12::D3D12BackendDevice*>(backend)->getVendorId() : 0;
}

// A texture view of DXUP's device whose components come as the D3D12
// Shader4ComponentMapping says, as Vulkan image views swizzle: DXUP's
// Direct3D 9 layer reads ATI2 (BC5) as green, red, 1, 1 with it.
HRESULT WINAPI DXUPCreateShaderResourceView(IUnknown* pDevice, ID3D11Resource* pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, UINT ComponentMapping, ID3D11ShaderResourceView** ppView) {
  ComPtr<ID3D11Device5> dxupDevice;
  if (!pDevice
   || FAILED(pDevice->QueryInterface(kDxupD3D11DeviceIid, reinterpret_cast<void**>(dxupDevice.GetAddressOf()))))
    return E_INVALIDARG;
  return static_cast<D3D11Device*>(dxupDevice.Get())->CreateMappedShaderResourceView(pResource, pDesc, ComponentMapping, ppView);
}

// Bytes into a dynamic buffer of DXUP's immediate context where writes after
// Map(WRITE_NO_OVERWRITE) go, written by its worker thread: DXUP's Direct3D 9
// layer uploads constants and vertices without writing write-combined memory
// on the game's thread. With no buffer: whether the context is DXUP's.
HRESULT WINAPI DXUPWriteBuffer(ID3D11DeviceContext* pContext, ID3D11Buffer* pBuffer, UINT Offset, const void* pData,
    UINT Size) {
  if (!pContext)
    return E_INVALIDARG;
  if (!pBuffer) {
    ComPtr<ID3D11Device> device;
    pContext->GetDevice(device.GetAddressOf());
    ComPtr<ID3D11Device5> dxupDevice;
    return device && pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
      && SUCCEEDED(device->QueryInterface(kDxupD3D11DeviceIid, reinterpret_cast<void**>(dxupDevice.GetAddressOf())))
      ? S_OK : E_NOINTERFACE;
  }
  return static_cast<D3D11ImmediateContext*>(pContext)->WriteDynamicBuffer(pBuffer, Offset, pData, Size);
}

// The same, the bytes written by fn(user, target, Size, data, DataSize) on the
// worker thread, which keeps fn's state there; pack(packUser, data) fills the
// DataSize bytes of data in the queue right away. DXUP's Direct3D 9 layer
// keeps its constants there and passes only those that changed. Without a
// buffer, fn(user, nullptr, 0, ...) runs after the commands queued before.
HRESULT WINAPI DXUPWriteBufferWith(ID3D11DeviceContext* pContext, ID3D11Buffer* pBuffer, UINT Offset, UINT Size,
    dxup::IBackendContext::WriteFn fn, void* user, dxup::IBackendContext::PackFn pack, void* packUser, UINT DataSize) {
  if (!pContext)
    return E_INVALIDARG;
  return static_cast<D3D11ImmediateContext*>(pContext)->WriteDynamicBufferWith(pBuffer, Offset, Size, fn, user, pack,
                                                                                packUser, DataSize);
}

// A sampler of DXUP's device that may also use D3D12's anisotropic filter
// with point mips (0x54), as Direct3D 9 filters: DXUP's Direct3D 9 layer.
HRESULT WINAPI DXUPCreateSamplerState(IUnknown* pDevice, const D3D11_SAMPLER_DESC* pDesc,
    ID3D11SamplerState** ppSampler) {
  ComPtr<ID3D11Device5> dxupDevice;
  if (!pDevice
   || FAILED(pDevice->QueryInterface(kDxupD3D11DeviceIid, reinterpret_cast<void**>(dxupDevice.GetAddressOf()))))
    return E_INVALIDARG;
  return static_cast<D3D11Device*>(dxupDevice.Get())->createSamplerState(pDesc, ppSampler, true);
}

} // extern "C"

#endif // _WIN32
