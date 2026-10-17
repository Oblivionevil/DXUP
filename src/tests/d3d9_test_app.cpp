// Direct3D 9 scenes drawn by Windows' d3d9.dll and by DXUP's (dxup_d3d9.dll
// next to this program, with DXUP's d3d11.dll and dxgi.dll), compared pixel
// by pixel.
//
//   d3d9_test_app [--dump] [scene-name-filter]
//   d3d9_test_app --bench FRAMES DRAWS     (draw calls per second on DXUP and Windows)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <functional>
#include <string>
#include <vector>

namespace {

  bool g_trace = false;
#define TRACE(...) do { if (g_trace) { std::printf(__VA_ARGS__); std::fflush(stdout); } } while (0)

  constexpr UINT kWidth = 64;
  constexpr UINT kHeight = 48;

  using CreateFunction = IDirect3D9* (WINAPI*)(UINT);

  struct Runtime {
    const char* name;
    HMODULE module = nullptr;
    CreateFunction create = nullptr;
  };

  HWND createWindow() {
    static bool registered = false;
    if (!registered) {
      WNDCLASSW wc = {};
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"dxup_d3d9_test";
      RegisterClassW(&wc);
      registered = true;
    }
    RECT r = { 0, 0, LONG(kWidth), LONG(kHeight) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowW(L"dxup_d3d9_test", L"d3d9 test", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
                         nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  }

  std::vector<DWORD> compile(const char* source, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    std::vector<DWORD> tokens;
    if (SUCCEEDED(D3DCompile(source, std::strlen(source), "scene", nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
      tokens.resize(code->GetBufferSize() / 4);
      std::memcpy(tokens.data(), code->GetBufferPointer(), tokens.size() * 4);
    } else if (errors) {
      std::printf("compile error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return tokens;
  }

  std::vector<DWORD> assemble(const char* source) {
    using Assemble = HRESULT (WINAPI*)(const char*, UINT, const void*, void*, DWORD, ID3DBlob**, ID3DBlob**);
    static Assemble function = [] {
      HMODULE module = LoadLibraryW(L"d3dx9_43.dll");
      return module ? reinterpret_cast<Assemble>(GetProcAddress(module, "D3DXAssembleShader")) : nullptr;
    }();
    std::vector<DWORD> tokens;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (function && SUCCEEDED(function(source, UINT(std::strlen(source)), nullptr, nullptr, 0, &code, &errors))) {
      tokens.resize(code->GetBufferSize() / 4);
      std::memcpy(tokens.data(), code->GetBufferPointer(), tokens.size() * 4);
    } else if (errors) {
      std::printf("assemble error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return tokens;
  }

  // ---- Helpers for scenes ----

  struct TLVertex { float x, y, z, rhw; DWORD color; };
  struct TexVertex { float x, y, z; float u, v; };
  struct LitVertex { float x, y, z; float nx, ny, nz; };

  D3DMATRIX identity() {
    D3DMATRIX m = {};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
  }

  D3DMATRIX perspective(float fovY, float aspect, float zn, float zf) {
    D3DMATRIX m = {};
    const float y = 1.0f / std::tan(fovY * 0.5f), x = y / aspect;
    m._11 = x;
    m._22 = y;
    m._33 = zf / (zf - zn);
    m._34 = 1.0f;
    m._43 = -zn * zf / (zf - zn);
    return m;
  }

  D3DMATRIX translation(float x, float y, float z) {
    D3DMATRIX m = identity();
    m._41 = x; m._42 = y; m._43 = z;
    return m;
  }

  D3DMATRIX rotationY(float a) {
    D3DMATRIX m = identity();
    m._11 = std::cos(a); m._13 = -std::sin(a);
    m._31 = std::sin(a); m._33 = std::cos(a);
    return m;
  }

  IDirect3DTexture9* checkerTexture(IDirect3DDevice9* device, UINT size, D3DFORMAT format = D3DFMT_A8R8G8B8, UINT levels = 1) {
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(size, size, levels, 0, format, D3DPOOL_MANAGED, &texture, nullptr)))
      return nullptr;
    for (UINT level = 0; level < texture->GetLevelCount(); level++) {
      D3DLOCKED_RECT rect;
      texture->LockRect(level, &rect, nullptr, 0);
      const UINT s = (std::max)(1u, size >> level);
      for (UINT y = 0; y < s; y++)
        for (UINT x = 0; x < s; x++) {
          const DWORD c = ((x / 2 + y / 2) & 1) ? 0xffe03020u : 0x8020c0f0u;
          reinterpret_cast<DWORD*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch)[x] = (c & 0x00ffffffu) | ((level * 60 + 80) << 24);
        }
      texture->UnlockRect(level);
    }
    return texture;
  }

  void fullscreenTL(IDirect3DDevice9* device, DWORD color, float z = 0.5f) {
    const TLVertex quad[4] = {
      { -0.5f, -0.5f, z, 1.0f, color }, { kWidth - 0.5f, -0.5f, z, 1.0f, color },
      { -0.5f, kHeight - 0.5f, z, 1.0f, color }, { kWidth - 0.5f, kHeight - 0.5f, z, 1.0f, color },
    };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
  }

  // ---- Scenes ----

  struct Scene {
    const char* name;
    std::function<bool(IDirect3DDevice9*)> draw;
    int tolerance = 2;
  };

  bool sceneClear(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff336699u, 1.0f, 0);
    D3DRECT rects[2] = { { 4, 4, 20, 20 }, { 30, 10, 60, 40 } };
    d->Clear(2, rects, D3DCLEAR_TARGET, 0xffff8000u, 1.0f, 0);
    return true;
  }

  bool sceneTransformed(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    const TLVertex tri[3] = {
      { 8.0f, 4.0f, 0.5f, 1.0f, 0xffff0000u }, { 58.0f, 20.0f, 0.5f, 1.0f, 0xff00ff00u }, { 20.0f, 44.0f, 0.5f, 1.0f, 0xff0000ffu },
    };
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    return true;
  }

  bool sceneTextured(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff101010u, 1.0f, 0);
    IDirect3DTexture9* texture = checkerTexture(d, 16);
    if (!texture) return false;
    const TexVertex quad[4] = {
      { -1.0f, 1.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f, 1.0f, 0.0f },
      { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };
    D3DMATRIX world = rotationY(0.4f), view = translation(0.0f, 0.0f, 2.5f), proj = perspective(1.0f, 4.0f / 3.0f, 0.5f, 10.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TexVertex));
    d->SetTexture(0, nullptr);
    texture->Release();
    return true;
  }

  bool sceneLit(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000020u, 1.0f, 0);
    std::vector<LitVertex> vertices;
    const int rings = 12, segments = 16;
    for (int r = 0; r < rings; r++) {
      for (int s = 0; s < segments; s++) {
        auto point = [&](int ri, int si) {
          const float t = 3.14159265f * float(ri) / rings, p = 6.2831853f * float(si) / segments;
          const float x = std::sin(t) * std::cos(p), y = std::cos(t), z = std::sin(t) * std::sin(p);
          return LitVertex{ x, y, z, x, y, z };
        };
        const LitVertex a = point(r, s), b = point(r, s + 1), c = point(r + 1, s), e = point(r + 1, s + 1);
        vertices.insert(vertices.end(), { a, b, c, b, e, c });
      }
    }
    D3DMATRIX world = identity(), view = translation(0.0f, 0.0f, 3.0f), proj = perspective(1.0f, 4.0f / 3.0f, 0.5f, 10.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    D3DMATERIAL9 material = {};
    material.Diffuse = { 1.0f, 0.6f, 0.2f, 1.0f };
    material.Ambient = { 0.2f, 0.2f, 0.4f, 1.0f };
    material.Specular = { 1.0f, 1.0f, 1.0f, 1.0f };
    material.Power = 16.0f;
    d->SetMaterial(&material);
    D3DLIGHT9 light = {};
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
    light.Specular = { 0.5f, 0.5f, 0.5f, 1.0f };
    light.Direction = { -0.5f, -0.7f, 0.6f };
    d->SetLight(0, &light);
    d->LightEnable(0, TRUE);
    D3DLIGHT9 point = {};
    point.Type = D3DLIGHT_POINT;
    point.Diffuse = { 0.2f, 0.8f, 0.3f, 1.0f };
    point.Position = { 1.5f, 1.0f, 1.0f };
    point.Range = 10.0f;
    point.Attenuation0 = 1.0f;
    point.Attenuation1 = 0.2f;
    d->SetLight(1, &point);
    d->LightEnable(1, TRUE);
    d->SetRenderState(D3DRS_LIGHTING, TRUE);
    d->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
    d->SetRenderState(D3DRS_AMBIENT, 0xff202020u);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(vertices.size() / 3), vertices.data(), sizeof(LitVertex));
    return true;
  }

  const char* kVs2 = R"(
float4x4 worldViewProj : register(c0);
float4 tint : register(c4);
struct VO { float4 p : POSITION; float4 c : COLOR0; float2 t : TEXCOORD0; };
VO main(float3 p : POSITION, float4 c : COLOR0, float2 t : TEXCOORD0) {
  VO o; o.p = mul(float4(p, 1), worldViewProj); o.c = c * tint; o.t = t; return o;
})";

  const char* kPs2 = R"(
sampler2D tex : register(s0);
float4 k : register(c0);
float4 main(float4 c : COLOR0, float2 t : TEXCOORD0) : COLOR { return tex2D(tex, t) * c + k * t.x; }
)";

  bool sceneShaders(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    const auto vsCode = compile(kVs2, "vs_2_0");
    const auto psCode = compile(kPs2, "ps_2_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    // Two streams: positions, colors and coordinates; indexed.
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
      { 1, 4, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
      D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    d->CreateVertexDeclaration(elements, &decl);
    const float positions[] = { -0.9f, 0.8f, 0.5f, 0.9f, 0.9f, 0.5f, -0.8f, -0.9f, 0.5f, 0.85f, -0.7f, 0.5f };
    struct Attr { DWORD color; float u, v; };
    const Attr attrs[] = { { 0xffffffffu, 0.0f, 0.0f }, { 0xffff8080u, 1.0f, 0.0f }, { 0xff80ff80u, 0.0f, 1.0f },
                           { 0xff8080ffu, 1.0f, 1.0f } };
    const WORD indices[] = { 0, 1, 2, 1, 3, 2 };
    IDirect3DVertexBuffer9 *vb0 = nullptr, *vb1 = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    void* data;
    d->CreateVertexBuffer(sizeof(positions), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb0, nullptr);
    vb0->Lock(0, 0, &data, 0); std::memcpy(data, positions, sizeof(positions)); vb0->Unlock();
    d->CreateVertexBuffer(sizeof(attrs), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb1, nullptr);
    vb1->Lock(0, 0, &data, 0); std::memcpy(data, attrs, sizeof(attrs)); vb1->Unlock();
    d->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    ib->Lock(0, 0, &data, 0); std::memcpy(data, indices, sizeof(indices)); ib->Unlock();
    IDirect3DTexture9* texture = checkerTexture(d, 8);
    D3DMATRIX m = identity();
    const float tint[4] = { 1.0f, 0.9f, 0.8f, 1.0f };
    const float k[4] = { 0.1f, 0.0f, 0.2f, 0.0f };
    d->SetVertexShaderConstantF(0, &m._11, 4);
    d->SetVertexShaderConstantF(4, tint, 1);
    d->SetPixelShaderConstantF(0, k, 1);
    d->SetVertexDeclaration(decl);
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetStreamSource(0, vb0, 0, 12);
    d->SetStreamSource(1, vb1, 0, sizeof(Attr));
    d->SetIndices(ib);
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetTexture(0, nullptr);
    d->SetStreamSource(0, nullptr, 0, 0);
    d->SetStreamSource(1, nullptr, 0, 0);
    d->SetIndices(nullptr);
    texture->Release(); ib->Release(); vb1->Release(); vb0->Release(); decl->Release(); ps->Release(); vs->Release();
    return true;
  }

  bool sceneBlendAlphaTest(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff204060u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    const TLVertex tri[3] = {
      { 2.0f, 2.0f, 0.5f, 1.0f, 0x80ff0000u }, { 62.0f, 2.0f, 0.5f, 1.0f, 0x20ffff00u }, { 30.0f, 46.0f, 0.5f, 1.0f, 0xf000ff00u },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    d->SetRenderState(D3DRS_ALPHAREF, 0x60);
    d->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    const TLVertex tri2[3] = {
      { 2.0f, 46.0f, 0.4f, 1.0f, 0x00ffffffu }, { 62.0f, 46.0f, 0.4f, 1.0f, 0xff0000ffu }, { 62.0f, 10.0f, 0.4f, 1.0f, 0xffff00ffu },
    };
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri2, sizeof(TLVertex));
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    return true;
  }

  bool sceneDepthStencil(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    // A near triangle writes stencil 1; a far quad draws where stencil is 0 only.
    d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    d->SetRenderState(D3DRS_STENCILREF, 1);
    const TLVertex tri[3] = {
      { 4.0f, 4.0f, 0.2f, 1.0f, 0xffff0000u }, { 40.0f, 4.0f, 0.2f, 1.0f, 0xffff0000u }, { 4.0f, 40.0f, 0.2f, 1.0f, 0xffff0000u },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    d->SetRenderState(D3DRS_STENCILREF, 0);
    d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    fullscreenTL(d, 0xff00ff00u, 0.6f);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    // Depth: behind the triangle, nothing; in front, blue.
    fullscreenTL(d, 0xff0000ffu, 0.1f);
    return true;
  }

  bool sceneRenderToTexture(IDirect3DDevice9* d) {
    IDirect3DTexture9* target = nullptr;
    if (FAILED(d->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target, nullptr)))
      return false;
    IDirect3DSurface9 *surface = nullptr, *back = nullptr;
    target->GetSurfaceLevel(0, &surface);
    d->GetRenderTarget(0, &back);
    d->SetRenderTarget(0, surface);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff800000u, 1.0f, 0);
    const TLVertex tri[3] = {
      { 0.0f, 0.0f, 0.5f, 1.0f, 0xff00ff00u }, { 32.0f, 0.0f, 0.5f, 1.0f, 0xff0000ffu }, { 0.0f, 32.0f, 0.5f, 1.0f, 0xffffffffu },
    };
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    d->SetRenderTarget(0, back);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    // Sampled, and stretched.
    struct TLTex { float x, y, z, rhw; float u, v; };
    const TLTex quad[4] = {
      { -0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f }, { 31.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
      { -0.5f, 47.5f, 0.5f, 1.0f, 0.0f, 1.0f }, { 31.5f, 47.5f, 0.5f, 1.0f, 1.0f, 1.0f },
    };
    d->SetTexture(0, target);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLTex));
    d->SetTexture(0, nullptr);
    RECT dst = { 32, 0, 64, 32 };
    d->StretchRect(surface, nullptr, back, &dst, D3DTEXF_POINT);
    RECT fill = { 40, 36, 60, 46 };
    d->ColorFill(back, &fill, 0xffffff00u);
    surface->Release();
    back->Release();
    target->Release();
    return true;
  }

  bool sceneFog(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    D3DMATRIX world = rotationY(1.2f), view = translation(0.0f, 0.0f, 2.0f), proj = perspective(1.2f, 4.0f / 3.0f, 0.5f, 20.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_FOGENABLE, TRUE);
    d->SetRenderState(D3DRS_FOGCOLOR, 0xff8090a0u);
    d->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    const float start = 1.0f, end = 3.5f;
    d->SetRenderState(D3DRS_FOGSTART, *reinterpret_cast<const DWORD*>(&start));
    d->SetRenderState(D3DRS_FOGEND, *reinterpret_cast<const DWORD*>(&end));
    struct V { float x, y, z; DWORD c; };
    const V quad[4] = { { -2.0f, 1.0f, 0.0f, 0xffff0000u }, { 2.0f, 1.0f, 0.0f, 0xff00ff00u },
                        { -2.0f, -1.0f, 0.0f, 0xff0000ffu }, { 2.0f, -1.0f, 0.0f, 0xffffffffu } };
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    return true;
  }

  bool sceneDynamicBuffer(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    IDirect3DVertexBuffer9* vb = nullptr;
    if (FAILED(d->CreateVertexBuffer(sizeof(TLVertex) * 12, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                     D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &vb, nullptr)))
      return false;
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->SetStreamSource(0, vb, 0, sizeof(TLVertex));
    for (int frame = 0; frame < 8; frame++) {
      const UINT slot = UINT(frame % 4) * 3;
      TLVertex* v = nullptr;
      vb->Lock(slot * sizeof(TLVertex), 3 * sizeof(TLVertex), reinterpret_cast<void**>(&v),
               slot ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD);
      const float x = float(frame * 7);
      const DWORD c = 0xff000000u | (frame * 30u << 16) | ((255 - frame * 30u) << 8);
      v[0] = { x, 2.0f + frame * 4.0f, 0.5f, 1.0f, c };
      v[1] = { x + 20.0f, 4.0f + frame * 4.0f, 0.5f, 1.0f, c };
      v[2] = { x + 4.0f, 14.0f + frame * 4.0f, 0.5f, 1.0f, c | 0xff };
      vb->Unlock();
      d->DrawPrimitive(D3DPT_TRIANGLELIST, slot, 1);
    }
    d->SetStreamSource(0, nullptr, 0, 0);
    vb->Release();
    return true;
  }

  bool sceneInstancing(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    const char* vsSource = R"(
struct VO { float4 p : POSITION; float4 c : COLOR0; };
VO main(float2 p : POSITION, float4 offset : TEXCOORD1, float4 c : COLOR0) {
  VO o; o.p = float4(p * 0.2 + offset.xy, 0.5, 1); o.c = c; return o;
})";
    const char* psSource = "float4 main(float4 c : COLOR0) : COLOR { return c; }";
    const auto vsCode = compile(vsSource, "vs_3_0");
    const auto psCode = compile(psSource, "ps_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
      { 1, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
      D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    d->CreateVertexDeclaration(elements, &decl);
    const float shape[] = { -1, 1, 1, 1, -1, -1, 1, -1 };
    struct Instance { float x, y, z, w; DWORD c; };
    const Instance instances[] = { { -0.6f, 0.5f, 0, 0, 0xffff0000u }, { 0.0f, 0.0f, 0, 0, 0xff00ff00u },
                                   { 0.6f, -0.5f, 0, 0, 0xff0000ffu }, { -0.5f, -0.6f, 0, 0, 0xffffff00u } };
    const WORD indices[] = { 0, 1, 2, 1, 3, 2 };
    IDirect3DVertexBuffer9 *vb0 = nullptr, *vb1 = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    void* data;
    d->CreateVertexBuffer(sizeof(shape), 0, 0, D3DPOOL_MANAGED, &vb0, nullptr);
    vb0->Lock(0, 0, &data, 0); std::memcpy(data, shape, sizeof(shape)); vb0->Unlock();
    d->CreateVertexBuffer(sizeof(instances), 0, 0, D3DPOOL_MANAGED, &vb1, nullptr);
    vb1->Lock(0, 0, &data, 0); std::memcpy(data, instances, sizeof(instances)); vb1->Unlock();
    d->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    ib->Lock(0, 0, &data, 0); std::memcpy(data, indices, sizeof(indices)); ib->Unlock();
    d->SetVertexDeclaration(decl);
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetStreamSource(0, vb0, 0, 8);
    d->SetStreamSource(1, vb1, 0, sizeof(Instance));
    d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 4);
    d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
    d->SetIndices(ib);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    d->SetStreamSourceFreq(0, 1);
    d->SetStreamSourceFreq(1, 1);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetStreamSource(0, nullptr, 0, 0);
    d->SetStreamSource(1, nullptr, 0, 0);
    d->SetIndices(nullptr);
    ib->Release(); vb1->Release(); vb0->Release(); decl->Release(); ps->Release(); vs->Release();
    return true;
  }

  bool sceneStateBlock(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->BeginStateBlock();
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    IDirect3DStateBlock9* additive = nullptr;
    d->EndStateBlock(&additive);
    IDirect3DStateBlock9* saved = nullptr;
    d->CreateStateBlock(D3DSBT_ALL, &saved);
    additive->Apply();
    fullscreenTL(d, 0xff400000u);
    fullscreenTL(d, 0xff004000u);
    saved->Apply();
    const TLVertex tri[3] = {
      { 4.0f, 4.0f, 0.5f, 1.0f, 0xff0000ffu }, { 40.0f, 4.0f, 0.5f, 1.0f, 0xff0000ffu }, { 4.0f, 40.0f, 0.5f, 1.0f, 0xff0000ffu },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    additive->Release();
    saved->Release();
    return true;
  }

  bool sceneSm3Loops(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    const char* vsSource = R"(
struct VO { float4 p : POSITION; float2 t : TEXCOORD0; float4 n : NORMAL; };
VO main(float2 p : POSITION) { VO o; o.p = float4(p, 0.5, 1); o.t = p * 0.5 + 0.5; o.n = float4(p, -p); return o; }
)";
    const char* psSource = R"(
float4 k[16] : register(c0);
int count : register(i0);
float4 main(float2 t : TEXCOORD0, float4 n : NORMAL, float face : VFACE) : COLOR {
  float4 r = 0;
  for (int i = 0; i < count; i++) r += k[i] * (t.x + i * 0.05);
  [branch] if (t.y > 0.5) r.yz = n.xy * 0.5 + 0.5;
  return float4(frac(r.xyz), face > 0 ? 1 : 0.25);
})";
    const auto vsCode = compile(vsSource, "vs_3_0");
    const auto psCode = compile(psSource, "ps_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    float k[16][4];
    for (int i = 0; i < 16; i++) { k[i][0] = 0.1f * i; k[i][1] = 0.05f * (16 - i); k[i][2] = 0.03f * i * i; k[i][3] = 1.0f; }
    const int count[4] = { 6, 0, 1, 0 };
    d->SetPixelShaderConstantF(0, &k[0][0], 16);
    d->SetPixelShaderConstantI(0, count, 1);
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetFVF(D3DFVF_XYZ);
    struct P { float x, y, z; };
    const P quad[4] = { { -1, 1, 0 }, { 1, 1, 0 }, { -1, -1, 0 }, { 1, -1, 0 } };
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    d->CreateVertexDeclaration(elements, &decl);
    d->SetVertexDeclaration(decl);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(P));
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    decl->Release(); ps->Release(); vs->Release();
    return true;
  }

  bool scenePs14(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    const auto psCode = assemble("ps_1_4\ntexld r0, t0\ntexcrd r1.rgb, t0\nmul r0.rgb, r0, r1\nadd r0.rgb, r0, c0\nmov r0.a, c0.a\n");
    IDirect3DPixelShader9* ps = nullptr;
    if (psCode.empty() || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    IDirect3DTexture9* texture = checkerTexture(d, 8);
    const float c0[4] = { 0.1f, 0.2f, 0.0f, 1.0f };
    d->SetPixelShaderConstantF(0, c0, 1);
    d->SetPixelShader(ps);
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    struct TLTex { float x, y, z, rhw; float u, v; };
    const TLTex quad[4] = {
      { -0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f }, { 63.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
      { -0.5f, 47.5f, 0.5f, 1.0f, 0.0f, 1.0f }, { 63.5f, 47.5f, 0.5f, 1.0f, 1.0f, 1.0f },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLTex));
    d->SetPixelShader(nullptr);
    d->SetTexture(0, nullptr);
    texture->Release();
    ps->Release();
    return true;
  }

  bool sceneMipFilter(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    IDirect3DTexture9* texture = checkerTexture(d, 32, D3DFMT_A8R8G8B8, 0);
    if (!texture) return false;
    D3DMATRIX world = identity(), view = translation(0.0f, -0.6f, 0.5f), proj = perspective(1.2f, 4.0f / 3.0f, 0.1f, 50.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    const TexVertex floor[4] = {
      { -4.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { 4.0f, 0.0f, 0.0f, 8.0f, 0.0f },
      { -4.0f, 0.0f, 30.0f, 0.0f, 30.0f }, { 4.0f, 0.0f, 30.0f, 8.0f, 30.0f },
    };
    d->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, floor, sizeof(TexVertex));
    d->SetTexture(0, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    texture->Release();
    return true;
  }

  bool sceneClipPlanes(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    // Fixed function: planes in world space.
    D3DMATRIX world = identity(), view = translation(0.0f, 0.0f, 2.5f), proj = perspective(1.0f, 4.0f / 3.0f, 0.5f, 10.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    const float plane0[4] = { 1.0f, 0.3f, 0.0f, 0.2f };
    const float plane1[4] = { 0.0f, -1.0f, 0.0f, 0.6f };
    d->SetClipPlane(0, plane0);
    d->SetClipPlane(1, plane1);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0 | D3DCLIPPLANE1);
    struct V { float x, y, z; DWORD c; };
    const V quad[4] = { { -1.2f, 1.0f, 0.0f, 0xffff0000u }, { 1.2f, 1.0f, 0.0f, 0xff00ff00u },
                        { -1.2f, -1.0f, 0.0f, 0xff0000ffu }, { 1.2f, -1.0f, 0.0f, 0xffffffffu } };
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    // Shaders: planes in clip space.
    const char* vsSource = R"(
struct VO { float4 p : POSITION; float4 c : COLOR0; };
VO main(float3 p : POSITION, float4 c : COLOR0) { VO o; o.p = float4(p.xy * 0.4 + float2(0.4, -0.3), 0.3, 1); o.c = c.bgra; return o; })";
    const auto vsCode = compile(vsSource, "vs_2_0");
    IDirect3DVertexShader9* vs = nullptr;
    if (vsCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs)))
      return false;
    const float plane2[4] = { -1.0f, 1.0f, 0.0f, 0.5f };
    d->SetClipPlane(0, plane2);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
    d->SetVertexShader(vs);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    d->SetVertexShader(nullptr);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    vs->Release();
    return true;
  }

  // Depth textures: compared (D24S8, NVIDIA's shadow maps) and read (INTZ).
  bool sceneDepthTextures(IDirect3DDevice9* d) {
    IDirect3D9* d3d = nullptr;
    d->GetDirect3D(&d3d);
    const bool intz = SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                                       D3DRTYPE_TEXTURE, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z'))));
    d3d->Release();
    IDirect3DSurface9 *back = nullptr, *oldDepth = nullptr;
    d->GetRenderTarget(0, &back);
    d->GetDepthStencilSurface(&oldDepth);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DTexture9* depthTextures[2] = {};
    const D3DFORMAT formats[2] = { D3DFMT_D24S8, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')) };
    for (int i = 0; i < (intz ? 2 : 1); i++) {
      if (FAILED(d->CreateTexture(32, 32, 1, D3DUSAGE_DEPTHSTENCIL, formats[i], D3DPOOL_DEFAULT, &depthTextures[i], nullptr)))
        return false;
      IDirect3DSurface9* depth = nullptr;
      depthTextures[i]->GetSurfaceLevel(0, &depth);
      d->SetDepthStencilSurface(depth);
      // Color writes off: the depth of a slanted triangle.
      IDirect3DSurface9* scratch = nullptr;
      d->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &scratch, nullptr);
      d->SetRenderTarget(0, scratch);
      d->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_TARGET, 0, 1.0f, 0);
      const TLVertex tri[3] = { { 0.0f, 0.0f, 0.2f, 1.0f, 0 }, { 32.0f, 0.0f, 0.5f, 1.0f, 0 }, { 0.0f, 32.0f, 0.8f, 1.0f, 0 } };
      d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
      depth->Release();
      scratch->Release();
    }
    d->SetRenderTarget(0, back);
    d->SetDepthStencilSurface(oldDepth);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    // Shadow map: tex2Dproj compares the coordinate's z with the depth.
    const char* psShadow = R"(
sampler2D shadow : register(s0);
float4 main(float4 t : TEXCOORD0) : COLOR { return tex2Dproj(shadow, float4(t.xy, t.z, 1)); }
)";
    const char* psRead = R"(
sampler2D depth : register(s0);
float4 main(float4 t : TEXCOORD0) : COLOR { return tex2D(depth, t.xy).rrrr; }
)";
    struct TLTex { float x, y, z, rhw; float u, v, r, q; };
    for (int i = 0; i < (intz ? 2 : 1); i++) {
      const auto code = compile(i == 0 ? psShadow : psRead, "ps_2_0");
      IDirect3DPixelShader9* ps = nullptr;
      if (code.empty() || FAILED(d->CreatePixelShader(code.data(), &ps)))
        return false;
      d->SetPixelShader(ps);
      d->SetTexture(0, depthTextures[i]);
      d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      const float x0 = i * 32.0f - 0.5f;
      const TLTex quad[4] = {
        { x0, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0.45f, 1.0f }, { x0 + 32.0f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f, 0.45f, 1.0f },
        { x0, 47.5f, 0.5f, 1.0f, 0.0f, 1.0f, 0.45f, 1.0f }, { x0 + 32.0f, 47.5f, 0.5f, 1.0f, 1.0f, 1.0f, 0.45f, 1.0f },
      };
      d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLTex));
      d->SetPixelShader(nullptr);
      d->SetTexture(0, nullptr);
      ps->Release();
    }
    for (auto* t : depthTextures) if (t) t->Release();
    back->Release();
    if (oldDepth) oldDepth->Release();
    return true;
  }

  bool sceneSrgbAndTargets(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    // Two targets at once; sRGB writes to the second.
    IDirect3DTexture9* second = nullptr;
    if (FAILED(d->CreateTexture(kWidth, kHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &second, nullptr)))
      return false;
    IDirect3DSurface9* surface = nullptr;
    second->GetSurfaceLevel(0, &surface);
    d->SetRenderTarget(1, surface);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    const char* psSource = R"(
struct PO { float4 c0 : COLOR0; float4 c1 : COLOR1; };
PO main(float4 c : COLOR0) { PO o; o.c0 = c; o.c1 = c.gbra * 0.8; return o; }
)";
    const auto code = compile(psSource, "ps_2_0");
    IDirect3DPixelShader9* ps = nullptr;
    if (code.empty() || FAILED(d->CreatePixelShader(code.data(), &ps)))
      return false;
    d->SetPixelShader(ps);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE);
    const TLVertex tri[3] = {
      { 4.0f, 4.0f, 0.5f, 1.0f, 0xffff0000u }, { 60.0f, 8.0f, 0.5f, 1.0f, 0xff00ff80u }, { 10.0f, 44.0f, 0.5f, 1.0f, 0xff4040ffu },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    const HRESULT drawn = d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    if (FAILED(drawn)) std::printf("  srgb draw failed 0x%08lx\n", drawn);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetPixelShader(nullptr);
    d->SetRenderTarget(1, nullptr);
    // The second target read with sRGB decoding onto the right half.
    d->SetTexture(0, second);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    struct TLTex { float x, y, z, rhw; float u, v; };
    const TLTex quad[4] = {
      { 31.5f, -0.5f, 0.5f, 1.0f, 0.5f, 0.0f }, { 63.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
      { 31.5f, 47.5f, 0.5f, 1.0f, 0.5f, 1.0f }, { 63.5f, 47.5f, 0.5f, 1.0f, 1.0f, 1.0f },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLTex));
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    d->SetTexture(0, nullptr);
    ps->Release();
    surface->Release();
    second->Release();
    return true;
  }

  // 0 times infinity or NaN in mul, mad and dp3 of a pixel shader: 0 with
  // Direct3D 9 drivers (IEEE would give NaN, written as black).
  bool sceneFloatSpecials(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff336699u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    const char* sources[3] = {
      "ps_2_0\nmov r1, c1\nmov r2, c2\nmul r0, c0, r1\nadd r0, r0, r2\nmov oC0, r0\n",
      "ps_2_0\nmov r1, c1\nmov r2, c2\nmad r0, c0, r1, r2\nmov oC0, r0\n",
      "ps_2_0\nmov r1, c1\nmov r2, c2\ndp3 r0, c0, r1\nadd r0, r0, r2\nmov oC0, r0\n",
    };
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, specials[4] = { inf, nan, -inf, 1.0f }, half[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    d->SetPixelShaderConstantF(0, zero, 1);
    d->SetPixelShaderConstantF(1, specials, 1);
    d->SetPixelShaderConstantF(2, half, 1);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    for (int i = 0; i < 3; i++) {
      const auto code = assemble(sources[i]);
      IDirect3DPixelShader9* ps = nullptr;
      if (code.empty() || FAILED(d->CreatePixelShader(code.data(), &ps)))
        return false;
      d->SetPixelShader(ps);
      const float x0 = float(i * kWidth / 3) - 0.5f, x1 = float((i + 1) * kWidth / 3) - 0.5f;
      const TLVertex quad[4] = {
        { x0, -0.5f, 0.5f, 1.0f, 0xffffffffu }, { x1, -0.5f, 0.5f, 1.0f, 0xffffffffu },
        { x0, float(kHeight) - 0.5f, 0.5f, 1.0f, 0xffffffffu }, { x1, float(kHeight) - 0.5f, 0.5f, 1.0f, 0xffffffffu },
      };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
      ps->Release();
    }
    d->SetPixelShader(nullptr);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    return true;
  }

  // A view-projection matrix whose z column is NaN (Mirror's Edge in some
  // frames), so every vertex gets a NaN depth. From the left: depth test
  // LESSEQUAL, depth test ALWAYS, depth test off, and finite depth.
  bool sceneNanDepth(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff203040u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    const auto vsCode = assemble("vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_color o1\n"
                                 "mul r0, c1, v0.y\nmad r0, c0, v0.x, r0\nmad r0, c2, v0.z, r0\nmad o0, c3, v0.w, r0\nmov o1, c4\n");
    const auto psCode = assemble("ps_3_0\ndcl_color v0\nmov oC0, v0\n");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetFVF(D3DFVF_XYZ);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float nanMatrix[16] = { 1, 0, nan, 0, 0, 1, nan, 0, 0, 0, nan, 0, 0, 0, nan, 1 };
    const float finiteMatrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    struct Case { const float* matrix; BOOL depth; D3DCMPFUNC func; float color[4]; };
    const Case cases[4] = {
      { nanMatrix, TRUE, D3DCMP_LESSEQUAL, { 1.0f, 0.2f, 0.2f, 1.0f } },
      { nanMatrix, TRUE, D3DCMP_ALWAYS, { 0.2f, 1.0f, 0.2f, 1.0f } },
      { nanMatrix, FALSE, D3DCMP_LESSEQUAL, { 1.0f, 1.0f, 0.2f, 1.0f } },
      { finiteMatrix, TRUE, D3DCMP_LESSEQUAL, { 0.2f, 0.4f, 1.0f, 1.0f } },
    };
    for (int i = 0; i < 4; i++) {
      d->SetVertexShaderConstantF(0, cases[i].matrix, 4);
      d->SetVertexShaderConstantF(4, cases[i].color, 1);
      d->SetRenderState(D3DRS_ZENABLE, cases[i].depth);
      d->SetRenderState(D3DRS_ZFUNC, cases[i].func);
      // No vertex at x = 0: 0 times NaN is 0 for some drivers.
      const float x0 = -0.95f + 0.5f * float(i), x1 = x0 + 0.4f;
      const float quad[12] = { x0, 0.8f, 0.5f, x1, 0.8f, 0.5f, x0, -0.8f, 0.5f, x1, -0.8f, 0.5f };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 3 * sizeof(float));
    }
    // NaN depth is written as 0: a white strip at depth 0.25 (test LESS)
    // stays hidden behind the first two quads, and shows over the last two.
    d->SetVertexShaderConstantF(0, finiteMatrix, 4);
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    d->SetVertexShaderConstantF(4, white, 1);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    const float strip[12] = { -0.95f, 0.3f, 0.25f, 0.95f, 0.3f, 0.25f, -0.95f, 0.0f, 0.25f, 0.95f, 0.0f, 0.25f };
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, strip, 3 * sizeof(float));
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    ps->Release();
    vs->Release();
    return true;
  }

  bool scenePointSprites(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    IDirect3DTexture9* texture = checkerTexture(d, 8);
    if (!texture) return false;
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    // Fixed function: sprites of a state's size, scaled by distance.
    D3DMATRIX world = identity(), view = translation(0.0f, 0.0f, 2.0f), proj = perspective(1.0f, 4.0f / 3.0f, 0.5f, 10.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    const float size = 0.15f, a = 0.0f, b = 0.0f, c = 1.0f, minimum = 1.0f;
    d->SetRenderState(D3DRS_POINTSPRITEENABLE, TRUE);
    d->SetRenderState(D3DRS_POINTSCALEENABLE, TRUE);
    d->SetRenderState(D3DRS_POINTSIZE, *reinterpret_cast<const DWORD*>(&size));
    d->SetRenderState(D3DRS_POINTSIZE_MIN, *reinterpret_cast<const DWORD*>(&minimum));
    d->SetRenderState(D3DRS_POINTSCALE_A, *reinterpret_cast<const DWORD*>(&a));
    d->SetRenderState(D3DRS_POINTSCALE_B, *reinterpret_cast<const DWORD*>(&b));
    d->SetRenderState(D3DRS_POINTSCALE_C, *reinterpret_cast<const DWORD*>(&c));
    struct V { float x, y, z; DWORD c; };
    const V points[3] = { { -0.6f, 0.4f, 0.0f, 0xffffffffu }, { 0.0f, 0.0f, 1.0f, 0xffff8080u }, { 0.6f, -0.3f, -0.5f, 0xff80ff80u } };
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_POINTLIST, 3, points, sizeof(V));
    d->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
    // A vertex shader's size.
    const char* vsSource = R"(
struct VO { float4 p : POSITION; float4 c : COLOR0; float size : PSIZE; float2 t : TEXCOORD0; };
VO main(float3 p : POSITION, float4 c : COLOR0) { VO o; o.p = float4(p.xy * 0.8 + float2(0.0, -0.5), 0.5, 1); o.c = c; o.size = 6 + p.x * 4; o.t = 0; return o; })";
    const auto vsCode = compile(vsSource, "vs_2_0");
    IDirect3DVertexShader9* vs = nullptr;
    if (vsCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs)))
      return false;
    d->SetVertexShader(vs);
    d->DrawPrimitiveUP(D3DPT_POINTLIST, 3, points, sizeof(V));
    d->SetVertexShader(nullptr);
    d->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
    d->SetTexture(0, nullptr);
    vs->Release();
    texture->Release();
    return true;
  }

  struct TLTex1 { float x, y, z, rhw; float u, v; };

  void texturedRect(IDirect3DDevice9* d, float x0, float y0, float x1, float y1) {
    const TLTex1 quad[4] = {
      { x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0.0f, 0.0f }, { x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
      { x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f }, { x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLTex1));
  }

  bool sceneMsaaResolve(IDirect3DDevice9* d) {
    IDirect3DSurface9 *msaa = nullptr, *back = nullptr;
    if (FAILED(d->CreateRenderTarget(kWidth, kHeight, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &msaa, nullptr)))
      return false;
    d->GetRenderTarget(0, &back);
    d->SetRenderTarget(0, msaa);
    d->SetDepthStencilSurface(nullptr);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000020u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    const TLVertex tri[3] = {
      { 3.3f, 2.7f, 0.5f, 1.0f, 0xffffffffu }, { 60.1f, 13.9f, 0.5f, 1.0f, 0xffff4000u }, { 17.6f, 45.2f, 0.5f, 1.0f, 0xff00c0ffu },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    d->StretchRect(msaa, nullptr, back, nullptr, D3DTEXF_NONE);
    d->SetRenderTarget(0, back);
    back->Release();
    msaa->Release();
    return true;
  }

  // Textures of the formats that convert or compress, sampled side by side.
  bool sceneFormats(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    const D3DFORMAT formats[6] = { D3DFMT_L8, D3DFMT_A8L8, D3DFMT_R5G6B5, D3DFMT_A4R4G4B4, D3DFMT_DXT1, D3DFMT_A1R5G5B5 };
    for (int f = 0; f < 6; f++) {
      IDirect3DTexture9* texture = nullptr;
      if (FAILED(d->CreateTexture(8, 8, 1, 0, formats[f], D3DPOOL_MANAGED, &texture, nullptr)))
        return false;
      D3DLOCKED_RECT rect;
      texture->LockRect(0, &rect, nullptr, 0);
      BYTE* bits = static_cast<BYTE*>(rect.pBits);
      if (formats[f] == D3DFMT_DXT1) {
        // Four blocks: two colors each, a pattern of indices.
        for (int block = 0; block < 4; block++) {
          WORD* colors = reinterpret_cast<WORD*>(bits + (block / 2) * rect.Pitch + (block % 2) * 8);
          colors[0] = WORD(0xf800 >> (block * 2)) | 0x001f;
          colors[1] = WORD(0x07e0 + block * 3);
          *reinterpret_cast<DWORD*>(colors + 2) = 0x1b1b4e4eu ^ (block * 0x01010101u);
        }
      } else {
        for (UINT y = 0; y < 8; y++)
          for (UINT x = 0; x < 8; x++) {
            const UINT v = (x * 37 + y * 23) & 0xff;
            BYTE* p = bits + y * rect.Pitch;
            switch (formats[f]) {
              case D3DFMT_L8: p[x] = BYTE(v); break;
              case D3DFMT_A8L8: p[x * 2] = BYTE(v); p[x * 2 + 1] = BYTE(255 - v); break;
              default: reinterpret_cast<WORD*>(p)[x] = WORD(v * 257 ^ (y << 11)); break;
            }
          }
      }
      texture->UnlockRect(0);
      d->SetTexture(0, texture);
      const float x = float((f % 3) * 21), y = float((f / 3) * 24);
      texturedRect(d, x, y, x + 20.0f, y + 23.0f);
      d->SetTexture(0, nullptr);
      texture->Release();
    }
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    return true;
  }

  bool sceneUpdateTextureAndQuery(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    IDirect3DTexture9 *system = nullptr, *video = nullptr;
    if (FAILED(d->CreateTexture(16, 16, 2, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &system, nullptr))
     || FAILED(d->CreateTexture(16, 16, 2, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &video, nullptr)))
      return false;
    for (UINT level = 0; level < 2; level++) {
      D3DLOCKED_RECT rect;
      system->LockRect(level, &rect, nullptr, 0);
      const UINT size = 16 >> level;
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++)
          reinterpret_cast<DWORD*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch)[x] = level ? 0xff00ff00u : (0xff000000u | (x * 16 << 16) | (y * 16));
      system->UnlockRect(level);
    }
    d->UpdateTexture(system, video);
    d->SetTexture(0, video);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    texturedRect(d, 0.0f, 0.0f, 32.0f, 32.0f);
    texturedRect(d, 40.0f, 0.0f, 48.0f, 8.0f);   // the second level
    d->SetTexture(0, nullptr);
    // An occlusion query: the pixels of a rectangle, shown as its width.
    IDirect3DQuery9* query = nullptr;
    if (FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query)))
      return false;
    query->Issue(D3DISSUE_BEGIN);
    const TLVertex quad[4] = {
      { 2.0f, 36.0f, 0.5f, 1.0f, 0xff404040u }, { 12.0f, 36.0f, 0.5f, 1.0f, 0xff404040u },
      { 2.0f, 44.0f, 0.5f, 1.0f, 0xff404040u }, { 12.0f, 44.0f, 0.5f, 1.0f, 0xff404040u },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
    query->Issue(D3DISSUE_END);
    DWORD pixels = 0;
    while (query->GetData(&pixels, sizeof(pixels), D3DGETDATA_FLUSH) == S_FALSE) {}
    query->Release();
    const float width = float(pixels) / 8.0f;
    const TLVertex bar[4] = {
      { 20.0f, 40.0f, 0.5f, 1.0f, 0xffffff00u }, { 20.0f + width, 40.0f, 0.5f, 1.0f, 0xffffff00u },
      { 20.0f, 44.0f, 0.5f, 1.0f, 0xffffff00u }, { 20.0f + width, 44.0f, 0.5f, 1.0f, 0xffffff00u },
    };
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bar, sizeof(TLVertex));
    system->Release();
    video->Release();
    return pixels == 80;
  }

  bool sceneFixedFunctionExtras(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    D3DMATRIX view = translation(0.0f, 0.0f, 3.0f), proj = perspective(1.0f, 4.0f / 3.0f, 0.5f, 10.0f);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    // Vertex blending between two world matrices by one weight.
    D3DMATRIX w0 = translation(-0.8f, 0.0f, 0.0f), w1 = rotationY(0.7f);
    w1._41 = 0.6f; w1._42 = 0.3f;
    d->SetTransform(D3DTS_WORLDMATRIX(0), &w0);
    d->SetTransform(D3DTS_WORLDMATRIX(1), &w1);
    d->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
    struct BV { float x, y, z, weight; DWORD c; };
    const BV strip[4] = { { -0.5f, 0.9f, 0.0f, 1.0f, 0xffff0000u }, { 0.5f, 0.9f, 0.0f, 0.7f, 0xff00ff00u },
                          { -0.5f, -0.1f, 0.0f, 0.3f, 0xff0000ffu }, { 0.5f, -0.1f, 0.0f, 0.0f, 0xffffffffu } };
    d->SetFVF(D3DFVF_XYZB1 | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, strip, sizeof(BV));
    d->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    D3DMATRIX world = identity();
    d->SetTransform(D3DTS_WORLD, &world);
    // A texture transform (scale, offset) and a bump-mapped stage.
    IDirect3DTexture9* base = checkerTexture(d, 8);
    IDirect3DTexture9* bump = nullptr;
    d->CreateTexture(8, 8, 1, 0, D3DFMT_V8U8, D3DPOOL_MANAGED, &bump, nullptr);
    D3DLOCKED_RECT rect;
    bump->LockRect(0, &rect, nullptr, 0);
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 8; x++) {
        signed char* p = reinterpret_cast<signed char*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch) + x * 2;
        p[0] = static_cast<signed char>((int(x) - 4) * 12);
        p[1] = static_cast<signed char>((int(y) - 4) * -9);
      }
    bump->UnlockRect(0);
    D3DMATRIX texMatrix = identity();
    texMatrix._11 = 2.0f; texMatrix._22 = 1.5f; texMatrix._31 = 0.25f;
    d->SetTransform(D3DTS_TEXTURE1, &texMatrix);
    d->SetTexture(0, bump);
    d->SetTexture(1, base);
    for (DWORD s = 0; s < 2; s++) {
      d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    const float m00 = 0.06f, m01 = 0.0f, m10 = 0.0f, m11 = 0.06f;
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BUMPENVMAP);
    d->SetTextureStageState(0, D3DTSS_BUMPENVMAT00, *reinterpret_cast<const DWORD*>(&m00));
    d->SetTextureStageState(0, D3DTSS_BUMPENVMAT01, *reinterpret_cast<const DWORD*>(&m01));
    d->SetTextureStageState(0, D3DTSS_BUMPENVMAT10, *reinterpret_cast<const DWORD*>(&m10));
    d->SetTextureStageState(0, D3DTSS_BUMPENVMAT11, *reinterpret_cast<const DWORD*>(&m11));
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    struct TV { float x, y, z; float u, v; };
    const TV quad[4] = { { -1.2f, -0.2f, 0.0f, 0.0f, 0.0f }, { 1.2f, -0.2f, 0.0f, 1.0f, 0.0f },
                         { -1.2f, -1.1f, 0.0f, 0.0f, 1.0f }, { 1.2f, -1.1f, 0.0f, 1.0f, 1.0f } };
    d->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TV));
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    d->SetTexture(0, nullptr);
    d->SetTexture(1, nullptr);
    base->Release();
    bump->Release();
    return true;
  }

  bool sceneCubeVolumeShader(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    IDirect3DCubeTexture9* cube = nullptr;
    IDirect3DVolumeTexture9* volume = nullptr;
    if (FAILED(d->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr))
     || FAILED(d->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, nullptr)))
      return false;
    for (UINT face = 0; face < 6; face++) {
      D3DLOCKED_RECT rect;
      cube->LockRect(D3DCUBEMAP_FACES(face), 0, &rect, nullptr, 0);
      for (UINT y = 0; y < 4; y++)
        for (UINT x = 0; x < 4; x++)
          reinterpret_cast<DWORD*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch)[x] = 0xff000000u | (face * 40 << 16) | (x * 60 << 8) | (y * 60);
      cube->UnlockRect(D3DCUBEMAP_FACES(face), 0);
    }
    D3DLOCKED_BOX box;
    volume->LockBox(0, &box, nullptr, 0);
    for (UINT z = 0; z < 4; z++)
      for (UINT y = 0; y < 4; y++)
        for (UINT x = 0; x < 4; x++)
          reinterpret_cast<DWORD*>(static_cast<BYTE*>(box.pBits) + z * box.SlicePitch + y * box.RowPitch)[x] =
            0xff000000u | (x * 60 << 16) | (y * 60 << 8) | (z * 60);
    volume->UnlockBox(0);
    const char* psSource = R"(
samplerCUBE envmap : register(s0);
sampler3D field : register(s1);
float4 main(float2 t : TEXCOORD0) : COLOR {
  float3 dir = float3(t.x * 2 - 1, t.y * 2 - 1, 0.35);
  return lerp(texCUBE(envmap, dir), tex3D(field, float3(t, t.x * 0.7 + 0.1)), t.y > 0.5 ? 1 : 0);
})";
    const auto code = compile(psSource, "ps_2_0");
    IDirect3DPixelShader9* ps = nullptr;
    if (code.empty() || FAILED(d->CreatePixelShader(code.data(), &ps)))
      return false;
    d->SetPixelShader(ps);
    d->SetTexture(0, cube);
    d->SetTexture(1, volume);
    for (DWORD s = 0; s < 2; s++) {
      d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    texturedRect(d, 0.0f, 0.0f, float(kWidth), float(kHeight));
    d->SetPixelShader(nullptr);
    d->SetTexture(0, nullptr);
    d->SetTexture(1, nullptr);
    ps->Release();
    cube->Release();
    volume->Release();
    return true;
  }

  const Scene kScenes[] = {
    { "msaa_resolve", sceneMsaaResolve, 80 },   // edge coverage: NVIDIA's 4x pattern differs
    { "formats", sceneFormats },
    { "update_texture_query", sceneUpdateTextureAndQuery },
    { "ffp_extras", sceneFixedFunctionExtras, 3 },
    { "cube_volume_shader", sceneCubeVolumeShader },
    { "point_sprites", scenePointSprites },
    { "float_specials", sceneFloatSpecials },
    { "nan_depth", sceneNanDepth },
    { "clip_planes", sceneClipPlanes },
    { "depth_textures", sceneDepthTextures },
    { "srgb_targets", sceneSrgbAndTargets, 3 },
    { "clear", sceneClear },
    { "transformed", sceneTransformed },
    { "textured", sceneTextured },
    { "lit", sceneLit, 3 },
    { "shaders", sceneShaders },
    { "blend_alphatest", sceneBlendAlphaTest },
    { "depth_stencil", sceneDepthStencil },
    { "render_to_texture", sceneRenderToTexture },
    { "fog", sceneFog, 3 },
    { "dynamic_buffer", sceneDynamicBuffer },
    { "instancing", sceneInstancing },
    { "state_block", sceneStateBlock },
    { "sm3_loops", sceneSm3Loops },
    { "ps_1_4", scenePs14 },
    { "mip_filter", sceneMipFilter, 6 },
  };

  // ---- Running ----

  bool run(Runtime& runtime, const Scene& scene, std::vector<DWORD>& pixels, std::string& error) {
    IDirect3D9* d3d = runtime.create(D3D_SDK_VERSION);
    if (!d3d) { error = "Direct3DCreate9 failed"; return false; }
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (FAILED(hr)) {
      error = "CreateDevice failed";
      d3d->Release();
      DestroyWindow(window);
      return false;
    }
    TRACE("[%s] device created\n", runtime.name);
    device->BeginScene();
    const bool drawn = scene.draw(device);
    device->EndScene();
    TRACE("[%s] drawn\n", runtime.name);
    bool ok = drawn;
    if (!drawn)
      error = "scene failed";
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    if (ok && SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))
     && SUCCEEDED(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr))
     && SUCCEEDED(device->GetRenderTargetData(back, copy))) {
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
    TRACE("[%s] read back\n", runtime.name);
    device->Present(nullptr, nullptr, nullptr, nullptr);
    TRACE("[%s] presented\n", runtime.name);
    if (copy) copy->Release();
    TRACE("[%s] copy released\n", runtime.name);
    if (back) back->Release();
    TRACE("[%s] back buffer released\n", runtime.name);
    const ULONG remaining = device->Release();
    if (remaining)
      std::printf("  (%s: device still referenced %lu times)\n", runtime.name, remaining);
    TRACE("[%s] released\n", runtime.name);
    d3d->Release();
    DestroyWindow(window);
    return ok;
  }

  void writeImage(const char* path, const std::vector<DWORD>& pixels) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file)
      return;
    std::fprintf(file, "P6\n%u %u\n255\n", kWidth, kHeight);
    for (DWORD p : pixels) {
      const unsigned char rgb[3] = { BYTE(p >> 16), BYTE(p >> 8), BYTE(p) };
      std::fwrite(rgb, 1, 3, file);
    }
    std::fclose(file);
  }

  // Bench parts (DXUP_BENCH_MASK): 1 constants, 2 textures, 4 blend state,
  // 8 a dynamic vertex buffer locked per draw.
  int benchMask() {
    char value[16] = {};
    return GetEnvironmentVariableA("DXUP_BENCH_MASK", value, sizeof(value)) ? std::atoi(value) : 7;
  }

  int bench(Runtime& runtime, int frames, int draws) {
    const int mask = benchMask();
    IDirect3D9* d3d = runtime.create(D3D_SDK_VERSION);
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* d = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d)))
      return 1;
    const auto vsCode = compile(kVs2, "vs_2_0");
    const auto psCode = compile(kPs2, "ps_2_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    d->CreateVertexShader(vsCode.data(), &vs);
    d->CreatePixelShader(psCode.data(), &ps);
    IDirect3DTexture9* textures[2] = { checkerTexture(d, 8), checkerTexture(d, 16) };
    struct V { float x, y, z; DWORD c; float u, v; };
    IDirect3DVertexBuffer9* vb = nullptr;
    d->CreateVertexBuffer(sizeof(V) * 4, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr);
    V* v = nullptr;
    vb->Lock(0, 0, reinterpret_cast<void**>(&v), 0);
    v[0] = { -0.05f, 0.05f, 0.5f, 0xffffffffu, 0, 0 }; v[1] = { 0.05f, 0.05f, 0.5f, 0xffffffffu, 1, 0 };
    v[2] = { -0.05f, -0.05f, 0.5f, 0xffffffffu, 0, 1 }; v[3] = { 0.05f, -0.05f, 0.5f, 0xffffffffu, 1, 1 };
    V quadData[4];
    std::memcpy(quadData, v, sizeof(quadData));
    vb->Unlock();
    v = quadData;
    IDirect3DVertexBuffer9* dynamicVb = nullptr;
    d->CreateVertexBuffer(sizeof(V) * 4 * 1024, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &dynamicVb, nullptr);
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 },
      { 0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    d->CreateVertexDeclaration(elements, &decl);
    d->SetVertexDeclaration(decl);
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetStreamSource(0, vb, 0, sizeof(V));
    double best = 1e30, total = 0;
    for (int f = 0; f < frames; f++) {
      LARGE_INTEGER t0, t1, freq;
      QueryPerformanceCounter(&t0);
      d->BeginScene();
      d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
      for (int i = 0; i < draws; i++) {
        D3DMATRIX m = translation(float(i % 17) * 0.1f - 0.8f, float(i % 13) * 0.1f - 0.6f, 0.0f);
        // Typical per-draw work: constants, a texture, a state.
        D3DMATRIX t = { m._11, m._21, m._31, m._41, m._12, m._22, m._32, m._42, m._13, m._23, m._33, m._43, m._14, m._24, m._34, m._44 };
        if (mask & 1) {
          d->SetVertexShaderConstantF(0, &t._11, 4);
          const float tint[4] = { 1.0f, float(i & 7) / 7.0f, 0.5f, 1.0f };
          d->SetVertexShaderConstantF(4, tint, 1);
        }
        if (mask & 2) d->SetTexture(0, textures[i & 1]);
        if (mask & 4) d->SetRenderState(D3DRS_ALPHABLENDENABLE, (i & 3) == 0);
        if (mask & 8) {
          // A dynamic buffer appended per draw, discarded when full.
          const UINT slot = UINT(i) % 1024;
          V* dv = nullptr;
          dynamicVb->Lock(slot * 4 * sizeof(V), 4 * sizeof(V), reinterpret_cast<void**>(&dv), slot ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD);
          std::memcpy(dv, v, 4 * sizeof(V));
          dynamicVb->Unlock();
          d->SetStreamSource(0, dynamicVb, 0, sizeof(V));
          d->DrawPrimitive(D3DPT_TRIANGLESTRIP, slot * 4, 2);
          continue;
        }
        d->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
      }
      d->EndScene();
      d->Present(nullptr, nullptr, nullptr, nullptr);
      QueryPerformanceCounter(&t1);
      QueryPerformanceFrequency(&freq);
      const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
      if (f >= 10) { best = (std::min)(best, ms); total += ms; }
    }
    const double avg = total / (std::max)(1, frames - 10);
    std::printf("%s: %d draws per frame, %.3f ms per frame avg, %.3f best (%.0f ns per draw)\n", runtime.name, draws, avg, best,
                best * 1e6 / draws);
    d->SetTexture(0, nullptr);
    dynamicVb->Release();
    decl->Release(); vb->Release(); textures[0]->Release(); textures[1]->Release(); ps->Release(); vs->Release();
    d->Release();
    d3d->Release();
    DestroyWindow(window);
    return 0;
  }

  // dxup.conf d3d11.msaaForce: the back buffer and the depth buffer made
  // with the device are a swap chain's size, so they get forced samples (on
  // DXUP's D3D11, the swap chain comes only at the first Present). A white
  // triangle on black: its edge has partly covered pixels with forced
  // multisampling, none without. Returns how many (first frame, before any
  // Present), or -1.
  int forcedMsaaEdgePixels(Runtime& runtime) {
    IDirect3D9* d3d = runtime.create(D3D_SDK_VERSION);
    if (!d3d) return -1;
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
      d3d->Release();
      DestroyWindow(window);
      return -1;
    }
    struct V { float x, y, z, rhw; DWORD color; };
    const V triangle[3] = {
      { 0.0f, 0.0f, 0.5f, 1.0f, 0xffffffffu }, { float(kWidth), 0.0f, 0.5f, 1.0f, 0xffffffffu },
      { 0.0f, float(kHeight), 0.5f, 1.0f, 0xffffffffu },
    };
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    device->BeginScene();
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(V));
    device->EndScene();
    int partial = -1;
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))
     && SUCCEEDED(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr))
     && SUCCEEDED(device->GetRenderTargetData(back, copy))) {
      D3DLOCKED_RECT rect;
      copy->LockRect(&rect, nullptr, D3DLOCK_READONLY);
      partial = 0;
      for (UINT y = 0; y < kHeight; y++)
        for (UINT x = 0; x < kWidth; x++) {
          const DWORD red = (reinterpret_cast<const DWORD*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch)[x] >> 16) & 255;
          partial += red > 16 && red < 240;
        }
      copy->UnlockRect();
    }
    if (copy) copy->Release();
    if (back) back->Release();
    device->Present(nullptr, nullptr, nullptr, nullptr);
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return partial;
  }

  // Runs this program with the arguments; its exit code (-1: not started).
  int runSelf(const char* arguments) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" ";
    for (const char* c = arguments; *c; c++)
      command += wchar_t(*c);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
      return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
  }

}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  bool dumpImages = false, memory = false;
  const char* filter = nullptr;
  int benchFrames = 0, benchDraws = 0;
  bool childForcedMsaa = false, identifier = false;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--dump") == 0) dumpImages = true;
    else if (std::strcmp(argv[i], "--trace") == 0) g_trace = true;
    else if (std::strcmp(argv[i], "--memory") == 0) memory = true;
    else if (std::strcmp(argv[i], "--forced-msaa-child") == 0) childForcedMsaa = true;
    else if (std::strcmp(argv[i], "--identifier") == 0) identifier = true;
    else if (std::strcmp(argv[i], "--bench") == 0 && i + 2 < argc) { benchFrames = std::atoi(argv[i + 1]); benchDraws = std::atoi(argv[i + 2]); i += 2; }
    else filter = argv[i];
  }

  wchar_t systemDir[MAX_PATH];
  GetSystemDirectoryW(systemDir, MAX_PATH);
  Runtime windows = { "Windows" };
  windows.module = LoadLibraryW((std::wstring(systemDir) + L"\\d3d9.dll").c_str());
  Runtime dxup = { "DXUP" };
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
  dxup.module = LoadLibraryW((dir + L"dxup_d3d9.dll").c_str());
  for (Runtime* r : { &windows, &dxup })
    r->create = r->module ? reinterpret_cast<CreateFunction>(GetProcAddress(r->module, "Direct3DCreate9")) : nullptr;
  if (!windows.create || !dxup.create) {
    std::printf("FAIL: d3d9.dll (Windows %s, DXUP %s)\n", windows.create ? "ok" : "missing", dxup.create ? "ok" : "missing");
    return 1;
  }

  if (childForcedMsaa) {
    const int partial = forcedMsaaEdgePixels(dxup);
    std::printf("%d\n", partial);
    return partial < 0 ? 255 : (std::min)(partial, 254);
  }

  // The first adapter as each runtime describes it (dxgi.spoofVendor).
  if (identifier) {
    for (Runtime* r : { &windows, &dxup }) {
      IDirect3D9* d3d = r->create(D3D_SDK_VERSION);
      D3DADAPTER_IDENTIFIER9 id = {};
      if (d3d && SUCCEEDED(d3d->GetAdapterIdentifier(0, 0, &id)))
        std::printf("%-8s %04lx:%04lx %s (%s %u.%u.%u.%u)\n", r->name, id.VendorId, id.DeviceId, id.Description, id.Driver,
          unsigned(HIWORD(id.DriverVersion.HighPart)), unsigned(LOWORD(id.DriverVersion.HighPart)),
          unsigned(HIWORD(id.DriverVersion.LowPart)), unsigned(LOWORD(id.DriverVersion.LowPart)));
      if (d3d)
        d3d->Release();
    }
    return 0;
  }

  if (benchFrames) {
    bench(dxup, benchFrames, benchDraws);
    bench(windows, benchFrames, benchDraws);
    return 0;
  }

  int passed = 0, failed = 0;
  for (const Scene& scene : kScenes) {
    if (filter && !std::strstr(scene.name, filter))
      continue;
    std::vector<DWORD> reference, result;
    std::string error;
    if (!run(windows, scene, reference, error)) {
      std::printf("FAIL %-20s Windows: %s\n", scene.name, error.c_str());
      failed++;
      continue;
    }
    auto addressSpace = [] {
      MEMORYSTATUSEX status = { sizeof(status) };
      GlobalMemoryStatusEx(&status);
      return int64_t((status.ullTotalVirtual - status.ullAvailVirtual) >> 20);
    };
    const int64_t beforeDxup = addressSpace();
    const bool dxupRan = run(dxup, scene, result, error);
    if (memory)
      std::printf("  [DXUP run: %+lld MB]", (long long)(addressSpace() - beforeDxup));
    if (!dxupRan) {
      std::printf("FAIL %-20s DXUP: %s\n", scene.name, error.c_str());
      failed++;
      continue;
    }
    int worst = 0, worstIndex = 0, differing = 0;
    for (size_t i = 0; i < reference.size(); i++) {
      int d = 0;
      for (int c = 0; c < 32; c += 8)
        d = (std::max)(d, std::abs(int((reference[i] >> c) & 255) - int((result[i] >> c) & 255)));
      if (d > scene.tolerance) differing++;
      if (d > worst) { worst = d; worstIndex = int(i); }
    }
    // A few edge pixels may differ by rasterization; most must match.
    const bool ok = differing <= int(reference.size() / 200);
    std::printf("%s %-20s max difference %3d, %4d pixels beyond %d", ok ? "ok  " : "FAIL", scene.name, worst, differing,
                scene.tolerance);
    if (worst)
      std::printf("  (worst at %d,%d: Windows %08lx, DXUP %08lx)", worstIndex % kWidth, worstIndex / kWidth,
                  reference[worstIndex], result[worstIndex]);
    if (memory) {
      // Address space in use: devices must give theirs back.
      MEMORYSTATUSEX status = { sizeof(status) };
      GlobalMemoryStatusEx(&status);
      std::printf("  [%llu MB address space]", (status.ullTotalVirtual - status.ullAvailVirtual) >> 20);
    }
    std::printf("\n");
    if (!ok || dumpImages) {
      writeImage((std::string(scene.name) + "_windows.ppm").c_str(), reference);
      writeImage((std::string(scene.name) + "_dxup.ppm").c_str(), result);
    }
    ok ? passed++ : failed++;
  }
  // Forced multisampling (dxup.conf next to DXUP's DLLs, in a child).
  if (!filter || std::strstr("forced_msaa", filter)) {
    const std::wstring conf = dir + L"dxup.conf";
    int with = -1, without = -1;
    if (FILE* f = _wfopen(conf.c_str(), L"w")) {
      std::fputs("d3d11.msaaSamples = 4\nd3d11.msaaForce = true\n", f);
      std::fclose(f);
      with = runSelf("--forced-msaa-child");
      DeleteFileW(conf.c_str());
      without = runSelf("--forced-msaa-child");
    }
    // The triangle's edge crosses about 80 pixels.
    const bool ok = with >= 40 && with < 255 && without == 0;
    std::printf("%s %-20s edge pixels partly covered: %d forced, %d without\n", ok ? "ok  " : "FAIL", "forced_msaa", with,
                without);
    ok ? passed++ : failed++;
  }
  std::printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
