// DirectDraw 7 / Direct3D 7 tests: the same checks with Windows' ddraw.dll
// and with DXUP's next to the executable.

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

  template<typename T>
  struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
    T* get() const { return p; }
  };

  std::string format(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return text;
  }

  struct Context {
    HWND window = nullptr;
    Com<IDirectDraw7> dd;
    Com<IDirect3D7> d3d;
    Com<IDirectDrawSurface7> target;
    Com<IDirectDrawSurface7> depth;
    Com<IDirect3DDevice7> device;
    static constexpr DWORD size = 64;
  };

  DDPIXELFORMAT rgbFormat(DWORD bits, DWORD r, DWORD g, DWORD b, DWORD a) {
    DDPIXELFORMAT pf = {};
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    pf.dwRGBBitCount = bits;
    pf.dwRBitMask = r;
    pf.dwGBitMask = g;
    pf.dwBBitMask = b;
    pf.dwRGBAlphaBitMask = a;
    return pf;
  }

  bool setup(Context& c, std::string& error) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "Dx7Test";
    RegisterClassA(&wc);
    c.window = CreateWindowExA(0, "Dx7Test", "dx7", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(c.dd.put()), IID_IDirectDraw7, nullptr);
    if (FAILED(hr)) { error = format("DirectDrawCreateEx 0x%08lx", hr); return false; }
    hr = c.dd->SetCooperativeLevel(c.window, DDSCL_NORMAL);
    if (FAILED(hr)) { error = format("SetCooperativeLevel 0x%08lx", hr); return false; }
    hr = c.dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(c.d3d.put()));
    if (FAILED(hr)) { error = "no IDirect3D7"; return false; }

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = desc.dwHeight = Context::size;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0);
    hr = c.dd->CreateSurface(&desc, c.target.put(), nullptr);
    if (FAILED(hr)) { error = format("render target 0x%08lx", hr); return false; }

    DDSURFACEDESC2 zd = {};
    zd.dwSize = sizeof(zd);
    zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    zd.dwWidth = zd.dwHeight = Context::size;
    zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
    zd.ddpfPixelFormat.dwZBitMask = 0xffff;
    hr = c.dd->CreateSurface(&zd, c.depth.put(), nullptr);
    if (FAILED(hr)) { error = format("z-buffer 0x%08lx", hr); return false; }
    hr = c.target->AddAttachedSurface(c.depth.get());
    if (FAILED(hr)) { error = format("AddAttachedSurface 0x%08lx", hr); return false; }

    hr = c.d3d->CreateDevice(IID_IDirect3DTnLHalDevice, c.target.get(), c.device.put());
    if (FAILED(hr))
      hr = c.d3d->CreateDevice(IID_IDirect3DHALDevice, c.target.get(), c.device.put());
    if (FAILED(hr)) { error = format("CreateDevice 0x%08lx", hr); return false; }
    D3DVIEWPORT7 vp = { 0, 0, Context::size, Context::size, 0.0f, 1.0f };
    c.device->SetViewport(&vp);
    return true;
  }

  bool readPixel(Context& c, DWORD x, DWORD y, DWORD& pixel, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    HRESULT hr = c.target->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
    if (FAILED(hr)) { error = format("Lock 0x%08lx", hr); return false; }
    pixel = static_cast<const DWORD*>(desc.lpSurface)[y * (desc.lPitch / 4) + x] & 0xffffff;
    c.target->Unlock(nullptr);
    return true;
  }

  bool closeTo(DWORD a, DWORD b, int tolerance = 3) {
    for (int shift = 0; shift < 24; shift += 8) {
      const int ca = int((a >> shift) & 255), cb = int((b >> shift) & 255);
      if (std::abs(ca - cb) > tolerance)
        return false;
    }
    return true;
  }

  bool expectPixel(Context& c, DWORD x, DWORD y, DWORD expected, const char* what, std::string& error, int tolerance = 3) {
    DWORD pixel = 0;
    if (!readPixel(c, x, y, pixel, error))
      return false;
    if (!closeTo(pixel, expected, tolerance)) {
      error = format("%s: pixel (%lu,%lu) 0x%06lx, expected 0x%06lx", what, x, y, pixel, expected);
      return false;
    }
    return true;
  }

  struct TLVertex { float x, y, z, rhw; DWORD diffuse; float u, v; };

  void quad(TLVertex* v, float x0, float y0, float x1, float y1, DWORD color, float z = 0.5f) {
    v[0] = { x0, y0, z, 1.0f, color, 0.0f, 0.0f };
    v[1] = { x1, y0, z, 1.0f, color, 1.0f, 0.0f };
    v[2] = { x0, y1, z, 1.0f, color, 0.0f, 1.0f };
    v[3] = { x1, y1, z, 1.0f, color, 1.0f, 1.0f };
  }

  const DWORD kTLFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

  bool clearAndRead(Context& c, std::string& error) {
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffff0000, 1.0f, 0);
    if (!expectPixel(c, 5, 5, 0xff0000, "clear red", error)) return false;
    D3DRECT rect = { 16, 16, 32, 32 };
    c.device->Clear(1, &rect, D3DCLEAR_TARGET, 0xff0000ff, 1.0f, 0);
    return expectPixel(c, 20, 20, 0x0000ff, "partial clear inside", error)
        && expectPixel(c, 40, 40, 0xff0000, "partial clear outside", error);
  }

  bool transformedQuad(Context& c, std::string& error) {
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    c.device->SetTexture(0, nullptr);
    c.device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    c.device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    c.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    c.device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    TLVertex v[4];
    quad(v, 8.0f, 8.0f, 40.0f, 40.0f, 0xff00ff00);
    c.device->BeginScene();
    HRESULT hr = c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, v, 4, 0);
    c.device->EndScene();
    if (FAILED(hr)) { error = format("DrawPrimitive 0x%08lx", hr); return false; }
    // Pixel coverage: (8,8) inside, (40,40) outside, as Direct3D 7 rasterizes.
    return expectPixel(c, 8, 8, 0x00ff00, "quad corner inside", error)
        && expectPixel(c, 39, 39, 0x00ff00, "quad last pixel", error)
        && expectPixel(c, 40, 40, 0x000000, "quad outside", error)
        && expectPixel(c, 7, 20, 0x000000, "quad left outside", error);
  }

  bool texturedQuad(Context& c, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = desc.dwHeight = 4;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0xff000000);
    Com<IDirectDrawSurface7> texture;
    HRESULT hr = c.dd->CreateSurface(&desc, texture.put(), nullptr);
    if (FAILED(hr)) { error = format("texture 0x%08lx", hr); return false; }
    DDSURFACEDESC2 locked = {};
    locked.dwSize = sizeof(locked);
    if (FAILED(texture->Lock(nullptr, &locked, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) { error = "texture Lock"; return false; }
    for (DWORD y = 0; y < 4; y++)
      for (DWORD x = 0; x < 4; x++)
        static_cast<DWORD*>(locked.lpSurface)[y * (locked.lPitch / 4) + x] = x < 2 ? 0xff0000ff : 0xffffff00;
    texture->Unlock(nullptr);

    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    c.device->SetTexture(0, texture.get());
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    c.device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    c.device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_POINT);
    TLVertex v[4];
    quad(v, 0.0f, 0.0f, 64.0f, 64.0f, 0xffffffff);
    c.device->BeginScene();
    c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, v, 4, 0);
    c.device->EndScene();
    c.device->SetTexture(0, nullptr);
    return expectPixel(c, 10, 30, 0x0000ff, "texture left half", error)
        && expectPixel(c, 50, 30, 0xffff00, "texture right half", error);
  }

  // Colour keying turned on after the texture was drawn without it: DXUP
  // uploads it again with the key from the application's copy, which in a
  // 32-bit process went after the first upload and comes back from the GPU.
  // The key's texels are dropped whatever the stages do with alpha (an
  // alpha operation taking the diffuse alpha here), as Windows' Direct3D 7
  // does. A lock reads the texels back too.
  bool lateColorKey(Context& c, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = desc.dwHeight = 128;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    // A format DXUP keeps as it is (64 KB): its copy goes after the upload.
    desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0xff000000);
    Com<IDirectDrawSurface7> texture;
    HRESULT hr = c.dd->CreateSurface(&desc, texture.put(), nullptr);
    if (FAILED(hr)) { error = format("texture 0x%08lx", hr); return false; }
    DDSURFACEDESC2 locked = {};
    locked.dwSize = sizeof(locked);
    if (FAILED(texture->Lock(nullptr, &locked, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) { error = "texture Lock"; return false; }
    for (DWORD y = 0; y < 128; y++)
      for (DWORD x = 0; x < 128; x++)
        reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.lpSurface) + y * locked.lPitch)[x] = x < 64 ? 0xff0000ffu : 0xffff00ffu;
    texture->Unlock(nullptr);
    DDCOLORKEY key = { 0xffff00ffu, 0xffff00ffu };   // the whole texel, alpha too (Windows compares it)
    if (FAILED(texture->SetColorKey(DDCKEY_SRCBLT, &key))) { error = "SetColorKey"; return false; }

    c.device->SetTexture(0, texture.get());
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    c.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    c.device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);   // as transformed_quad left them
    c.device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    c.device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_POINT);
    TLVertex v[4];
    quad(v, 0.0f, 0.0f, 64.0f, 64.0f, 0xffffffff);
    for (int pass = 0; pass < 2; pass++) {
      c.device->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, pass == 1);
      c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffff0000, 1.0f, 0);
      c.device->BeginScene();
      c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, v, 4, 0);
      c.device->EndScene();
      if (pass == 0 && !expectPixel(c, 50, 30, 0xff00ff, "without keying", error))
        return false;
    }
    c.device->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
    c.device->SetTexture(0, nullptr);
    if (!expectPixel(c, 10, 30, 0x0000ff, "left half", error) || !expectPixel(c, 50, 30, 0xff0000, "keyed half", error))
      return false;
    locked = {};
    locked.dwSize = sizeof(locked);
    if (FAILED(texture->Lock(nullptr, &locked, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) { error = "texture Lock again"; return false; }
    const DWORD left = reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.lpSurface) + 100 * locked.lPitch)[10];
    const DWORD right = reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.lpSurface) + 100 * locked.lPitch)[100];
    texture->Unlock(nullptr);
    if (left != 0xff0000ffu || right != 0xffff00ffu) {
      error = format("locked again: texels 0x%08lx 0x%08lx", left, right);
      return false;
    }
    return true;
  }

  bool alphaBlend(Context& c, std::string& error) {
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff0000ff, 1.0f, 0);
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    c.device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    c.device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    c.device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    TLVertex v[4];
    quad(v, 0.0f, 0.0f, 64.0f, 64.0f, 0x80ff0000);
    c.device->BeginScene();
    c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, v, 4, 0);
    c.device->EndScene();
    c.device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    return expectPixel(c, 32, 32, 0x80007f, "alpha blend", error, 4);
  }

  bool depthTest(Context& c, std::string& error) {
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    c.device->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
    c.device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
    c.device->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    TLVertex front[4], back[4];
    quad(front, 0.0f, 0.0f, 64.0f, 64.0f, 0xff00ff00, 0.2f);
    quad(back, 0.0f, 0.0f, 64.0f, 64.0f, 0xffff0000, 0.6f);
    c.device->BeginScene();
    c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, front, 4, 0);
    c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, back, 4, 0);
    c.device->EndScene();
    return expectPixel(c, 32, 32, 0x00ff00, "depth test keeps the front", error);
  }

  bool transformAndLight(Context& c, std::string& error) {
    struct Vertex { float x, y, z, nx, ny, nz; };
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    c.device->SetTransform(D3DTRANSFORMSTATE_WORLD, &identity);
    c.device->SetTransform(D3DTRANSFORMSTATE_VIEW, &identity);
    c.device->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &identity);
    c.device->SetRenderState(D3DRENDERSTATE_LIGHTING, TRUE);
    c.device->SetRenderState(D3DRENDERSTATE_AMBIENT, 0);
    D3DMATERIAL7 material = {};
    material.diffuse.r = 1.0f; material.diffuse.g = 0.5f; material.diffuse.b = 0.25f; material.diffuse.a = 1.0f;
    c.device->SetMaterial(&material);
    D3DLIGHT7 light = {};
    light.dltType = D3DLIGHT_DIRECTIONAL;
    light.dcvDiffuse.r = light.dcvDiffuse.g = light.dcvDiffuse.b = 1.0f;
    light.dvDirection.x = 0.0f; light.dvDirection.y = 0.0f; light.dvDirection.z = 1.0f;
    c.device->SetLight(0, &light);
    c.device->LightEnable(0, TRUE);
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    const Vertex v[4] = { { -1, -1, 0.5f, 0, 0, -1 }, { -1, 1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 } };
    c.device->BeginScene();
    HRESULT hr = c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZ | D3DFVF_NORMAL, const_cast<Vertex*>(v), 4, 0);
    c.device->EndScene();
    c.device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    c.device->LightEnable(0, FALSE);
    if (FAILED(hr)) { error = format("DrawPrimitive 0x%08lx", hr); return false; }
    return expectPixel(c, 32, 32, 0xff7f3f, "directional light on the material", error, 4);
  }

  // Software transform and lighting into a vertex buffer of transformed
  // vertices, then drawn: as the transform_and_light quad, at half size.
  bool processVertices(Context& c, std::string& error) {
    struct Vertex { float x, y, z, nx, ny, nz; };
    D3DVERTEXBUFFERDESC desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwCaps = D3DVBCAPS_SYSTEMMEMORY;
    desc.dwFVF = D3DFVF_XYZ | D3DFVF_NORMAL;
    desc.dwNumVertices = 4;
    Com<IDirect3DVertexBuffer7> source, transformed;
    HRESULT hr = c.d3d->CreateVertexBuffer(&desc, source.put(), 0);
    desc.dwFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    if (SUCCEEDED(hr)) hr = c.d3d->CreateVertexBuffer(&desc, transformed.put(), 0);
    if (FAILED(hr)) { error = format("CreateVertexBuffer 0x%08lx", hr); return false; }
    void* data = nullptr;
    if (FAILED(source->Lock(DDLOCK_WAIT, &data, nullptr))) { error = "VB Lock"; return false; }
    const Vertex v[4] = { { -1, -1, 0.5f, 0, 0, -1 }, { -1, 1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 } };
    std::memcpy(data, v, sizeof(v));
    source->Unlock();
    D3DMATRIX identity = {}, half = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    half._11 = half._22 = 0.5f;
    half._33 = half._44 = 1.0f;
    c.device->SetTransform(D3DTRANSFORMSTATE_WORLD, &half);
    c.device->SetTransform(D3DTRANSFORMSTATE_VIEW, &identity);
    c.device->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &identity);
    c.device->SetRenderState(D3DRENDERSTATE_LIGHTING, TRUE);
    c.device->SetRenderState(D3DRENDERSTATE_AMBIENT, 0);
    D3DMATERIAL7 material = {};
    material.diffuse.r = 1.0f; material.diffuse.g = 0.5f; material.diffuse.b = 0.25f; material.diffuse.a = 1.0f;
    c.device->SetMaterial(&material);
    D3DLIGHT7 light = {};
    light.dltType = D3DLIGHT_DIRECTIONAL;
    light.dcvDiffuse.r = light.dcvDiffuse.g = light.dcvDiffuse.b = 1.0f;
    light.dvDirection.z = 1.0f;
    c.device->SetLight(0, &light);
    c.device->LightEnable(0, TRUE);
    hr = transformed->ProcessVertices(D3DVOP_TRANSFORM | D3DVOP_LIGHT, 0, 4, source.get(), 0, c.device.get(), 0);
    c.device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    c.device->LightEnable(0, FALSE);
    c.device->SetTransform(D3DTRANSFORMSTATE_WORLD, &identity);
    if (FAILED(hr)) { error = format("ProcessVertices 0x%08lx", hr); return false; }
    D3DVIEWPORT7 viewport = {};
    c.device->GetViewport(&viewport);
    struct TL { float x, y, z, rhw; DWORD diffuse; };
    if (FAILED(transformed->Lock(DDLOCK_WAIT | DDLOCK_READONLY, &data, nullptr))) { error = "VB Lock"; return false; }
    const TL first = static_cast<const TL*>(data)[0];
    transformed->Unlock();
    const float expectedX = float(viewport.dwX) + 0.25f * float(viewport.dwWidth);
    const float expectedY = float(viewport.dwY) + 0.75f * float(viewport.dwHeight);
    if (std::fabs(first.x - expectedX) > 0.01f || std::fabs(first.y - expectedY) > 0.01f || std::fabs(first.rhw - 1.0f) > 1e-5f
     || !closeTo(first.diffuse & 0xffffff, 0xff8040, 1)) {
      error = format("vertex 0: %.2f %.2f %.3f %.3f 0x%08lx, expected %.2f %.2f, 0xffff8040", first.x, first.y, first.z, first.rhw,
        first.diffuse, expectedX, expectedY);
      return false;
    }
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    c.device->BeginScene();
    hr = c.device->DrawPrimitiveVB(D3DPT_TRIANGLESTRIP, transformed.get(), 0, 4, 0);
    c.device->EndScene();
    if (FAILED(hr)) { error = format("DrawPrimitiveVB 0x%08lx", hr); return false; }
    const DWORD cx = viewport.dwX + viewport.dwWidth / 2, cy = viewport.dwY + viewport.dwHeight / 2;
    return expectPixel(c, cx, cy, 0xff7f3f, "lit by ProcessVertices", error, 4)
        && expectPixel(c, viewport.dwX + 2, viewport.dwY + 2, 0x000000, "outside the half-size quad", error);
  }

  bool vertexBufferFan(Context& c, std::string& error) {
    D3DVERTEXBUFFERDESC desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwCaps = D3DVBCAPS_WRITEONLY;
    desc.dwFVF = kTLFvf;
    desc.dwNumVertices = 4;
    Com<IDirect3DVertexBuffer7> vb;
    HRESULT hr = c.d3d->CreateVertexBuffer(&desc, vb.put(), 0);
    if (FAILED(hr)) { error = format("CreateVertexBuffer 0x%08lx", hr); return false; }
    void* data = nullptr;
    if (FAILED(vb->Lock(DDLOCK_WAIT | DDLOCK_WRITEONLY | DDLOCK_DISCARDCONTENTS, &data, nullptr))) { error = "VB Lock"; return false; }
    auto* v = static_cast<TLVertex*>(data);
    v[0] = { 0.0f, 0.0f, 0.5f, 1.0f, 0xffffff00, 0, 0 };
    v[1] = { 64.0f, 0.0f, 0.5f, 1.0f, 0xffffff00, 0, 0 };
    v[2] = { 64.0f, 64.0f, 0.5f, 1.0f, 0xffffff00, 0, 0 };
    v[3] = { 0.0f, 64.0f, 0.5f, 1.0f, 0xffffff00, 0, 0 };
    vb->Unlock();
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    c.device->BeginScene();
    hr = c.device->DrawPrimitiveVB(D3DPT_TRIANGLEFAN, vb.get(), 0, 4, 0);
    c.device->EndScene();
    if (FAILED(hr)) { error = format("DrawPrimitiveVB 0x%08lx", hr); return false; }
    return expectPixel(c, 10, 50, 0xffff00, "fan from a vertex buffer", error)
        && expectPixel(c, 50, 10, 0xffff00, "fan from a vertex buffer, second triangle", error);
  }

  bool fogTable(Context& c, std::string& error) {
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    c.device->SetRenderState(D3DRENDERSTATE_FOGENABLE, TRUE);
    c.device->SetRenderState(D3DRENDERSTATE_FOGCOLOR, 0xff0000ff);
    c.device->SetRenderState(D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_LINEAR);
    float start = 0.0f, end = 1.0f;
    c.device->SetRenderState(D3DRENDERSTATE_FOGSTART, *reinterpret_cast<DWORD*>(&start));
    c.device->SetRenderState(D3DRENDERSTATE_FOGEND, *reinterpret_cast<DWORD*>(&end));
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    TLVertex v[4];
    quad(v, 0.0f, 0.0f, 64.0f, 64.0f, 0xffff0000, 0.5f);
    for (auto& vertex : v) vertex.rhw = 2.0f;   // w = 0.5: half fogged
    c.device->BeginScene();
    c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, v, 4, 0);
    c.device->EndScene();
    c.device->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
    return expectPixel(c, 32, 32, 0x7f0080, "linear table fog at half depth", error, 6);
  }

  bool bltAndFill(Context& c, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = desc.dwHeight = 16;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0);
    Com<IDirectDrawSurface7> source;
    if (FAILED(c.dd->CreateSurface(&desc, source.put(), nullptr))) { error = "system memory surface"; return false; }
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x00ff00ff;
    if (FAILED(source->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx))) { error = "color fill"; return false; }
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    RECT dst = { 24, 24, 40, 40 };
    if (FAILED(c.target->Blt(&dst, source.get(), nullptr, DDBLT_WAIT, nullptr))) { error = "Blt"; return false; }
    return expectPixel(c, 30, 30, 0xff00ff, "Blt from system memory", error)
        && expectPixel(c, 10, 10, 0x000000, "Blt leaves the rest", error);
  }

  bool dxtTexture(Context& c, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = desc.dwHeight = 8;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
    desc.ddpfPixelFormat.dwFourCC = MAKEFOURCC('D', 'X', 'T', '1');
    Com<IDirectDrawSurface7> texture;
    HRESULT hr = c.dd->CreateSurface(&desc, texture.put(), nullptr);
    if (FAILED(hr)) { error = format("DXT1 texture 0x%08lx", hr); return false; }
    DDSURFACEDESC2 locked = {};
    locked.dwSize = sizeof(locked);
    if (FAILED(texture->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr))) { error = "DXT1 Lock"; return false; }
    // Every block: color 0 = pure green (565 0x07e0), all texels index 0.
    auto* bytes = static_cast<uint8_t*>(locked.lpSurface);
    for (int block = 0; block < 4; block++) {
      // Rows of blocks lie one after the other (Lock reports the linear size).
      uint8_t* b = bytes + (block / 2) * 16 + (block % 2) * 8;
      b[0] = 0xe0; b[1] = 0x07; b[2] = 0x00; b[3] = 0x00;
      b[4] = b[5] = b[6] = b[7] = 0;
    }
    texture->Unlock(nullptr);
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    c.device->SetTexture(0, texture.get());
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    TLVertex v[4];
    quad(v, 0.0f, 0.0f, 64.0f, 64.0f, 0xffffffff);
    c.device->BeginScene();
    c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, v, 4, 0);
    c.device->EndScene();
    c.device->SetTexture(0, nullptr);
    return expectPixel(c, 32, 32, 0x00ff00, "DXT1 texture", error);
  }

  // A source colour key on a DXT1 texture: Windows' Direct3D 7 draws it as
  // it is with colour keying on, whatever the key (as a 565 colour of its
  // blocks or as RGB). The key comes after a first draw (in a 32-bit
  // process the 32 KB copy went after the upload), and the blocks lock as
  // written.
  bool dxtColorKey(Context& c, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = desc.dwHeight = 256;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
    desc.ddpfPixelFormat.dwFourCC = MAKEFOURCC('D', 'X', 'T', '1');
    Com<IDirectDrawSurface7> texture;
    HRESULT hr = c.dd->CreateSurface(&desc, texture.put(), nullptr);
    if (FAILED(hr)) { error = format("DXT1 texture 0x%08lx", hr); return false; }
    DDSURFACEDESC2 locked = {};
    locked.dwSize = sizeof(locked);
    if (FAILED(texture->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr))) { error = "DXT1 Lock"; return false; }
    // Left half magenta (565 0xf81f), right half magenta and blue texels
    // alternating (4-colour blocks, indices 0 and 1).
    auto block = [&](uint32_t bx, uint32_t by) { return static_cast<uint8_t*>(locked.lpSurface) + (by * 64 + bx) * 8; };
    for (uint32_t by = 0; by < 64; by++)
      for (uint32_t bx = 0; bx < 64; bx++) {
        uint8_t* b = block(bx, by);
        const uint8_t right = bx >= 32;
        const uint8_t texels[8] = { 0x1f, 0xf8, uint8_t(right ? 0x1f : 0), 0, uint8_t(right ? 0x44 : 0),
                                    uint8_t(right ? 0x44 : 0), uint8_t(right ? 0x44 : 0), uint8_t(right ? 0x44 : 0) };
        std::memcpy(b, texels, 8);
      }
    texture->Unlock(nullptr);
    c.device->SetTexture(0, texture.get());
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    c.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    c.device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    c.device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    c.device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_POINT);
    const DWORD keys[3] = { 0, 0xf81f, 0xff00ff };
    for (int k = 0; k < 3; k++) {
      DDCOLORKEY key = { keys[k], keys[k] };
      if (k && FAILED(texture->SetColorKey(DDCKEY_SRCBLT, &key))) { error = "SetColorKey"; return false; }
      c.device->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, k != 0);
      c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff404040, 1.0f, 0);
      // Texels 96 to 159 of each row, one per pixel (pixel 36: texel 132).
      const TLVertex v[4] = { { -0.5f, -0.5f, 0.5f, 1.0f, 0xffffffff, 0.375f, 0.0f },
                              { 63.5f, -0.5f, 0.5f, 1.0f, 0xffffffff, 0.625f, 0.0f },
                              { -0.5f, 63.5f, 0.5f, 1.0f, 0xffffffff, 0.375f, 1.0f },
                              { 63.5f, 63.5f, 0.5f, 1.0f, 0xffffffff, 0.625f, 1.0f } };
      c.device->BeginScene();
      c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, const_cast<TLVertex*>(v), 4, 0);
      c.device->EndScene();
      const std::string what = format("key 0x%06lx", keys[k]);
      if (!expectPixel(c, 10, 20, 0xff00ff, (what + " left").c_str(), error)
       || !expectPixel(c, 36, 20, 0xff00ff, (what + " right, index 0").c_str(), error)
       || !expectPixel(c, 37, 20, 0x0000ff, (what + " right, index 1").c_str(), error))
        return false;
    }
    c.device->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
    c.device->SetTexture(0, nullptr);
    locked = {};
    locked.dwSize = sizeof(locked);
    if (FAILED(texture->Lock(nullptr, &locked, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) { error = "DXT1 Lock again"; return false; }
    const uint8_t* b = block(40, 20);
    const bool same = b[0] == 0x1f && b[1] == 0xf8 && b[2] == 0x1f && b[3] == 0 && b[4] == 0x44 && b[7] == 0x44;
    texture->Unlock(nullptr);
    if (!same) {
      error = format("locked again: block %02x%02x %02x%02x %02x", b[0], b[1], b[2], b[3], b[4]);
      return false;
    }
    return true;
  }

  // State blocks of each type hold the states Windows' Direct3D 7 puts in
  // them: pixel blocks the blend states and all stage states, vertex blocks
  // the lighting states, TEXCOORDINDEX, TEXTURETRANSFORMFLAGS and whether
  // lights are enabled (not the lights), ALL blocks the lights that exist
  // (captured again by CaptureStateBlock; lights made later stay out).
  // Unset lights do not exist; enabled ones read 128.
  bool stateBlockTypes(Context& c, std::string& error) {
    IDirect3DDevice7* d = c.device.get();
    struct Probe { D3DRENDERSTATETYPE state; DWORD captured, changed; bool pixel, vertex; };
    const Probe probes[] = {
      { D3DRENDERSTATE_ALPHABLENDENABLE, 1, 0, true, false }, { D3DRENDERSTATE_ZBIAS, 1, 0, true, false },
      { D3DRENDERSTATE_LIGHTING, 1, 0, false, true }, { D3DRENDERSTATE_CULLMODE, D3DCULL_CW, D3DCULL_CCW, false, true },
      { D3DRENDERSTATE_FOGSTART, 1, 0, true, true }, { D3DRENDERSTATE_COLORKEYENABLE, 1, 0, true, false },
    };
    for (D3DSTATEBLOCKTYPE type : { D3DSBT_PIXELSTATE, D3DSBT_VERTEXSTATE, D3DSBT_ALL }) {
      for (const Probe& p : probes) {
        DWORD old = 0, value = 0, block = 0;
        d->GetRenderState(p.state, &old);
        d->SetRenderState(p.state, p.captured);
        if (FAILED(d->CreateStateBlock(type, &block))) { error = "CreateStateBlock"; return false; }
        d->SetRenderState(p.state, p.changed);
        d->ApplyStateBlock(block);
        d->DeleteStateBlock(block);
        d->GetRenderState(p.state, &value);
        d->SetRenderState(p.state, old);
        const bool expected = type == D3DSBT_ALL || (type == D3DSBT_PIXELSTATE ? p.pixel : p.vertex);
        if ((value == p.captured) != expected) {
          error = format("type %d: render state %d %s", int(type), int(p.state), expected ? "not captured" : "captured");
          return false;
        }
      }
      for (D3DTEXTURESTAGESTATETYPE t : { D3DTSS_COLOROP, D3DTSS_TEXCOORDINDEX }) {
        DWORD value = 0, block = 0;
        d->SetTextureStageState(1, t, 1);
        d->CreateStateBlock(type, &block);
        d->SetTextureStageState(1, t, 2);
        d->ApplyStateBlock(block);
        d->DeleteStateBlock(block);
        d->GetTextureStageState(1, t, &value);
        const bool expected = type != D3DSBT_VERTEXSTATE || t == D3DTSS_TEXCOORDINDEX;
        if ((value == 1) != expected) {
          error = format("type %d: stage state %d %s", int(type), int(t), expected ? "not captured" : "captured");
          return false;
        }
      }
    }
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    D3DLIGHT7 light = {}, got = {};
    BOOL enabled = 5;
    if (d->GetLight(6, &got) != DDERR_INVALIDPARAMS || d->GetLightEnable(6, &enabled) != DDERR_INVALIDPARAMS) {
      error = "an unset light exists";
      return false;
    }
    light.dltType = D3DLIGHT_DIRECTIONAL;
    light.dvDirection.z = 1.0f;
    for (D3DSTATEBLOCKTYPE type : { D3DSBT_ALL, D3DSBT_VERTEXSTATE, D3DSBT_PIXELSTATE }) {
      light.dcvDiffuse.r = 0.25f;
      d->SetLight(4, &light);
      d->LightEnable(4, FALSE);
      DWORD block = 0;
      d->CreateStateBlock(type, &block);
      light.dcvDiffuse.r = 0.5f;
      d->SetLight(4, &light);
      d->LightEnable(4, TRUE);
      d->SetLight(10 + type, &light);   // made after the block
      d->CaptureStateBlock(block);
      light.dcvDiffuse.r = 0.75f;
      d->SetLight(4, &light);
      d->LightEnable(4, FALSE);
      d->SetLight(10 + type, &light);
      d->ApplyStateBlock(block);
      d->DeleteStateBlock(block);
      D3DLIGHT7 later = {};
      d->GetLight(4, &got);
      d->GetLight(10 + type, &later);
      d->GetLightEnable(4, &enabled);
      const float expectedLight = type == D3DSBT_ALL ? 0.5f : 0.75f;
      const BOOL expectedEnable = type == D3DSBT_PIXELSTATE ? FALSE : 128;
      if (got.dcvDiffuse.r != expectedLight || enabled != expectedEnable || later.dcvDiffuse.r != 0.75f) {
        error = format("type %d: light %.2f enabled %d, light made later %.2f", int(type), got.dcvDiffuse.r, enabled,
          later.dcvDiffuse.r);
        return false;
      }
    }
    d->LightEnable(4, FALSE);
    return true;
  }

  // Direct3D 7 keeps the viewport when the render target changes.
  bool renderTargetViewport(Context& c, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = desc.dwHeight = 32;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0);
    Com<IDirectDrawSurface7> other;
    if (FAILED(c.dd->CreateSurface(&desc, other.put(), nullptr))) { error = "second target"; return false; }
    D3DVIEWPORT7 narrow = { 2, 3, 20, 10, 0.0f, 1.0f }, vp = {};
    c.device->SetViewport(&narrow);
    HRESULT hr = c.device->SetRenderTarget(other.get(), 0);
    c.device->GetViewport(&vp);
    c.device->SetRenderTarget(c.target.get(), 0);
    D3DVIEWPORT7 full = { 0, 0, Context::size, Context::size, 0.0f, 1.0f };
    c.device->SetViewport(&full);
    if (FAILED(hr) || vp.dwX != 2 || vp.dwY != 3 || vp.dwWidth != 20 || vp.dwHeight != 10) {
      error = format("SetRenderTarget 0x%08lx, viewport %lu %lu %lu %lu", hr, vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight);
      return false;
    }
    return true;
  }

  // DXT1 mip levels: descriptions and locks report each level's linear
  // size (not a pitch), with the flags of the description.
  bool dxtMipLevels(Context& c, std::string& error) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT;
    desc.dwWidth = desc.dwHeight = 16;
    desc.dwMipMapCount = 3;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
    desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
    desc.ddpfPixelFormat.dwFourCC = MAKEFOURCC('D', 'X', 'T', '1');
    Com<IDirectDrawSurface7> texture;
    if (FAILED(c.dd->CreateSurface(&desc, texture.put(), nullptr))) { error = "DXT1 mipmap"; return false; }
    IDirectDrawSurface7* level = texture.get();
    level->AddRef();
    for (DWORD i = 0, size = 128; level; i++, size /= 4) {
      DDSURFACEDESC2 got = {}, locked = {};
      got.dwSize = locked.dwSize = sizeof(DDSURFACEDESC2);
      level->GetSurfaceDesc(&got);
      const HRESULT hr = level->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr);
      if (SUCCEEDED(hr)) level->Unlock(nullptr);
      DDSCAPS2 caps = {};
      caps.dwCaps = DDSCAPS_MIPMAP;
      IDirectDrawSurface7* next = nullptr;
      level->GetAttachedSurface(&caps, &next);
      level->Release();
      level = next;
      if (FAILED(hr) || got.dwLinearSize != size || !(got.dwFlags & DDSD_LINEARSIZE) || (got.dwFlags & DDSD_PITCH)
       || locked.dwLinearSize != size || (locked.dwFlags & DDSD_PITCH) || !locked.lpSurface) {
        error = format("level %lu: description flags 0x%lx size %lu, lock 0x%08lx flags 0x%lx size %lu", i, got.dwFlags,
          got.dwLinearSize, hr, locked.dwFlags, locked.dwLinearSize);
        if (level) level->Release();
        return false;
      }
    }
    return true;
  }

  // GetDC of surfaces with alpha: what GDI draws gets alpha 0 (the DC
  // draws into the surface), untouched texels keep theirs.
  bool getDcAlpha(Context& c, std::string& error) {
    struct Case { DWORD bits, r, g, b, a, texel, drawn; };
    const Case cases[] = { { 32, 0xff0000, 0xff00, 0xff, 0xff000000u, 0x5500ff00u, 0x00ff0000u },
                           { 16, 0x7c00, 0x3e0, 0x1f, 0x8000, 0x83e0, 0x7c00 } };
    for (const Case& k : cases) {
      DDSURFACEDESC2 desc = {};
      desc.dwSize = sizeof(desc);
      desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
      desc.dwWidth = desc.dwHeight = 8;
      desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
      desc.ddpfPixelFormat = rgbFormat(k.bits, k.r, k.g, k.b, k.a);
      Com<IDirectDrawSurface7> surface;
      if (FAILED(c.dd->CreateSurface(&desc, surface.put(), nullptr))) { error = "surface"; return false; }
      const DWORD bytes = k.bits / 8;
      DDSURFACEDESC2 locked = {};
      locked.dwSize = sizeof(locked);
      if (FAILED(surface->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr))) { error = "Lock"; return false; }
      for (DWORD y = 0; y < 8; y++)
        for (DWORD x = 0; x < 8; x++)
          std::memcpy(static_cast<BYTE*>(locked.lpSurface) + y * locked.lPitch + x * bytes, &k.texel, bytes);
      surface->Unlock(nullptr);
      HDC dc = nullptr;
      if (FAILED(surface->GetDC(&dc))) { error = "GetDC"; return false; }
      const RECT r = { 0, 0, 4, 4 };
      HBRUSH brush = CreateSolidBrush(RGB(255, 0, 0));
      FillRect(dc, &r, brush);
      DeleteObject(brush);
      surface->ReleaseDC(dc);
      locked = {};
      locked.dwSize = sizeof(locked);
      if (FAILED(surface->Lock(nullptr, &locked, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) { error = "Lock again"; return false; }
      DWORD drawn = 0, kept = 0;
      std::memcpy(&drawn, static_cast<BYTE*>(locked.lpSurface) + locked.lPitch + bytes, bytes);
      std::memcpy(&kept, static_cast<BYTE*>(locked.lpSurface) + 6 * locked.lPitch + 6 * bytes, bytes);
      surface->Unlock(nullptr);
      if (drawn != k.drawn || kept != k.texel) {
        error = format("%lu bits: drawn 0x%08lx, untouched 0x%08lx", k.bits, drawn, kept);
        return false;
      }
    }
    return true;
  }

  bool runningOnDxup() {
    HMODULE module = GetModuleHandleA("ddraw.dll");
    char path[MAX_PATH] = {}, exe[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) *slash = 0;
    if (char* slash = strrchr(exe, '\\')) *slash = 0;
    return _stricmp(path, exe) == 0;
  }

  // CPU cost of a Gothic-like frame: many small draws from application
  // memory with a texture and a state change between them. Reports
  // milliseconds per frame (the last frame waits for the GPU).
  int runBench(Context& c, int frames, int draws, bool transformAndLight) {
    std::vector<Com<IDirectDrawSurface7>> textures(8);
    for (auto& texture : textures) {
      DDSURFACEDESC2 desc = {};
      desc.dwSize = sizeof(desc);
      desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
      desc.dwWidth = desc.dwHeight = 64;
      desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
      desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
      desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0xff000000);
      if (FAILED(c.dd->CreateSurface(&desc, texture.put(), nullptr))) {
        std::printf("texture creation failed\n");
        return 1;
      }
    }
    c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    c.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    TLVertex quadVertices[4];
    struct LitVertex { float x, y, z, nx, ny, nz, u, v; };
    LitVertex litQuad[4] = { { -1, -1, 0, 0, 0, -1, 0, 1 }, { -1, 1, 0, 0, 0, -1, 0, 0 },
                             { 1, -1, 0, 0, 0, -1, 1, 1 }, { 1, 1, 0, 0, 0, -1, 1, 0 } };
    if (transformAndLight) {
      D3DMATRIX identity = {};
      identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
      c.device->SetTransform(D3DTRANSFORMSTATE_VIEW, &identity);
      c.device->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &identity);
      D3DMATERIAL7 material = {};
      material.diffuse.r = material.diffuse.g = material.diffuse.b = material.diffuse.a = 1.0f;
      c.device->SetMaterial(&material);
      D3DLIGHT7 light = {};
      light.dltType = D3DLIGHT_DIRECTIONAL;
      light.dcvDiffuse.r = light.dcvDiffuse.g = light.dcvDiffuse.b = 1.0f;
      light.dvDirection.z = 1.0f;
      c.device->SetLight(0, &light);
      c.device->LightEnable(0, TRUE);
      c.device->SetRenderState(D3DRENDERSTATE_LIGHTING, TRUE);
    }
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    double best = 1e9, total = 0.0;
    for (int frame = 0; frame < frames; frame++) {
      QueryPerformanceCounter(&start);
      c.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
      c.device->BeginScene();
      for (int i = 0; i < draws; i++) {
        c.device->SetTexture(0, textures[i % 8].get());
        c.device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, (i & 1) ? TRUE : FALSE);
        if (transformAndLight) {
          // Objects: a world matrix each, lit.
          D3DMATRIX world = {};
          world._11 = world._22 = world._33 = 0.05f;
          world._44 = 1.0f;
          world._41 = float(i % 40) / 20.0f - 1.0f;
          world._42 = float((i / 40) % 40) / 20.0f - 1.0f;
          world._43 = 0.5f;
          c.device->SetTransform(D3DTRANSFORMSTATE_WORLD, &world);
          c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1, litQuad, 4, 0);
        } else {
          const float x = float(i % 60), y = float((i / 60) % 60);
          quad(quadVertices, x, y, x + 4.0f, y + 4.0f, 0xff808080u + DWORD(i & 0x7f), 0.5f);
          c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, quadVertices, 4, 0);
        }
      }
      c.device->EndScene();
      if (frame == frames - 1) {
        DDSURFACEDESC2 locked = {};
        locked.dwSize = sizeof(locked);
        if (SUCCEEDED(c.target->Lock(nullptr, &locked, DDLOCK_WAIT | DDLOCK_READONLY, nullptr)))
          c.target->Unlock(nullptr);
      }
      QueryPerformanceCounter(&end);
      const double ms = double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
      if (frame >= 3 && frame < frames - 1) {
        best = ms < best ? ms : best;
        total += ms;
      }
    }
    std::printf("bench%s: %d draws per frame, %.3f ms per frame avg, %.3f best (%.0f ns per draw)\n",
      transformAndLight ? " (transform and light)" : "", draws, total / (frames - 4), best, best * 1e6 / draws);
    return 0;
  }

}

// DirectDraw 1 to 6 objects (Windows' ddraw.dll draws them under DXUP).
static bool directDrawV1(Context&, std::string& error) {
  IDirectDraw* dd = nullptr;
  HRESULT hr = DirectDrawCreate(nullptr, &dd, nullptr);
  if (FAILED(hr) || !dd) { error = "DirectDrawCreate failed"; return false; }
  IDirectDraw2* dd2 = nullptr;
  hr = dd->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&dd2));
  DDCAPS caps = { sizeof(caps) };
  if (SUCCEEDED(hr)) hr = dd2->GetCaps(&caps, nullptr);
  if (dd2) dd2->Release();
  dd->Release();
  if (FAILED(hr)) { error = "IDirectDraw2 0x" + std::to_string(hr); return false; }
  return true;
}

// A surface of Windows' DirectDraw blitted into ours, as DirectShow's
// video streams hand them to games (16-bit, so the pixels are converted).
static bool foreignSurfaceBlt(Context& c, std::string& error) {
  // Windows refuses blits between its DirectDraw objects (E_NOTIMPL).
  if (!runningOnDxup())
    return true;
  IDirectDraw* dd = nullptr;
  if (FAILED(DirectDrawCreate(nullptr, &dd, nullptr)) || !dd) { error = "DirectDrawCreate failed"; return false; }
  Com<IDirectDraw7> dd7;
  HRESULT hr = dd->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(dd7.put()));
  dd->Release();
  if (FAILED(hr)) { error = "IDirectDraw7 of Windows' DirectDraw"; return false; }
  dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
  DDSURFACEDESC2 desc = {};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  desc.dwWidth = desc.dwHeight = 16;
  desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
  desc.ddpfPixelFormat = rgbFormat(16, 0xf800, 0x07e0, 0x001f, 0);
  Com<IDirectDrawSurface7> source;
  if (FAILED(hr = dd7->CreateSurface(&desc, source.put(), nullptr))) { error = format("foreign surface 0x%08lx", hr); return false; }
  DDSURFACEDESC2 locked = {};
  locked.dwSize = sizeof(locked);
  if (FAILED(source->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr))) { error = "foreign Lock"; return false; }
  for (DWORD y = 0; y < 16; y++)
    for (DWORD x = 0; x < 16; x++)
      reinterpret_cast<WORD*>(static_cast<uint8_t*>(locked.lpSurface) + y * locked.lPitch)[x] = 0x07ff;  // cyan
  source->Unlock(nullptr);
  c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
  RECT dst = { 24, 24, 56, 56 };
  if (FAILED(hr = c.target->Blt(&dst, source.get(), nullptr, DDBLT_WAIT, nullptr))) { error = format("Blt 0x%08lx", hr); return false; }
  return expectPixel(c, 40, 40, 0x00ffff, "Blt from a foreign surface", error)
      && expectPixel(c, 10, 10, 0x000000, "Blt leaves the rest", error);
}

// The game's DirectDraw under its DirectDraw 1 interface, as DirectShow's
// multimedia streams use it for their video samples.
static bool directDraw1Interface(Context& c, std::string& error) {
  Com<IDirectDraw> dd1;
  if (FAILED(c.dd->QueryInterface(IID_IDirectDraw, reinterpret_cast<void**>(dd1.put())))) { error = "no IDirectDraw"; return false; }
  DDSURFACEDESC desc = {};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  desc.dwWidth = desc.dwHeight = 16;
  desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
  desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0);
  Com<IDirectDrawSurface> surface;
  HRESULT hr = dd1->CreateSurface(&desc, surface.put(), nullptr);
  if (FAILED(hr)) { error = format("IDirectDraw::CreateSurface 0x%08lx", hr); return false; }
  DDSURFACEDESC locked = {};
  locked.dwSize = sizeof(locked);
  if (FAILED(surface->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr))) { error = "IDirectDrawSurface::Lock"; return false; }
  if (locked.dwWidth != 16 || !locked.lpSurface) { error = "Lock description"; surface->Unlock(nullptr); return false; }
  for (DWORD y = 0; y < 16; y++)
    for (DWORD x = 0; x < 16; x++)
      reinterpret_cast<DWORD*>(static_cast<uint8_t*>(locked.lpSurface) + y * locked.lPitch)[x] = 0x00ffff00;
  surface->Unlock(locked.lpSurface);
  Com<IDirectDrawSurface7> surface7;
  if (FAILED(surface->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(surface7.put())))) { error = "IDirectDrawSurface7"; return false; }
  c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
  RECT dst = { 8, 8, 24, 24 };
  if (FAILED(c.target->Blt(&dst, surface7.get(), nullptr, DDBLT_WAIT, nullptr))) { error = "Blt"; return false; }
  if (!expectPixel(c, 16, 16, 0xffff00, "Blt of a DirectDraw 1 surface", error))
    return false;
  // Games pass the DirectDraw 1 interface where DirectDraw 7 takes a surface
  // (DirectShow's samples hand out only that one); Windows accepts it.
  c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
  if (FAILED(c.target->Blt(&dst, reinterpret_cast<IDirectDrawSurface7*>(surface.get()), nullptr, DDBLT_WAIT, nullptr))) {
    error = "Blt of the DirectDraw 1 interface";
    return false;
  }
  return expectPixel(c, 16, 16, 0xffff00, "Blt of the DirectDraw 1 interface", error);
}

// dxup.conf d3d11.msaaForce in a windowed DirectDraw game: after the primary
// surface, render targets of the window's size get forced samples (DXUP's
// swap chain comes only at the first Blt to the primary). A white triangle
// on black: partly covered edge pixels with forced multisampling, none
// without. Returns how many, or -1.
static int forcedMsaaEdgePixels() {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "Dx7MsaaTest";
  RegisterClassA(&wc);
  RECT r = { 0, 0, 160, 120 };
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExA(0, "Dx7MsaaTest", "dx7 msaa", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
  // The game's buffers: the window's client area (Windows widens small windows).
  RECT client = {};
  GetClientRect(window, &client);
  const DWORD width = DWORD(client.right), height = DWORD(client.bottom);
  Com<IDirectDraw7> dd;
  Com<IDirect3D7> d3d;
  Com<IDirectDrawSurface7> primary, target, depth;
  Com<IDirect3DDevice7> device;
  if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(dd.put()), IID_IDirectDraw7, nullptr))
   || FAILED(dd->SetCooperativeLevel(window, DDSCL_NORMAL))
   || FAILED(dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(d3d.put()))))
    return -1;
  DDSURFACEDESC2 pd = {};
  pd.dwSize = sizeof(pd);
  pd.dwFlags = DDSD_CAPS;
  pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
  DDSURFACEDESC2 desc = {};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  desc.dwWidth = width;
  desc.dwHeight = height;
  desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
  desc.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0);
  DDSURFACEDESC2 zd = {};
  zd.dwSize = sizeof(zd);
  zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  zd.dwWidth = width;
  zd.dwHeight = height;
  zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
  zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
  zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
  zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
  zd.ddpfPixelFormat.dwZBitMask = 0xffff;
  if (FAILED(dd->CreateSurface(&pd, primary.put(), nullptr)) || FAILED(dd->CreateSurface(&desc, target.put(), nullptr))
   || FAILED(dd->CreateSurface(&zd, depth.put(), nullptr)) || FAILED(target->AddAttachedSurface(depth.get())))
    return -1;
  if (FAILED(d3d->CreateDevice(IID_IDirect3DTnLHalDevice, target.get(), device.put()))
   && FAILED(d3d->CreateDevice(IID_IDirect3DHALDevice, target.get(), device.put())))
    return -1;
  D3DVIEWPORT7 vp = { 0, 0, width, height, 0.0f, 1.0f };
  device->SetViewport(&vp);
  struct V { float x, y, z, rhw; DWORD color; };
  V triangle[3] = {
    { 0.0f, 0.0f, 0.5f, 1.0f, 0xffffffffu }, { float(width), 0.0f, 0.5f, 1.0f, 0xffffffffu },
    { 0.0f, float(height), 0.5f, 1.0f, 0xffffffffu },
  };
  device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
  device->BeginScene();
  device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
  device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
  device->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, triangle, 3, 0);
  device->EndScene();
  DDSURFACEDESC2 locked = {};
  locked.dwSize = sizeof(locked);
  if (FAILED(target->Lock(nullptr, &locked, DDLOCK_WAIT | DDLOCK_READONLY, nullptr)))
    return -1;
  int partial = 0;
  for (DWORD y = 0; y < height; y++)
    for (DWORD x = 0; x < width; x++) {
      const DWORD red = (reinterpret_cast<const DWORD*>(static_cast<const uint8_t*>(locked.lpSurface) + y * locked.lPitch)[x] >> 16) & 255;
      partial += red > 16 && red < 240;
    }
  target->Unlock(nullptr);
  DestroyWindow(window);
  return partial;
}

// A P8 texture takes palette entries set after SetPalette for the next draw
// (DXUP only: Windows' Direct3D 7 draws P8 textures black on today's drivers).
static bool paletteEntries(Context& c, std::string& error) {
  if (!runningOnDxup())
    return true;
  DDSURFACEDESC2 desc = {};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  desc.dwWidth = desc.dwHeight = 4;
  desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
  desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
  desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
  desc.ddpfPixelFormat.dwRGBBitCount = 8;
  Com<IDirectDrawSurface7> texture;
  if (FAILED(c.dd->CreateSurface(&desc, texture.put(), nullptr))) { error = "P8 texture"; return false; }
  PALETTEENTRY entries[256] = {};
  entries[3] = { 255, 0, 0, 0 };
  Com<IDirectDrawPalette> palette;
  if (FAILED(c.dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, palette.put(), nullptr))
   || FAILED(texture->SetPalette(palette.get()))) { error = "palette"; return false; }
  DDSURFACEDESC2 locked = {};
  locked.dwSize = sizeof(locked);
  if (FAILED(texture->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr))) { error = "Lock"; return false; }
  for (DWORD y = 0; y < 4; y++)
    std::memset(static_cast<BYTE*>(locked.lpSurface) + y * locked.lPitch, 3, 4);
  texture->Unlock(nullptr);
  c.device->SetTexture(0, texture.get());
  c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  c.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  TLVertex v[4];
  quad(v, 0.0f, 0.0f, 64.0f, 64.0f, 0xffffffff);
  bool ok = true;
  for (int pass = 0; pass < 2 && ok; pass++) {
    if (pass == 1) {
      const PALETTEENTRY blue = { 0, 0, 255, 0 };
      palette->SetEntries(0, 3, 1, const_cast<PALETTEENTRY*>(&blue));
    }
    c.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    c.device->BeginScene();
    c.device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTLFvf, v, 4, 0);
    c.device->EndScene();
    ok = expectPixel(c, 32, 32, pass ? 0x0000ff : 0xff0000, pass ? "entry set after SetPalette" : "palette", error);
  }
  c.device->SetTexture(0, nullptr);
  return ok;
}

// A windowed primary is the screen: blits land at their screen rectangle in
// the window - from system memory and P8 surfaces too (with their palette)
// - and several small blits do not each wait for the display. On DXUP only
// (Windows draws on the desktop); the window shows for a moment, away from
// overlays' corner. The capture may come darker (HDR desktops): hues only.
static bool windowedPrimary(Context&, std::string& error) {
  if (!runningOnDxup())
    return true;
  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "Dx7PrimaryTest";
  RegisterClassA(&wc);
  RECT frame = { 0, 0, 256, 192 };
  AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExA(0, "Dx7PrimaryTest", "dx7 primary", WS_OVERLAPPEDWINDOW, 0, 0, frame.right - frame.left,
                                frame.bottom - frame.top, nullptr, nullptr, wc.hInstance, nullptr);
  // Without the opening animation (it would fade the capture).
  if (HMODULE dwm = LoadLibraryA("dwmapi.dll")) {
    using SetAttribute = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto set = reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
      const BOOL disabled = TRUE;
      set(window, 3 /* DWMWA_TRANSITIONS_FORCEDISABLED */, &disabled, sizeof(disabled));
    }
  }
  ShowWindow(window, SW_SHOWNOACTIVATE);
  RECT client = {};
  GetClientRect(window, &client);
  POINT origin = { 0, 0 };
  ClientToScreen(window, &origin);
  auto screen = [&](LONG x0, LONG y0, LONG x1, LONG y1) { return RECT{ origin.x + x0, origin.y + y0, origin.x + x1, origin.y + y1 }; };
  // Each channel of the expected colour lit (0xff) or dark (0).
  auto hue = [](DWORD pixel, DWORD expected) {
    for (int shift = 0; shift < 24; shift += 8) {
      const DWORD value = (pixel >> shift) & 255;
      if (((expected >> shift) & 255) ? value < 0x30 : value > 0x18)
        return false;
    }
    return true;
  };
  bool ok = false;
  {
    Com<IDirectDraw7> dd;
    Com<IDirectDrawSurface7> primary, source, indexed;
    Com<IDirectDrawClipper> clipper;
    Com<IDirectDrawPalette> palette;
    DDSURFACEDESC2 pd = {};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    DDSURFACEDESC2 sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.dwWidth = sd.dwHeight = 16;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    sd.ddpfPixelFormat = rgbFormat(32, 0xff0000, 0xff00, 0xff, 0);
    DDSURFACEDESC2 id = sd;
    id.ddpfPixelFormat = {};
    id.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    id.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    id.ddpfPixelFormat.dwRGBBitCount = 8;
    PALETTEENTRY entries[256] = {};
    entries[7] = { 0, 255, 0, 0 };
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(dd.put()), IID_IDirectDraw7, nullptr))
     || FAILED(dd->SetCooperativeLevel(window, DDSCL_NORMAL)) || FAILED(dd->CreateSurface(&pd, primary.put(), nullptr))
     || FAILED(dd->CreateClipper(0, clipper.put(), nullptr)) || FAILED(clipper->SetHWnd(0, window))
     || FAILED(primary->SetClipper(clipper.get())) || FAILED(dd->CreateSurface(&sd, source.put(), nullptr))
     || FAILED(dd->CreateSurface(&id, indexed.put(), nullptr))
     || FAILED(dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, palette.put(), nullptr))
     || FAILED(indexed->SetPalette(palette.get()))) {
      error = "setup";
    } else {
      DDBLTFX fx = {};
      fx.dwSize = sizeof(fx);
      fx.dwFillColor = 0x00ff00ff;
      source->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
      fx.dwFillColor = 7;
      indexed->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
      fx.dwFillColor = 0;
      RECT whole = screen(0, 0, client.right, client.bottom);
      RECT a = screen(150, 120, 166, 136), b = screen(190, 120, 206, 136);
      HRESULT hr = primary->Blt(&whole, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
      if (SUCCEEDED(hr)) hr = primary->Blt(&a, source.get(), nullptr, DDBLT_WAIT, nullptr);
      if (SUCCEEDED(hr)) hr = primary->Blt(&b, indexed.get(), nullptr, DDBLT_WAIT, nullptr);
      // Entries set after the first blit: the next one shows them.
      const PALETTEENTRY blue = { 0, 0, 255, 0 };
      palette->SetEntries(0, 7, 1, const_cast<PALETTEENTRY*>(&blue));
      if (SUCCEEDED(hr)) hr = primary->Blt(&b, indexed.get(), nullptr, DDBLT_WAIT, nullptr);
      LARGE_INTEGER frequency, start, end;
      QueryPerformanceFrequency(&frequency);
      QueryPerformanceCounter(&start);
      const RECT texel = { 0, 0, 1, 1 };
      for (int i = 0; i < 30 && SUCCEEDED(hr); i++) {
        RECT dot = screen(150 + i, 160, 151 + i, 161);
        hr = primary->Blt(&dot, source.get(), const_cast<RECT*>(&texel), DDBLT_WAIT, nullptr);
      }
      QueryPerformanceCounter(&end);
      const double elapsed = double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
      double period = 1000.0 / 60.0;
      DEVMODEA mode = {};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        period = 1000.0 / mode.dmDisplayFrequency;
      // The last blits show by the window's timer.
      for (int i = 0; i < 20; i++) {
        MSG message;
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
          DispatchMessageA(&message);
        Sleep(10);
      }
      HDC windowDc = GetDC(window);
      HDC memory = CreateCompatibleDC(windowDc);
      BITMAPINFO info = {};
      info.bmiHeader.biSize = sizeof(info.bmiHeader);
      info.bmiHeader.biWidth = client.right;
      info.bmiHeader.biHeight = -client.bottom;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      void* bits = nullptr;
      HBITMAP bitmap = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      HGDIOBJ old = SelectObject(memory, bitmap);
      PrintWindow(window, memory, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
      GdiFlush();
      const auto at = [&](int x, int y) { return static_cast<const DWORD*>(bits)[y * client.right + x] & 0xffffff; };
      const DWORD pixels[5] = { at(158, 128), at(198, 128), at(120, 100), at(151, 160), at(178, 160) };
      SelectObject(memory, old);
      DeleteObject(bitmap);
      DeleteDC(memory);
      ReleaseDC(window, windowDc);
      ok = SUCCEEDED(hr) && hue(pixels[0], 0xff00ff) && hue(pixels[1], 0x0000ff) && hue(pixels[2], 0) && hue(pixels[3], 0xff00ff)
        && hue(pixels[4], 0xff00ff) && elapsed < 10.0 * period;
      if (!ok)
        error = format("hr 0x%08lx, pixels 0x%06lx 0x%06lx 0x%06lx, dots 0x%06lx 0x%06lx, 30 blits took %.1f ms (refresh %.1f ms)",
          hr, pixels[0], pixels[1], pixels[2], pixels[3], pixels[4], elapsed, period);
    }
  }
  DestroyWindow(window);
  return ok;
}

// Runs this program with the argument; its exit code (-1: not started).
static int runSelf(const char* argument) {
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string command = std::string("\"") + exe + "\" " + argument;
  STARTUPINFOA si = { sizeof(si) };
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, 60000);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

// On DXUP: forced multisampling from dxup.conf next to the DLLs, in a child.
static bool forcedMsaa(Context&, std::string& error) {
  if (!runningOnDxup())
    return true;
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string conf = exe;
  conf.resize(conf.find_last_of('\\') + 1);
  conf += "dxup.conf";
  FILE* file = nullptr;
  if (fopen_s(&file, conf.c_str(), "w") || !file) { error = "cannot write " + conf; return false; }
  std::fputs("d3d11.msaaSamples = 4\nd3d11.msaaForce = true\n", file);
  std::fclose(file);
  const int with = runSelf("--forced-msaa-child");
  DeleteFileA(conf.c_str());
  const int without = runSelf("--forced-msaa-child");
  // The triangle's edge crosses about 200 pixels.
  if (with < 40 || with >= 255 || without != 0) {
    error = format("edge pixels partly covered: %d forced, %d without", with, without);
    return false;
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--forced-msaa-child") == 0) {
    const int partial = forcedMsaaEdgePixels();
    return partial < 0 ? 255 : (std::min)(partial, 254);
  }
  Context context;
  std::string error;
  std::printf("DirectDraw 7 tests on %s\n", runningOnDxup() ? "DXUP" : "Windows");
  if (!setup(context, error)) {
    std::printf("[FAIL] setup - %s\n", error.c_str());
    return 1;
  }
  if (argc > 1 && (std::strcmp(argv[1], "--bench") == 0 || std::strcmp(argv[1], "--bench-tl") == 0))
    return runBench(context, argc > 2 ? std::atoi(argv[2]) : 50, argc > 3 ? std::atoi(argv[3]) : 3000,
      std::strcmp(argv[1], "--bench-tl") == 0);
  const std::pair<const char*, std::function<bool(Context&, std::string&)>> tests[] = {
    { "clear", clearAndRead },
    { "transformed_quad", transformedQuad },
    { "textured_quad", texturedQuad },
    { "late_color_key", lateColorKey },
    { "alpha_blend", alphaBlend },
    { "depth_test", depthTest },
    { "transform_and_light", transformAndLight },
    { "vertex_buffer_fan", vertexBufferFan },
    { "process_vertices", processVertices },
    { "fog_table", fogTable },
    { "blt_and_fill", bltAndFill },
    { "dxt_texture", dxtTexture },
    { "dxt_color_key", dxtColorKey },
    { "directdraw_v1", directDrawV1 },
    { "foreign_surface_blt", foreignSurfaceBlt },
    { "directdraw1_interface", directDraw1Interface },
    { "forced_msaa", forcedMsaa },
    { "state_block_types", stateBlockTypes },
    { "render_target_viewport", renderTargetViewport },
    { "dxt_mip_levels", dxtMipLevels },
    { "getdc_alpha", getDcAlpha },
    { "palette_entries", paletteEntries },
    { "windowed_primary", windowedPrimary },
  };
  int failed = 0;
  for (const auto& [name, test] : tests) {
    error.clear();
    const bool ok = test(context, error);
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : " - ", ok ? "" : error.c_str());
    failed += ok ? 0 : 1;
  }
  std::printf("Summary: %d failed, %zu total\n", failed, sizeof(tests) / sizeof(tests[0]));
  return failed ? 1 : 0;
}
