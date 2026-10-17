#ifdef _WIN32

#include "dxgi_interfaces.h"
#include "../util/util_types.h"
#include <windows.h>
#include <intrin.h>
#include <mutex>
#include <new>
#include <utility>

static HINSTANCE s_dxgiDllInstance = nullptr;

namespace {

  using PFN_CreateDXGIFactory = HRESULT (WINAPI*)(REFIID, void**);
  using PFN_CreateDXGIFactory1 = HRESULT (WINAPI*)(REFIID, void**);
  using PFN_CreateDXGIFactory2 = HRESULT (WINAPI*)(UINT, REFIID, void**);
  using PFN_DXGIDeclareAdapterRemovalSupport = HRESULT (WINAPI*)();

  enum class FactoryEntryPoint {
    Legacy,
    Version1,
    Version2,
  };

  HMODULE loadSystemDxgiModule() {
    static HMODULE s_module = []() -> HMODULE {
      char sysDir[MAX_PATH] = {};
      if (!GetSystemDirectoryA(sysDir, MAX_PATH))
        return nullptr;

      std::string dxgiPath = std::string(sysDir) + "\\dxgi.dll";
      return LoadLibraryA(dxgiPath.c_str());
    }();

    return s_module;
  }

  HRESULT createSystemFactory(
      FactoryEntryPoint entryPoint,
      UINT flags,
      REFIID riid,
      void** ppFactory) {
    if (!ppFactory)
      return E_INVALIDARG;

    *ppFactory = nullptr;

    HMODULE sysDxgi = loadSystemDxgiModule();
    if (!sysDxgi)
      return E_FAIL;

    switch (entryPoint) {
      case FactoryEntryPoint::Legacy: {
        auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory>(
          GetProcAddress(sysDxgi, "CreateDXGIFactory"));
        return createFactory
          ? createFactory(riid, ppFactory)
          : E_NOINTERFACE;
      }

      case FactoryEntryPoint::Version1: {
        auto createFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
          GetProcAddress(sysDxgi, "CreateDXGIFactory1"));
        return createFactory1
          ? createFactory1(riid, ppFactory)
          : E_NOINTERFACE;
      }

      case FactoryEntryPoint::Version2: {
        auto createFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
          GetProcAddress(sysDxgi, "CreateDXGIFactory2"));
        return createFactory2
          ? createFactory2(flags, riid, ppFactory)
          : E_NOINTERFACE;
      }
    }

    return E_FAIL;
  }

  // Windows' d3d10_1.dll loads d3d11.dll from the system folder unless a
  // d3d11.dll is loaded already (NVAPI creates a D3D10.1 device on an
  // adapter of this factory in Just Cause 2: the system's D3D11 then gets
  // DXUP's adapter, and the game's own D3D10 device would come from there
  // too). DXUP's d3d11.dll next to this one is loaded first.
  void loadSiblingD3D11() {
    static std::once_flag once;
    std::call_once(once, [] {
      if (GetModuleHandleW(L"d3d11.dll") || !s_dxgiDllInstance)
        return;
      wchar_t path[MAX_PATH] = {};
      const DWORD length = GetModuleFileNameW(s_dxgiDllInstance, path, MAX_PATH);
      if (!length || length >= MAX_PATH)
        return;
      wchar_t* name = wcsrchr(path, L'\\');
      if (!name || size_t(name - path) + 11 >= MAX_PATH)
        return;
      wcscpy_s(name + 1, MAX_PATH - size_t(name + 1 - path), L"d3d11.dll");
      if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && LoadLibraryW(path))
        logMessage(dxup::LogLevel::Info, "DXGI: loaded d3d11.dll next to dxgi.dll");
    });
  }

  HRESULT createWrappedFactory(
      FactoryEntryPoint entryPoint,
      UINT flags,
      REFIID requestedIid,
      void** ppFactory) {
    if (!ppFactory)
      return E_INVALIDARG;
    loadSiblingD3D11();

    *ppFactory = nullptr;

    // A DXGI 1.0 factory (CreateDXGIFactory) cannot be queried for the later
    // interfaces the wrapper forwards to; the wrapper itself answers the
    // interface the application asked for.
    if (entryPoint == FactoryEntryPoint::Legacy)
      entryPoint = FactoryEntryPoint::Version1;

    dxup::ComPtr<IDXGIFactory4> realFactory;
    HRESULT hr = createSystemFactory(
      entryPoint,
      flags,
      __uuidof(IDXGIFactory4),
      reinterpret_cast<void**>(realFactory.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !realFactory) {
      logMessage(
        dxup::LogLevel::Error,
        "DXGI: system factory creation failed entry=%u flags=0x%x hr=0x%08x factory=%p",
        static_cast<unsigned>(entryPoint),
        flags,
        static_cast<unsigned>(hr),
        realFactory.Get());
      return FAILED(hr) ? hr : E_FAIL;
    }

    auto* factory = new (std::nothrow) dxup::DXGIFactory(std::move(realFactory));
    if (!factory)
      return E_OUTOFMEMORY;

    hr = factory->QueryInterface(requestedIid, ppFactory);
    factory->Release();
    return hr;
  }

}

using namespace dxup;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)lpvReserved;

  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      s_dxgiDllInstance = hinstDLL;
      DisableThreadLibraryCalls(hinstDLL);
      logMessage(LogLevel::Info, "DXUP dxgi.dll loaded (DLL_PROCESS_ATTACH)");
      break;
    case DLL_PROCESS_DETACH:
      logMessage(LogLevel::Info, "DXUP dxgi.dll unloading (DLL_PROCESS_DETACH)");
      break;
  }
  return TRUE;
}

extern "C" {

HRESULT __stdcall CreateDXGIFactory(REFIID riid, void** ppFactory) {
  if (!ppFactory)
    return E_INVALIDARG;

  logMessage(LogLevel::Debug, "CreateDXGIFactory(riid=%s)", formatGuid(riid).c_str());

  return createWrappedFactory(FactoryEntryPoint::Legacy, 0, riid, ppFactory);
}

HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  if (!ppFactory)
    return E_INVALIDARG;

  return createWrappedFactory(FactoryEntryPoint::Version1, 0, riid, ppFactory);
}

HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
  if (!ppFactory)
    return E_INVALIDARG;

  logMessage(LogLevel::Debug, "CreateDXGIFactory2(flags=0x%x, riid=%s)", Flags, formatGuid(riid).c_str());

  return createWrappedFactory(FactoryEntryPoint::Version2, Flags, riid, ppFactory);
}

HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
  logMessage(LogLevel::Debug, "DXGIDeclareAdapterRemovalSupport()");

  HMODULE sysDxgi = loadSystemDxgiModule();
  if (!sysDxgi)
    return S_OK;

  auto declareAdapterRemovalSupport = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(
    GetProcAddress(sysDxgi, "DXGIDeclareAdapterRemovalSupport"));
  if (!declareAdapterRemovalSupport)
    return S_OK;

  return declareAdapterRemovalSupport();
}

} // extern "C"

#endif // _WIN32
