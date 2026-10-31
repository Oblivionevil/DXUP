// Direct3D 8 scenes drawn by Windows' d3d8.dll and by DXUP's (dxup_d3d8.dll
// next to this program, with DXUP's d3d11.dll and dxgi.dll), compared pixel
// by pixel. Runs on Windows' runtime first, which checks the interface
// declarations (src/d3d8/d3d8_types.h) too.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../d3d8/d3d8_types.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

  constexpr UINT kWidth = 64;
  constexpr UINT kHeight = 48;

  using CreateFunction = IDirect3D8* (WINAPI*)(UINT);

  struct Runtime {
    const char* name;
    CreateFunction create = nullptr;
  };

  HWND createWindow() {
    static bool registered = false;
    if (!registered) {
      WNDCLASSW wc = {};
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"dxup_d3d8_test";
      RegisterClassW(&wc);
      registered = true;
    }
    RECT r = { 0, 0, LONG(kWidth), LONG(kHeight) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowW(L"dxup_d3d8_test", L"d3d8 test", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
                         nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  }

  struct TLVertex { float x, y, z, rhw; DWORD color; };
  struct TexVertex { float x, y, z; float u, v; };

  D3DMATRIX identity() {
    D3DMATRIX m = {};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
  }

  D3DMATRIX perspective(float fovY, float aspect, float zn, float zf) {
    D3DMATRIX m = {};
    const float y = 1.0f / std::tan(fovY * 0.5f), x = y / aspect;
    m._11 = x; m._22 = y; m._33 = zf / (zf - zn); m._34 = 1.0f; m._43 = -zn * zf / (zf - zn);
    return m;
  }

  D3DMATRIX translation(float x, float y, float z) {
    D3DMATRIX m = identity();
    m._41 = x; m._42 = y; m._43 = z;
    return m;
  }

  IDirect3DTexture8* checkerTexture(IDirect3DDevice8* device, UINT size) {
    IDirect3DTexture8* texture = nullptr;
    if (FAILED(device->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture)))
      return nullptr;
    D3DLOCKED_RECT rect;
    texture->LockRect(0, &rect, nullptr, 0);
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++)
        reinterpret_cast<DWORD*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch)[x] = ((x / 2 + y / 2) & 1) ? 0xffe03020u : 0x8020c0f0u;
    texture->UnlockRect(0);
    return texture;
  }

  // ---- Scenes ----

  struct Scene {
    const char* name;
    std::function<bool(IDirect3DDevice8*)> draw;
    int tolerance = 2;
  };

  bool sceneClearAndTriangle(IDirect3DDevice8* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff203040u, 1.0f, 0);
    const TLVertex tri[3] = {
      { 8.0f, 4.0f, 0.5f, 1.0f, 0xffff0000u }, { 58.0f, 20.0f, 0.5f, 1.0f, 0xff00ff00u }, { 20.0f, 44.0f, 0.5f, 1.0f, 0xff0000ffu },
    };
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    return SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex)));
  }

  bool sceneTexturedStates(IDirect3DDevice8* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    IDirect3DTexture8* texture = checkerTexture(d, 16);
    if (!texture) return false;
    D3DMATRIX world = identity(), view = translation(0.0f, 0.0f, 2.2f), proj = perspective(1.0f, 4.0f / 3.0f, 0.5f, 10.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetTexture(0, texture);
    // Sampler states as texture stage states.
    d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(D3DTSS8_MINFILTER), D3DTEXF_POINT);
    d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(D3DTSS8_MAGFILTER), D3DTEXF_POINT);
    d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(D3DTSS8_ADDRESSU), D3DTADDRESS_MIRROR);
    const TexVertex quad[4] = {
      { -1.0f, 1.0f, 0.0f, -0.5f, -0.5f }, { 1.0f, 1.0f, 0.0f, 1.5f, -0.5f },
      { -1.0f, -1.0f, 0.0f, -0.5f, 1.5f }, { 1.0f, -1.0f, 0.0f, 1.5f, 1.5f },
    };
    d->SetVertexShader(D3DFVF_XYZ | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TexVertex));
    DWORD value = 0;
    d->GetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(D3DTSS8_ADDRESSU), &value);
    d->SetTexture(0, nullptr);
    texture->Release();
    return value == D3DTADDRESS_MIRROR;
  }

  // vs.1.1 with a declaration of two streams; ps.1.1 modulating.
  bool sceneShaders(IDirect3DDevice8* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    const DWORD declaration[] = {
      0x20000000u,                       // D3DVSD_STREAM(0)
      0x40020000u | 0,                   // D3DVSD_REG(0, FLOAT3)
      0x20000001u,                       // D3DVSD_STREAM(1)
      0x40040000u | 5,                   // D3DVSD_REG(5, D3DCOLOR)
      0x40010000u | 7,                   // D3DVSD_REG(7, FLOAT2)
      0x80000000u | (1u << 25) | 4,      // D3DVSD_CONST(4, 1)
      0x3f800000u, 0x3f400000u, 0x3f000000u, 0x3f800000u,
      0xffffffffu,                       // D3DVSD_END
    };
    // vs.1.1: m4x4 oPos, v0, c0; mul oD0, v5, c4; mov oT0, v7
    const DWORD vs[] = {
      0xfffe0101u,
      0x00000014u, 0xc00f0000u, 0x90e40000u, 0xa0e40000u,
      0x00000005u, 0xd00f0000u, 0x90e40005u, 0xa0e40004u,
      0x00000001u, 0xe00f0000u, 0x90e40007u,
      0x0000ffffu,
    };
    // ps.1.1: tex t0; mul r0, t0, v0
    const DWORD ps[] = {
      0xffff0101u,
      0x00000042u, 0xb00f0000u,
      0x00000005u, 0x800f0000u, 0xb0e40000u, 0x90e40000u,
      0x0000ffffu,
    };
    DWORD vsHandle = 0, psHandle = 0;
    if (FAILED(d->CreateVertexShader(declaration, vs, &vsHandle, 0)) || FAILED(d->CreatePixelShader(ps, &psHandle)))
      return false;
    const float positions[] = { -0.9f, 0.8f, 0.5f, 0.9f, 0.9f, 0.5f, -0.8f, -0.9f, 0.5f, 0.85f, -0.7f, 0.5f };
    struct Attr { DWORD color; float u, v; };
    const Attr attrs[] = { { 0xffffffffu, 0.0f, 0.0f }, { 0xffff8080u, 1.0f, 0.0f }, { 0xff80ff80u, 0.0f, 1.0f },
                           { 0xff8080ffu, 1.0f, 1.0f } };
    const WORD indices[] = { 0, 1, 2, 1, 3, 2 };
    IDirect3DVertexBuffer8 *vb0 = nullptr, *vb1 = nullptr;
    IDirect3DIndexBuffer8* ib = nullptr;
    BYTE* data;
    d->CreateVertexBuffer(sizeof(positions), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb0);
    vb0->Lock(0, 0, &data, 0); std::memcpy(data, positions, sizeof(positions)); vb0->Unlock();
    d->CreateVertexBuffer(sizeof(attrs), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb1);
    vb1->Lock(0, 0, &data, 0); std::memcpy(data, attrs, sizeof(attrs)); vb1->Unlock();
    d->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
    ib->Lock(0, 0, &data, 0); std::memcpy(data, indices, sizeof(indices)); ib->Unlock();
    IDirect3DTexture8* texture = checkerTexture(d, 8);
    D3DMATRIX m = identity();
    d->SetVertexShaderConstant(0, &m._11, 4);
    d->SetVertexShader(vsHandle);
    d->SetPixelShader(psHandle);
    d->SetStreamSource(0, vb0, 12);
    d->SetStreamSource(1, vb1, sizeof(Attr));
    d->SetIndices(ib, 0);
    d->SetTexture(0, texture);
    d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(D3DTSS8_MINFILTER), D3DTEXF_POINT);
    d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(D3DTSS8_MAGFILTER), D3DTEXF_POINT);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    const HRESULT hr = d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 0, 2);
    d->SetPixelShader(0);
    d->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->SetTexture(0, nullptr);
    d->SetStreamSource(0, nullptr, 0);
    d->SetStreamSource(1, nullptr, 0);
    d->SetIndices(nullptr, 0);
    d->DeleteVertexShader(vsHandle);
    d->DeletePixelShader(psHandle);
    texture->Release(); ib->Release(); vb1->Release(); vb0->Release();
    return SUCCEEDED(hr);
  }

  bool sceneStateBlocks(IDirect3DDevice8* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->BeginStateBlock();
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    DWORD additive = 0, saved = 0;
    d->EndStateBlock(&additive);
    d->CreateStateBlock(D3DSBT_ALL, &saved);
    d->ApplyStateBlock(additive);
    const TLVertex quad[4] = {
      { -0.5f, -0.5f, 0.5f, 1.0f, 0xff400000u }, { 63.5f, -0.5f, 0.5f, 1.0f, 0xff400000u },
      { -0.5f, 47.5f, 0.5f, 1.0f, 0xff004000u }, { 63.5f, 47.5f, 0.5f, 1.0f, 0xff004000u },
    };
    d->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
    d->ApplyStateBlock(saved);
    const TLVertex tri[3] = {
      { 4.0f, 4.0f, 0.5f, 1.0f, 0xff0000ffu }, { 40.0f, 4.0f, 0.5f, 1.0f, 0xff0000ffu }, { 4.0f, 40.0f, 0.5f, 1.0f, 0xff0000ffu },
    };
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    d->DeleteStateBlock(additive);
    d->DeleteStateBlock(saved);
    return true;
  }

  bool sceneCopyRects(IDirect3DDevice8* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    IDirect3DSurface8* image = nullptr;
    if (FAILED(d->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &image)))
      return false;
    D3DLOCKED_RECT rect;
    image->LockRect(&rect, nullptr, 0);
    for (UINT y = 0; y < 16; y++)
      for (UINT x = 0; x < 16; x++)
        reinterpret_cast<DWORD*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch)[x] = 0xff000000u | (x * 16 << 16) | (y * 16 << 8);
    image->UnlockRect();
    IDirect3DSurface8* back = nullptr;
    d->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
    const RECT rects[2] = { { 0, 0, 16, 16 }, { 4, 4, 12, 12 } };
    const POINT points[2] = { { 10, 10 }, { 40, 20 } };
    const HRESULT hr = d->CopyRects(image, rects, 2, back, points);
    D3DSURFACE_DESC8 desc = {};
    image->GetDesc(&desc);
    back->Release();
    image->Release();
    return SUCCEEDED(hr) && desc.Size == 16 * 16 * 4;
  }

  const Scene kScenes[] = {
    { "clear_triangle", sceneClearAndTriangle },
    { "textured_states", sceneTexturedStates },
    { "shaders", sceneShaders },
    { "state_blocks", sceneStateBlocks },
    { "copy_rects", sceneCopyRects },
  };

  // Interface checks: the declarations match Windows' runtime.
  bool checkInterfaces(IDirect3D8* d3d, IDirect3DDevice8* device, std::string& error) {
    D3DCAPS8 caps = {};
    if (FAILED(d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps)) || caps.MaxTextureWidth < 2048 || caps.MaxSimultaneousTextures < 2) {
      error = "GetDeviceCaps";
      return false;
    }
    D3DADAPTER_IDENTIFIER8 id = {};
    if (FAILED(d3d->GetAdapterIdentifier(0, 0, &id)) || !id.VendorId) {
      error = "GetAdapterIdentifier";
      return false;
    }
    if (!d3d->GetAdapterModeCount(0)) {
      error = "GetAdapterModeCount";
      return false;
    }
    IDirect3DTexture8* texture = nullptr;
    if (FAILED(device->CreateTexture(8, 8, 2, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &texture))) {
      error = "CreateTexture";
      return false;
    }
    bool ok = true;
    void* object = nullptr;
    for (const GUID& id2 : { __uuidof(IDirect3DTexture8), __uuidof(IDirect3DBaseTexture8), __uuidof(IDirect3DResource8) }) {
      if (FAILED(texture->QueryInterface(id2, &object))) { ok = false; error = "texture QueryInterface"; }
      else static_cast<IUnknown*>(object)->Release();
    }
    IDirect3DSurface8* surface = nullptr;
    if (FAILED(texture->GetSurfaceLevel(1, &surface))) { ok = false; error = "GetSurfaceLevel"; }
    else {
      if (FAILED(surface->QueryInterface(__uuidof(IDirect3DSurface8), &object))) { ok = false; error = "surface QueryInterface"; }
      else static_cast<IUnknown*>(object)->Release();
      IDirect3DTexture8* container = nullptr;
      if (FAILED(surface->GetContainer(__uuidof(IDirect3DTexture8), reinterpret_cast<void**>(&container))) || container != texture) {
        ok = false;
        error = "GetContainer";
      }
      if (container) container->Release();
      D3DSURFACE_DESC8 desc = {};
      surface->GetDesc(&desc);
      if (desc.Width != 4 || desc.Size != 4 * 4 * 4) { ok = false; error = "surface GetDesc"; }
      surface->Release();
    }
    texture->Release();
    IDirect3DVertexBuffer8* vb = nullptr;
    if (FAILED(device->CreateVertexBuffer(64, 0, 0, D3DPOOL_MANAGED, &vb)) || FAILED(vb->QueryInterface(__uuidof(IDirect3DVertexBuffer8), &object))) {
      ok = false;
      error = "vertex buffer";
    } else {
      static_cast<IUnknown*>(object)->Release();
    }
    if (vb) vb->Release();
    IDirect3DCubeTexture8* cube = nullptr;
    if (FAILED(device->CreateCubeTexture(8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube))
     || FAILED(cube->QueryInterface(__uuidof(IDirect3DCubeTexture8), &object))) {
      ok = false;
      error = "cube texture";
    } else {
      static_cast<IUnknown*>(object)->Release();
    }
    if (cube) cube->Release();
    if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice8), &object))) { ok = false; error = "device QueryInterface"; }
    else static_cast<IUnknown*>(object)->Release();
    if (FAILED(d3d->QueryInterface(__uuidof(IDirect3D8), &object))) { ok = false; error = "IDirect3D8 QueryInterface"; }
    else static_cast<IUnknown*>(object)->Release();
    return ok;
  }

  bool run(Runtime& runtime, const Scene* scene, std::vector<DWORD>& pixels, std::string& error) {
    IDirect3D8* d3d = runtime.create(D3D8_SDK_VERSION);
    if (!d3d) { error = "Direct3DCreate8 failed"; return false; }
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS8 pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = window;
    IDirect3DDevice8* device = nullptr;
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device))) {
      error = "CreateDevice failed";
      d3d->Release();
      DestroyWindow(window);
      return false;
    }
    bool ok = true;
    if (!scene) {
      ok = checkInterfaces(d3d, device, error);
    } else {
      device->BeginScene();
      ok = scene->draw(device);
      device->EndScene();
      if (!ok) error = "scene failed";
      IDirect3DSurface8 *back = nullptr, *copy = nullptr;
      if (ok && SUCCEEDED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back))
       && SUCCEEDED(device->CreateImageSurface(kWidth, kHeight, D3DFMT_A8R8G8B8, &copy))
       && SUCCEEDED(device->CopyRects(back, nullptr, 0, copy, nullptr))) {
        D3DLOCKED_RECT rect;
        copy->LockRect(&rect, nullptr, D3DLOCK_READONLY);
        pixels.resize(kWidth * kHeight);
        for (UINT y = 0; y < kHeight; y++)
          std::memcpy(&pixels[y * kWidth], static_cast<BYTE*>(rect.pBits) + y * rect.Pitch, kWidth * 4);
        copy->UnlockRect();
      } else if (ok) {
        ok = false;
        error = "readback failed";
      }
      device->Present(nullptr, nullptr, nullptr, nullptr);
      if (copy) copy->Release();
      if (back) back->Release();
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return ok;
  }

}

// Windows has no 64-bit d3d8.dll: the 32-bit run saves Windows' images
// (--save-reference DIR), the 64-bit run compares DXUP's against them
// (--reference DIR).
static bool writeReference(const std::wstring& path, const std::vector<DWORD>& pixels) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
    return false;
  const bool ok = std::fwrite(pixels.data(), 4, pixels.size(), f) == pixels.size();
  std::fclose(f);
  return ok;
}

static bool readReference(const std::wstring& path, std::vector<DWORD>& pixels) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
    return false;
  pixels.assign(size_t(kWidth) * kHeight, 0);
  const bool ok = std::fread(pixels.data(), 4, pixels.size(), f) == pixels.size();
  std::fclose(f);
  return ok;
}

// Windows' runtime on a driver that crashes in a scene (AMD's Direct3D 9
// driver, amdxn32.dll, in shaders on an RX 9070 XT): -1, the scene is
// skipped and Windows' runtime not used again.
static int runGuarded(Runtime& runtime, const Scene* scene, std::vector<DWORD>& pixels, std::string& error) {
  __try {
    return run(runtime, scene, pixels, error) ? 1 : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

static std::wstring widen(const char* text) {
  std::wstring w;
  for (const char* c = text; *c; c++) w += wchar_t(*c);
  return w;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* filter = nullptr;
  std::wstring saveDir, referenceDir;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--save-reference") == 0 && i + 1 < argc) saveDir = widen(argv[++i]) + L"\\";
    else if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc) referenceDir = widen(argv[++i]) + L"\\";
    else filter = argv[i];
  }
  wchar_t systemDir[MAX_PATH];
  GetSystemDirectoryW(systemDir, MAX_PATH);
  Runtime windows = { "Windows" };
  Runtime dxup = { "DXUP" };
  HMODULE native = LoadLibraryW((std::wstring(systemDir) + L"\\d3d8.dll").c_str());
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
  HMODULE ours = LoadLibraryW((dir + L"dxup_d3d8.dll").c_str());
  windows.create = native ? reinterpret_cast<CreateFunction>(GetProcAddress(native, "Direct3DCreate8")) : nullptr;
  dxup.create = ours ? reinterpret_cast<CreateFunction>(GetProcAddress(ours, "Direct3DCreate8")) : nullptr;
  if ((!windows.create && referenceDir.empty()) || !dxup.create) {
    std::printf("FAIL: d3d8.dll (Windows %s, DXUP %s)\n", windows.create ? "ok" : "missing", dxup.create ? "ok" : "missing");
    return 1;
  }
  int passed = 0, failed = 0, skipped = 0;
  bool windowsCrashed = false;
  for (Runtime* r : { &windows, &dxup }) {
    if (!r->create)
      continue;
    std::vector<DWORD> unused;
    std::string error;
    const bool ok = run(*r, nullptr, unused, error);
    std::printf("%s interfaces on %-8s %s\n", ok ? "ok  " : "FAIL", r->name, error.c_str());
    ok ? passed++ : failed++;
  }
  for (const Scene& scene : kScenes) {
    if (filter && !std::strstr(scene.name, filter))
      continue;
    std::vector<DWORD> reference, result;
    std::string error;
    if (!referenceDir.empty()) {
      if (!readReference(referenceDir + widen(scene.name) + L".ref", reference)) {
        std::printf("FAIL %-18s no reference image\n", scene.name);
        failed++;
        continue;
      }
    } else {
      const int ran = windowsCrashed ? -1 : runGuarded(windows, &scene, reference, error);
      if (ran < 0) {
        // DXUP still has to draw it.
        std::vector<DWORD> unused;
        const bool drawn = run(dxup, &scene, unused, error);
        std::printf("%s %-18s %s; DXUP %s\n", drawn ? "skip" : "FAIL", scene.name,
                    windowsCrashed ? "not compared" : "Windows' driver crashed", drawn ? "drew it" : error.c_str());
        windowsCrashed = true;
        drawn ? skipped++ : failed++;
        continue;
      }
      if (!ran) {
        std::printf("FAIL %-18s Windows: %s\n", scene.name, error.c_str());
        failed++;
        continue;
      }
    }
    if (!saveDir.empty() && !writeReference(saveDir + widen(scene.name) + L".ref", reference))
      std::printf("     %-18s reference not saved\n", scene.name);
    if (!run(dxup, &scene, result, error)) {
      std::printf("FAIL %-18s DXUP: %s\n", scene.name, error.c_str());
      failed++;
      continue;
    }
    int worst = 0, worstIndex = 0, differing = 0;
    for (size_t i = 0; i < reference.size(); i++) {
      int diff = 0;
      for (int c = 0; c < 32; c += 8)
        diff = (std::max)(diff, std::abs(int((reference[i] >> c) & 255) - int((result[i] >> c) & 255)));
      if (diff > scene.tolerance) differing++;
      if (diff > worst) { worst = diff; worstIndex = int(i); }
    }
    const bool ok = differing <= int(reference.size() / 200);
    std::printf("%s %-18s max difference %3d, %4d pixels beyond %d", ok ? "ok  " : "FAIL", scene.name, worst, differing,
                scene.tolerance);
    if (worst)
      std::printf("  (worst at %d,%d: Windows %08lx, DXUP %08lx)", worstIndex % kWidth, worstIndex / kWidth,
                  reference[worstIndex], result[worstIndex]);
    std::printf("\n");
    ok ? passed++ : failed++;
  }
  if (skipped)
    std::printf("%d passed, %d failed, %d skipped\n", passed, failed, skipped);
  else
    std::printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
