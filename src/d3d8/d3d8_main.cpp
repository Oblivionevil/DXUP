// d3d8.dll: Direct3D 8 on DXUP's Direct3D 9 (see d3d8_objects.h).

#include "d3d8_objects.h"

#include "../d3d9/d3d9_objects.h"
#include "../util/util_types.h"

using namespace dxup;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    logMessage(LogLevel::Info, "DXUP d3d8.dll loaded (DLL_PROCESS_ATTACH)");
  }
  return TRUE;
}

extern "C" {

IDirect3D8* WINAPI Direct3DCreate8(UINT sdkVersion) {
  logMessage(LogLevel::Info, "Direct3DCreate8 (SDK %u)", sdkVersion);
  return new d3d8::Interface8(new d3d9::Interface(false));
}

// Shaders are checked when they are created.
HRESULT WINAPI ValidatePixelShader(const DWORD* function, const D3DCAPS8*, BOOL, char** errors) {
  if (errors) *errors = nullptr;
  return function ? S_OK : E_FAIL;
}

HRESULT WINAPI ValidateVertexShader(const DWORD* function, const DWORD*, const D3DCAPS8*, BOOL, char** errors) {
  if (errors) *errors = nullptr;
  return function ? S_OK : E_FAIL;
}

void WINAPI DebugSetMute() {}

// A setting of Windows' compatibility shims, nothing to do here.
int WINAPI Direct3D8EnableMaximizedWindowedModeShim(int) { return 0; }

}
