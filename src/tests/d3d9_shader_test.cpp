// Direct3D 9 shaders through DXUP's translator against Windows' Direct3D 9:
// each case draws a quad with a vertex and a pixel shader on both, the
// translated shaders on Direct3D 11 (Windows' or DXUP's d3d11.dll next to
// this program), and compares the float render targets pixel by pixel.
//
//   d3d9_shader_test [--dump] [case-name-filter]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "d3d9_shader.h"

using namespace dxup::legacy;

namespace {

  constexpr UINT kSize = 16;

  struct Vertex {
    float    position[4];
    float    tex0[4];
    float    tex1[4];
    uint32_t color0;
    uint32_t color1;
    float    normal[4];
    uint8_t  indices[4];
  };

  const Vertex kQuad[4] = {
    { { -1.0f,  1.0f, 0.25f, 1.0f }, { 0.03f, 0.03f, -0.9f, 1.0f }, {  0.3f, -0.7f, 1.5f, 2.0f }, 0xff336699u, 0x80ff0000u, {  0.2f, 0.5f, -0.8f, 0.0f }, { 0, 1, 2, 3 } },
    { {  1.0f,  1.0f, 0.50f, 1.0f }, { 1.03f, 0.03f,  0.4f, 0.5f }, { -1.2f,  0.4f, 0.0f, 3.0f }, 0xff99cc33u, 0x4000ff00u, { -0.6f, 0.1f,  0.3f, 1.0f }, { 3, 2, 1, 0 } },
    { { -1.0f, -1.0f, 0.75f, 1.0f }, { 0.03f, 1.03f,  1.7f, 0.0f }, {  2.5f,  1.1f, -0.3f, 1.0f }, 0xff0000ffu, 0xc00000ffu, {  0.9f, -0.2f,  0.1f, 2.0f }, { 1, 1, 4, 5 } },
    { {  1.0f, -1.0f, 0.90f, 1.0f }, { 1.03f, 1.03f, -0.2f, 0.7f }, { -0.4f, -2.2f,  0.8f, 0.5f }, 0xffffffffu, 0x00808080u, {  0.0f, 0.0f,  1.0f, 3.0f }, { 5, 0, 3, 2 } },
  };

  struct Constants {
    float    c[256][4];
    int32_t  i[16][4];
    uint32_t b[16];
  };

  Constants makeConstants() {
    Constants k = {};
    for (int n = 0; n < 256; n++) {
      k.c[n][0] = float(std::sin(n * 1.37) * 1.5);
      k.c[n][1] = float(std::cos(n * 0.71));
      k.c[n][2] = float((n % 7) * 0.25 - 0.75);
      k.c[n][3] = float(0.5 + (n % 5) * 0.1);
    }
    for (int n = 0; n < 16; n++) {
      k.i[n][0] = n + 2;   // loop count
      k.i[n][1] = n;       // start
      k.i[n][2] = 1;       // step
      k.i[n][3] = 0;
      k.b[n] = (n % 2) == 0;
    }
    return k;
  }

  // Bump matrices of stage 1 and 2: M00, M01, M10, M11.
  const float kBump[8][4] = { {}, { 0.5f, 0.1f, -0.2f, 0.7f }, { -0.3f, 0.6f, 0.25f, 0.4f } };

  uint32_t texel(uint32_t face, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t r = (x * 67 + y * 31 + z * 13 + face * 101) & 0xff;
    const uint32_t g = (x * 19 + y * 89 + z * 47 + face * 7) & 0xff;
    const uint32_t b = (x * 151 + y * 11 + z * 73 + face * 59) & 0xff;
    const uint32_t a = (x * 37 + y * 53 + z * 29 + face * 83 + 64) & 0xff;
    return (a << 24) | (r << 16) | (g << 8) | b;
  }

  // ---- Shader building ----

  struct Blob {
    std::vector<uint32_t> tokens;
    std::string error;
  };

  Blob compileHlsl(const char* source, const char* target) {
    Blob blob;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, "main", target,
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
      blob.error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
    } else {
      blob.tokens.resize(code->GetBufferSize() / 4);
      std::memcpy(blob.tokens.data(), code->GetBufferPointer(), blob.tokens.size() * 4);
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return blob;
  }

  using AssembleShader = HRESULT (WINAPI*)(const char*, UINT, const void*, void*, DWORD, ID3DBlob**, ID3DBlob**);

  Blob assemble(const char* source) {
    static AssembleShader function = [] {
      HMODULE module = LoadLibraryW(L"d3dx9_43.dll");
      return module ? reinterpret_cast<AssembleShader>(GetProcAddress(module, "D3DXAssembleShader")) : nullptr;
    }();
    Blob blob;
    if (!function) {
      blob.error = "d3dx9_43.dll missing";
      return blob;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(function(source, UINT(std::strlen(source)), nullptr, nullptr, 0, &code, &errors))) {
      blob.error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer())) : "assembly failed";
    } else {
      blob.tokens.resize(code->GetBufferSize() / 4);
      std::memcpy(blob.tokens.data(), code->GetBufferPointer(), blob.tokens.size() * 4);
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return blob;
  }

  Blob build(const char* source, const char* target) {
    return target ? compileHlsl(source, target) : assemble(source);
  }

  // ---- Shaders ----

  const char* kVsPass = R"(
struct VI { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 c0 : COLOR0; float4 c1 : COLOR1; float4 n : NORMAL; };
struct VO { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 c0 : COLOR0; float4 c1 : COLOR1; };
VO main(VI i) { VO o; o.p = i.p; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.n; o.c0 = i.c0; o.c1 = i.c1; return o; }
)";

  const char* kVsPass11 = R"(vs_1_1
dcl_position v0
dcl_texcoord0 v1
dcl_texcoord1 v2
dcl_color0 v3
dcl_color1 v4
dcl_normal v5
mov oPos, v0
mov oT0, v1
mov oT1, v2
mov oT2, v5
mov oD0, v3
mov oD1, v4
)";

  const char* kPsShow2 = R"(
float4 main(float4 t0 : TEXCOORD0) : COLOR { return t0; }
)";

  const char* kPsShow3 = R"(
float4 main(float4 t0 : TEXCOORD0) : COLOR { return t0; }
)";

  struct Case {
    const char* name;
    const char* vs;
    const char* vsTarget;   // null: assembly
    const char* ps;
    const char* psTarget;
  };

  const Case kCases[] = {
    // ---- Pixel shaders ----
    { "ps2_arith", kVsPass, "vs_2_0", R"(
float4 k[8] : register(c0);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR {
  float4 r;
  r.x = t0.x * k[3].x + t1.y;
  r.y = rsqrt(abs(t1.z) + 0.1);
  r.z = log2(abs(t1.w) + 0.5);
  r.w = frac(t0.z * 3.7);
  return r * k[1] + k[2].wzyx;
})", "ps_2_0" },
    { "ps2_math", kVsPass, "vs_2_0", R"(
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1, float4 t2 : TEXCOORD2) : COLOR {
  float3 n = normalize(t2.xyz + 0.01);
  float3 x = cross(n, t1.xyz);
  return float4(sin(t0.x * 6.28), cos(t0.y * 3.0), dot(x, n) + exp2(t1.x * 0.5), pow(abs(t1.y) + 0.1, t0.x * 2.0));
})", "ps_2_0" },
    { "ps2_select", kVsPass, "vs_2_0", R"(
float4 k[8] : register(c0);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR {
  float4 a = t1 - 0.2;
  float4 r = a >= 0 ? t0 : k[4];
  r.xy = lerp(r.xy, t1.zw, saturate(t0.y));
  return float4(min(r.xy, t0.zw), max(r.zw, k[5].xy)) + float4(abs(t1.x), sign(t1.y), 0, 0);
})", "ps_2_0" },
    { "ps2_colors", kVsPass, "vs_2_0", R"(
float4 main(float4 c0 : COLOR0, float4 c1 : COLOR1, float4 t0 : TEXCOORD0) : COLOR {
  return c0 * t0.x + c1 * (1.0 - t0.y);
})", "ps_2_0" },
    { "ps2_texture", kVsPass, "vs_2_0", R"(
sampler2D s0 : register(s0);
float4 k[8] : register(c0);
float4 main(float4 t0 : TEXCOORD0) : COLOR {
  return tex2D(s0, t0.xy) * k[0] + tex2D(s0, t0.yx * 2.0);
})", "ps_2_0" },
    { "ps2_texproj", kVsPass, "vs_2_0", R"(
sampler2D s0 : register(s0);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR {
  return tex2Dproj(s0, float4(t0.xy * 3.0, 0, 3.0)) + tex2Dbias(s0, float4(t0.yx, 0, 0.0));
})", "ps_2_0" },
    { "ps2_cube_volume", kVsPass, "vs_2_0", R"(
samplerCUBE s1 : register(s1);
sampler3D s2 : register(s2);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR {
  return texCUBE(s1, float3(t0.x * 2 - 1, t0.y * 2 - 1, 0.6)) * 0.5 + tex3D(s2, float3(t0.xy, t0.x * 0.5 + 0.13));
})", "ps_2_0" },
    { "ps2b_many", kVsPass, "vs_2_0", R"(
float4 k[32] : register(c0);
sampler2D s0 : register(s0);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1, float4 t2 : TEXCOORD2) : COLOR {
  float4 r = 0;
  for (int i = 0; i < 12; i++) r += k[i] * t0.x + k[i + 12] * t1.y;
  return r * tex2D(s0, t0.xy) + t2;
})", "ps_2_b" },
    { "ps3_loop", kVsPass, "vs_3_0", R"(
float4 k[64] : register(c0);
int count : register(i3);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR {
  float4 r = 0;
  for (int i = 0; i < count; i++) r += k[i + 4] * (t0.x + i * 0.1);
  return r;
})", "ps_3_0" },
    { "ps3_branch", kVsPass, "vs_3_0", R"(
float4 k[8] : register(c0);
bool flag : register(b2);
bool off : register(b3);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1, float face : VFACE, float2 pos : VPOS) : COLOR {
  float4 r = float4(pos * 0.0625, face, 1);
  [branch] if (t0.x > 0.5) r.xy = t1.xy; else r.xy -= t1.zw;
  if (flag) r.z += k[2].x;
  if (off) r.w = 7;
  return r;
})", "ps_3_0" },
    { "ps3_derivatives", kVsPass, "vs_3_0", R"(
sampler2D s0 : register(s0);
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR {
  float2 uv = t0.xy * t0.xy * 4.0;
  return float4(ddx(uv) * 4.0, ddy(uv) * 4.0) + tex2Dlod(s0, float4(t0.xy, 0, 0.0)) + tex2Dgrad(s0, t0.yx, ddx(t0.xy), ddy(t0.xy));
})", "ps_3_0" },
    { "ps3_dynamic_loop", kVsPass, "vs_3_0", R"(
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR {
  float4 r = 0;
  int n = (int)(t0.x * 8.0);
  [loop] for (int i = 0; i < n; i++) { r.x += 0.1; if (r.x > 0.45) break; }
  r.y = n;
  return r;
})", "ps_3_0" },
    { "ps3_predicate", kVsPass, "vs_3_0", R"(ps_3_0
def c10, 0.5, 0.25, -1.0, 2.0
dcl_texcoord0 v0
dcl_texcoord1 v1
mov r0, c10
setp_gt p0, v0, c10.x
(p0) mov r0, v1
(!p0.y) add r0.y, r0.y, c10.w
if p0.x
  mul r0.z, r0.z, c10.z
endif
mov oC0, r0
)", nullptr },
    { "ps3_rep_aL", kVsPass, "vs_3_0", R"(ps_3_0
defi i0, 3, 2, 2, 0
defi i1, 4, 0, 0, 0
dcl_texcoord0 v0
mov r0, c0
loop aL, i0
  add r0, r0, v0[aL]
endloop
rep i1
  mul r0.xy, r0.xy, v0.xy
endrep
mov oC0, r0
)", nullptr },
    { "ps1_1", kVsPass11, nullptr, R"(ps_1_1
tex t0
texcoord t1
mul r0, t0, v0
mad r0.rgb, t1, c2, r0
+add_sat r0.a, t0.a, -v1.a
)", nullptr },
    { "ps1_3_bem", kVsPass11, nullptr, R"(ps_1_3
tex t0
texbem t1, t0
mul r0, t1, v0
)", nullptr },
    { "ps1_4", kVsPass11, nullptr, R"(ps_1_4
texld r0, t0
texcrd r1.rgb, t1
mul r0.rgb, r0, r1
add_d2 r0.rgb, r0, c1
cnd r0.a, r0.a, c3.a, c4.a
)", nullptr },
    { "ps1_1_m3x2", kVsPass11, nullptr, R"(ps_1_1
tex t0
texm3x2pad t1, t0_bx2
texm3x2tex t2, t0_bx2
add r0, t2, t0
)", nullptr },

    // ---- Vertex shaders ----
    { "vs2_matrix", R"(
float4x4 m : register(c8);
struct VO { float4 p : POSITION; float4 t0 : TEXCOORD0; };
VO main(float4 p : POSITION, float4 t1 : TEXCOORD1) { VO o; o.p = p; o.t0 = mul(t1, m) * 0.25; return o; }
)", "vs_2_0", kPsShow2, "ps_2_0" },
    { "vs2_math", R"(
struct VO { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; };
VO main(float4 p : POSITION, float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) {
  VO o; o.p = p;
  o.t0 = float4(sin(t1.x), cos(t1.y), rsqrt(abs(t1.z) + 0.01), log(abs(t1.w) + 0.2));
  o.t1 = float4(exp(t0.z), frac(t1.x * 3.3), lit(t1.x, t1.y, t1.w).yz);
  return o;
})", "vs_2_0", R"(
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR { return t0 + t1 * 0.5; }
)", "ps_2_0" },
    { "vs2_relative", R"(
float4 k[64] : register(c0);
struct VO { float4 p : POSITION; float4 t0 : TEXCOORD0; };
VO main(float4 p : POSITION, float4 n : NORMAL) { VO o; o.p = p; o.t0 = k[(int)(n.w + 20.0)] + k[(int)n.w]; return o; }
)", "vs_2_0", kPsShow2, "ps_2_0" },
    { "vs2_ubyte_indices", R"(
float4 k[64] : register(c0);
struct VO { float4 p : POSITION; float4 t0 : TEXCOORD0; };
VO main(float4 p : POSITION, float4 i : BLENDINDICES) { VO o; o.p = p; o.t0 = k[i.x + 30] * 0.5 + k[i.w] + i * 0.01; return o; }
)", "vs_2_0", kPsShow2, "ps_2_0" },
    { "vs3_loop_branch", R"(
float4 k[64] : register(c0);
int count : register(i2);
bool flag : register(b0);
struct VO { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 c0 : COLOR0; };
VO main(float4 p : POSITION, float4 t1 : TEXCOORD1, float4 c : COLOR0) {
  VO o; o.p = p; o.t0 = 0; o.c0 = c;
  for (int i = 0; i < count; i++) o.t0 += k[i + 10] * t1.x;
  if (flag) o.t0.w = -o.t0.w;
  [branch] if (t1.y > 0) o.t0.xy += t1.zw; else o.t0.z = 5;
  return o;
})", "vs_3_0", R"(
float4 main(float4 t0 : TEXCOORD0, float4 c0 : COLOR0) : COLOR { return t0 + c0; }
)", "ps_3_0" },
    { "vs3_packed_outputs", R"(vs_3_0
dcl_position v0
dcl_texcoord1 v1
dcl_position o0
dcl_texcoord0 o1.xy
dcl_texcoord1 o1.zw
dcl_color0 o2
mov o0, v0
mov o1.xy, v1.yx
mov o1.zw, v1.wz
mul o2, v1, c3
)", nullptr, R"(ps_3_0
dcl_texcoord0 v0.xy
dcl_texcoord1 v0.zw
dcl_color0 v1
add r0.xy, v0.xy, v0.zw
mov r1, v1
mul r0.zw, r1.xyzw, v0.wzyx
mov oC0, r0
)", nullptr },
    { "vs1_1_asm", R"(vs_1_1
dcl_position v0
dcl_texcoord0 v1
dcl_texcoord1 v2
dcl_normal v3
mov oPos, v0
mov a0.x, v3.w
mov r0, c[a0.x + 20]
m4x4 r1, v2, c8
expp r2, v1.x
logp r3, v2.y
mad oT0, r1, c0, r0
add oT1.xy, r2.xy, r3.zw
mov oT1.zw, v1
frc r4.xy, v2
mov oT2.xy, r4
mov oT2.zw, v2
)", nullptr, R"(
float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1, float4 t2 : TEXCOORD2) : COLOR { return t0 * 0.3 + t1 * 0.5 + t2; }
)", "ps_2_0" },
  };

  // ---- Direct3D 9 (reference) ----

  struct D3D9Runner {
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* readback = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    IDirect3DVertexBuffer9* vertices = nullptr;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DCubeTexture9* cube = nullptr;
    IDirect3DVolumeTexture9* volume = nullptr;

    bool init(HWND hwnd, const Constants& k) {
      window = hwnd;
      d3d = Direct3DCreate9(D3D_SDK_VERSION);
      if (!d3d) return false;
      D3DPRESENT_PARAMETERS pp = {};
      pp.Windowed = TRUE;
      pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
      pp.BackBufferWidth = kSize;
      pp.BackBufferHeight = kSize;
      pp.BackBufferFormat = D3DFMT_A8R8G8B8;
      pp.hDeviceWindow = hwnd;
      D3DCAPS9 caps = {};
      d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
      std::printf("Direct3D 9: PixelShader1xMaxValue %g, vs %lx, ps %lx\n", caps.PixelShader1xMaxValue,
        caps.VertexShaderVersion & 0xffff, caps.PixelShaderVersion & 0xffff);
      if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)))
        return false;
      if (FAILED(device->CreateRenderTarget(kSize, kSize, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr))
       || FAILED(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &readback, nullptr)))
        return false;
      const D3DVERTEXELEMENT9 elements[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 32, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 48, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        { 0, 52, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1 },
        { 0, 56, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
        { 0, 72, D3DDECLTYPE_UBYTE4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
        D3DDECL_END(),
      };
      if (FAILED(device->CreateVertexDeclaration(elements, &declaration))
       || FAILED(device->CreateVertexBuffer(sizeof(kQuad), 0, 0, D3DPOOL_MANAGED, &vertices, nullptr)))
        return false;
      void* data = nullptr;
      vertices->Lock(0, 0, &data, 0);
      std::memcpy(data, kQuad, sizeof(kQuad));
      vertices->Unlock();

      if (FAILED(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))
       || FAILED(device->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr))
       || FAILED(device->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, nullptr)))
        return false;
      D3DLOCKED_RECT rect;
      texture->LockRect(0, &rect, nullptr, 0);
      for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++)
          reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch)[x] = texel(0, x, y, 0);
      texture->UnlockRect(0);
      for (uint32_t f = 0; f < 6; f++) {
        cube->LockRect(D3DCUBEMAP_FACES(f), 0, &rect, nullptr, 0);
        for (uint32_t y = 0; y < 4; y++)
          for (uint32_t x = 0; x < 4; x++)
            reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch)[x] = texel(f + 1, x, y, 0);
        cube->UnlockRect(D3DCUBEMAP_FACES(f), 0);
      }
      D3DLOCKED_BOX box;
      volume->LockBox(0, &box, nullptr, 0);
      for (uint32_t z = 0; z < 4; z++)
        for (uint32_t y = 0; y < 4; y++)
          for (uint32_t x = 0; x < 4; x++)
            reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(box.pBits) + z * box.SlicePitch + y * box.RowPitch)[x] = texel(9, x, y, z);
      volume->UnlockBox(0);

      device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      device->SetRenderState(D3DRS_ZENABLE, FALSE);
      device->SetRenderState(D3DRS_LIGHTING, FALSE);
      device->SetRenderState(D3DRS_FOGENABLE, FALSE);
      for (DWORD s = 0; s < 4; s++) {
        device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
      }
      for (DWORD s = 1; s < 3; s++) {
        device->SetTextureStageState(s, D3DTSS_BUMPENVMAT00, *reinterpret_cast<const DWORD*>(&kBump[s][0]));
        device->SetTextureStageState(s, D3DTSS_BUMPENVMAT01, *reinterpret_cast<const DWORD*>(&kBump[s][1]));
        device->SetTextureStageState(s, D3DTSS_BUMPENVMAT10, *reinterpret_cast<const DWORD*>(&kBump[s][2]));
        device->SetTextureStageState(s, D3DTSS_BUMPENVMAT11, *reinterpret_cast<const DWORD*>(&kBump[s][3]));
      }
      BOOL bools[16];
      for (int n = 0; n < 16; n++) bools[n] = k.b[n];
      device->SetVertexShaderConstantF(0, &k.c[0][0], 256);
      device->SetPixelShaderConstantF(0, &k.c[0][0], 224);
      device->SetVertexShaderConstantI(0, &k.i[0][0], 16);
      device->SetPixelShaderConstantI(0, &k.i[0][0], 16);
      device->SetVertexShaderConstantB(0, bools, 16);
      device->SetPixelShaderConstantB(0, bools, 16);
      return true;
    }

    // Textures by case: ps_1_x samples the stage's texture with its type.
    void bindTextures(const ShaderInfo9& ps) {
      for (DWORD s = 0; s < 4; s++) {
        IDirect3DBaseTexture9* t = texture;
        if (ps.samplerTypes[s] == SamplerCube) t = cube;
        else if (ps.samplerTypes[s] == SamplerVolume) t = volume;
        device->SetTexture(s, t);
      }
    }

    bool draw(const Blob& vs, const Blob& ps, const ShaderInfo9& psInfo, std::vector<float>& pixels, std::string& error) {
      IDirect3DVertexShader9* vertexShader = nullptr;
      IDirect3DPixelShader9* pixelShader = nullptr;
      if (FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs.tokens.data()), &vertexShader))
       || FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.tokens.data()), &pixelShader))) {
        error = "Direct3D 9 refused a shader";
        if (vertexShader) vertexShader->Release();
        return false;
      }
      device->SetRenderTarget(0, target);
      device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);
      device->BeginScene();
      device->SetVertexDeclaration(declaration);
      device->SetStreamSource(0, vertices, 0, sizeof(Vertex));
      device->SetVertexShader(vertexShader);
      device->SetPixelShader(pixelShader);
      bindTextures(psInfo);
      const HRESULT hr = device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
      device->EndScene();
      vertexShader->Release();
      pixelShader->Release();
      if (FAILED(hr) || FAILED(device->GetRenderTargetData(target, readback))) {
        error = "Direct3D 9 draw failed";
        return false;
      }
      D3DLOCKED_RECT rect;
      readback->LockRect(&rect, nullptr, D3DLOCK_READONLY);
      pixels.resize(kSize * kSize * 4);
      for (UINT y = 0; y < kSize; y++)
        std::memcpy(&pixels[y * kSize * 4], static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch, kSize * 16);
      readback->UnlockRect();
      return true;
    }
  };

  // ---- Direct3D 11 (translated) ----

  struct D3D11Runner {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* readback = nullptr;
    ID3D11Buffer* vertices = nullptr;
    ID3D11Buffer* constants = nullptr;
    ID3D11Buffer* intConstants = nullptr;
    ID3D11Buffer* fixed = nullptr;
    ID3D11ShaderResourceView* views[3] = {};
    ID3D11SamplerState* sampler = nullptr;
    ID3D11RasterizerState* raster = nullptr;

    bool init(const Constants& k) {
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
        return false;
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = kSize;
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(device->CreateTexture2D(&td, nullptr, &target)) || FAILED(device->CreateRenderTargetView(target, nullptr, &rtv)))
        return false;
      td.BindFlags = 0;
      td.Usage = D3D11_USAGE_STAGING;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device->CreateTexture2D(&td, nullptr, &readback)))
        return false;

      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = sizeof(kQuad);
      bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      D3D11_SUBRESOURCE_DATA init = { kQuad };
      if (FAILED(device->CreateBuffer(&bd, &init, &vertices)))
        return false;
      // c[256]; ci[16], cb[4] as uint4.
      bd.ByteWidth = 4096;
      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      init.pSysMem = k.c;
      if (FAILED(device->CreateBuffer(&bd, &init, &constants)))
        return false;
      std::vector<uint8_t> intData(256 + 64 + 16, 0);
      std::memcpy(intData.data(), k.i, 256);
      std::memcpy(intData.data() + 256, k.b, 64);
      intData[320] = 255;   // the last float constant
      bd.ByteWidth = UINT(intData.size());
      init.pSysMem = intData.data();
      if (FAILED(device->CreateBuffer(&bd, &init, &intConstants)))
        return false;
      // Fixed: alpha test, fog color, fog parameters, flags, bump matrices, luminance.
      std::vector<float> fixedData(4 * (4 + 16), 0.0f);
      for (int s = 0; s < 8; s++)
        std::memcpy(&fixedData[16 + s * 4], kBump[s], 16);
      bd.ByteWidth = UINT(fixedData.size() * 4);
      init.pSysMem = fixedData.data();
      if (FAILED(device->CreateBuffer(&bd, &init, &fixed)))
        return false;

      // Textures: B8G8R8A8 like D3DFMT_A8R8G8B8.
      std::vector<uint32_t> texels;
      for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) texels.push_back(texel(0, x, y, 0));
      D3D11_TEXTURE2D_DESC t2 = {};
      t2.Width = t2.Height = 4;
      t2.MipLevels = t2.ArraySize = 1;
      t2.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      t2.SampleDesc.Count = 1;
      t2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA sd[6] = {};
      sd[0] = { texels.data(), 16, 0 };
      ID3D11Texture2D* tex = nullptr;
      if (FAILED(device->CreateTexture2D(&t2, sd, &tex)) || FAILED(device->CreateShaderResourceView(tex, nullptr, &views[0])))
        return false;
      tex->Release();
      std::vector<uint32_t> faces;
      for (uint32_t f = 0; f < 6; f++) for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) faces.push_back(texel(f + 1, x, y, 0));
      for (uint32_t f = 0; f < 6; f++) sd[f] = { faces.data() + f * 16, 16, 0 };
      t2.ArraySize = 6;
      t2.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
      if (FAILED(device->CreateTexture2D(&t2, sd, &tex)) || FAILED(device->CreateShaderResourceView(tex, nullptr, &views[1])))
        return false;
      tex->Release();
      std::vector<uint32_t> voxels;
      for (uint32_t z = 0; z < 4; z++) for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) voxels.push_back(texel(9, x, y, z));
      D3D11_TEXTURE3D_DESC t3 = {};
      t3.Width = t3.Height = t3.Depth = 4;
      t3.MipLevels = 1;
      t3.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      t3.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      sd[0] = { voxels.data(), 16, 64 };
      ID3D11Texture3D* tex3 = nullptr;
      if (FAILED(device->CreateTexture3D(&t3, sd, &tex3)) || FAILED(device->CreateShaderResourceView(tex3, nullptr, &views[2])))
        return false;
      tex3->Release();

      D3D11_SAMPLER_DESC smp = {};
      smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
      smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      smp.MaxLOD = D3D11_FLOAT32_MAX;
      smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
      D3D11_RASTERIZER_DESC rs = {};
      rs.FillMode = D3D11_FILL_SOLID;
      rs.CullMode = D3D11_CULL_NONE;
      rs.DepthClipEnable = TRUE;
      return SUCCEEDED(device->CreateSamplerState(&smp, &sampler)) && SUCCEEDED(device->CreateRasterizerState(&rs, &raster));
    }

    static bool compile(const std::string& hlsl, const char* target, ID3DBlob** code, std::string& error) {
      ID3DBlob* errors = nullptr;
      const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), "translated", nullptr, nullptr, "main", target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, &errors);
      if (FAILED(hr))
        error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
      if (errors) errors->Release();
      return SUCCEEDED(hr);
    }

    bool draw(const std::string& vsHlsl, const ShaderInfo9& vsInfo, const std::string& psHlsl, const ShaderInfo9& psInfo,
              std::vector<float>& pixels, std::string& error) {
      ID3DBlob* vsCode = nullptr;
      ID3DBlob* psCode = nullptr;
      if (!compile(vsHlsl, "vs_5_0", &vsCode, error))
        return false;
      if (!compile(psHlsl, "ps_5_0", &psCode, error)) {
        vsCode->Release();
        return false;
      }
      ID3D11VertexShader* vs = nullptr;
      ID3D11PixelShader* ps = nullptr;
      ID3D11InputLayout* layout = nullptr;
      std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
      static const char* names[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
        "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
      for (const auto& in : vsInfo.inputs) {
        D3D11_INPUT_ELEMENT_DESC e = {};
        e.SemanticName = names[in.usage < 14 ? in.usage : 5];
        e.SemanticIndex = in.usageIndex;
        e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        if (in.usage == UsagePosition) { e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; e.AlignedByteOffset = 0; }
        else if (in.usage == UsageTexCoord && in.usageIndex == 0) { e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; e.AlignedByteOffset = 16; }
        else if (in.usage == UsageTexCoord && in.usageIndex == 1) { e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; e.AlignedByteOffset = 32; }
        else if (in.usage == UsageColor && in.usageIndex == 0) { e.Format = DXGI_FORMAT_B8G8R8A8_UNORM; e.AlignedByteOffset = 48; }
        else if (in.usage == UsageColor && in.usageIndex == 1) { e.Format = DXGI_FORMAT_B8G8R8A8_UNORM; e.AlignedByteOffset = 52; }
        else if (in.usage == UsageNormal) { e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; e.AlignedByteOffset = 56; }
        else if (in.usage == UsageBlendIndices) { e.Format = DXGI_FORMAT_R8G8B8A8_UINT; e.AlignedByteOffset = 72; }
        else { error = "no vertex element for an input"; break; }
        elements.push_back(e);
      }
      HRESULT hr = error.empty() ? device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs) : E_FAIL;
      if (SUCCEEDED(hr)) hr = device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps);
      if (SUCCEEDED(hr)) hr = device->CreateInputLayout(elements.data(), UINT(elements.size()), vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout);
      vsCode->Release();
      psCode->Release();
      if (FAILED(hr)) {
        if (error.empty()) error = "Direct3D 11 refused a shader or the layout";
        if (vs) vs->Release();
        if (ps) ps->Release();
        return false;
      }
      const float clear[4] = {};
      context->ClearRenderTargetView(rtv, clear);
      context->OMSetRenderTargets(1, &rtv, nullptr);
      // Direct3D 9 puts pixel centers on integers: half a pixel right and down.
      D3D11_VIEWPORT viewport = { 0.5f, 0.5f, float(kSize), float(kSize), 0.0f, 1.0f };
      context->RSSetViewports(1, &viewport);
      context->RSSetState(raster);
      context->IASetInputLayout(layout);
      const UINT stride = sizeof(Vertex), offset = 0;
      context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context->VSSetShader(vs, nullptr, 0);
      context->PSSetShader(ps, nullptr, 0);
      ID3D11Buffer* buffers[3] = { constants, fixed, intConstants };
      ID3D11Buffer* vsBuffers[3] = { constants, nullptr, intConstants };
      context->VSSetConstantBuffers(0, 3, vsBuffers);
      context->PSSetConstantBuffers(0, 3, buffers);
      ID3D11ShaderResourceView* srvs[4];
      ID3D11SamplerState* samplers[4];
      for (int s = 0; s < 4; s++) {
        srvs[s] = psInfo.samplerTypes[s] == SamplerCube ? views[1] : psInfo.samplerTypes[s] == SamplerVolume ? views[2] : views[0];
        samplers[s] = sampler;
      }
      context->PSSetShaderResources(0, 4, srvs);
      context->PSSetSamplers(0, 4, samplers);
      context->Draw(4, 0);
      context->CopyResource(readback, target);
      vs->Release();
      ps->Release();
      layout->Release();
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (FAILED(context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "readback failed";
        return false;
      }
      pixels.resize(kSize * kSize * 4);
      for (UINT y = 0; y < kSize; y++)
        std::memcpy(&pixels[y * kSize * 4], static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, kSize * 16);
      context->Unmap(readback, 0);
      (void)vsInfo;
      return true;
    }
  };

  // Compile time of translated shaders per optimization level.
  struct CompileTimes {
    double   ms[3] = {};
    size_t   bytes[3] = {};
    void add(const std::string& hlsl, const char* target) {
      static const UINT flags[3] = { D3DCOMPILE_OPTIMIZATION_LEVEL3, D3DCOMPILE_OPTIMIZATION_LEVEL1, D3DCOMPILE_SKIP_OPTIMIZATION };
      for (int level = 0; level < 3; level++) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 4; i++) {
          ID3DBlob* code = nullptr;
          ID3DBlob* errors = nullptr;
          if (SUCCEEDED(D3DCompile(hlsl.data(), hlsl.size(), "translated", nullptr, nullptr, "main", target, flags[level], 0, &code, &errors))) {
            if (i == 0) bytes[level] += code->GetBufferSize();
            code->Release();
          }
          if (errors) errors->Release();
        }
        ms[level] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 4;
      }
    }
  };

  void dump(const char* title, const std::string& text) {
    std::printf("----- %s -----\n%s\n", title, text.c_str());
  }

}

int main(int argc, char** argv) {
  bool dumpAll = false, compileBench = false, floatEmulation = false;
  const char* filter = nullptr;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--dump") == 0) dumpAll = true;
    else if (std::strcmp(argv[i], "--compile-bench") == 0) compileBench = true;
    else if (std::strcmp(argv[i], "--float-emulation") == 0) floatEmulation = true;
    else filter = argv[i];
  }
  CompileTimes times;

  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"dxup_d3d9_shader_test";
  RegisterClassW(&wc);
  HWND window = CreateWindowW(wc.lpszClassName, L"d3d9 shader test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

  const Constants k = makeConstants();
  D3D9Runner d3d9;
  D3D11Runner d3d11;
  if (!d3d9.init(window, k)) { std::printf("FAIL: Direct3D 9 setup\n"); return 1; }
  if (!d3d11.init(k)) { std::printf("FAIL: Direct3D 11 setup\n"); return 1; }

  int passed = 0, failed = 0;
  for (const Case& c : kCases) {
    if (filter && !std::strstr(c.name, filter))
      continue;
    std::string error;
    const Blob vs = build(c.vs, c.vsTarget);
    const Blob ps = build(c.ps, c.psTarget);
    if (!vs.error.empty() || !ps.error.empty()) {
      std::printf("FAIL %-22s source: %s%s\n", c.name, vs.error.c_str(), ps.error.c_str());
      failed++;
      continue;
    }
    ShaderInfo9 psInfo, vsInfo;
    std::string psHlsl, vsHlsl;
    TranslateOptions9 options;
    options.floatEmulation = options.dotEmulation = floatEmulation;
    if (!translateShader9(ps.tokens.data(), ps.tokens.size(), options, psHlsl, psInfo, error)) {
      std::printf("FAIL %-22s pixel translation: %s\n", c.name, error.c_str());
      failed++;
      continue;
    }
    options.uintInputs = 0;
    // BLENDINDICES (UBYTE4) feeds integers.
    for (int pass = 0; pass < 2; pass++) {
      if (!translateShader9(vs.tokens.data(), vs.tokens.size(), options, vsHlsl, vsInfo, error))
        break;
      uint16_t uints = 0;
      for (const auto& in : vsInfo.inputs)
        if (in.usage == UsageBlendIndices) uints |= uint16_t(1u << in.reg);
      if (uints == options.uintInputs && vsInfo.varyingCount >= psInfo.varyingCount)
        break;
      options.uintInputs = uints;
      options.minVaryings = psInfo.varyingCount;
    }
    if (!error.empty()) {
      std::printf("FAIL %-22s vertex translation: %s\n", c.name, error.c_str());
      failed++;
      continue;
    }
    if (compileBench) {
      const double before[3] = { times.ms[0], times.ms[1], times.ms[2] };
      times.add(vsHlsl, "vs_5_0");
      const double vsMs = times.ms[0] - before[0];
      times.add(psHlsl, "ps_5_0");
      std::printf("     %-22s O3 vs %.1f ps %.1f ms, O1 %.1f, skip %.1f\n", c.name, vsMs, times.ms[0] - before[0] - vsMs,
        times.ms[1] - before[1], times.ms[2] - before[2]);
    }
    std::vector<float> reference, result;
    if (!d3d9.draw(vs, ps, psInfo, reference, error) || !d3d11.draw(vsHlsl, vsInfo, psHlsl, psInfo, result, error)) {
      std::printf("FAIL %-22s %s\n", c.name, error.c_str());
      dump("vertex", vsHlsl);
      dump("pixel", psHlsl);
      failed++;
      continue;
    }
    float worst = 0.0f;
    int worstIndex = 0;
    for (size_t i = 0; i < reference.size(); i++) {
      const float a = reference[i], b = result[i];
      float d = std::fabs(a - b) / (1.0f + std::fabs(a));
      if (a != a || b != b) d = (a != a) == (b != b) ? 0.0f : 1.0f;
      if (d > worst) { worst = d; worstIndex = int(i); }
    }
    const bool ok = worst <= 2e-3f;
    const int px = worstIndex / 4;
    std::printf("%s %-22s max error %.2e", ok ? "ok  " : "FAIL", c.name, worst);
    if (!ok)
      std::printf(" at (%d, %d).%c: Direct3D 9 %.6g, DXUP %.6g", px % kSize, px / kSize, "xyzw"[worstIndex % 4],
        reference[worstIndex], result[worstIndex]);
    double magnitude = 0.0;
    size_t distinct = 0;
    for (size_t i = 0; i < reference.size(); i++) {
      magnitude += std::fabs(reference[i]);
      if (i >= 4 && reference[i] != reference[i - 4]) distinct++;
    }
    std::printf("  mean |x| %.3f, %zu changes", magnitude / reference.size(), distinct);
    std::printf("  [vs %u.%u %u slots, ps %u.%u %u slots]\n", vsInfo.major, vsInfo.minor, vsInfo.varyingCount,
      psInfo.major, psInfo.minor, psInfo.varyingCount);
    if (!ok || dumpAll) {
      dump("vertex", vsHlsl);
      dump("pixel", psHlsl);
    }
    ok ? passed++ : failed++;
  }
  if (compileBench)
    for (int level = 0; level < 3; level++)
      std::printf("compile %s: %.2f ms, %zu bytes\n", level == 0 ? "O3  " : level == 1 ? "O1  " : "skip", times.ms[level], times.bytes[level]);
  std::printf("%d passed, %d failed\n", passed, failed);
  DestroyWindow(window);
  return failed ? 1 : 0;
}
