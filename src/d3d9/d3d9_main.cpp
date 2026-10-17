// d3d9.dll: Direct3D 9 and 9Ex on DXUP (see d3d9_objects.h).

#include "d3d9_objects.h"

#include "../util/util_types.h"

using namespace dxup;

namespace {
  // What d3dx9 asks of the runtime to validate shaders: everything passes.
  struct ShaderValidator : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Begin(void*, void*, DWORD) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Instruction(const char*, UINT, const DWORD*, DWORD) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE End() { return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
      if (!object) return E_POINTER;
      *object = nullptr;
      if (riid == __uuidof(IUnknown)) {
        AddRef();
        *object = this;
        return S_OK;
      }
      return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
      const ULONG count = --m_ref;
      if (!count) delete this;
      return count;
    }
    std::atomic<ULONG> m_ref{1};
  };
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    logMessage(LogLevel::Info, "DXUP d3d9.dll loaded (DLL_PROCESS_ATTACH)");
  }
  return TRUE;
}

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
  logMessage(LogLevel::Info, "Direct3DCreate9 (SDK %u)", sdkVersion);
  return new d3d9::Interface(false);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** d3d) {
  if (!d3d)
    return D3DERR_INVALIDCALL;
  logMessage(LogLevel::Info, "Direct3DCreate9Ex (SDK %u)", sdkVersion);
  *d3d = new d3d9::Interface(true);
  return D3D_OK;
}

void* WINAPI Direct3DShaderValidatorCreate9() {
  return static_cast<IUnknown*>(new ShaderValidator());
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
int WINAPI D3DPERF_EndEvent() { return 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
void WINAPI D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
BOOL WINAPI D3DPERF_QueryRepeatFrame() { return FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD) {}
DWORD WINAPI D3DPERF_GetStatus() { return 0; }

void WINAPI DebugSetMute() {}
int WINAPI DebugSetLevel() { return 0; }
HRESULT WINAPI PSGPError(void*, DWORD, DWORD) { return D3DERR_INVALIDCALL; }
HRESULT WINAPI PSGPSampleTexture(void*, DWORD, float (*)[4], DWORD, float (*)[4]) { return D3DERR_INVALIDCALL; }
int WINAPI Direct3D9EnableMaximizedWindowedModeShim(int) { return 0; }

// Windows' unnamed exports (parameter counts as there): settings of its
// compatibility shims, nothing to do here.
HRESULT WINAPI Direct3D9ForceHybridEnumeration(UINT) { return S_OK; }
HRESULT WINAPI Direct3D9SetMaximizedWindowedModeShim(UINT, UINT) { return S_OK; }
HRESULT WINAPI Direct3D9SetSwapEffectUpgradeShim(int) { return S_OK; }
HRESULT WINAPI Direct3D9Force9on12(BOOL) { return S_OK; }
HRESULT WINAPI Direct3D9Ordinal22(UINT_PTR) { return S_OK; }
HRESULT WINAPI Direct3D9Ordinal23(UINT_PTR) { return S_OK; }

IDirect3D9* WINAPI Direct3DCreate9On12(UINT sdkVersion, void*, UINT) {
  return Direct3DCreate9(sdkVersion);
}

HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdkVersion, void*, UINT, IDirect3D9Ex** d3d) {
  return Direct3DCreate9Ex(sdkVersion, d3d);
}

}
