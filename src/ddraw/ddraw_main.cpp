// ddraw.dll: DirectDraw 7 and Direct3D 7 on DXUP (see ddraw_objects.h).

#include "ddraw_objects.h"

#include "../util/util_types.h"

using namespace dxup;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    logMessage(LogLevel::Info, "DXUP ddraw.dll loaded (DLL_PROCESS_ATTACH)");
  }
  return TRUE;
}

namespace {
  // Windows' ddraw.dll, for what this layer leaves to it.
  FARPROC systemProc(const char* name) {
    static HMODULE module = [] {
      wchar_t path[MAX_PATH] = {};
      const UINT length = GetSystemDirectoryW(path, MAX_PATH);
      if (!length || length + 11 >= MAX_PATH)
        return HMODULE(nullptr);
      wcscat_s(path, L"\\ddraw.dll");
      return LoadLibraryW(path);
    }();
    return module ? GetProcAddress(module, name) : nullptr;
  }
}

extern "C" {

HRESULT WINAPI DirectDrawCreateEx(GUID* guid, LPVOID* dd, REFIID riid, IUnknown* outer) {
  (void)guid;
  if (!dd) return DDERR_INVALIDPARAMS;
  *dd = nullptr;
  if (outer) return CLASS_E_NOAGGREGATION;
  auto* object = new ddraw::DirectDraw();
  const HRESULT hr = object->QueryInterface(riid, dd);
  object->Release();
  logMessage(LogLevel::Info, "DirectDrawCreateEx -> 0x%08lx", hr);
  return hr == E_NOINTERFACE ? DDERR_INVALIDPARAMS : hr;
}

// DirectDraw 1 to 6 (and Direct3D up to 6): Windows' ddraw.dll draws them.
HRESULT WINAPI DirectDrawCreate(GUID* guid, LPDIRECTDRAW* dd, IUnknown* outer) {
  using Create = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);
  if (auto create = reinterpret_cast<Create>(systemProc("DirectDrawCreate"))) {
    logMessage(LogLevel::Warn, "ddraw: DirectDrawCreate (DirectDraw 1-6) left to Windows' ddraw.dll");
    return create(guid, dd, outer);
  }
  if (dd) *dd = nullptr;
  ddraw::logUnsupported("DirectDrawCreate (DirectDraw 1-6 interfaces)");
  return DDERR_UNSUPPORTED;
}

void WINAPI AcquireDDThreadLock() {
  if (auto acquire = reinterpret_cast<void(WINAPI*)()>(systemProc("AcquireDDThreadLock")))
    acquire();
}

void WINAPI ReleaseDDThreadLock() {
  if (auto release = reinterpret_cast<void(WINAPI*)()>(systemProc("ReleaseDDThreadLock")))
    release();
}

// Internal exports of Windows' ddraw.dll. Windows' d3dim700.dll imports
// some of them from the ddraw.dll already loaded - this one - when an
// application loads it (Gothic II's SystemPack patches it): Windows'
// ddraw.dll does the work. The parameter counts are Windows'.
#define DXUP_DDRAW_FORWARD(name, params, args)                            ULONG_PTR WINAPI name params {                                            using Function = ULONG_PTR(WINAPI*) params;                             if (auto function = reinterpret_cast<Function>(systemProc(#name)))        return function args;                                                 return ULONG_PTR(DDERR_GENERIC);                                      }

DXUP_DDRAW_FORWARD(CompleteCreateSysmemSurface, (void* a), (a))
DXUP_DDRAW_FORWARD(D3DParseUnknownCommand, (void* a, void* b), (a, b))
DXUP_DDRAW_FORWARD(DDGetAttachedSurfaceLcl, (void* a, void* b, void* c), (a, b, c))
DXUP_DDRAW_FORWARD(DDInternalLock, (void* a, void* b), (a, b))
DXUP_DDRAW_FORWARD(DDInternalUnlock, (void* a), (a))
DXUP_DDRAW_FORWARD(DSoundHelp, (void* a, void* b, void* c), (a, b, c))
DXUP_DDRAW_FORWARD(GetDDSurfaceLocal, (void* a, void* b, void* c), (a, b, c))
DXUP_DDRAW_FORWARD(GetOLEThunkData, (void* a), (a))
DXUP_DDRAW_FORWARD(GetSurfaceFromDC, (void* a, void* b, void* c), (a, b, c))
DXUP_DDRAW_FORWARD(RegisterSpecialCase, (void* a, void* b, void* c, void* d), (a, b, c, d))

#undef DXUP_DDRAW_FORWARD

HRESULT WINAPI SetAppCompatData(DWORD type, DWORD value) {
  if (auto set = reinterpret_cast<HRESULT(WINAPI*)(DWORD, DWORD)>(systemProc("SetAppCompatData")))
    return set(type, value);
  return DDERR_GENERIC;
}

HRESULT WINAPI DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* clipper, IUnknown*) {
  if (!clipper) return DDERR_INVALIDPARAMS;
  *clipper = new ddraw::Clipper();
  return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA callback, LPVOID context, DWORD) {
  if (!callback) return DDERR_INVALIDPARAMS;
  char description[] = "Primary Display Driver";
  char name[] = "display";
  callback(nullptr, description, name, context, nullptr);
  return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW callback, LPVOID context, DWORD) {
  if (!callback) return DDERR_INVALIDPARAMS;
  wchar_t description[] = L"Primary Display Driver";
  wchar_t name[] = L"display";
  callback(nullptr, description, name, context, nullptr);
  return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, LPVOID context) {
  if (!callback) return DDERR_INVALIDPARAMS;
  char description[] = "Primary Display Driver";
  char name[] = "display";
  callback(nullptr, description, name, context);
  return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW callback, LPVOID context) {
  if (!callback) return DDERR_INVALIDPARAMS;
  wchar_t description[] = L"Primary Display Driver";
  wchar_t name[] = L"display";
  callback(nullptr, description, name, context);
  return DD_OK;
}

HRESULT WINAPI DllCanUnloadNow() {
  return S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* object) {
  if (object) *object = nullptr;
  return CLASS_E_CLASSNOTAVAILABLE;
}

}
