// Direct3D 9 scenes drawn by Windows' d3d9.dll and by DXUP's (dxup_d3d9.dll
// next to this program, with DXUP's d3d11.dll and dxgi.dll), compared pixel
// by pixel.
//
//   d3d9_test_app [--dump] [scene-name-filter]
//   d3d9_test_app --bench FRAMES DRAWS     (draw calls per second on DXUP and Windows)
//   d3d9_test_app --bench-shaders FILE [N] (GPU time of a d3d9_variants.bin's pixel shaders on both;
//     DXUP_BENCH_SIZE=N an N x N target, DXUP_BENCH_DXUP_FIRST, DXUP_BENCH_DXUP_ONLY (Windows' D3D9
//     needs the display on), DXUP_BENCH_CHECK pixels and failures of the first shaders)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

  // Indexed draws whose vertex range (MinVertexIndex, NumVertices) is
  // smaller than what their indices reach: Direct3D 9 drivers draw all, as
  // games rely on. A buffer written once, and one written every frame.
  bool sceneVertexRanges(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DIndexBuffer9* ib = nullptr;
    if (FAILED(d->CreateIndexBuffer(6 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr)))
      return false;
    WORD* indices = nullptr;
    ib->Lock(0, 0, reinterpret_cast<void**>(&indices), 0);
    for (WORD i = 0; i < 6; i++) indices[i] = i;
    ib->Unlock();
    d->SetIndices(ib);
    for (int buffer = 0; buffer < 2; buffer++) {
      IDirect3DVertexBuffer9* vb = nullptr;
      if (FAILED(d->CreateVertexBuffer(sizeof(TLVertex) * 6, D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                       D3DPOOL_MANAGED, &vb, nullptr)))
        return false;
      const float y = buffer * 24.0f;
      const DWORD c = buffer ? 0xff00ff00u : 0xffff8000u;
      // Written again and again by the second: DXUP streams it.
      for (int write = 0; write < (buffer ? 6 : 1); write++) {
        TLVertex* v = nullptr;
        vb->Lock(0, 0, reinterpret_cast<void**>(&v), 0);
        v[0] = { 2.0f, y + 2.0f, 0.5f, 1.0f, c };
        v[1] = { 28.0f, y + 2.0f, 0.5f, 1.0f, c };
        v[2] = { 2.0f, y + 20.0f, 0.5f, 1.0f, c };
        v[3] = { 34.0f, y + 2.0f, 0.5f, 1.0f, c | 0xff };
        v[4] = { 62.0f, y + 2.0f, 0.5f, 1.0f, c | 0xff };
        v[5] = { 34.0f, y + 20.0f, 0.5f, 1.0f, c | 0xff };
        vb->Unlock();
        d->SetStreamSource(0, vb, 0, sizeof(TLVertex));
        // Earlier writes drawn too, with the right range.
        if (write + 1 < (buffer ? 6 : 1))
          d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 6, 0, 2);
      }
      d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 2);
      d->SetStreamSource(0, nullptr, 0, 0);
      vb->Release();
    }
    d->SetIndices(nullptr);
    ib->Release();
    return true;
  }

  // L16 keeps 16 bits: two texels 0x8000 and 0x80ff, the difference times
  // 200 (0 with 8 bits), and L16 read as (L, L, L, 1).
  bool sceneL16(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    const char* psSource = R"(
sampler s : register(s0);
float4 main(float2 t : TEXCOORD0) : COLOR {
  float4 a = tex2D(s, float2(0.25, 0.5)), b = tex2D(s, float2(0.75, 0.5));
  return t.x < 0.5 ? float4((b.x - a.x) * 200.0, (b.y - a.y) * 200.0, (b.z - a.z) * 200.0, 1) : float4(a.xyz, a.w * 0.5);
})";
    const auto psCode = compile(psSource, "ps_2_0");
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DTexture9* texture = nullptr;
    if (psCode.empty() || FAILED(d->CreatePixelShader(psCode.data(), &ps))
     || FAILED(d->CreateTexture(2, 1, 1, 0, D3DFMT_L16, D3DPOOL_MANAGED, &texture, nullptr)))
      return false;
    D3DLOCKED_RECT locked = {};
    texture->LockRect(0, &locked, nullptr, 0);
    static_cast<WORD*>(locked.pBits)[0] = 0x8000;
    static_cast<WORD*>(locked.pBits)[1] = 0x80ff;
    texture->UnlockRect(0);
    struct V { float x, y, z, w, u, v; };
    const V quad[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { float(kWidth), 0, 0.5f, 1, 1, 0 }, { 0, float(kHeight), 0.5f, 1, 0, 1 },
                        { float(kWidth), float(kHeight), 0.5f, 1, 1, 1 } };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetPixelShader(ps);
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    d->SetPixelShader(nullptr);
    d->SetTexture(0, nullptr);
    texture->Release();
    ps->Release();
    return true;
  }

  // What shaders read from formats with fewer than four channels (missing
  // color channels read 1 in Direct3D 9, 0 in Direct3D 10+) and from
  // A2R10G10B10 (red in the high bits): one 8x8 cell per format, its left
  // half (x, y, z), its right half (w, the bits below 8 of x, 0).
  bool sceneFormatChannels(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    const char* psSource = R"(
sampler s : register(s0);
float4 main(float2 t : TEXCOORD0) : COLOR {
  float4 v = tex2D(s, float2(0.5, 0.5));
  return t.x < 0.5 ? float4(v.xyz, 1) : float4(v.w, frac(v.x * 255.0), 0, 1);
})";
    const auto psCode = compile(psSource, "ps_2_0");
    IDirect3DPixelShader9* ps = nullptr;
    if (psCode.empty() || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    auto half = [](float f) {
      uint32_t bits; std::memcpy(&bits, &f, 4);
      return WORD(((bits >> 16) & 0x8000) | (((bits >> 23) & 255) - 112) << 10 | ((bits >> 13) & 1023));
    };
    struct Cell { D3DFORMAT format; uint8_t bytes[8]; UINT size; };
    std::vector<Cell> cells;
    auto add = [&](D3DFORMAT format, const void* data, UINT size) {
      Cell c = { format, {}, size };
      std::memcpy(c.bytes, data, size);
      cells.push_back(c);
    };
    const WORD g16r16[2] = { 0x4000, 0xc123 };
    const DWORD a2r10 = (3u << 30) | (1000u << 20) | (512u << 10) | 37u;
    const DWORD a2b10 = (2u << 30) | (37u << 20) | (512u << 10) | 1000u;
    const BYTE v8u8[2] = { 64, 32 };
    const WORD v16u16[2] = { 0x2000, 0x4567 };
    const WORD r16f = half(0.25f), g16r16f[2] = { half(0.25f), half(0.625f) };
    const float r32f = 0.375f, g32r32f[2] = { 0.375f, 0.8125f };
    const WORD l16 = 0x80ff;
    const BYTE a8 = 0xc0, l8 = 0x60, a8l8[2] = { 0x60, 0xc0 };
    const WORD a16b16[4] = { 0x1234, 0x8000, 0xfedc, 0x4000 };
    add(D3DFMT_G16R16, g16r16, 4);
    add(D3DFMT_A2R10G10B10, &a2r10, 4);
    add(D3DFMT_A2B10G10R10, &a2b10, 4);
    add(D3DFMT_V8U8, v8u8, 2);
    add(D3DFMT_V16U16, v16u16, 4);
    add(D3DFMT_R16F, &r16f, 2);
    add(D3DFMT_G16R16F, g16r16f, 4);
    add(D3DFMT_R32F, &r32f, 4);
    add(D3DFMT_G32R32F, g32r32f, 8);
    add(D3DFMT_L16, &l16, 2);
    add(D3DFMT_A8, &a8, 1);
    add(D3DFMT_L8, &l8, 1);
    add(D3DFMT_A8L8, a8l8, 2);
    add(D3DFMT_A16B16G16R16, a16b16, 8);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetPixelShader(ps);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    struct V { float x, y, z, w, u, v; };
    for (size_t i = 0; i < cells.size(); i++) {
      IDirect3DTexture9* texture = nullptr;
      if (FAILED(d->CreateTexture(1, 1, 1, 0, cells[i].format, D3DPOOL_MANAGED, &texture, nullptr)))
        continue;
      D3DLOCKED_RECT locked = {};
      texture->LockRect(0, &locked, nullptr, 0);
      std::memcpy(locked.pBits, cells[i].bytes, cells[i].size);
      texture->UnlockRect(0);
      const float x = float(i % 8) * 8.0f, y = float(i / 8) * 8.0f;
      const V quad[4] = { { x, y, 0.5f, 1, 0, 0 }, { x + 8, y, 0.5f, 1, 1, 0 }, { x, y + 8, 0.5f, 1, 0, 1 },
                          { x + 8, y + 8, 0.5f, 1, 1, 1 } };
      d->SetTexture(0, texture);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
      d->SetTexture(0, nullptr);
      texture->Release();
    }
    d->SetPixelShader(nullptr);
    ps->Release();
    return true;
  }

  // Vertex element types as a shader reads them (missing components
  // (0, 0, 0, 1)): one 8x8 cell
  // per type, left half (x, y, z), right half (w, 0, 0), as value * scale +
  // offset.
  bool sceneDeclTypes(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    const char* vsSource = R"(
float4 scale : register(c0);
float4 offset : register(c1);
struct VO { float4 p : POSITION; float4 v : TEXCOORD0; float2 t : TEXCOORD1; };
VO main(float4 p : POSITION, float4 a : TEXCOORD0, float2 t : TEXCOORD1) {
  VO o; o.p = p; o.v = a * scale + offset; o.t = t; return o;
})";
    const char* psSource = R"(
float4 main(float4 v : TEXCOORD0, float2 t : TEXCOORD1) : COLOR {
  return t.x < 0.5 ? float4(v.xyz, 1) : float4(v.w, 0, 0, 1);
})";
    const auto vsCode = compile(vsSource, "vs_3_0");
    const auto psCode = compile(psSource, "ps_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    auto half = [](float f) {
      uint32_t bits; std::memcpy(&bits, &f, 4);
      return WORD(((bits >> 16) & 0x8000) | (((bits >> 23) & 255) - 112) << 10 | ((bits >> 13) & 1023));
    };
    struct Type { BYTE type; uint8_t bytes[16]; float scale, offset; };
    std::vector<Type> types;
    auto add = [&](BYTE type, const void* data, UINT size, float scale, float offset) {
      Type t = { type, {}, scale, offset };
      std::memcpy(t.bytes, data, size);
      types.push_back(t);
    };
    const float f4[4] = { 0.25f, 0.5f, 0.75f, 0.875f };
    const DWORD color = 0x40c08020u;
    const BYTE ubyte4[4] = { 10, 200, 30, 150 };
    const SHORT short4[4] = { 100, -200, 300, -400 };
    const SHORT short4n[4] = { 16000, -16000, 32767, -32768 };
    const WORD ushort4n[4] = { 0x4000, 0xc000, 0xffff, 0x1234 };
    const DWORD udec3 = (2u << 30) | (37u << 20) | (512u << 10) | 1000u;
    const DWORD dec3n = (1u << 30) | ((uint32_t(-511) & 1023u) << 20) | (200u << 10) | (uint32_t(-300) & 1023u);
    const WORD f16[4] = { half(0.25f), half(0.5f), half(0.75f), half(0.875f) };
    add(D3DDECLTYPE_FLOAT1, f4, 4, 1.0f, 0.0f);
    add(D3DDECLTYPE_FLOAT2, f4, 8, 1.0f, 0.0f);
    add(D3DDECLTYPE_FLOAT3, f4, 12, 1.0f, 0.0f);
    add(D3DDECLTYPE_FLOAT4, f4, 16, 1.0f, 0.0f);
    add(D3DDECLTYPE_D3DCOLOR, &color, 4, 1.0f, 0.0f);
    add(D3DDECLTYPE_UBYTE4, ubyte4, 4, 1.0f / 255.0f, 0.0f);
    add(D3DDECLTYPE_SHORT2, short4, 4, 1.0f / 1024.0f, 0.5f);
    add(D3DDECLTYPE_SHORT4, short4, 8, 1.0f / 1024.0f, 0.5f);
    add(D3DDECLTYPE_UBYTE4N, ubyte4, 4, 1.0f, 0.0f);
    add(D3DDECLTYPE_SHORT2N, short4n, 4, 0.5f, 0.5f);
    add(D3DDECLTYPE_SHORT4N, short4n, 8, 0.5f, 0.5f);
    // USHORT2N, USHORT4N, UDEC3 and DEC3N: Windows' NVIDIA driver does not
    // draw them; DXUP's self-test checks them (self_test).
    (void)ushort4n; (void)udec3; (void)dec3n;
    add(D3DDECLTYPE_FLOAT16_2, f16, 4, 1.0f, 0.0f);
    add(D3DDECLTYPE_FLOAT16_4, f16, 8, 1.0f, 0.0f);
    struct V { float x, y, z, w, u, v; uint8_t a[16]; };
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    for (size_t i = 0; i < types.size(); i++) {
      const D3DVERTEXELEMENT9 elements[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 24, types[i].type, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END(),
      };
      IDirect3DVertexDeclaration9* decl = nullptr;
      if (FAILED(d->CreateVertexDeclaration(elements, &decl)))
        continue;
      // The cell in clip space (64x48 target).
      const float x0 = float(i % 8) * 8.0f, y0 = float(i / 8) * 8.0f;
      auto cx = [](float x) { return x / 32.0f - 1.0f; };
      auto cy = [](float y) { return 1.0f - y / 24.0f; };
      V quad[4] = { { cx(x0), cy(y0), 0.5f, 1, 0, 0 }, { cx(x0 + 8), cy(y0), 0.5f, 1, 1, 0 },
                    { cx(x0), cy(y0 + 8), 0.5f, 1, 0, 1 }, { cx(x0 + 8), cy(y0 + 8), 0.5f, 1, 1, 1 } };
      for (auto& v : quad)
        std::memcpy(v.a, types[i].bytes, 16);
      const float scale[4] = { types[i].scale, types[i].scale, types[i].scale, types[i].scale };
      const float offset[4] = { types[i].offset, types[i].offset, types[i].offset, types[i].offset };
      d->SetVertexShaderConstantF(0, scale, 1);
      d->SetVertexShaderConstantF(1, offset, 1);
      d->SetVertexDeclaration(decl);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
      d->SetVertexDeclaration(nullptr);
      decl->Release();
    }
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    ps->Release();
    vs->Release();
    return true;
  }

  // Two elements of a declaration on the same bytes (UE3: COLOR0 and
  // TEXCOORD1 at one offset, lightmap coordinates): both read them.
  bool sceneAliasedElements(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    const char* vsSource = R"(
struct VO { float4 p : POSITION; float4 a : TEXCOORD0; float4 b : TEXCOORD1; };
VO main(float4 p : POSITION, float4 c : COLOR0, float4 t : TEXCOORD1, float4 t2 : TEXCOORD2) {
  VO o; o.p = p; o.a = float4(c.xy, t.xy); o.b = float4(t2.xy, c.zw); return o;
})";
    const char* psSource = R"(
float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1, float2 v : VPOS) : COLOR {
  return v.x < 32 ? float4(a.xyz, 1) : float4(a.w, b.xy, 1);
})";
    const auto vsCode = compile(vsSource, "vs_3_0");
    const auto psCode = compile(psSource, "ps_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 0, 16, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
      { 0, 16, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
      { 0, 20, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
      { 0, 20, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
      D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(d->CreateVertexDeclaration(elements, &decl)))
      return false;
    auto half = [](float f) {
      uint32_t bits; std::memcpy(&bits, &f, 4);
      return WORD(((bits >> 16) & 0x8000) | (((bits >> 23) & 255) - 112) << 10 | ((bits >> 13) & 1023));
    };
    struct V { float x, y, z, w; WORD c[2], t[2]; };
    V quad[4] = { { -1, 1, 0.5f, 1 }, { 1, 1, 0.5f, 1 }, { -1, -1, 0.5f, 1 }, { 1, -1, 0.5f, 1 } };
    for (auto& v : quad) { v.c[0] = half(0.25f); v.c[1] = half(0.75f); v.t[0] = half(0.5f); v.t[1] = half(0.375f); }
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetVertexDeclaration(decl);
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetVertexDeclaration(nullptr);
    decl->Release(); ps->Release(); vs->Release();
    return true;
  }

  // Unreal Engine 3's occlusion tests: boxes with color writes off and a
  // depth test against an FP16 target's own depth buffer, many queries in a
  // row, the results read after Present without a flush. Each result is
  // shown as a bar.
  // Hand-made token streams: def literals that look like comment or end
  // tokens (the shader's length is found instruction by instruction), and
  // malformed or unusual flow control. A bar per shader shows whether the
  // runtime took it; the two valid ones also draw.
  bool sceneShaderTokens(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    constexpr DWORD kPs20 = 0xffff0200u, kPs30 = 0xffff0300u, kEnd = 0x0000ffffu;
    constexpr DWORD kOC0 = 0x800f0800u, kC0 = 0xa0e40000u, kB0 = 0xe0e40800u, kL0 = 0xa0e41000u, kL1 = 0xa0e41001u;
    constexpr DWORD kDefC0 = 0xa00f0000u, kDefC1 = 0xa00f0001u;
    constexpr DWORD kMov = 0x02000001u, kDef = 0x05000051u, kIf = 0x01000028u, kEndIf = 0x0000002bu, kRet = 0x0000001cu;
    constexpr DWORD kLabel = 0x0100001eu, kCall = 0x01000019u, kRep = 0x01000026u, kEndRep = 0x00000027u;
    const std::vector<std::vector<DWORD>> shaders = {
      // 0.99999988 is 0x3f7ffffe: a comment token's low half.
      { kPs20, kDef, kDefC0, 0x3f7ffffeu, 0x3f000000u, 0x3e800000u, 0x3f800000u, kMov, kOC0, kC0, kEnd },
      // A literal equal to the end token.
      { kPs20, kDef, kDefC1, 0x0000ffffu, 0xfffffffeu, 0x0000fffeu, 0x3f800000u,
        kDef, kDefC0, 0x3e800000u, 0x3f800000u, 0x3f000000u, 0x3f800000u, kMov, kOC0, kC0, kEnd },
      // if without its operand.
      { kPs30, kMov, kOC0, kC0, 0x00000028u, kEndIf, kEnd },
      // endif without if.
      { kPs30, kMov, kOC0, kC0, kEndIf, kEnd },
      // rep closed by endif.
      { kPs30, kMov, kOC0, kC0, kRep, 0xf0e40000u, kEndIf, kEnd },
      // A subroutine with a conditional ret.
      { kPs30, kMov, kOC0, kC0, kCall, kL0, kRet, kLabel, kL0, kIf, kB0, kRet, kEndIf, kRet, kEnd },
      // A call of a missing label.
      { kPs30, kMov, kOC0, kC0, kCall, kL1, kRet, kLabel, kL0, kRet, kEnd },
      // A plain subroutine.
      { kPs30, kMov, kOC0, kC0, kCall, kL0, kRet, kLabel, kL0, kRet, kEnd },
    };
    // The ps_2_0 constants: c0 drawn as it is.
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    for (size_t i = 0; i < shaders.size(); i++) {
      IDirect3DPixelShader9* ps = nullptr;
      const bool created = SUCCEEDED(d->CreatePixelShader(shaders[i].data(), &ps)) && ps;
      const float y = 2.0f + float(i) * 4.0f;
      const DWORD color = created ? 0xffffffffu : 0xff404040u;
      const TLVertex bar[4] = { { 2, y, 0.5f, 1, color }, { 20, y, 0.5f, 1, color },
                                { 2, y + 3, 0.5f, 1, color }, { 20, y + 3, 0.5f, 1, color } };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bar, sizeof(TLVertex));
      if (created && i < 2) {
        d->SetPixelShader(ps);
        const TLVertex quad[4] = { { 24, y, 0.5f, 1, 0 }, { 60, y, 0.5f, 1, 0 }, { 24, y + 3, 0.5f, 1, 0 }, { 60, y + 3, 0.5f, 1, 0 } };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
        d->SetPixelShader(nullptr);
      }
      if (ps) ps->Release();
    }
    return true;
  }

  // A depth texture in a fixed-function stage compares with the third
  // coordinate (shadow map): left 0.3 against 0.5, right 0.7.
  bool sceneFfpShadow(IDirect3DDevice9* d) {
    IDirect3DTexture9* depth = nullptr;
    IDirect3DSurface9 *depthSurface = nullptr, *target = nullptr, *back = nullptr, *backDepth = nullptr;
    if (FAILED(d->CreateTexture(32, 32, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &depth, nullptr))
     || FAILED(depth->GetSurfaceLevel(0, &depthSurface))
     || FAILED(d->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)))
      return false;
    d->GetRenderTarget(0, &back);
    d->GetDepthStencilSurface(&backDepth);
    d->SetRenderTarget(0, target);
    d->SetDepthStencilSurface(depthSurface);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 0.5f, 0);
    d->SetRenderTarget(0, back);
    d->SetDepthStencilSurface(backDepth);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff404040u, 1.0f, 0);
    d->SetTexture(0, depth);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
    struct V { float x, y, z, rhw, u, v, w; };
    for (int i = 0; i < 2; i++) {
      const float x0 = i ? 34.0f : 2.0f, x1 = x0 + 28.0f, ref = i ? 0.7f : 0.3f;
      const V v[4] = { { x0, 8, 0.5f, 1, 0, 0, ref }, { x1, 8, 0.5f, 1, 1, 0, ref },
                       { x0, 40, 0.5f, 1, 0, 1, ref }, { x1, 40, 0.5f, 1, 1, 1, ref } };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
    }
    d->SetTexture(0, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    back->Release(); backDepth->Release(); target->Release(); depthSurface->Release(); depth->Release();
    return true;
  }

  // Fixed-function points with their own size (D3DFVF_PSIZE).
  bool scenePointSizes(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    struct V { float x, y, z, rhw, size; DWORD color; };
    const V points[3] = { { 10, 24, 0.5f, 1, 2, 0xffff0000u }, { 30, 24, 0.5f, 1, 8, 0xff00ff00u },
                          { 50, 24, 0.5f, 1, 14, 0xff0000ffu } };
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_POINTLIST, 3, points, sizeof(V));
    return true;
  }

  // ps_1_1 tex t# with D3DTTFF_PROJECTED: the coordinates divided by w
  // whatever the count (Windows' NVIDIA Direct3D 9), w 1 for three
  // coordinates. Top: projected, not; middle: four, three coordinates;
  // bottom: three coordinates, COUNT4 and COUNT2.
  bool scenePs1Projected(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    const auto psCode = assemble("ps_1_1\n tex t0\n mov r0, t0\n");
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DTexture9* texture = nullptr;
    if (psCode.empty() || FAILED(d->CreatePixelShader(psCode.data(), &ps))
     || FAILED(d->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
      return false;
    D3DLOCKED_RECT locked = {};
    if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
      const DWORD texels[4] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };
      for (int y = 0; y < 2; y++)
        std::memcpy(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch, texels + y * 2, 8);
      texture->UnlockRect(0);
    }
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetPixelShader(ps);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
    struct V { float x, y, z, rhw, u, v, s, q; };
    // (0.25, 0.75) * 4 in x, y; z 2, w 4: projected by w -> texel (0, 1) blue,
    // by z -> (0.5, 1.5) clamped -> white; not projected -> (1, 3) -> white.
    // Three coordinates (1, 3, 4): by w -> white, by z -> blue.
    auto quad = [&](float x0, float x1, DWORD flags, float row) {
      d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, flags);
      const V v[4] = { { x0, row, 0.5f, 1, 1, 3, 2, 4 }, { x1, row, 0.5f, 1, 1, 3, 2, 4 },
                       { x0, row + 10, 0.5f, 1, 1, 3, 2, 4 }, { x1, row + 10, 0.5f, 1, 1, 3, 2, 4 } };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
    };
    quad(2, 30, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED, 2);
    quad(34, 62, D3DTTFF_COUNT4, 2);
    quad(2, 30, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED, 20);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
    struct V3 { float x, y, z, rhw, u, v, s; };
    auto quad3 = [&](float x0, float x1, DWORD flags, float row) {
      d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, flags);
      const V3 v[4] = { { x0, row, 0.5f, 1, 1, 3, 4 }, { x1, row, 0.5f, 1, 1, 3, 4 },
                        { x0, row + 10, 0.5f, 1, 1, 3, 4 }, { x1, row + 10, 0.5f, 1, 1, 3, 4 } };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V3));
    };
    quad3(34, 62, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED, 20);
    quad3(2, 30, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED, 34);
    quad3(34, 62, D3DTTFF_COUNT2 | D3DTTFF_PROJECTED, 34);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetPixelShader(nullptr);
    d->SetTexture(0, nullptr);
    ps->Release();
    texture->Release();
    return true;
  }

  // YUY2 and UYVY surfaces (video) copied onto the back buffer: two pixels
  // share U and V; Y, U and V vary apart (BT.601 full range on Windows).
  // Top YUY2, bottom UYVY; 1:1, or stretched 3 times as wide (filtered
  // whatever the filter).
  bool yuvSurfaces(IDirect3DDevice9* d, bool stretch) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    IDirect3DSurface9* back = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
      return false;
    int half = 0;
    for (D3DFORMAT format : { D3DFMT_YUY2, D3DFMT_UYVY }) {
      IDirect3DSurface9* surface = nullptr;
      if (FAILED(d->CreateOffscreenPlainSurface(16, 8, format, D3DPOOL_DEFAULT, &surface, nullptr))) {
        half++;
        continue;
      }
      D3DLOCKED_RECT locked = {};
      if (SUCCEEDED(surface->LockRect(&locked, nullptr, 0))) {
        for (UINT y = 0; y < 8; y++) {
          BYTE* row = static_cast<BYTE*>(locked.pBits) + y * locked.Pitch;
          for (UINT pair = 0; pair < 8; pair++) {
            const BYTE y0 = BYTE(16 + pair * 28 + y * 3), y1 = BYTE(y0 + 9);
            const BYTE u = BYTE(40 + y * 22), v = BYTE(60 + ((y * 3) % 8) * 21);
            BYTE* p = row + pair * 4;
            if (format == D3DFMT_YUY2) { p[0] = y0; p[1] = u; p[2] = y1; p[3] = v; }
            else { p[0] = u; p[1] = y0; p[2] = v; p[3] = y1; }
          }
        }
        surface->UnlockRect();
      }
      const RECT dest = { 0, LONG(half * 24), stretch ? 48 : 16, LONG(half * 24 + 8) };
      d->StretchRect(surface, nullptr, back, &dest, D3DTEXF_POINT);
      surface->Release();
      half++;
    }
    back->Release();
    return true;
  }

  bool sceneYuvSurfaces(IDirect3DDevice9* d) { return yuvSurfaces(d, false); }
  bool sceneYuvStretch(IDirect3DDevice9* d) { return yuvSurfaces(d, true); }

  // Clear inside the scissor rectangle when the scissor test is on, and
  // rectangles with a count of 0 clear nothing.
  bool sceneClearRules(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    const RECT scissor = { 10, 8, 30, 24 };
    d->SetScissorRect(&scissor);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff00ff00u, 1.0f, 0);
    const D3DRECT partly = { 20, 16, 50, 40 };
    d->Clear(1, &partly, D3DCLEAR_TARGET, 0xff0000ffu, 1.0f, 0);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    const D3DRECT ignored = { 40, 4, 60, 14 };
    d->Clear(0, &ignored, D3DCLEAR_TARGET, 0xffff0000u, 1.0f, 0);
    return true;
  }

  // MultiplyTransform: the given matrix times the current one. Bars: white
  // for that order, gray for the other, dark for neither.
  bool sceneMultiplyTransform(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    D3DMATRIX a = {}, b = {};
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        a.m[r][c] = float(r * 4 + c + 1);
        b.m[r][c] = float((r + 2) * (c + 1) % 7) - 3.0f;
      }
    auto product = [](const D3DMATRIX& x, const D3DMATRIX& y) {
      D3DMATRIX p = {};
      for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
          for (int k = 0; k < 4; k++)
            p.m[r][c] += x.m[r][k] * y.m[k][c];
      return p;
    };
    const D3DMATRIX ba = product(b, a), ab = product(a, b);
    int i = 0;
    for (D3DTRANSFORMSTATETYPE state : { D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0 }) {
      D3DMATRIX result = {};
      d->SetTransform(state, &a);
      d->MultiplyTransform(state, &b);
      d->GetTransform(state, &result);
      const DWORD color = std::memcmp(&result, &ba, sizeof(result)) == 0 ? 0xffffffffu
                        : std::memcmp(&result, &ab, sizeof(result)) == 0 ? 0xff808080u : 0xff202020u;
      const D3DRECT bar = { 2, 2 + i * 4, 20, 5 + i * 4 };
      d->Clear(1, &bar, D3DCLEAR_TARGET, color, 1.0f, 0);
      i++;
    }
    return true;
  }

  // Reset while the application holds the back buffer or the automatic
  // depth buffer fails; holding the swap chain does not stop it. Bars:
  // white where Reset succeeded.
  bool sceneResetHeld(IDirect3DDevice9* d) {
    d->EndScene();
    IDirect3DSwapChain9* chain = nullptr;
    D3DPRESENT_PARAMETERS params = {};
    if (FAILED(d->GetSwapChain(0, &chain)) || FAILED(chain->GetPresentParameters(&params)))
      return false;
    HRESULT results[4] = {};
    IDirect3DSurface9* back = nullptr;
    d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DPRESENT_PARAMETERS p = params;
    results[0] = d->Reset(&p);              // back buffer and swap chain held
    if (back) back->Release();
    p = params;
    results[1] = d->Reset(&p);              // swap chain held
    chain->Release();
    IDirect3DSurface9* depth = nullptr;
    d->GetDepthStencilSurface(&depth);
    p = params;
    results[2] = d->Reset(&p);              // automatic depth buffer held
    if (depth) depth->Release();
    p = params;
    results[3] = d->Reset(&p);              // nothing held
    d->BeginScene();
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    for (int i = 0; i < 4; i++) {
      const D3DRECT bar = { 2, 2 + i * 4, 20, 5 + i * 4 };
      d->Clear(1, &bar, D3DCLEAR_TARGET, SUCCEEDED(results[i]) ? 0xffffffffu : 0xff404040u, 1.0f, 0);
    }
    return true;
  }

  bool sceneOcclusionBoxes(IDirect3DDevice9* d) {
    IDirect3DSurface9 *back = nullptr, *backDepth = nullptr, *target = nullptr, *depth = nullptr;
    d->GetRenderTarget(0, &back);
    d->GetDepthStencilSurface(&backDepth);
    IDirect3DSurface9 *half = nullptr, *halfDepth = nullptr, *readback = nullptr;
    if (FAILED(d->CreateRenderTarget(kWidth, kHeight, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr))
     || FAILED(d->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &readback, nullptr))
     || FAILED(d->CreateDepthStencilSurface(kWidth, kHeight, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr))
     || FAILED(d->CreateRenderTarget(kWidth / 2, kHeight / 2, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &half, nullptr))
     || FAILED(d->CreateDepthStencilSurface(kWidth / 2, kHeight / 2, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &halfDepth,
                                           nullptr)))
      return false;
    // The game's shader: w of the FLOAT3 position read from the stream.
    const auto vsCode = assemble("vs_3_0\n dcl_position v0\n dcl_position o0\n mul r0, c1, v0.y\n"
      " mad r0, c0, v0.x, r0\n mad r0, c2, v0.z, r0\n mad o0, c3, v0.w, r0\n");
    const auto psCode = compile("float4 main() : COLOR { return float4(1, 0.5, 0.25, 1); }", "ps_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(d->CreateVertexDeclaration(elements, &decl)))
      return false;
    // The view-projection, its rows in c0-c3.
    const D3DMATRIX proj = perspective(1.0f, 4.0f / 3.0f, 0.5f, 100.0f);
    float columns[16];
    std::memcpy(columns, &proj, sizeof(columns));
    // The boxes: in front of the wall, behind it, beside it, around the
    // camera, through it, far away, partly outside the view.
    struct Box { float x, y, z, h; };
    const Box boxes[] = { { -1.5f, 0.5f, 3, 0.5f }, { -1.5f, -0.5f, 8, 0.6f }, { 1.5f, 0, 8, 0.7f }, { 0, 0, 0, 2 },
                          { -0.3f, 0.2f, 5, 1 }, { 3, 2, 30, 0.8f }, { 2.6f, -1.2f, 4, 1 } };
    const WORD indices[36] = { 0, 1, 2, 2, 1, 3, 4, 6, 5, 5, 6, 7, 0, 2, 4, 4, 2, 6,
                               1, 5, 3, 3, 5, 7, 0, 4, 1, 1, 4, 5, 2, 3, 6, 6, 3, 7 };
    std::vector<IDirect3DQuery9*> queries(2 * (sizeof(boxes) / sizeof(boxes[0])));
    for (auto& query : queries)
      if (FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query)))
        return false;
    std::vector<DWORD> results;
    // As the game polls the last frame's result before testing again:
    // without a flush first.
    auto poll = [&](IDirect3DQuery9* query) {
      DWORD pixels = 0;
      HRESULT hr = S_FALSE;
      for (int tries = 0; tries < 200 && (hr = query->GetData(&pixels, sizeof(pixels), 0)) == S_FALSE; tries++)
        Sleep(1);
      while (hr == S_FALSE)
        hr = query->GetData(&pixels, sizeof(pixels), D3DGETDATA_FLUSH);
      results.push_back(hr == S_OK ? pixels : ~0u);
    };
    const float wall[4][3] = { { -10, 10, 5 }, { 0, 10, 5 }, { -10, -10, 5 }, { 0, -10, 5 } };
    const float screen[4][3] = { { -1, 1, 0.5f }, { 1, 1, 0.5f }, { -1, -1, 0.5f }, { 1, -1, 0.5f } };
    const float identityRows[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    // Three frames: the queries tested in each, read in the next.
    for (int frame = 0; frame < 3; frame++) {
      if (frame) {
        d->EndScene();
        d->Present(nullptr, nullptr, nullptr, nullptr);
        d->BeginScene();
      }
      d->SetRenderTarget(0, target);
      d->SetDepthStencilSurface(depth);
      d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
      d->SetVertexDeclaration(decl);
      d->SetVertexShader(vs);
      d->SetPixelShader(ps);
      d->SetVertexShaderConstantF(0, columns, 4);
      d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
      d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
      d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      // The occluder: the left half at depth 5.
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, wall, 12);
      // A pass at half size with its own depth, then back: a quad over
      // everything without the depth test, as the game draws between.
      d->SetRenderTarget(0, half);
      d->SetDepthStencilSurface(halfDepth);
      d->SetVertexShaderConstantF(0, identityRows, 4);
      d->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screen, 12);
      d->SetRenderTarget(0, target);
      d->SetDepthStencilSurface(depth);
      d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      d->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, screen, 12);
      d->SetVertexShaderConstantF(0, columns, 4);
      d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
      d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
      size_t next = 0;
      for (int round = 0; round < 2; round++) {
        for (const Box& b : boxes) {
          const float o = round * 0.07f, h = b.h;
          float v[8][3];
          for (int i = 0; i < 8; i++) {
            v[i][0] = b.x + o + ((i & 1) ? h : -h);
            v[i][1] = b.y - o + ((i & 2) ? -h : h);
            v[i][2] = b.z + ((i & 4) ? h : -h);
          }
          IDirect3DQuery9* query = queries[next++];
          if (frame)
            poll(query);
          query->Issue(D3DISSUE_BEGIN);
          d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 8, 12, indices, D3DFMT_INDEX16, v, 12);
          // In the last frame the second round reads the target back while
          // the query runs (a submission in the middle).
          if (frame == 2 && round == 1)
            d->GetRenderTargetData(target, readback);
          query->Issue(D3DISSUE_END);
          // The last frame waits for each result right away.
          if (frame == 2) {
            DWORD pixels = 0;
            while (query->GetData(&pixels, sizeof(pixels), D3DGETDATA_FLUSH) == S_FALSE) {}
            results.push_back(pixels);
          }
        }
      }
      // The next pass.
      d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
      d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      d->SetRenderTarget(0, back);
      d->SetDepthStencilSurface(backDepth);
    }
    for (auto* query : queries)
      query->Release();
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetVertexDeclaration(nullptr);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    for (size_t i = 0; i < results.size(); i++) {
      const bool done = results[i] != ~0u;
      const float width = done ? float(results[i]) * kWidth / float(kWidth * kHeight) : 0.0f;
      const float y = float(i) + 3.0f;
      const DWORD color = done ? 0xffffffffu : 0xffff0000u;
      const TLVertex bar[4] = { { 0, y, 0.5f, 1, color }, { width + 1, y, 0.5f, 1, color },
                                { 0, y + 1, 0.5f, 1, color }, { width + 1, y + 1, 0.5f, 1, color } };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bar, sizeof(TLVertex));
    }
    decl->Release(); ps->Release(); vs->Release();
    target->Release(); depth->Release(); half->Release(); halfDepth->Release(); readback->Release();
    back->Release(); backDepth->Release();
    return true;
  }

  bool sceneInstancing(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
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
    // Instances appended to a dynamic buffer draw by draw (StarCraft II's
    // particles): 3 instances, then 4 with every element used twice, then 2.
    IDirect3DVertexBuffer9* ring = nullptr;
    if (SUCCEEDED(d->CreateVertexBuffer(64 * sizeof(Instance), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &ring,
                                        nullptr))) {
      struct Draw { UINT count, divider; DWORD flags; };
      const Draw draws[3] = { { 3, 1, D3DLOCK_DISCARD }, { 4, 2, D3DLOCK_NOOVERWRITE }, { 2, 1, D3DLOCK_NOOVERWRITE } };
      UINT used = 0;
      for (int i = 0; i < 3; i++) {
        const UINT elements = (draws[i].count + draws[i].divider - 1) / draws[i].divider;
        Instance* written = nullptr;
        if (FAILED(ring->Lock(used * sizeof(Instance), elements * sizeof(Instance), reinterpret_cast<void**>(&written), draws[i].flags)))
          break;
        for (UINT e = 0; e < elements; e++) {
          const float x = -0.8f + 0.3f * float(e) + 0.1f * float(i), y = 0.8f - 0.25f * float(i);
          written[e] = { x, y, 0, 0, 0xff000000u | (0x40u << (8 * (e % 3))) | (0x30u * DWORD(i + 1)) };
        }
        ring->Unlock();
        d->SetStreamSource(1, ring, used * sizeof(Instance), sizeof(Instance));
        d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | draws[i].count);
        d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | draws[i].divider);
        d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
        used += elements;
      }
      d->SetStreamSource(1, nullptr, 0, 0);
      ring->Release();
    }
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

  // Sampler states as Unreal Engine 3 sets them: anisotropic minification
  // with a point mip filter, LOD biases (one NaN). A floor at a flat angle
  // per quarter, each mip level one color: which levels the filter reads.
  bool sceneSamplerFilters(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(d->CreateTexture(64, 64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
      return false;
    const DWORD colors[7] = { 0xffff4040u, 0xff40ff40u, 0xff4040ffu, 0xffffff40u, 0xffff40ffu, 0xff40ffffu, 0xffffffffu };
    for (UINT level = 0; level < texture->GetLevelCount(); level++) {
      D3DLOCKED_RECT rect;
      texture->LockRect(level, &rect, nullptr, 0);
      const UINT s = (std::max)(1u, 64u >> level);
      for (UINT y = 0; y < s; y++)
        for (UINT x = 0; x < s; x++)
          reinterpret_cast<DWORD*>(static_cast<BYTE*>(rect.pBits) + y * rect.Pitch)[x] = colors[level % 7];
      texture->UnlockRect(level);
    }
    D3DMATRIX world = identity(), view = translation(0.0f, -0.4f, 0.3f), proj = perspective(1.2f, 4.0f / 3.0f, 0.1f, 50.0f);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetTexture(0, texture);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    const TexVertex floor[4] = {
      { -4.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { 4.0f, 0.0f, 0.0f, 16.0f, 0.0f },
      { -4.0f, 0.0f, 30.0f, 0.0f, 60.0f }, { 4.0f, 0.0f, 30.0f, 16.0f, 60.0f },
    };
    struct Case { DWORD min, mag, mip, anisotropy; float bias; bool nan; };
    const Case cases[4] = {
      { D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, D3DTEXF_POINT, 16, 0.0f, false },
      { D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, D3DTEXF_LINEAR, 8, -1.0f, false },
      { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_POINT, 1, 1.5f, false },
      { D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, D3DTEXF_POINT, 16, 0.0f, true },
    };
    d->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    for (int i = 0; i < 4; i++) {
      const D3DVIEWPORT9 viewport = { DWORD(i & 1) * kWidth / 2, DWORD(i >> 1) * kHeight / 2, kWidth / 2, kHeight / 2, 0.0f, 1.0f };
      d->SetViewport(&viewport);
      const Case& c = cases[i];
      d->SetSamplerState(0, D3DSAMP_MINFILTER, c.min);
      d->SetSamplerState(0, D3DSAMP_MAGFILTER, c.mag);
      d->SetSamplerState(0, D3DSAMP_MIPFILTER, c.mip);
      d->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, c.anisotropy);
      DWORD bias;
      std::memcpy(&bias, &c.bias, 4);
      d->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, c.nan ? 0xffffffffu : bias);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, floor, sizeof(TexVertex));
    }
    const D3DVIEWPORT9 full = { 0, 0, kWidth, kHeight, 0.0f, 1.0f };
    d->SetViewport(&full);
    d->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    d->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1);
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
float4 main(float4 t : TEXCOORD0) : COLOR { float4 v = tex2D(depth, t.xy); return float4(v.r, v.g * 0.5 + v.b * 0.5, v.a, 1); }
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

  // ATI1 and ATI2 (BC4, BC5) filled as games fill them: height rows of the
  // width with the pitch the lock reports (the width, not a row of blocks:
  // Trine 2 wrote past DXUP's buffer with that), sampled onto the halves.
  bool sceneAtiLock(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    const D3DFORMAT formats[3] = { D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '1')), D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2')),
                                   D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2')) };
    // The third third samples ATI2 in a pixel shader.
    const auto psCode = assemble("ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n");
    IDirect3DPixelShader9* ps = nullptr;
    if (psCode.empty() || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    for (int i = 0; i < 3; i++) {
      IDirect3DTexture9* texture = nullptr;
      if (FAILED(d->CreateTexture(32, 32, 1, 0, formats[i], D3DPOOL_MANAGED, &texture, nullptr)))
        return false;
      D3DLOCKED_RECT rect = {};
      if (FAILED(texture->LockRect(0, &rect, nullptr, 0)) || rect.Pitch < 32) {
        texture->Release();
        return false;
      }
      // ATI1 holds half a byte per pixel: the first 16 rows are its blocks.
      const UINT rows = i == 0 ? 16 : 32;
      for (UINT y = 0; y < rows; y++)
        for (UINT x = 0; x < 32; x++)
          static_cast<BYTE*>(rect.pBits)[y * rect.Pitch + x] = BYTE((x * 37 + y * 11 + (x & 4) * 29) ^ (y * 5));
      texture->UnlockRect(0);
      d->SetTexture(0, texture);
      d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      struct TLTex { float x, y, z, rhw; float u, v; };
      const float x0 = float(i * kWidth / 3) - 0.5f, x1 = float((i + 1) * kWidth / 3) - 0.5f;
      const TLTex quad[4] = {
        { x0, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f }, { x1, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
        { x0, float(kHeight) - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f }, { x1, float(kHeight) - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f },
      };
      d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
      d->SetPixelShader(i == 2 ? ps : nullptr);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLTex));
      d->SetPixelShader(nullptr);
      d->SetTexture(0, nullptr);
      texture->Release();
    }
    ps->Release();
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    return true;
  }

  // d3d9.floatEmulation = strict (the float_specials child): mul and mad too.
  bool g_floatStrict = false;

  // 0 times infinity or NaN in dp3 of a pixel shader, and with
  // d3d9.floatEmulation = strict in mul and mad: 0 with Direct3D 9 drivers
  // (IEEE would give NaN, written as black). DXUP's default leaves mul and
  // mad IEEE, as Intel's driver and DXVK where the GPU lacks such a multiply.
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
    for (int i = g_floatStrict ? 0 : 2; i < 3; i++) {
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

  // Dot products with an infinite or NaN factor where the other is 0 give 0
  // on NVIDIA's Direct3D 9. DXUP emulates that only for draws whose
  // shaders read such a constant: the same shaders draw with finite
  // constants, then an infinite pixel shader constant, then a NaN vertex
  // shader constant reaching the pixel shader through a texture
  // coordinate, then finite constants again. Every strip is gray (0.5);
  // a draw that missed the switch gives NaN, black.
  bool sceneFiniteSwitch(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff336699u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    const auto vsCode = assemble("vs_2_0\ndcl_position v0\nmov oPos, v0\nmov oT0, c4\n");
    const auto constantCode = assemble("ps_2_0\nmov r1, c1\ndp3 r0, c0, r1\nmov oC0, r0\n");
    const auto varyingCode = assemble("ps_2_0\ndcl t0\ndp3 r0, t0, c0\nmov oC0, r0\n");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* constantPs = nullptr;
    IDirect3DPixelShader9* varyingPs = nullptr;
    if (vsCode.empty() || constantCode.empty() || varyingCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(constantCode.data(), &constantPs))
     || FAILED(d->CreatePixelShader(varyingCode.data(), &varyingPs)))
      return false;
    d->SetVertexShader(vs);
    d->SetFVF(D3DFVF_XYZ);
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const float weights[4] = { 0.0f, 0.5f, 0.0f, 0.0f };
    const float ones[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float infinite[4] = { inf, 1.0f, 0.0f, 1.0f };
    const float nanCoord[4] = { nan, 1.0f, 0.0f, 1.0f };
    d->SetPixelShaderConstantF(0, weights, 1);
    struct Case { IDirect3DPixelShader9* ps; const float* c1; const float* c4; };
    const Case cases[4] = {
      { constantPs, ones, ones }, { constantPs, infinite, ones }, { varyingPs, infinite, nanCoord }, { constantPs, ones, ones },
    };
    for (int i = 0; i < 4; i++) {
      d->SetPixelShader(cases[i].ps);
      d->SetPixelShaderConstantF(1, cases[i].c1, 1);
      d->SetVertexShaderConstantF(4, cases[i].c4, 1);
      const float x0 = -0.95f + 0.5f * float(i), x1 = x0 + 0.4f;
      const float quad[12] = { x0, 0.8f, 0.5f, x1, 0.8f, 0.5f, x0, -0.8f, 0.5f, x1, -0.8f, 0.5f };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 3 * sizeof(float));
    }
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    vs->Release();
    constantPs->Release();
    varyingPs->Release();
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

  // A render target generating its mipmaps, drawn into (a clear, a triangle
  // fan, a clear in another viewport) and then sampled at levels 0 to 4; a
  // multisampled target resolved into two parts of the back buffer.
  bool sceneAutogenTarget(IDirect3DDevice9* d) {
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(d->CreateTexture(32, 32, 0, D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &texture, nullptr)))
      return false;
    IDirect3DSurface9 *level = nullptr, *back = nullptr, *depth = nullptr;
    texture->GetSurfaceLevel(0, &level);
    d->GetRenderTarget(0, &back);
    d->GetDepthStencilSurface(&depth);
    d->SetRenderTarget(0, level);
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xffff0000u, 1.0f, 0);
    const TLVertex fan[4] = {
      { -0.5f, -0.5f, 0.5f, 1.0f, 0xff00ff00u }, { 15.5f, -0.5f, 0.5f, 1.0f, 0xff00ff00u },
      { 15.5f, 31.5f, 0.5f, 1.0f, 0xff00ff00u }, { -0.5f, 31.5f, 0.5f, 1.0f, 0xff00ff00u },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, fan, sizeof(TLVertex));
    D3DVIEWPORT9 quarter = { 16, 16, 16, 16, 0.0f, 1.0f };
    d->SetViewport(&quarter);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff0000ffu, 1.0f, 0);
    d->SetRenderTarget(0, back);
    d->SetDepthStencilSurface(depth);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020u, 1.0f, 0);
    d->SetTexture(0, texture);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    texturedRect(d, 0.0f, 0.0f, 32.0f, 32.0f);
    texturedRect(d, 36.0f, 0.0f, 44.0f, 8.0f);
    texturedRect(d, 48.0f, 0.0f, 52.0f, 4.0f);
    texturedRect(d, 56.0f, 0.0f, 58.0f, 2.0f);
    d->SetTexture(0, nullptr);
    IDirect3DSurface9* msaa = nullptr;
    if (SUCCEEDED(d->CreateRenderTarget(12, 12, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &msaa, nullptr))) {
      d->SetRenderTarget(0, msaa);
      d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xffffff00u, 1.0f, 0);
      d->SetRenderTarget(0, back);
      const RECT a = { 36, 34, 48, 46 }, b = { 50, 34, 62, 46 };
      d->StretchRect(msaa, nullptr, back, &a, D3DTEXF_NONE);
      d->StretchRect(msaa, nullptr, back, &b, D3DTEXF_NONE);
      msaa->Release();
    }
    if (depth) depth->Release();
    back->Release();
    level->Release();
    texture->Release();
    return true;
  }

  // Draws reading the texture they draw into, each pixel its own texel
  // (StarCraft II's depth of field blurs the scene in place): a float target
  // through a pixel shader, an 8-bit one through the fixed stages; both
  // then shown in the back buffer.
  bool sceneTargetFeedback(IDirect3DDevice9* d) {
    const char* psSource = R"(
sampler2D scene : register(s0);
float4 size : register(c0);
float4 main(float2 pos : VPOS) : COLOR {
  float4 c = tex2Dlod(scene, float4((pos + 0.5) * size.zw, 0, 0));
  return c * float4(0.5, 0.25, 2.0, 1.0) + float4(0.125, 0.25, 0.0625, -0.25);
})";
    const auto psCode = compile(psSource, "ps_3_0");
    IDirect3DPixelShader9* ps = nullptr;
    if (psCode.empty() || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    IDirect3DTexture9 *hdr = nullptr, *ldr = nullptr;
    if (FAILED(d->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &hdr, nullptr))
     || FAILED(d->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ldr, nullptr)))
      return false;
    IDirect3DSurface9 *hdrLevel = nullptr, *ldrLevel = nullptr, *back = nullptr, *depth = nullptr;
    hdr->GetSurfaceLevel(0, &hdrLevel);
    ldr->GetSurfaceLevel(0, &ldrLevel);
    d->GetRenderTarget(0, &back);
    d->GetDepthStencilSurface(&depth);
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    const TLVertex tri[3] = {
      { 2.0f, 1.0f, 0.5f, 1.0f, 0xffffffffu }, { 30.0f, 6.0f, 0.5f, 1.0f, 0xffff4000u }, { 9.0f, 29.0f, 0.5f, 1.0f, 0x8000c0ffu },
    };
    const TLVertex full[4] = {
      { -0.5f, -0.5f, 0.5f, 1.0f, 0xffffffffu }, { 31.5f, -0.5f, 0.5f, 1.0f, 0xffffffffu },
      { -0.5f, 31.5f, 0.5f, 1.0f, 0xffffffffu }, { 31.5f, 31.5f, 0.5f, 1.0f, 0xffffffffu },
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    for (IDirect3DSurface9* level : { hdrLevel, ldrLevel }) {
      d->SetRenderTarget(0, level);
      d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff306090u, 1.0f, 0);
      d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(TLVertex));
    }
    // The float target reads itself through the shader. (A second such
    // draw reads what the first drew or not: Windows' NVIDIA driver gives
    // the texture before the first.)
    d->SetRenderTarget(0, hdrLevel);
    d->SetTexture(0, hdr);
    const float size[4] = { 32.0f, 32.0f, 1.0f / 32.0f, 1.0f / 32.0f };
    d->SetPixelShaderConstantF(0, size, 1);
    d->SetPixelShader(ps);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, full, sizeof(TLVertex));
    d->SetPixelShader(nullptr);
    // The 8-bit target reads itself through the fixed stages (texture
    // times the texture factor).
    d->SetRenderTarget(0, ldrLevel);
    d->SetTexture(0, ldr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    const TLTex1 ffpQuad[4] = {
      { -0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f }, { 31.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
      { -0.5f, 31.5f, 0.5f, 1.0f, 0.0f, 1.0f }, { 31.5f, 31.5f, 0.5f, 1.0f, 1.0f, 1.0f },
    };
    d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff80c040u);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ffpQuad, sizeof(TLTex1));
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    // Both in the back buffer.
    d->SetRenderTarget(0, back);
    d->SetDepthStencilSurface(depth);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTexture(0, hdr);
    texturedRect(d, 0.0f, 0.0f, 32.0f, 32.0f);
    d->SetTexture(0, ldr);
    texturedRect(d, 32.0f, 0.0f, 64.0f, 32.0f);
    d->SetTexture(0, nullptr);
    if (depth) depth->Release();
    back->Release();
    ldrLevel->Release();
    hdrLevel->Release();
    ldr->Release();
    hdr->Release();
    ps->Release();
    return true;
  }

  // A vertex shader reading c[i] relatively, i per quad: constants set
  // (colours), never set (black) and set later past the first ones.
  bool sceneRelativeWindow(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202020u, 1.0f, 0);
    const char* vsSource = R"(
float4 c[96] : register(c0);
struct VO { float4 p : POSITION; float4 col : COLOR0; };
VO main(float4 p : POSITION, float index : TEXCOORD0) { VO o; o.p = p; o.col = c[(int)index]; return o; })";
    const char* psSource = "float4 main(float4 col : COLOR0) : COLOR { return col; }";
    const auto vsCode = compile(vsSource, "vs_3_0");
    const auto psCode = compile(psSource, "ps_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (vsCode.empty() || psCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs))
     || FAILED(d->CreatePixelShader(psCode.data(), &ps)))
      return false;
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE1(0));
    struct V { float x, y, z, w, index; };
    auto quad = [&](int cell, float index) {
      const float x0 = -1.0f + 0.25f * float(cell % 8), y0 = 1.0f - 0.5f * float(cell / 8);
      const V v[4] = { { x0, y0, 0.5f, 1, index }, { x0 + 0.25f, y0, 0.5f, 1, index },
                       { x0, y0 - 0.5f, 0.5f, 1, index }, { x0 + 0.25f, y0 - 0.5f, 0.5f, 1, index } };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
    };
    float colours[8][4];
    for (int i = 0; i < 8; i++) {
      colours[i][0] = float(i & 1);
      colours[i][1] = float((i >> 1) & 1);
      colours[i][2] = float((i >> 2) & 1) * 0.5f + 0.25f;
      colours[i][3] = 1.0f;
    }
    d->SetVertexShaderConstantF(0, &colours[0][0], 8);
    const float indices[8] = { 0, 3, 7, 8, 9, 31, 60, 95 };
    for (int i = 0; i < 8; i++)
      quad(i, indices[i]);
    const float late[4] = { 1.0f, 0.5f, 0.0f, 1.0f };
    d->SetVertexShaderConstantF(60, late, 1);
    for (int i = 0; i < 8; i++)
      quad(8 + i, indices[i]);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    vs->Release();
    ps->Release();
    return true;
  }

  // Fixed-function stages whose operation takes a texture none is bound
  // to: one 8x16 cell per operation (stage 0, then stage 1 after a stage 0
  // adding the texture factor), blended by the result's alpha; a third row
  // of such stages 0 before a stage 1 adding the factor.
  bool sceneFfpNoTexture(IDirect3DDevice9* d) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff204060u, 1.0f, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_TEXTUREFACTOR, 0x60402010u);
    d->SetTexture(0, nullptr);
    d->SetTexture(1, nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    struct Op { D3DTEXTUREOP color; DWORD arg1, arg2; D3DTEXTUREOP alpha; DWORD alphaArg1, alphaArg2; };
    const Op ops[8] = {
      { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },   // the defaults
      { D3DTOP_ADD, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE },
      { D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_TFACTOR },
      { D3DTOP_SELECTARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT, D3DTA_DIFFUSE, D3DTOP_ADD, D3DTA_DIFFUSE, D3DTA_TEXTURE },
      { D3DTOP_SUBTRACT, D3DTA_DIFFUSE, D3DTA_TEXTURE, D3DTOP_SELECTARG1, D3DTA_TFACTOR, D3DTA_TEXTURE },
      { D3DTOP_BLENDTEXTUREALPHA, D3DTA_TFACTOR, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_DIFFUSE },
      { D3DTOP_MODULATE2X, D3DTA_DIFFUSE, D3DTA_TFACTOR, D3DTOP_MODULATE, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE, D3DTA_DIFFUSE },
      { D3DTOP_ADDSIGNED, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SUBTRACT, D3DTA_DIFFUSE, D3DTA_TEXTURE },
    };
    for (int row = 0; row < 2; row++) {
      const DWORD stage = DWORD(row);
      if (row == 1) {
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
      }
      for (int i = 0; i < 8; i++) {
        const Op& op = ops[i];
        d->SetTextureStageState(stage, D3DTSS_COLOROP, op.color);
        d->SetTextureStageState(stage, D3DTSS_COLORARG1, op.arg1);
        d->SetTextureStageState(stage, D3DTSS_COLORARG2, row == 1 && op.arg2 == D3DTA_DIFFUSE ? D3DTA_CURRENT : op.arg2);
        d->SetTextureStageState(stage, D3DTSS_ALPHAOP, op.alpha);
        d->SetTextureStageState(stage, D3DTSS_ALPHAARG1, op.alphaArg1);
        d->SetTextureStageState(stage, D3DTSS_ALPHAARG2, op.alphaArg2);
        const float x0 = float(i * 8), x1 = x0 + 8.0f, y0 = float(row * 16), y1 = y0 + 16.0f;
        const TLVertex quad[4] = {
          { x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0x80ff8040u }, { x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0x80ff8040u },
          { x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0xc040c0ffu }, { x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0xc040c0ffu },
        };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
      }
      d->SetTextureStageState(stage, D3DTSS_COLOROP, row ? D3DTOP_DISABLE : D3DTOP_MODULATE);
    }
    // Third row: stage 0 without its texture, stage 1 adding the factor.
    struct Pair { D3DTEXTUREOP color; DWORD arg1, arg2; D3DTEXTUREOP alpha; DWORD alphaArg1, alphaArg2; bool second; };
    const Pair pairs[4] = {
      { D3DTOP_SUBTRACT, D3DTA_DIFFUSE, D3DTA_TEXTURE, D3DTOP_SELECTARG1, D3DTA_TFACTOR, D3DTA_DIFFUSE, true },
      { D3DTOP_ADD, D3DTA_DIFFUSE, D3DTA_TFACTOR, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE, false },
      { D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_TEXTURE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_TFACTOR, true },
      { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_TEXTURE, true },
    };
    for (int i = 0; i < 4; i++) {
      const Pair& op = pairs[i];
      d->SetTextureStageState(0, D3DTSS_COLOROP, op.color);
      d->SetTextureStageState(0, D3DTSS_COLORARG1, op.arg1);
      d->SetTextureStageState(0, D3DTSS_COLORARG2, op.arg2);
      d->SetTextureStageState(0, D3DTSS_ALPHAOP, op.alpha);
      d->SetTextureStageState(0, D3DTSS_ALPHAARG1, op.alphaArg1);
      d->SetTextureStageState(0, D3DTSS_ALPHAARG2, op.alphaArg2);
      d->SetTextureStageState(1, D3DTSS_COLOROP, op.second ? D3DTOP_ADD : D3DTOP_DISABLE);
      d->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
      d->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
      d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      d->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
      const float x0 = float(i * 16), x1 = x0 + 16.0f, y0 = 32.0f, y1 = 48.0f;
      const TLVertex quad[4] = {
        { x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0x80ff8040u }, { x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0x80ff8040u },
        { x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0xc040c0ffu }, { x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0xc040c0ffu },
      };
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TLVertex));
    }
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
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

  // Objects the application released while bound: the device keeps them
  // until they are unbound (a texture and a vertex buffer drawn after their
  // release), then lets them go (textures of 1 MB created, bound, released
  // and unbound frame by frame must not pile up).
  double privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    return double(counters.PrivateUsage);
  }

  bool sceneReleasedBound(IDirect3DDevice9* d) {
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    struct V { float x, y, z, w, u, v; };
    const V tiny[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 4, 0, 0.5f, 1, 1, 0 }, { 0, 4, 0.5f, 1, 0, 1 }, { 4, 4, 0.5f, 1, 1, 1 } };
    // The memory of the second 300 frames against the first: allocators
    // keep some (a plateau), released textures that stay grow without end.
    double settled = 0.0;
    for (int frame = 0; frame < 600; frame++) {
      if (frame == 300)
        settled = privateBytes();
      IDirect3DTexture9* big = nullptr;
      if (FAILED(d->CreateTexture(512, 512, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &big, nullptr)))
        return false;
      d->SetTexture(0, big);
      big->Release();
      d->BeginScene();
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tiny, sizeof(V));
      d->EndScene();
      d->SetTexture(0, nullptr);
      d->Present(nullptr, nullptr, nullptr, nullptr);
    }
    const double grown = (privateBytes() - settled) / (1024.0 * 1024.0);
    if (grown > 64.0) {
      std::printf("  released_bound: private bytes grew by %.0f MB over another 300 textures of 1 MB\n", grown);
      return false;
    }
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    IDirect3DTexture9* texture = checkerTexture(d, 16);
    IDirect3DVertexBuffer9* vb = nullptr;
    if (!texture || FAILED(d->CreateVertexBuffer(sizeof(V) * 4, D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_TEX1,
                                                 D3DPOOL_MANAGED, &vb, nullptr)))
      return false;
    V* v = nullptr;
    vb->Lock(0, 0, reinterpret_cast<void**>(&v), 0);
    const V quad[4] = { { 16, 16, 0.5f, 1, 0, 0 }, { 112, 16, 0.5f, 1, 1, 0 }, { 16, 112, 0.5f, 1, 0, 1 }, { 112, 112, 0.5f, 1, 1, 1 } };
    std::memcpy(v, quad, sizeof(quad));
    vb->Unlock();
    d->SetTexture(0, texture);
    d->SetStreamSource(0, vb, 0, sizeof(V));
    texture->Release();
    vb->Release();
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    d->SetTexture(0, nullptr);
    d->SetStreamSource(0, nullptr, 0, 0);
    return true;
  }

  // Memory textures take while their levels are locked, filled and
  // uploaded (private bytes: address space in a 32-bit process, where
  // Overlord II ran out of it). Megabytes 32-bit / 64-bit:
  // - 48 MB of DXT5: the application's copy takes the size of its blocks,
  //   not a row of blocks per pixel row (four times the memory). Windows
  //   85-89 / 111-118, DXUP 101 / 252, 247 / 397 with the rows.
  // - 128 MB of A8R8G8B8: levels written once keep no CPU-visible staging
  //   copy. Windows 134 / 271, DXUP 198 / 395, 253 / 387 with the copies
  //   (in 64-bit the buffers kept for reuse take as much).
  bool sceneTextureMemory(IDirect3DDevice9* d) {
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    struct V { float x, y, z, w, u, v; };
    const V tiny[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 4, 0, 0.5f, 1, 1, 0 }, { 0, 4, 0.5f, 1, 0, 1 }, { 4, 4, 0.5f, 1, 1, 1 } };
    // Opaque DXT5 blocks of one colour: alpha 255, colour 0 a green.
    const uint8_t block[16] = { 255, 255, 0, 0, 0, 0, 0, 0, 0xe0, 0x07, 0, 0, 0, 0, 0, 0 };
    const bool smallSpace = sizeof(void*) == 4;
    const struct {
      D3DFORMAT format;
      int count;            // 1024x1024 with mips
      int dataMb;
      double limitMb;
      const char* name;
    } passes[] = {
      { D3DFMT_DXT5, 36, 48, smallSpace ? 160.0 : 330.0, "DXT5" },
      { D3DFMT_A8R8G8B8, 24, 128, smallSpace ? 225.0 : 450.0, "A8R8G8B8" },
    };
    // Kept to the end: memory one pass gave back must not lower the next.
    std::vector<IDirect3DTexture9*> textures;
    IDirect3DTexture9* last[2] = {};
    bool ok = true;
    for (int p = 0; p < 2 && ok; p++) {
      const double before = privateBytes();
      for (int i = 0; i < passes[p].count && ok; i++) {
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(d->CreateTexture(1024, 1024, 0, 0, passes[p].format, D3DPOOL_MANAGED, &texture, nullptr))) {
          ok = false;
          break;
        }
        textures.push_back(texture);
        for (UINT level = 0; level < texture->GetLevelCount(); level++) {
          D3DSURFACE_DESC desc;
          D3DLOCKED_RECT locked;
          texture->GetLevelDesc(level, &desc);
          if (FAILED(texture->LockRect(level, &locked, nullptr, 0))) {
            ok = false;
            break;
          }
          auto* bits = static_cast<uint8_t*>(locked.pBits);
          if (passes[p].format == D3DFMT_DXT5) {
            for (UINT y = 0; y < (desc.Height + 3) / 4; y++)
              for (UINT x = 0; x < (desc.Width + 3) / 4; x++)
                std::memcpy(bits + y * locked.Pitch + x * 16, block, 16);
          } else {
            for (UINT y = 0; y < desc.Height; y++)
              for (UINT x = 0; x < desc.Width; x++)
                reinterpret_cast<uint32_t*>(bits + y * locked.Pitch)[x] = 0xff3060c0u;
          }
          texture->UnlockRect(level);
        }
        // Drawn: uploaded.
        d->SetTexture(0, texture);
        d->BeginScene();
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tiny, sizeof(V));
        d->EndScene();
      }
      d->SetTexture(0, nullptr);
      d->Present(nullptr, nullptr, nullptr, nullptr);
      const double grown = (privateBytes() - before) / (1024.0 * 1024.0);
      if (ok && grown > passes[p].limitMb) {
        std::printf("  texture_memory: private bytes grew by %.0f MB for %d MB of %s textures\n", grown, passes[p].dataMb,
                    passes[p].name);
        ok = false;
      }
      last[p] = textures.empty() ? nullptr : textures.back();
    }
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    if (ok) {
      const V quad[4] = { { 16, 16, 0.5f, 1, 0, 0 }, { 112, 16, 0.5f, 1, 1, 0 }, { 16, 112, 0.5f, 1, 0, 1 }, { 112, 112, 0.5f, 1, 1, 1 } };
      const V inner[4] = { { 48, 48, 0.5f, 1, 0, 0 }, { 80, 48, 0.5f, 1, 1, 0 }, { 48, 80, 0.5f, 1, 0, 1 }, { 80, 80, 0.5f, 1, 1, 1 } };
      d->BeginScene();
      d->SetTexture(0, last[0]);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
      d->SetTexture(0, last[1]);
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, inner, sizeof(V));
      d->EndScene();
      d->SetTexture(0, nullptr);
    }
    for (IDirect3DTexture9* texture : textures)
      texture->Release();
    return ok;
  }

  // Managed textures locked again after they were drawn: in a 32-bit
  // process DXUP lets their copies go once uploaded and reads them back
  // from the GPU then. Read-only locks see what was written, partial
  // writes keep the rest of the level.
  bool sceneRelockedManaged(IDirect3DDevice9* d) {
    struct Case { D3DFORMAT format; UINT size; UINT blockBytes; };   // blockBytes 0: 4 bytes a texel
    const Case cases[] = { { D3DFMT_A8R8G8B8, 128, 0 }, { D3DFMT_DXT1, 256, 8 }, { D3DFMT_DXT5, 128, 16 } };
    auto bytesOf = [](const Case& c, UINT level, UINT& rows, UINT& rowBytes) {
      const UINT s = (std::max)(1u, c.size >> level);
      rows = c.blockBytes ? (s + 3) / 4 : s;
      rowBytes = c.blockBytes ? (s + 3) / 4 * c.blockBytes : s * 4;
    };
    auto pattern = [](UINT seed, UINT row, UINT column) {
      uint32_t v = seed * 2654435761u ^ row * 40503u ^ column * 2246822519u;
      v ^= v >> 13;
      v *= 0x5bd1e995u;
      return uint8_t(v >> 24);
    };
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    struct V { float x, y, z, w, u, v; };
    const V tiny[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 4, 0, 0.5f, 1, 1, 0 }, { 0, 4, 0.5f, 1, 0, 1 }, { 4, 4, 0.5f, 1, 1, 1 } };
    IDirect3DTexture9* textures[3] = {};
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) {
      const Case& c = cases[i];
      if (FAILED(d->CreateTexture(c.size, c.size, 2, 0, c.format, D3DPOOL_MANAGED, &textures[i], nullptr)))
        return false;
      for (UINT level = 0; level < 2; level++) {
        D3DLOCKED_RECT locked;
        UINT rows, rowBytes;
        bytesOf(c, level, rows, rowBytes);
        if (FAILED(textures[i]->LockRect(level, &locked, nullptr, 0)))
          return false;
        for (UINT y = 0; y < rows; y++)
          for (UINT x = 0; x < rowBytes; x++)
            static_cast<uint8_t*>(locked.pBits)[y * locked.Pitch + x] = pattern(i * 2 + level, y, x);
        textures[i]->UnlockRect(level);
      }
      // Drawn: uploaded (and the copies gone in a 32-bit process).
      d->SetTexture(0, textures[i]);
      d->BeginScene();
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tiny, sizeof(V));
      d->EndScene();
    }
    d->SetTexture(0, nullptr);
    d->Present(nullptr, nullptr, nullptr, nullptr);
    for (int i = 0; i < 3 && ok; i++) {
      const Case& c = cases[i];
      // Level 1 read-only, level 0 partly written: the rest stays.
      for (UINT level = 0; level < 2 && ok; level++) {
        D3DLOCKED_RECT locked;
        UINT rows, rowBytes;
        bytesOf(c, level, rows, rowBytes);
        if (FAILED(textures[i]->LockRect(level, &locked, nullptr, level ? D3DLOCK_READONLY : 0)))
          return false;
        for (UINT y = 0; y < rows && ok; y++)
          for (UINT x = 0; x < rowBytes && ok; x++)
            if (static_cast<uint8_t*>(locked.pBits)[y * locked.Pitch + x] != pattern(i * 2 + level, y, x)) {
              std::printf("  relocked_managed: format %u level %u differs at row %u byte %u\n", unsigned(c.format), level, y, x);
              ok = false;
            }
        if (level == 0 && c.format == D3DFMT_A8R8G8B8)
          for (UINT y = 8; y < 24; y++)
            for (UINT x = 8; x < 24; x++)
              reinterpret_cast<DWORD*>(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch)[x] = 0xff40c040u;
        textures[i]->UnlockRect(level);
      }
    }
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    if (ok) {
      // The A8R8G8B8 texture's top-left 32x32 texels, one a pixel.
      const V quad[4] = { { -0.5f, -0.5f, 0.5f, 1, 0, 0 }, { 31.5f, -0.5f, 0.5f, 1, 0.25f, 0 },
                          { -0.5f, 31.5f, 0.5f, 1, 0, 0.25f }, { 31.5f, 31.5f, 0.5f, 1, 0.25f, 0.25f } };
      d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      d->SetTexture(0, textures[0]);
      d->BeginScene();
      d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
      d->EndScene();
      d->SetTexture(0, nullptr);
    }
    for (IDirect3DTexture9* texture : textures)
      if (texture) texture->Release();
    return ok;
  }

  // A managed vertex buffer locked again after it was drawn: in a 32-bit
  // process DXUP lets its copy go once all of it is on the GPU and reads it
  // back then. A read-only lock sees the vertices, a partial write moves
  // one quad and keeps the others.
  bool sceneRelockedVertexBuffer(IDirect3DDevice9* d) {
    struct V { float x, y, z, w; DWORD color; };
    // 64x48 one-pixel quads as triangle lists: 18432 vertices, 368 KB.
    const UINT quads = kWidth * kHeight, vertices = quads * 6;
    IDirect3DVertexBuffer9* vb = nullptr;
    if (FAILED(d->CreateVertexBuffer(vertices * sizeof(V), D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                     D3DPOOL_MANAGED, &vb, nullptr)))
      return false;
    auto quad = [](V* v, float x, float y, DWORD color) {
      const V corners[6] = { { x, y, 0.5f, 1, color }, { x + 1, y, 0.5f, 1, color }, { x, y + 1, 0.5f, 1, color },
                             { x + 1, y, 0.5f, 1, color }, { x + 1, y + 1, 0.5f, 1, color }, { x, y + 1, 0.5f, 1, color } };
      std::memcpy(v, corners, sizeof(corners));
    };
    auto colorOf = [](UINT i) { return 0xff000000u | (i * 2654435761u >> 8); };
    V* v = nullptr;
    if (FAILED(vb->Lock(0, 0, reinterpret_cast<void**>(&v), 0)))
      return false;
    for (UINT i = 0; i < quads; i++)
      quad(v + i * 6, float(i % kWidth) - 0.5f, float(i / kWidth) - 0.5f, colorOf(i));
    vb->Unlock();
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d->SetStreamSource(0, vb, 0, sizeof(V));
    d->BeginScene();
    d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, quads * 2);
    d->EndScene();
    d->Present(nullptr, nullptr, nullptr, nullptr);
    bool ok = true;
    if (FAILED(vb->Lock(0, 0, reinterpret_cast<void**>(&v), D3DLOCK_READONLY)))
      return false;
    for (UINT i = 0; i < quads && ok; i++)
      if (v[i * 6 + 4].color != colorOf(i) || v[i * 6 + 4].x != float(i % kWidth) + 0.5f) {
        std::printf("  relocked_vertex_buffer: quad %u reads 0x%08lx at %.1f\n", i, v[i * 6 + 4].color, v[i * 6 + 4].x);
        ok = false;
      }
    vb->Unlock();
    // Quad 100 (row 1, column 36) becomes white: the rest stays.
    if (FAILED(vb->Lock(100 * 6 * sizeof(V), 6 * sizeof(V), reinterpret_cast<void**>(&v), 0)))
      return false;
    quad(v, float(100 % kWidth) - 0.5f, float(100 / kWidth) - 0.5f, 0xffffffffu);
    vb->Unlock();
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    d->BeginScene();
    d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, quads * 2);
    d->EndScene();
    d->SetStreamSource(0, nullptr, 0, 0);
    vb->Release();
    return ok;
  }

  const Scene kScenes[] = {
    { "msaa_resolve", sceneMsaaResolve, 80 },   // edge coverage: NVIDIA's 4x pattern differs
    { "formats", sceneFormats },
    { "update_texture_query", sceneUpdateTextureAndQuery },
    { "ffp_extras", sceneFixedFunctionExtras, 3 },
    { "cube_volume_shader", sceneCubeVolumeShader },
    { "point_sprites", scenePointSprites },
    { "float_specials", sceneFloatSpecials },
    { "finite_switch", sceneFiniteSwitch },
    { "ati_lock", sceneAtiLock },
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
    { "vertex_ranges", sceneVertexRanges },
    { "l16", sceneL16 },
    { "format_channels", sceneFormatChannels },
    { "decl_types", sceneDeclTypes },
    { "aliased_elements", sceneAliasedElements },
    { "occlusion_boxes", sceneOcclusionBoxes },
    { "shader_tokens", sceneShaderTokens },
    { "reset_held", sceneResetHeld },
    { "multiply_transform", sceneMultiplyTransform },
    { "clear_rules", sceneClearRules },
    { "yuv_surfaces", sceneYuvSurfaces },
    { "yuv_stretch", sceneYuvStretch, 20 },   // Windows' video processor maps the source a little narrower
    { "ps1_projected", scenePs1Projected },
    { "point_sizes", scenePointSizes },
    { "ffp_shadow", sceneFfpShadow },
    { "state_block", sceneStateBlock },
    { "sm3_loops", sceneSm3Loops },
    { "ps_1_4", scenePs14 },
    { "mip_filter", sceneMipFilter, 6 },
    { "sampler_filters", sceneSamplerFilters, 6 },
    { "autogen_target", sceneAutogenTarget },
    { "target_feedback", sceneTargetFeedback },
    { "ffp_no_texture", sceneFfpNoTexture },
    { "relative_window", sceneRelativeWindow },
    { "released_bound", sceneReleasedBound },
    { "texture_memory", sceneTextureMemory },
    { "relocked_managed", sceneRelockedManaged },
    { "relocked_vertex_buffer", sceneRelockedVertexBuffer },
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
    // Mask 16, a game's scene (StarCraft II): per draw 8 vertex and 2 pixel
    // constant registers, 3 of 6 textures, indexed; every 16 draws another
    // of 4 shader pairs and blending on or off.
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DVertexShader9* sceneVs[4] = {};
    IDirect3DPixelShader9* scenePs[4] = {};
    IDirect3DTexture9* sceneTextures[6] = {};
    if (mask & 16) {
      d->CreateIndexBuffer(12, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
      WORD* indices = nullptr;
      ib->Lock(0, 0, reinterpret_cast<void**>(&indices), 0);
      const WORD quad[6] = { 0, 1, 2, 2, 1, 3 };
      std::memcpy(indices, quad, sizeof(quad));
      ib->Unlock();
      for (int k = 0; k < 4; k++) {
        const std::string vsSource = "float4x4 world : register(c0);\nfloat4x4 viewProj : register(c4);\n"
          "struct VO { float4 p : POSITION; float4 c : COLOR0; float2 t : TEXCOORD0; };\n"
          "VO main(float3 p : POSITION, float4 c : COLOR0, float2 t : TEXCOORD0) {\n"
          "  VO o; o.p = mul(mul(float4(p, 1), world), viewProj); o.c = c * " + std::to_string(k + 1) + ".0 / 4.0; o.t = t; return o;\n}\n";
        const std::string psSource = "sampler2D a : register(s0);\nsampler2D b : register(s1);\nsampler2D c : register(s2);\n"
          "float4 k[2] : register(c0);\n"
          "float4 main(float4 col : COLOR0, float2 t : TEXCOORD0) : COLOR {\n"
          "  return (tex2D(a, t) * tex2D(b, t * " + std::to_string(k + 2) + ".0) + tex2D(c, t) * k[1]) * col + k[0];\n}\n";
        const auto vsCode16 = compile(vsSource.c_str(), "vs_2_0");
        const auto psCode16 = compile(psSource.c_str(), "ps_2_0");
        d->CreateVertexShader(vsCode16.data(), &sceneVs[k]);
        d->CreatePixelShader(psCode16.data(), &scenePs[k]);
      }
      for (int k = 0; k < 6; k++)
        sceneTextures[k] = checkerTexture(d, 8u << (k % 3));
    }
    double best = 1e30, total = 0, issueBest = 1e30, issueTotal = 0;
    for (int f = 0; f < frames; f++) {
      LARGE_INTEGER t0, t1, issued, freq;
      QueryPerformanceCounter(&t0);
      d->BeginScene();
      d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
      for (int i = 0; i < draws; i++) {
        D3DMATRIX m = translation(float(i % 17) * 0.1f - 0.8f, float(i % 13) * 0.1f - 0.6f, 0.0f);
        // Typical per-draw work: constants, a texture, a state.
        D3DMATRIX t = { m._11, m._21, m._31, m._41, m._12, m._22, m._32, m._42, m._13, m._23, m._33, m._43, m._14, m._24, m._34, m._44 };
        if (mask & 16) {
          if (i % 16 == 0) {
            const int pair = (i / 16) & 3;
            d->SetVertexShader(sceneVs[pair]);
            d->SetPixelShader(scenePs[pair]);
            d->SetRenderState(D3DRS_ALPHABLENDENABLE, (i / 16) & 1);
          }
          const D3DMATRIX identityMatrix = identity();
          float vsConstants[32];
          std::memcpy(vsConstants, &t._11, 64);
          std::memcpy(vsConstants + 16, &identityMatrix._11, 64);
          const float psConstants[8] = { 0.1f, 0.0f, 0.0f, 0.0f, 1.0f, float(i & 3) / 3.0f, 0.5f, 1.0f };
          if (!(mask & 32)) {   // 32: the constants once only
            d->SetVertexShaderConstantF(0, vsConstants, 8);
            d->SetPixelShaderConstantF(0, psConstants, 2);
          } else if (i == 0) {
            d->SetVertexShaderConstantF(0, vsConstants, 8);
            d->SetPixelShaderConstantF(0, psConstants, 2);
          }
          for (DWORD s = 0; s < 3; s++)
            d->SetTexture(s, sceneTextures[(i + s * 2) % 6]);
          d->SetStreamSource(0, vb, 0, sizeof(V));
          d->SetIndices(ib);
          d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
          continue;
        }
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
      // The calls alone (what a game's thread spends), then the frame with
      // Present (flip-model presents may wait for the display).
      QueryPerformanceCounter(&issued);
      d->Present(nullptr, nullptr, nullptr, nullptr);
      QueryPerformanceCounter(&t1);
      QueryPerformanceFrequency(&freq);
      const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
      const double issue = double(issued.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
      if (f >= 10) {
        best = (std::min)(best, ms);
        total += ms;
        issueBest = (std::min)(issueBest, issue);
        issueTotal += issue;
      }
    }
    const double avg = total / (std::max)(1, frames - 10);
    const double issueAvg = issueTotal / (std::max)(1, frames - 10);
    std::printf("%s: %d draws per frame, calls %.0f ns per draw (avg %.0f), frame %.3f ms avg, %.3f best\n", runtime.name, draws,
                issueBest * 1e6 / draws, issueAvg * 1e6 / draws, avg, best);
    for (DWORD s = 0; s < 3; s++)
      d->SetTexture(s, nullptr);
    d->SetIndices(nullptr);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    for (int k = 0; k < 4; k++) {
      if (sceneVs[k]) sceneVs[k]->Release();
      if (scenePs[k]) scenePs[k]->Release();
    }
    for (auto* texture : sceneTextures)
      if (texture) texture->Release();
    if (ib) ib->Release();
    dynamicVb->Release();
    decl->Release(); vb->Release(); textures[0]->Release(); textures[1]->Release(); ps->Release(); vs->Release();
    d->Release();
    d3d->Release();
    DestroyWindow(window);
    return 0;
  }

  // A benchmark option: the environment variable's value, empty when unset.
  std::string benchOption(const char* name) {
    char value[64] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length && length < sizeof(value) ? std::string(value) : std::string();
  }

  // ---- GPU time of a game's pixel shaders on Windows' Direct3D 9 and DXUP ----

  // Pixel shaders 2.0-3.0 of a d3d9_variants.bin (dxup_pso_cache next to a
  // game), each drawn full screen at 1920x1080 into a half-float target with
  // a vertex shader writing every input it reads, textures of its sampler
  // types and constants in [0, 1]: --bench-shaders FILE [COUNT].
  struct BenchShader {
    uint64_t hash = 0;         // as trace9 and d3d9_shader_test --dump-record name it
    std::vector<DWORD> tokens;
    std::string vs;            // HLSL source
    uint8_t samplers[16] = {}; // 0 none, 2 2D, 3 cube, 4 volume
    UINT outputs = 1;          // render targets written
  };

  std::vector<BenchShader> benchShaderList(const char* path) {
    std::vector<BenchShader> list;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
      return list;
    uint32_t header[2] = {}, head[2];
    if (std::fread(header, sizeof(header), 1, f) != 1) {
      std::fclose(f);
      return list;
    }
    static const char* kUsages[14] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                                       "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
    while (std::fread(head, sizeof(head), 1, f) == 1 && head[1] <= (4u << 20)) {
      std::vector<uint8_t> data(head[1]);
      if (std::fread(data.data(), 1, data.size(), f) != data.size())
        break;
      if (head[0] != 1 || data.size() < 12)
        continue;
      BenchShader s;
      std::memcpy(&s.hash, data.data(), 8);
      s.tokens.resize((data.size() - 8) / 4);
      std::memcpy(s.tokens.data(), data.data() + 8, s.tokens.size() * 4);
      const DWORD version = s.tokens[0];
      if ((version >> 16) != 0xffff || ((version >> 8) & 0xff) < 2)
        continue;
      const bool sm3 = ((version >> 8) & 0xff) >= 3;
      std::string outputs;
      bool ok = true;
      for (size_t i = 1; i < s.tokens.size() && ok;) {
        const DWORD token = s.tokens[i];
        const DWORD opcode = token & 0xffff;
        if (opcode == 0xffff)
          break;
        if (opcode == 0xfffe) {   // comment
          i += 1 + ((token >> 16) & 0x7fff);
          continue;
        }
        const DWORD length = (token >> 24) & 0xf;
        if (opcode == 31 && i + 2 < s.tokens.size()) {   // dcl
          const DWORD usage = s.tokens[i + 1], reg = s.tokens[i + 2];
          const DWORD type = ((reg >> 28) & 7) | ((reg >> 8) & 0x18);
          const DWORD index = reg & 0x7ff;
          if (type == 10 && index < 16)   // sampler
            s.samplers[index] = uint8_t((usage >> 27) & 0xf);
          else if (type == 1 && sm3) {    // input
            const DWORD u = usage & 0x1f, n = (usage >> 16) & 0xf;
            if (u >= 14) { ok = false; break; }
            char line[96];
            sprintf_s(line, "  float4 o%u : %s%u;\n", index, kUsages[u], n);
            outputs += line;
          }
        }
        if (length) {
          // Writes to oC1-oC3 need targets.
          if (opcode != 31 && opcode != 81 && opcode != 47 && i + 1 < s.tokens.size()) {
            const DWORD dst = s.tokens[i + 1];
            const DWORD type = ((dst >> 28) & 7) | ((dst >> 8) & 0x18);
            if (type == 8)
              s.outputs = (std::max)(s.outputs, UINT((dst & 0x7ff) + 1));
          }
          i += 1 + length;
        } else {
          i++;
        }
      }
      if (!ok)
        continue;
      if (!sm3) {
        outputs = "  float4 c0 : COLOR0;\n  float4 c1 : COLOR1;\n";
        for (int t = 0; t < 8; t++)
          outputs += "  float4 t" + std::to_string(t) + " : TEXCOORD" + std::to_string(t) + ";\n";
      }
      s.vs = "struct VO {\n  float4 position : POSITION;\n" + outputs + "};\n"
             "VO main(float4 p : POSITION) {\n  VO o = (VO)0;\n  o.position = p;\n";
      // Every output a different function of the position.
      std::string body;
      size_t at = 0;
      int n = 0;
      while ((at = outputs.find("float4 ", at)) != std::string::npos) {
        at += 7;
        const size_t end = outputs.find(' ', at);
        const std::string name = outputs.substr(at, end - at);
        char line[160];
        sprintf_s(line, "  o.%s = float4(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5, %0.2f, 1) * %0.2f + %0.2f;\n", name.c_str(),
                  0.25 + 0.05 * n, 0.8 + 0.1 * (n % 3), 0.05 * (n % 4));
        body += line;
        n++;
      }
      s.vs += body + "  return o;\n}\n";
      list.push_back(std::move(s));
    }
    std::fclose(f);
    return list;
  }

  // ms per full-screen draw of each shader (negative: not drawn).
  std::vector<double> benchShaderTimes(Runtime& runtime, const std::vector<BenchShader>& shaders) {
    std::vector<double> times(shaders.size(), -1.0);
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
    if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d))) {
      if (d3d) d3d->Release();
      DestroyWindow(window);
      return times;
    }
    // DXUP_BENCH_SIZE=N: N x N pixels (small: what a draw costs besides its pixels).
    const std::string size = benchOption("DXUP_BENCH_SIZE");
    const UINT width = size.empty() ? 1920 : UINT(std::atoi(size.c_str())), height = size.empty() ? 1080 : width;
    IDirect3DSurface9* targets[4] = {};
    for (auto& t : targets)
      d->CreateRenderTarget(width, height, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &t, nullptr);
    // Textures of noise, with mips.
    uint32_t seed = 12345;
    auto noise = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
    IDirect3DTexture9* flat = nullptr;
    IDirect3DCubeTexture9* cube = nullptr;
    IDirect3DVolumeTexture9* volume = nullptr;
    d->CreateTexture(512, 512, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &flat, nullptr);
    d->CreateCubeTexture(128, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr);
    d->CreateVolumeTexture(32, 32, 32, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, nullptr);
    for (UINT level = 0; flat && level < flat->GetLevelCount(); level++) {
      D3DLOCKED_RECT r;
      D3DSURFACE_DESC desc;
      flat->GetLevelDesc(level, &desc);
      if (SUCCEEDED(flat->LockRect(level, &r, nullptr, 0))) {
        for (UINT y = 0; y < desc.Height; y++)
          for (UINT x = 0; x < desc.Width; x++)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits) + y * r.Pitch)[x] = noise();
        flat->UnlockRect(level);
      }
    }
    for (UINT face = 0; cube && face < 6; face++)
      for (UINT level = 0; level < cube->GetLevelCount(); level++) {
        D3DLOCKED_RECT r;
        D3DSURFACE_DESC desc;
        cube->GetLevelDesc(level, &desc);
        if (SUCCEEDED(cube->LockRect(D3DCUBEMAP_FACES(face), level, &r, nullptr, 0))) {
          for (UINT y = 0; y < desc.Height; y++)
            for (UINT x = 0; x < desc.Width; x++)
              reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits) + y * r.Pitch)[x] = noise();
          cube->UnlockRect(D3DCUBEMAP_FACES(face), level);
        }
      }
    for (UINT level = 0; volume && level < volume->GetLevelCount(); level++) {
      D3DLOCKED_BOX b;
      D3DVOLUME_DESC desc;
      volume->GetLevelDesc(level, &desc);
      if (SUCCEEDED(volume->LockBox(level, &b, nullptr, 0))) {
        for (UINT z = 0; z < desc.Depth; z++)
          for (UINT y = 0; y < desc.Height; y++)
            for (UINT x = 0; x < desc.Width; x++)
              reinterpret_cast<DWORD*>(static_cast<BYTE*>(b.pBits) + z * b.SlicePitch + y * b.RowPitch)[x] = noise();
        volume->UnlockBox(level);
      }
    }
    std::vector<float> constants(224 * 4);
    for (float& c : constants)
      c = float(noise() % 1000) / 1000.0f;
    const int loops[16 * 4] = { 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0,
                                4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0, 4, 0, 1, 0 };
    const float quad[4][4] = { { -1, 1, 0.5f, 1 }, { 1, 1, 0.5f, 1 }, { -1, -1, 0.5f, 1 }, { 1, -1, 0.5f, 1 } };
    IDirect3DQuery9* event = nullptr;
    d->CreateQuery(D3DQUERYTYPE_EVENT, &event);
    d->SetFVF(D3DFVF_XYZW);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetPixelShaderConstantF(0, constants.data(), 224);
    d->SetVertexShaderConstantF(0, constants.data(), 224);
    d->SetPixelShaderConstantI(0, loops, 16);
    for (DWORD s = 0; s < 16; s++) {
      d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    }
    auto wait = [&](IDirect3DQuery9* q, void* data, DWORD size) {
      const ULONGLONG start = GetTickCount64();
      while (GetTickCount64() - start < 5000) {
        const HRESULT hr = q->GetData(data, size, D3DGETDATA_FLUSH);
        if (hr == S_OK) return true;
        if (FAILED(hr)) return false;
      }
      return false;
    };
    {
      // Half a second of full-screen clears first: the GPU at its clocks.
      d->SetRenderTarget(0, targets[0]);
      const ULONGLONG start = GetTickCount64();
      BOOL done = FALSE;
      while (GetTickCount64() - start < 500) {
        for (int i = 0; i < 20; i++)
          d->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, i, 0, 0), 1.0f, 0);
        event->Issue(D3DISSUE_END);
        wait(event, &done, sizeof(done));
      }
    }
    for (size_t n = 0; n < shaders.size(); n++) {
      const BenchShader& s = shaders[n];
      const auto vsCode = compile(s.vs.c_str(), (s.tokens[0] >> 8 & 0xff) >= 3 ? "vs_3_0" : "vs_2_0");
      IDirect3DVertexShader9* vs = nullptr;
      IDirect3DPixelShader9* ps = nullptr;
      if (vsCode.empty() || FAILED(d->CreateVertexShader(vsCode.data(), &vs)) || FAILED(d->CreatePixelShader(s.tokens.data(), &ps))) {
        if (!benchOption("DXUP_BENCH_CHECK").empty())
          std::printf("%s shader %zu: %s\n", runtime.name, n, vsCode.empty() ? "vertex shader not compiled" : "shader not created");
        if (vs) vs->Release();
        continue;
      }
      d->SetVertexShader(vs);
      d->SetPixelShader(ps);
      // Added up: Windows' NVIDIA D3D9 leaves out draws that repeat the one
      // before without blending.
      d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
      for (DWORD t = 0; t < 4; t++)
        d->SetRenderTarget(t, t < s.outputs ? targets[t] : nullptr);
      for (DWORD i = 0; i < 16; i++) {
        IDirect3DBaseTexture9* texture = s.samplers[i] == 3 ? static_cast<IDirect3DBaseTexture9*>(cube)
          : s.samplers[i] == 4 ? static_cast<IDirect3DBaseTexture9*>(volume)
          : s.samplers[i] ? static_cast<IDirect3DBaseTexture9*>(flat) : nullptr;
        d->SetTexture(i, texture);
      }
      // Wall time until an event after the draws completes (Windows' D3D9
      // writes timestamps before the draws before them finish): the GPU is
      // idle first, and enough draws that their submission does not count.
      double best = 1e30;
      LARGE_INTEGER hz;
      QueryPerformanceFrequency(&hz);
      for (int round = 0; round < 3; round++) {
        d->BeginScene();
        for (int i = 0; i < 2; i++)
          d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        d->EndScene();
        BOOL done = FALSE;
        event->Issue(D3DISSUE_END);
        wait(event, &done, sizeof(done));
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        d->BeginScene();
        constexpr int kDraws = 200;
        for (int i = 0; i < kDraws; i++)
          d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        d->EndScene();
        event->Issue(D3DISSUE_END);
        if (wait(event, &done, sizeof(done)) && round) {
          QueryPerformanceCounter(&t1);
          best = (std::min)(best, double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(hz.QuadPart) / kDraws);
        }
      }
      if (best < 1e30)
        times[n] = best;
      if (!benchOption("DXUP_BENCH_CHECK").empty() && n < 3) {
        // What the draws wrote: the target's centre and corner.
        IDirect3DSurface9* copy = nullptr;
        if (SUCCEEDED(d->CreateOffscreenPlainSurface(width, height, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &copy, nullptr))
         && SUCCEEDED(d->GetRenderTargetData(targets[0], copy))) {
          D3DLOCKED_RECT r;
          if (SUCCEEDED(copy->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
            const WORD* centre = reinterpret_cast<const WORD*>(static_cast<const BYTE*>(r.pBits) + (height / 2) * r.Pitch) + (width / 2) * 4;
            const WORD* corner = static_cast<const WORD*>(r.pBits);
            std::printf("%s shader %zu: centre %04x %04x %04x %04x corner %04x %04x %04x %04x\n", runtime.name, n, centre[0], centre[1],
                        centre[2], centre[3], corner[0], corner[1], corner[2], corner[3]);
            copy->UnlockRect();
          }
        }
        if (copy) copy->Release();
      }
      d->SetVertexShader(nullptr);
      d->SetPixelShader(nullptr);
      vs->Release();
      ps->Release();
    }
    for (DWORD i = 0; i < 16; i++)
      d->SetTexture(i, nullptr);
    d->SetRenderTarget(0, nullptr);
    if (event)
      event->Release();
    for (auto* t : targets)
      if (t) t->Release();
    if (flat) flat->Release();
    if (cube) cube->Release();
    if (volume) volume->Release();
    d->Release();
    d3d->Release();
    DestroyWindow(window);
    return times;
  }

  int benchShaderFile(Runtime& windows, Runtime& dxup, const char* path, size_t limit) {
    // No screen saver or display off meanwhile: Windows' D3D9 then draws nothing.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    std::vector<BenchShader> shaders = benchShaderList(path);
    if (limit && shaders.size() > limit)
      shaders.resize(limit);
    std::printf("%zu pixel shaders (2.0-3.0) in %s\n", shaders.size(), path);
    // DXUP_BENCH_DXUP_FIRST: DXUP measured first (the GPU's clocks change as it warms).
    const bool dxupFirst = !benchOption("DXUP_BENCH_DXUP_FIRST").empty();
    const std::vector<double> b0 = dxupFirst ? benchShaderTimes(dxup, shaders) : std::vector<double>();
    const std::vector<double> a = !benchOption("DXUP_BENCH_DXUP_ONLY").empty() ? std::vector<double>(shaders.size(), -1.0)
                                                                      : benchShaderTimes(windows, shaders);
    const std::vector<double> b = dxupFirst ? b0 : benchShaderTimes(dxup, shaders);
    struct Row { size_t n; double windows, dxup; };
    std::vector<Row> rows;
    double totalA = 0.0, totalB = 0.0;
    // DXUP_BENCH_DXUP_ONLY (Windows' D3D9 needs the display on): DXUP's times.
    if (!benchOption("DXUP_BENCH_DXUP_ONLY").empty()) {
      double total = 0.0;
      for (size_t n = 0; n < shaders.size(); n++) {
        if (b[n] <= 0.0)
          continue;
        total += b[n];
        std::printf("  shader %4zu (%5zu tokens, %016llX): DXUP %.4f ms\n", n, shaders[n].tokens.size(),
                    static_cast<unsigned long long>(shaders[n].hash), b[n]);
      }
      std::printf("DXUP total %.3f ms\n", total);
      return 0;
    }
    for (size_t n = 0; n < shaders.size(); n++) {
      if (a[n] <= 0.0 || b[n] <= 0.0)
        continue;
      rows.push_back({ n, a[n], b[n] });
      totalA += a[n];
      totalB += b[n];
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.dxup - x.windows > y.dxup - y.windows; });
    for (size_t i = 0; i < rows.size() && i < 20; i++) {
      std::printf("  shader %4zu (%5zu tokens, %016llX): Windows %.4f ms, DXUP %.4f ms (%+.0f %%)\n", rows[i].n,
                  shaders[rows[i].n].tokens.size(), static_cast<unsigned long long>(shaders[rows[i].n].hash), rows[i].windows,
                  rows[i].dxup,
                  (rows[i].dxup / rows[i].windows - 1.0) * 100.0);
    }
    std::printf("%zu drawn on both, 1920x1080 each: Windows %.3f ms, DXUP %.3f ms (%+.1f %%)\n", rows.size(), totalA, totalB,
                totalA > 0 ? (totalB / totalA - 1.0) * 100.0 : 0.0);
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

  // A vertex shader new to DXUP (nonce in its code), drawn with a user clip
  // plane (a variant its creation does not compile ahead) right after its
  // creation. DXUP records the shaders and variants draws use and compiles
  // them from the device's creation on in a later run. Returns 0, or -1.
  int variantChild(Runtime& runtime, unsigned nonce) {
    IDirect3D9* d3d = runtime.create(D3D_SDK_VERSION);
    if (!d3d) return -1;
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
      d3d->Release();
      DestroyWindow(window);
      return -1;
    }
    char source[512];
    std::snprintf(source, sizeof(source), R"(
struct VO { float4 p : POSITION; float4 c : COLOR0; };
VO main(float3 p : POSITION, float4 c : COLOR0) { VO o; o.p = float4(p.xy, 0.5, 1) + float4(%u.0e-9, 0, 0, 0); o.c = c; return o; })",
      nonce);
    const auto code = compile(source, "vs_2_0");
    // What DXUP compiles ahead from earlier runs is done by now; the draw
    // follows the shader's creation at once.
    Sleep(1500);
    IDirect3DVertexShader9* vs = nullptr;
    int result = -1;
    if (!code.empty() && SUCCEEDED(device->CreateVertexShader(code.data(), &vs))) {
      struct V { float x, y, z; DWORD color; };
      const V triangle[3] = { { -1.0f, -1.0f, 0.5f, 0xffffffffu }, { -1.0f, 1.0f, 0.5f, 0xffffffffu }, { 1.0f, -1.0f, 0.5f, 0xffffffffu } };
      const float plane[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
      device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
      device->BeginScene();
      device->SetRenderState(D3DRS_LIGHTING, FALSE);
      device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      device->SetClipPlane(0, plane);
      device->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
      device->SetVertexShader(vs);
      device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
      device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(V));
      device->EndScene();
      device->Present(nullptr, nullptr, nullptr, nullptr);
      device->SetVertexShader(nullptr);
      vs->Release();
      result = 0;
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return result;
  }

  // With saved frames on (dxup_frame_dump.txt): a draw without vertex
  // declaration, a texture Direct3D 9 does not allow (generated mipmaps in
  // system memory), and a draw of application vertices larger than DXUP's
  // ring (drawn: the ring grows).
  int droppedChild(Runtime& runtime) {
    IDirect3D9* d3d = runtime.create(D3D_SDK_VERSION);
    if (!d3d) return -1;
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
      d3d->Release();
      DestroyWindow(window);
      return -1;
    }
    IDirect3DTexture9* texture = nullptr;
    const HRESULT textureHr = device->CreateTexture(64, 64, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                    &texture, nullptr);
    if (texture) texture->Release();
    struct V { float x, y, z; DWORD color; };
    // 10 MB of degenerate triangles, then one over the whole target.
    std::vector<V> big((size_t(10) << 20) / sizeof(V) / 3 * 3);
    for (auto& v : big) v = { 0.0f, 0.0f, 0.5f, 0xffffffffu };
    const V cover[3] = { { -1.0f, -1.0f, 0.5f, 0xffffffffu }, { -1.0f, 3.0f, 0.5f, 0xffffffffu }, { 3.0f, -1.0f, 0.5f, 0xffffffffu } };
    std::copy(cover, cover + 3, big.end() - 3);
    DWORD center = 0;
    for (int frame = 0; frame < 2; frame++) {
      device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
      device->BeginScene();
      device->SetRenderState(D3DRS_LIGHTING, FALSE);
      device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      device->SetVertexDeclaration(nullptr);
      device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, cover, sizeof(V));
      device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
      device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(big.size() / 3), big.data(), sizeof(V));
      device->EndScene();
      if (frame == 1) {
        IDirect3DSurface9* backBuffer = nullptr;
        IDirect3DSurface9* copy = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))
         && SUCCEEDED(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr))
         && SUCCEEDED(device->GetRenderTargetData(backBuffer, copy))) {
          D3DLOCKED_RECT locked = {};
          if (SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            center = static_cast<const DWORD*>(locked.pBits)[(kHeight / 2) * (locked.Pitch / 4) + kWidth / 2];
            copy->UnlockRect();
          }
        }
        if (copy) copy->Release();
        if (backBuffer) backBuffer->Release();
      }
      device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    // Frames whose occlusion queries all see nothing (degenerate triangles),
    // each read in the next frame: the glitch report names it.
    std::vector<IDirect3DQuery9*> queries(20);
    for (auto& query : queries)
      device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
    const V nothing[3] = { { 0.0f, 0.0f, 0.5f, 0xffffffffu }, { 0.0f, 0.0f, 0.5f, 0xffffffffu }, { 0.0f, 0.0f, 0.5f, 0xffffffffu } };
    for (int frame = 0; frame < 35; frame++) {
      device->BeginScene();
      for (auto* query : queries) {
        if (!query) continue;
        if (frame) {
          DWORD pixels = 0;
          while (query->GetData(&pixels, sizeof(pixels), D3DGETDATA_FLUSH) == S_FALSE) {}
        }
        query->Issue(D3DISSUE_BEGIN);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, nothing, sizeof(V));
        query->Issue(D3DISSUE_END);
      }
      device->EndScene();
      device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    for (auto* query : queries)
      if (query) query->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return SUCCEEDED(textureHr) ? 2 : (center & 0xffffff) == 0xffffff ? 0 : 1;
  }

  // Full screen on a scaled monitor: the process becomes per-monitor DPI
  // aware (as with Windows' d3d9.dll) and the window covers the monitor in
  // pixels. 0 ok, 1 not DPI aware, 2 window not the monitor's size, 3 the
  // monitor is not scaled, 4 no device. The window covers the screen for a
  // moment: run on request only.
  int dpiFullscreenChild(Runtime& runtime) {
    HMONITOR monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    GetMonitorInfoW(monitor, &info);
    EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    if (LONG(mode.dmPelsWidth) == info.rcMonitor.right - info.rcMonitor.left)
      return 3;
    IDirect3D9* d3d = runtime.create(D3D_SDK_VERSION);
    if (!d3d) return 4;
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = mode.dmPelsWidth;
    pp.BackBufferHeight = mode.dmPelsHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
      d3d->Release();
      DestroyWindow(window);
      return 4;
    }
    RECT rect = {};
    GetWindowRect(window, &rect);
    const bool aware = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) == DPI_AWARENESS_PER_MONITOR_AWARE;
    const bool covers = rect.right - rect.left == LONG(mode.dmPelsWidth) && rect.bottom - rect.top == LONG(mode.dmPelsHeight);
    std::printf("window %ldx%ld, monitor %lux%lu pixels, per-monitor aware %d\n", rect.right - rect.left, rect.bottom - rect.top,
      mode.dmPelsWidth, mode.dmPelsHeight, int(aware));
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return !aware ? 1 : !covers ? 2 : 0;
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
  const char* benchShadersPath = nullptr;
  size_t benchShadersLimit = 0;
  bool childForcedMsaa = false, identifier = false, childDpi = false, childFloatStrict = false, childDropped = false;
  bool childSelfTest = false, dxupOnly = false;
  unsigned variantNonce = 0;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--dump") == 0) dumpImages = true;
    else if (std::strcmp(argv[i], "--trace") == 0) g_trace = true;
    else if (std::strcmp(argv[i], "--memory") == 0) memory = true;
    else if (std::strcmp(argv[i], "--forced-msaa-child") == 0) childForcedMsaa = true;
    else if (std::strcmp(argv[i], "--identifier") == 0) identifier = true;
    else if (std::strcmp(argv[i], "--dpi-child") == 0) childDpi = true;
    else if (std::strcmp(argv[i], "--float-strict-child") == 0) childFloatStrict = true;
    else if (std::strcmp(argv[i], "--dropped-child") == 0) childDropped = true;
    else if (std::strcmp(argv[i], "--selftest-child") == 0) childSelfTest = true;
    else if (std::strcmp(argv[i], "--dxup-only") == 0) dxupOnly = true;
    else if (std::strcmp(argv[i], "--variant-child") == 0 && i + 1 < argc) variantNonce = unsigned(std::strtoul(argv[++i], nullptr, 10));
    else if (std::strcmp(argv[i], "--bench") == 0 && i + 2 < argc) { benchFrames = std::atoi(argv[i + 1]); benchDraws = std::atoi(argv[i + 2]); i += 2; }
    else if (std::strcmp(argv[i], "--bench-shaders") == 0 && i + 1 < argc) {
      benchShadersPath = argv[++i];
      if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) benchShadersLimit = std::strtoul(argv[++i], nullptr, 10);
    }
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

  if (childDpi)
    return dpiFullscreenChild(dxup);
  if (childDropped)
    return droppedChild(dxup);
  // A device with diagnostics on runs DXUP's self-test.
  if (childSelfTest) {
    IDirect3D9* d3d = dxup.create(D3D_SDK_VERSION);
    HWND window = createWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    const HRESULT hr = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                               &pp, &device) : E_FAIL;
    if (device) device->Release();
    if (d3d) d3d->Release();
    DestroyWindow(window);
    return SUCCEEDED(hr) ? 0 : 1;
  }

  // float_specials with d3d9.floatEmulation = strict (dxup.conf beside the
  // DLLs): 0 matching, 1 differing, 2 failed.
  if (childFloatStrict) {
    g_floatStrict = true;
    for (const Scene& scene : kScenes) {
      if (std::strcmp(scene.name, "float_specials") != 0)
        continue;
      std::vector<DWORD> reference, result;
      std::string error;
      if (!run(windows, scene, reference, error) || !run(dxup, scene, result, error))
        return 2;
      int differing = 0;
      for (size_t i = 0; i < reference.size(); i++)
        differing += reference[i] != result[i];
      std::printf("%d pixels differ\n", differing);
      return differing <= int(reference.size() / 200) ? 0 : 1;
    }
    return 2;
  }

  if (variantNonce)
    return variantChild(dxup, variantNonce) == 0 ? 0 : 1;

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

  if (benchShadersPath)
    return benchShaderFile(windows, dxup, benchShadersPath, benchShadersLimit);

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
    // Without Windows' D3D9 (no display): DXUP's picture only, to look at.
    if (dxupOnly) {
      const bool ran = run(dxup, scene, result, error);
      std::printf("%s %-20s DXUP only%s%s\n", ran ? "ran " : "FAIL", scene.name, ran ? "" : ": ", ran ? "" : error.c_str());
      if (ran)
        writeImage((std::string(scene.name) + "_dxup.ppm").c_str(), result);
      ran ? passed++ : failed++;
      continue;
    }
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
  // Shader variants draws needed are compiled ahead in a later run: the
  // info log names the variants draws compiled (children, as two runs).
  if (!filter || std::strstr("shader_warmup", filter)) {
    const std::wstring marker = dir + L"dxup_info_log.txt";
    const std::wstring log = dir + L"logdxup_dxup_d3d9.log";
    // Draws that compiled or waited for a vertex shader's variant.
    auto compiledAtDraws = [&] {
      int count = 0;
      if (FILE* f = _wfopen(log.c_str(), L"r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), f))
          count += std::strstr(line, "a draw compiled vertex shader") != nullptr
                || (std::strstr(line, "a draw waited") != nullptr && std::strstr(line, "for vertex shader") != nullptr);
        std::fclose(f);
      }
      return count;
    };
    int first = -1, second = -1, firstExit = -1, secondExit = -1;
    if (FILE* f = _wfopen(marker.c_str(), L"w")) {
      std::fclose(f);
      const unsigned nonce = (GetTickCount() ^ (GetCurrentProcessId() << 12)) % 1000000000u + 1;
      const std::string arguments = "--variant-child " + std::to_string(nonce);
      firstExit = runSelf(arguments.c_str());
      first = compiledAtDraws();
      // As after an update that changed the translator: no compiled shader
      // kept, the second run compiles from the recorded code.
      DeleteFileW((dir + L"dxup_pso_cache\\d3d9_shaders.bin").c_str());
      secondExit = runSelf(arguments.c_str());
      second = compiledAtDraws();
      DeleteFileW(marker.c_str());
    }
    const bool ok = firstExit == 0 && secondExit == 0 && first >= 1 && second == 0;
    std::printf("%s %-20s draws compiling or waiting: %d first run, %d second (exit %d, %d)\n", ok ? "ok  " : "FAIL",
                "shader_warmup", first, second, firstExit, secondExit);
    ok ? passed++ : failed++;
  }
  // 0 times infinity or NaN in mul, mad and dp3 with d3d9.floatEmulation =
  // strict, as Direct3D 9 drivers on NVIDIA (a child with dxup.conf).
  if (!filter || std::strstr("float_strict", filter)) {
    const std::wstring conf = dir + L"dxup.conf";
    int code = -1;
    if (FILE* f = _wfopen(conf.c_str(), L"w")) {
      std::fputs("d3d9.floatEmulation = strict\n", f);
      std::fclose(f);
      code = runSelf("--float-strict-child");
      DeleteFileW(conf.c_str());
    }
    const bool ok = code == 0;
    std::printf("%s %-20s mul, mad, dp3 with d3d9.floatEmulation = strict (child exit %d)\n", ok ? "ok  " : "FAIL", "float_strict",
                code);
    ok ? passed++ : failed++;
  }
  // What leaves objects out is logged with saved frames on: a draw DXUP
  // could not draw and a failed call (a child with dxup_frame_dump.txt).
  if (!filter || std::strstr("dropped_report", filter)) {
    const std::wstring marker = dir + L"dxup_frame_dump.txt";
    const std::wstring log = dir + L"logdxup_dxup_d3d9.log";
    int code = -1;
    bool drawLine = false, frameLine = false, callLine = false, ringLine = false, occlusionLine = false;
    if (FILE* f = _wfopen(marker.c_str(), L"w")) {
      std::fclose(f);
      DeleteFileW(log.c_str());
      code = runSelf("--dropped-child");
      DeleteFileW(marker.c_str());
      if (FILE* l = _wfopen(log.c_str(), L"r")) {
        char line[2048];
        while (std::fgets(line, sizeof(line), l)) {
          drawLine |= std::strstr(line, "a draw was not drawn: no vertex declaration") != nullptr;
          frameLine |= std::strstr(line, "draws not drawn (1 without declaration)") != nullptr;
          callLine |= std::strstr(line, "call failed with D3DERR_INVALIDCALL: CreateTexture 64x64") != nullptr
                   && std::strstr(line, "A8R8G8B8, pool 2") != nullptr;
          ringLine |= std::strstr(line, "did not fit") != nullptr;
          occlusionLine |= std::strstr(line, "nearly every occlusion query saw no pixel (now 20 of 20)") != nullptr;
        }
        std::fclose(l);
      }
      // The child's frames are saved as images.
      for (const wchar_t* name : { L"dxup_frame_60.bmp", L"dxup_frame_300.bmp" })
        DeleteFileW((dir + name).c_str());
    }
    const bool ok = code == 0 && drawLine && frameLine && callLine && !ringLine && occlusionLine;
    std::printf("%s %-20s draw line %d, frame line %d, failed call %d, large draw %s, occlusion line %d (child exit %d)\n",
                ok ? "ok  " : "FAIL", "dropped_report", drawLine, frameLine, callLine, ringLine ? "dropped" : "drawn",
                occlusionLine, code);
    ok ? passed++ : failed++;
  }
  // DXUP's self-test of texture formats and vertex element types against
  // Direct3D 9 (with diagnostics on): nothing differs.
  if (!filter || std::strstr("self_test", filter)) {
    const std::wstring marker = dir + L"dxup_frame_dump.txt";
    const std::wstring log = dir + L"logdxup_dxup_d3d9.log";
    int code = -1, differing = 0;
    bool formats = false, types = false;
    if (FILE* f = _wfopen(marker.c_str(), L"w")) {
      std::fclose(f);
      DeleteFileW(log.c_str());
      code = runSelf("--selftest-child");
      DeleteFileW(marker.c_str());
      if (FILE* l = _wfopen(log.c_str(), L"r")) {
        char line[2048];
        while (std::fgets(line, sizeof(line), l)) {
          formats |= std::strstr(line, "texture formats read as with Direct3D 9") != nullptr;
          types |= std::strstr(line, "vertex element types read as with Direct3D 9") != nullptr;
          if (std::strstr(line, "glitch: self-test:")) {
            differing++;
            std::printf("     %s", std::strstr(line, "self-test:"));
          }
        }
        std::fclose(l);
      }
    }
    const bool ok = code == 0 && formats && types && !differing;
    std::printf("%s %-20s texture formats %s, vertex element types %s, %d differences (child exit %d)\n", ok ? "ok  " : "FAIL",
                "self_test", formats ? "ok" : "not checked", types ? "ok" : "not checked", differing, code);
    ok ? passed++ : failed++;
  }
  // Full screen on a scaled monitor (requested by name: covers the screen).
  if (filter && std::strstr("dpi_fullscreen", filter)) {
    const int code = runSelf("--dpi-child");
    if (code == 3) {
      std::printf("skip %-20s the monitor is not scaled\n", "dpi_fullscreen");
    } else {
      const bool ok = code == 0;
      std::printf("%s %-20s %s\n", ok ? "ok  " : "FAIL", "dpi_fullscreen",
        code == 0 ? "per-monitor DPI aware, window covers the monitor" : code == 1 ? "not DPI aware"
          : code == 2 ? "window not the monitor's size" : "no device");
      ok ? passed++ : failed++;
    }
  }
  std::printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
