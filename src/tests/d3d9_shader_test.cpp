// Direct3D 9 shaders through DXUP's translator against Windows' Direct3D 9:
// each case draws a quad with a vertex and a pixel shader on both, the
// translated shaders on Direct3D 11 (Windows' or DXUP's d3d11.dll next to
// this program), and compares the float render targets pixel by pixel.
//
//   d3d9_shader_test [--dump] [--dxbc] [--float-emulation] [--compile-bench] [case-name-filter]
//     --dxbc: DXUP's DXBC compiler (d3d9_dxbc.cpp) in place of HLSL and fxc
//   d3d9_shader_test --compile-records FILE        fxc time per float mode
//   d3d9_shader_test --compare-records FILE        fxc's DXBC against DXUP's, drawn alike
//   d3d9_shader_test --bench-records FILE          GPU time of both, pixel shaders at 1920x1080
//   d3d9_shader_test --fuzz COUNT [SEED] [--verbose]
//     random shaders of every model through both (DXUP_FUZZ_MODEL one model,
//     DXUP_FUZZ_ONE an index with every output, DXUP_FXC_SKIP_OPTIMIZATION
//     fxc without its optimizer: it folds some expressions wrongly)
//   FILE: a d3d9_variants.bin (dxup_pso_cache next to a game).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <map>
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
    std::vector<uint8_t> intData;   // ci[16], cb[4], last constant, vertex input modes
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
      intData.assign(256 + 64 + 16, 0);
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

    static bool blob(const std::vector<uint8_t>& bytes, ID3DBlob** code) {
      if (FAILED(D3DCreateBlob(bytes.size(), code)))
        return false;
      std::memcpy((*code)->GetBufferPointer(), bytes.data(), bytes.size());
      return true;
    }

    // HLSL compiled with fxc, or DXBC from DXUP's emitter when given.
    bool draw(const std::string& vsHlsl, const ShaderInfo9& vsInfo, const std::string& psHlsl, const ShaderInfo9& psInfo,
              std::vector<float>& pixels, std::string& error, const std::vector<uint8_t>* vsDxbc = nullptr,
              const std::vector<uint8_t>* psDxbc = nullptr) {
      ID3DBlob* vsCode = nullptr;
      ID3DBlob* psCode = nullptr;
      if (vsDxbc ? !blob(*vsDxbc, &vsCode) : !compile(vsHlsl, "vs_5_0", &vsCode, error))
        return false;
      if (psDxbc ? !blob(*psDxbc, &psCode) : !compile(psHlsl, "ps_5_0", &psCode, error)) {
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
      // BLENDINDICES (UBYTE4) feeds integers: dxup_inputModes 2.
      uint32_t inputModes = 0;
      for (const auto& in : vsInfo.inputs)
        if (in.usage == UsageBlendIndices) inputModes |= 2u << (in.reg * 2);
      std::memcpy(intData.data() + 324, &inputModes, 4);
      context->UpdateSubresource(intConstants, 0, nullptr, intData.data(), 0, 0);
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

  void disassemble(const char* title, const std::vector<uint8_t>& code) {
    ID3DBlob* text = nullptr;
    const HRESULT hr = D3DDisassemble(code.data(), code.size(), 0, nullptr, &text);
    std::printf("----- %s -----\n%s\n", title, SUCCEEDED(hr) && text ? static_cast<const char*>(text->GetBufferPointer())
                                                                 : "(does not disassemble)");
    if (text) text->Release();
  }

}

// ---- Old path against new: fxc's DXBC and the emitter's, drawn alike ----

namespace {

  // Draws a translated shader both ways on Windows' Direct3D 11 and compares
  // what comes out: pixel shaders into float targets (fed by a generated
  // vertex shader), vertex shaders through stream output.
  struct DiffRunner {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Buffer* cb[4] = {};
    ID3D11ShaderResourceView* views[4] = {};   // 2D, cube, 3D, R32F 2D (shadow)
    ID3D11SamplerState* sampler = nullptr;
    ID3D11SamplerState* compareSampler = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    static constexpr UINT kSide = 16, kVertices = 16;

    bool init() {
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                   nullptr, &context)))
        return false;
      // b0 c[256], b1 fixed (fog and alpha test off), b2 ints/bools/last/modes, b3 clip planes.
      std::vector<float> c(256 * 4);
      for (int n = 0; n < 256; n++) {
        c[n * 4 + 0] = float(std::sin(n * 1.37) * 1.5);
        c[n * 4 + 1] = float(std::cos(n * 0.71));
        c[n * 4 + 2] = float((n % 7) * 0.25 - 0.75);
        c[n * 4 + 3] = float(0.5 + (n % 5) * 0.1);
      }
      std::vector<float> fixed(20 * 4, 0.0f);
      for (int s = 0; s < 8; s++) {
        fixed[(4 + s) * 4 + 0] = 0.5f; fixed[(4 + s) * 4 + 1] = -0.25f;
        fixed[(4 + s) * 4 + 2] = 0.125f; fixed[(4 + s) * 4 + 3] = 0.75f;
        fixed[(12 + s) * 4 + 0] = 0.8f; fixed[(12 + s) * 4 + 1] = 0.1f;
      }
      std::vector<uint32_t> ints(21 * 4, 0);
      for (int n = 0; n < 16; n++) {
        ints[n * 4 + 0] = uint32_t(n % 5 + 2); ints[n * 4 + 1] = uint32_t(n % 3); ints[n * 4 + 2] = 1;
      }
      for (int n = 0; n < 16; n++) ints[64 + n] = (n % 3) == 0 ? 1u : 0u;
      ints[80] = 255;
      std::vector<float> planes(6 * 4);
      for (int n = 0; n < 6; n++) { planes[n * 4] = 0.3f * n; planes[n * 4 + 1] = -0.2f; planes[n * 4 + 2] = 0.5f; planes[n * 4 + 3] = 0.1f; }
      const void* data[4] = { c.data(), fixed.data(), ints.data(), planes.data() };
      const UINT sizes[4] = { 4096, 320, 336, 96 };
      for (int i = 0; i < 4; i++) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizes[i];
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init = { data[i] };
        if (FAILED(device->CreateBuffer(&bd, &init, &cb[i])))
          return false;
      }
      // Textures: 2D, cube, 3D with varied float texels.
      auto texel = [](int a, int b, int c, int d) {
        return float(std::sin(a * 1.3 + b * 0.7 + c * 2.1 + d * 0.37) * 0.5 + 0.5);
      };
      std::vector<float> t2(4 * 4 * 4), tc(6 * 4 * 4 * 4), t3(4 * 4 * 4 * 4);
      for (int i = 0; i < 64; i++) t2[i] = texel(i, 0, 0, 1);
      for (int i = 0; i < 384; i++) tc[i] = texel(i, 1, 0, 2);
      for (int i = 0; i < 256; i++) t3[i] = texel(i, 2, 1, 3);
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 4; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA sd[6] = {};
      sd[0] = { t2.data(), 64, 0 };
      ID3D11Texture2D* tex = nullptr;
      if (FAILED(device->CreateTexture2D(&td, sd, &tex)) || FAILED(device->CreateShaderResourceView(tex, nullptr, &views[0])))
        return false;
      tex->Release();
      td.ArraySize = 6; td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
      for (int f = 0; f < 6; f++) sd[f] = { tc.data() + f * 64, 64, 0 };
      if (FAILED(device->CreateTexture2D(&td, sd, &tex)) || FAILED(device->CreateShaderResourceView(tex, nullptr, &views[1])))
        return false;
      tex->Release();
      D3D11_TEXTURE3D_DESC t3d = {};
      t3d.Width = t3d.Height = t3d.Depth = 4; t3d.MipLevels = 1; t3d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      t3d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      sd[0] = { t3.data(), 64, 256 };
      ID3D11Texture3D* tex3 = nullptr;
      if (FAILED(device->CreateTexture3D(&t3d, sd, &tex3)) || FAILED(device->CreateShaderResourceView(tex3, nullptr, &views[2])))
        return false;
      tex3->Release();
      {
        std::vector<float> depth(16);
        for (int i = 0; i < 16; i++) depth[i] = texel(i, 3, 2, 1);
        D3D11_TEXTURE2D_DESC dd = {};
        dd.Width = dd.Height = 4; dd.MipLevels = 1; dd.ArraySize = 1; dd.Format = DXGI_FORMAT_R32_FLOAT;
        dd.SampleDesc.Count = 1; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { depth.data(), 16, 0 };
        if (FAILED(device->CreateTexture2D(&dd, &init, &tex)) || FAILED(device->CreateShaderResourceView(tex, nullptr, &views[3])))
          return false;
        tex->Release();
      }
      // Linear filtering: coordinates that differ in the last bit (fxc folds
      // x / x to 1, the GPU divides) give close texels, not the next one.
      D3D11_SAMPLER_DESC smp = {};
      smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      smp.MaxLOD = D3D11_FLOAT32_MAX;
      if (FAILED(device->CreateSamplerState(&smp, &sampler)))
        return false;
      smp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
      smp.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
      if (FAILED(device->CreateSamplerState(&smp, &compareSampler)))
        return false;
      D3D11_RASTERIZER_DESC rs = {};
      rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
      return SUCCEEDED(device->CreateRasterizerState(&rs, &raster));
    }

    void bindTextures(const ShaderInfo9& info, bool vertex, uint16_t shadow = 0) {
      ID3D11ShaderResourceView* srvs[16] = {};
      ID3D11SamplerState* samplers[16] = {};
      for (int i = 0; i < 16; i++) {
        const uint8_t t = info.samplerTypes[i];
        const bool compare = !vertex && t == Sampler2D && ((shadow >> i) & 1);
        srvs[i] = compare ? views[3] : t == SamplerCube ? views[1] : t == SamplerVolume ? views[2] : views[0];
        samplers[i] = compare ? compareSampler : sampler;
      }
      if (vertex) {
        context->VSSetShaderResources(0, 16, srvs);
        context->VSSetSamplers(0, 16, samplers);
      } else {
        context->PSSetShaderResources(0, 16, srvs);
        context->PSSetSamplers(0, 16, samplers);
      }
    }

    // A vertex shader feeding slots 0 .. count-1 with varied values.
    ID3D11VertexShader* feeder(uint32_t count, std::string& error) {
      std::string hlsl = "struct O {\n  float4 position : SV_Position;\n";
      for (uint32_t i = 0; i < count; i++)
        hlsl += "  float4 s" + std::to_string(i) + " : " + varyingSemantic9(i) + ";\n";
      hlsl += "};\nO main(uint id : SV_VertexID) {\n  O o;\n  float2 p = float2((id & 1) ? 1.0f : -1.0f, (id & 2) ? -1.0f : 1.0f);\n"
              "  o.position = float4(p, 0.25f, 1.0f);\n";
      for (uint32_t i = 0; i < count; i++) {
        const std::string k = std::to_string(i + 1);
        hlsl += "  o.s" + std::to_string(i) + " = float4(p.x * 0." + k + "3f + 0.4f, p.y * 0.2" + k + "f - 0.1f, 0.3f + p.x * p.y * 0." + k
          + "f, 0.7f - p.y * 0.0" + k + "f);\n";
      }
      hlsl += "  return o;\n}\n";
      ID3DBlob* code = nullptr;
      ID3DBlob* errors = nullptr;
      if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), "feeder", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &errors))) {
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "feeder";
        if (errors) errors->Release();
        return nullptr;
      }
      ID3D11VertexShader* vs = nullptr;
      device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
      code->Release();
      return vs;
    }

    bool drawPixel(const std::vector<uint8_t>& code, const ShaderInfo9& info, std::vector<float>& out, std::string& error,
                   uint16_t shadow = 0) {
      ID3D11PixelShader* ps = nullptr;
      if (FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &ps))) {
        error = "Direct3D 11 refused the pixel shader";
        return false;
      }
      ID3D11VertexShader* vs = feeder(info.varyingCount, error);
      if (!vs) { ps->Release(); return false; }
      ID3D11Texture2D* targets[4] = {};
      ID3D11RenderTargetView* rtvs[4] = {};
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = kSide; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
      const float clear[4] = { -7.0f, -7.0f, -7.0f, -7.0f };
      for (int i = 0; i < 4; i++) {
        device->CreateTexture2D(&td, nullptr, &targets[i]);
        device->CreateRenderTargetView(targets[i], nullptr, &rtvs[i]);
        context->ClearRenderTargetView(rtvs[i], clear);
      }
      ID3D11Texture2D* depth = nullptr;
      ID3D11DepthStencilView* dsv = nullptr;
      D3D11_TEXTURE2D_DESC dd = td;
      dd.Format = DXGI_FORMAT_D32_FLOAT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
      device->CreateTexture2D(&dd, nullptr, &depth);
      device->CreateDepthStencilView(depth, nullptr, &dsv);
      context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
      D3D11_DEPTH_STENCIL_DESC ds = {};
      ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
      ID3D11DepthStencilState* dss = nullptr;
      device->CreateDepthStencilState(&ds, &dss);
      context->OMSetDepthStencilState(dss, 0);
      context->OMSetRenderTargets(4, rtvs, dsv);
      D3D11_VIEWPORT vp = { 0.5f, 0.5f, float(kSide), float(kSide), 0.0f, 1.0f };
      context->RSSetViewports(1, &vp);
      context->RSSetState(raster);
      context->IASetInputLayout(nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context->VSSetShader(vs, nullptr, 0);
      context->GSSetShader(nullptr, nullptr, 0);
      context->PSSetShader(ps, nullptr, 0);
      context->PSSetConstantBuffers(0, 3, cb);
      bindTextures(info, false, shadow);
      context->Draw(4, 0);
      out.clear();
      D3D11_TEXTURE2D_DESC sdsc = td;
      sdsc.BindFlags = 0; sdsc.Usage = D3D11_USAGE_STAGING; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      for (int i = 0; i < 5; i++) {
        ID3D11Texture2D* staging = nullptr;
        D3D11_TEXTURE2D_DESC desc = i < 4 ? sdsc : dd;
        if (i == 4) { desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; }
        device->CreateTexture2D(&desc, nullptr, &staging);
        context->CopyResource(staging, i < 4 ? targets[i] : depth);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
          const UINT floats = i < 4 ? 4 : 1;
          for (UINT y = 0; y < kSide; y++) {
            const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
            out.insert(out.end(), row, row + kSide * floats);
          }
          context->Unmap(staging, 0);
        }
        staging->Release();
      }
      for (int i = 0; i < 4; i++) { rtvs[i]->Release(); targets[i]->Release(); }
      dsv->Release(); depth->Release(); dss->Release();
      vs->Release(); ps->Release();
      context->ClearState();
      return true;
    }

    bool drawVertex(const std::vector<uint8_t>& code, const ShaderInfo9& info, uint32_t clipPlanes, std::vector<float>& out,
                    std::string& error) {
      ID3D11VertexShader* vs = nullptr;
      if (FAILED(device->CreateVertexShader(code.data(), code.size(), nullptr, &vs))) {
        error = "Direct3D 11 refused the vertex shader";
        return false;
      }
      // Stream output of the position, the slots and the clip distances.
      std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
      std::vector<std::string> names;
      names.reserve(64);
      entries.push_back({ 0, "SV_Position", 0, 0, 4, 0 });
      for (uint32_t i = 0; i < info.varyingCount; i++) {
        std::string semantic = varyingSemantic9(i);
        size_t end = semantic.size();
        while (end && semantic[end - 1] >= '0' && semantic[end - 1] <= '9') end--;
        names.push_back(semantic.substr(0, end));
        entries.push_back({ 0, names.back().c_str(), UINT(std::atoi(semantic.c_str() + end)), 0, 4, 0 });
      }
      UINT stride = UINT(entries.size()) * 16;
      if (clipPlanes) {
        entries.push_back({ 0, "SV_ClipDistance", 0, 0, 4, 0 });
        entries.push_back({ 0, "SV_ClipDistance", 1, 0, 2, 0 });
        stride += 24;
      }
      ID3D11GeometryShader* gs = nullptr;
      if (FAILED(device->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), entries.data(), UINT(entries.size()),
                                                             &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs))) {
        vs->Release();
        error = "stream output refused";
        return false;
      }
      // Inputs: uint4 bits of floats (input modes 0).
      std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
      std::vector<std::string> inNames;
      inNames.reserve(32);
      for (size_t i = 0; i < info.inputs.size(); i++) {
        inNames.push_back(semanticName9(info.inputs[i].usage));
        elements.push_back({ inNames.back().c_str(), info.inputs[i].usageIndex, DXGI_FORMAT_R32G32B32A32_UINT, 0,
                             UINT(i * 16), D3D11_INPUT_PER_VERTEX_DATA, 0 });
      }
      ID3D11InputLayout* layout = nullptr;
      if (!elements.empty() && FAILED(device->CreateInputLayout(elements.data(), UINT(elements.size()), code.data(), code.size(),
                                                               &layout))) {
        vs->Release(); gs->Release();
        error = "input layout refused";
        return false;
      }
      const UINT vstride = (std::max)(UINT(info.inputs.size()) * 16, 16u);
      std::vector<float> vertices(size_t(vstride / 4) * kVertices);
      for (UINT v = 0; v < kVertices; v++)
        for (UINT k = 0; k < vstride / 4; k++)
          vertices[v * (vstride / 4) + k] = float(std::sin(v * 0.9 + k * 1.7) * 2.0);
      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = UINT(vertices.size() * 4); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      D3D11_SUBRESOURCE_DATA init = { vertices.data() };
      ID3D11Buffer* vb = nullptr;
      device->CreateBuffer(&bd, &init, &vb);
      D3D11_BUFFER_DESC sob = {};
      sob.ByteWidth = stride * kVertices; sob.BindFlags = D3D11_BIND_STREAM_OUTPUT;
      ID3D11Buffer* so = nullptr;
      device->CreateBuffer(&sob, nullptr, &so);
      const UINT offset = 0;
      context->IASetInputLayout(layout);
      context->IASetVertexBuffers(0, 1, &vb, &vstride, &offset);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
      context->VSSetShader(vs, nullptr, 0);
      context->GSSetShader(gs, nullptr, 0);
      context->PSSetShader(nullptr, nullptr, 0);
      ID3D11Buffer* vsBuffers[4] = { cb[0], nullptr, cb[2], cb[3] };
      context->VSSetConstantBuffers(0, 4, vsBuffers);
      bindTextures(info, true);
      context->SOSetTargets(1, &so, &offset);
      context->Draw(kVertices, 0);
      sob.BindFlags = 0; sob.Usage = D3D11_USAGE_STAGING; sob.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Buffer* staging = nullptr;
      device->CreateBuffer(&sob, nullptr, &staging);
      context->CopyResource(staging, so);
      D3D11_MAPPED_SUBRESOURCE m = {};
      out.clear();
      if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        const float* f = static_cast<const float*>(m.pData);
        out.assign(f, f + stride * kVertices / 4);
        context->Unmap(staging, 0);
      }
      staging->Release(); so->Release(); vb->Release();
      if (layout) layout->Release();
      gs->Release(); vs->Release();
      context->ClearState();
      return true;
    }
  };

  // The assembly of generated shaders, for reports.
  const std::vector<std::string>* g_fuzzSources = nullptr;
  bool g_printAll = false;   // every output value of both paths

  struct CorpusShader {
    std::vector<uint32_t> tokens;
    std::vector<TranslateOptions9> variants;   // at least the default
  };

  // Shaders and their recorded variants from a d3d9_variants.bin.
  std::vector<CorpusShader> readRecords(const char* path) {
    std::vector<CorpusShader> shaders;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
      return shaders;
    std::vector<std::pair<uint64_t, size_t>> index;
    uint32_t header[2] = {}, head[2];
    struct Record { uint64_t shader; uint32_t minVaryings; uint16_t r0, r1, shadow, r2; uint8_t clip, typed, r3[2]; uint8_t types[16]; };
    static_assert(sizeof(Record) == 40, "record");
    std::vector<Record> records;
    if (std::fread(header, sizeof(header), 1, f) == 1)
      while (std::fread(head, sizeof(head), 1, f) == 1 && head[1] <= (4u << 20)) {
        std::vector<uint8_t> data(head[1]);
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) break;
        if (head[0] == 1 && data.size() > 8) {
          uint64_t hash;
          std::memcpy(&hash, data.data(), 8);
          CorpusShader s;
          s.tokens.resize((data.size() - 8) / 4);
          std::memcpy(s.tokens.data(), data.data() + 8, s.tokens.size() * 4);
          index.emplace_back(hash, shaders.size());
          shaders.push_back(std::move(s));
        } else if (head[0] == 2 && data.size() == sizeof(Record)) {
          Record r;
          std::memcpy(&r, data.data(), sizeof(r));
          records.push_back(r);
        }
      }
    std::fclose(f);
    static std::vector<std::array<uint8_t, 16>> typeStore;
    typeStore.reserve(records.size() + 1);
    for (const Record& r : records) {
      for (const auto& e : index) {
        if (e.first != r.shader) continue;
        TranslateOptions9 o;
        o.minVaryings = r.minVaryings;
        o.shadowSamplers = r.shadow;
        o.clipPlanes = r.clip;
        if (r.typed) {
          typeStore.emplace_back();
          std::memcpy(typeStore.back().data(), r.types, 16);
          o.boundSamplerTypes = typeStore.back().data();
        }
        shaders[e.second].variants.push_back(o);
      }
    }
    for (auto& s : shaders)
      if (s.variants.empty()) s.variants.emplace_back();
    return shaders;
  }

  // Worst relative difference (NaN matches only NaN).
  float worstDifference(const std::vector<float>& a, const std::vector<float>& b, size_t& at) {
    float worst = a.size() == b.size() ? 0.0f : 1.0f;
    at = 0;
    for (size_t i = 0; i < (std::min)(a.size(), b.size()); i++) {
      float d = std::fabs(a[i] - b[i]) / (1.0f + std::fabs(a[i]));
      if (a[i] != a[i] || b[i] != b[i]) d = (a[i] != a[i]) == (b[i] != b[i]) ? 0.0f : 1.0f;
      if (d > worst) { worst = d; at = i; }
    }
    return worst;
  }

  // Each shader (and recorded variant) through fxc and through the emitter:
  // compile times, and whether they draw the same.
  int compareShaders(const std::vector<CorpusShader>& shaders, bool verbose, bool floatEmulation) {
    DiffRunner runner;
    if (!runner.init()) { std::printf("FAIL: Direct3D 11 setup\n"); return 1; }
    double fxcMs = 0.0, dxbcMs = 0.0, fxcWorst = 0.0, dxbcWorst = 0.0;
    int compared = 0, differing = 0, refused = 0, fallback = 0;
    for (size_t n = 0; n < shaders.size(); n++) {
      for (TranslateOptions9 options : shaders[n].variants) {
        options.floatEmulation = options.floatEmulation || floatEmulation;
        options.dotEmulation = true;
        const auto& tokens = shaders[n].tokens;
        std::string hlsl, error;
        ShaderInfo9 info, info2;
        auto t0 = std::chrono::steady_clock::now();
        if (!translateShader9(tokens.data(), tokens.size(), options, hlsl, info, error)) {
          if (verbose) std::printf("skip %zu: %s\n", n, error.c_str());
          continue;
        }
        const bool vertex = info.stage == ShaderStage9::Vertex;
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        // DXUP_FXC_SKIP_OPTIMIZATION: fxc without its optimizer (to tell its
        // folding from the emitter's).
        const UINT fxcFlags = std::getenv("DXUP_FXC_SKIP_OPTIMIZATION") ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), "r", nullptr, nullptr, "main", vertex ? "vs_5_0" : "ps_5_0",
                                      fxcFlags, 0, &code, &errors);
        const double fxc = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (errors) errors->Release();
        if (FAILED(hr)) { if (verbose) std::printf("skip %zu: fxc failed\n", n); continue; }
        std::vector<uint8_t> fxcCode(static_cast<const uint8_t*>(code->GetBufferPointer()),
                                     static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
        code->Release();
        std::vector<uint8_t> dxbc;
        t0 = std::chrono::steady_clock::now();
        const bool emitted = compileShader9(tokens.data(), tokens.size(), options, dxbc, info2, error);
        const double emit = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        fxcMs += fxc; dxbcMs += emit;
        fxcWorst = (std::max)(fxcWorst, fxc); dxbcWorst = (std::max)(dxbcWorst, emit);
        if (!emitted) {
          fallback++;
          std::printf("fallback %zu (%s %u.%u): %s\n", n, vertex ? "vs" : "ps", info.major, info.minor, error.c_str());
          if (g_fuzzSources && n < g_fuzzSources->size() && verbose) dump("assembly", (*g_fuzzSources)[n]);
          continue;
        }
        std::vector<float> a, b;
        std::string e1, e2;
        const bool okA = vertex ? runner.drawVertex(fxcCode, info, options.clipPlanes, a, e1)
                                : runner.drawPixel(fxcCode, info, a, e1, options.shadowSamplers);
        const bool okB = vertex ? runner.drawVertex(dxbc, info, options.clipPlanes, b, e2)
                                : runner.drawPixel(dxbc, info, b, e2, options.shadowSamplers);
        if (!okB) {
          refused++;
          std::printf("REFUSED %zu (%s %u.%u): %s\n", n, vertex ? "vs" : "ps", info.major, info.minor, e2.c_str());
          if (g_fuzzSources && n < g_fuzzSources->size()) dump("assembly", (*g_fuzzSources)[n]);
          disassemble("emitter", dxbc);
          continue;
        }
        if (!okA)
          continue;
        compared++;
        if (g_printAll)
          for (size_t i = 0; i < (std::max)(a.size(), b.size()); i++)
            std::printf("  %4zu fxc %12.6g emitter %12.6g%s\n", i, i < a.size() ? a[i] : 0.0f, i < b.size() ? b[i] : 0.0f,
                        i < a.size() && i < b.size() && a[i] != b[i] ? "  *" : "");
        size_t at = 0;
        const float worst = worstDifference(a, b, at);
        if (worst > 2e-3f) {
          differing++;
          std::printf("DIFF %zu (%s %u.%u) worst %.3g at %zu: fxc %.6g, emitter %.6g\n", n, vertex ? "vs" : "ps", info.major,
                      info.minor, worst, at, at < a.size() ? a[at] : 0.0f, at < b.size() ? b[at] : 0.0f);
          if (g_fuzzSources && n < g_fuzzSources->size()) dump("assembly", (*g_fuzzSources)[n]);
          if (verbose) {
            int shown = 0;
            for (size_t i = 0; i < a.size() && i < b.size() && shown < 24; i++)
              if (a[i] != b[i] && !(a[i] != a[i] && b[i] != b[i])) {
                std::printf("  [%zu] fxc %.9g emitter %.9g\n", i, a[i], b[i]);
                shown++;
              }
            dump("hlsl", hlsl);
            disassemble("fxc", fxcCode);
            disassemble("emitter", dxbc);
          }
        }
      }
    }
    std::printf("compared %d, differing %d, refused %d, fallback %d; fxc %.0f ms (slowest %.1f), emitter %.1f ms (slowest %.2f)\n",
                compared, differing, refused, fallback, fxcMs, fxcWorst, dxbcMs, dxbcWorst);
    return differing || refused ? 1 : 0;
  }

}

// ---- Random shaders: every model and opcode group through both paths ----

namespace {

  struct Random {
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t next() {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return uint32_t(state >> 33);
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    bool chance(uint32_t percent) { return below(100) < percent; }
    template<typename T, size_t N> const T& pick(const T (&items)[N]) { return items[below(N)]; }
  };

  // Writes random but valid Direct3D 9 assembly of one shader model.
  struct ShaderFuzzer {
    Random& rng;
    std::string model;       // "ps_2_0", "vs_3_0", ...
    bool vertex = false;
    int major = 2, minor = 0;
    std::string text;
    std::vector<int> written;      // temps written
    std::vector<std::string> inputs;
    int depth = 0;
    bool usedPredicate = false;
    std::string instructionConstant;   // one constant register per instruction
    std::string instructionInput;      // and one input register

    ShaderFuzzer(Random& r, const std::string& m) : rng(r), model(m) {
      vertex = m[0] == 'v';
      major = m[3] - '0';
      minor = m[5] == 'x' ? 1 : m[5] - '0';
    }

    void line(const std::string& l) { text += l + "\n"; }

    std::string swizzle() {
      static const char* s[] = { "", "", "", ".x", ".y", ".z", ".w", ".xxxx", ".yzxw", ".wzyx", ".zwxy", ".xyzz" };
      return rng.pick(s);
    }
    std::string mask() {
      static const char* m[] = { "", "", "", ".x", ".xy", ".xyz", ".yzw", ".w", ".xz" };
      return rng.pick(m);
    }
    int temps() const { return major >= 3 ? 12 : major == 2 ? 8 : 2; }

    std::string temp(bool forWrite) {
      if (forWrite || written.empty()) {
        const int r = int(rng.below(uint32_t(temps())));
        return "r" + std::to_string(r);
      }
      return "r" + std::to_string(written[rng.below(uint32_t(written.size()))]);
    }

    void markWritten(const std::string& dst) {
      if (dst[0] != 'r') return;
      const int r = std::atoi(dst.c_str() + 1);
      if (std::find(written.begin(), written.end(), r) == written.end()) written.push_back(r);
    }

    // A source register: temp, constant (one per instruction), input.
    std::string sourceRegister() {
      const uint32_t k = rng.below(10);
      if (k < 4 && !written.empty()) return temp(false);
      if (k < 7 || inputs.empty()) {
        if (!instructionConstant.empty())
          return rng.chance(50) || written.empty() ? instructionConstant : temp(false);
        instructionConstant = "c" + std::to_string(rng.below(vertex ? 40 : 20));
        return instructionConstant;
      }
      if (!instructionInput.empty())
        return rng.chance(50) || written.empty() ? instructionInput : temp(false);
      instructionInput = inputs[rng.below(uint32_t(inputs.size()))];
      return instructionInput;
    }

    // A source: register with swizzle and modifier.
    std::string source(bool allowModifier = true) {
      const std::string base = sourceRegister();
      if (base.find('.') != std::string::npos) return base;   // vFace.x, vPos.xyxy
      std::string sw = swizzle();
      if (major < 2 && !vertex) sw = rng.chance(20) ? ".w" : "";   // ps 1.x: few swizzles
      if (major == 2 && !vertex && sw.size() == 5 && sw != ".xxxx" && sw != ".yyyy" && sw != ".zzzz" && sw != ".wwww") {
        // ps_2_0 allows only replicate and xyzw / yzxw / zxyw / wzyx swizzles.
        static const char* ok[] = { ".yzxw", ".zxyw", ".wzyx", ".xxxx", "" };
        if (!model.compare(0, 6, "ps_2_0")) sw = rng.pick(ok);
      }
      if (!allowModifier || !rng.chance(25)) return base + sw;
      if (rng.chance(60)) return "-" + base + sw;
      if (major >= 3 && base[0] != 'c' && rng.chance(50)) return base + "_abs" + sw;
      return base + sw;
    }

    void op(const std::string& name, int sources, bool scalarDst = false) {
      instructionConstant.clear(); instructionInput.clear();
      std::string dst = temp(true);
      std::string m = scalarDst ? (rng.chance(50) ? ".x" : "") : mask();
      std::string sat = !vertex && rng.chance(15) ? "_sat" : "";
      std::string line_ = name + sat + " " + dst + m;
      for (int i = 0; i < sources; i++) line_ += ", " + source();
      line(line_);
      markWritten(dst);
    }

    // A scalar source: register and one component.
    std::string scalarSource() {
      static const char* comps[] = { ".x", ".y", ".z", ".w" };
      const std::string base = sourceRegister();
      if (base.find('.') != std::string::npos) return base.substr(0, base.find('.')) + ".x";
      return (rng.chance(20) ? "-" : "") + base + rng.pick(comps);
    }

    void scalarOp(const std::string& name) {
      instructionConstant.clear(); instructionInput.clear();
      std::string dst = temp(true);
      line(name + " " + dst + mask() + ", " + scalarSource());
      markWritten(dst);
    }

    void declare() {
      line(model);
      // Constants with specials sometimes.
      line("def c30, 0.5, -1.25, 2.0, 0.0");
      line("def c31, 1.0, 0.25, -0.5, 3.0");
      if (vertex) {
        line("dcl_position v0");
        line("dcl_texcoord v1");
        line("dcl_normal v2");
        line("dcl_color v3");
        inputs = { "v0", "v1", "v2", "v3" };
        if (major >= 3) {
          line("dcl_position o0");
          line("dcl_texcoord o1");
          line("dcl_texcoord1 o2");
          line("dcl_color o3");
          if (rng.chance(30)) line("dcl_2d s0");
          line("defi i0, 2, 0, 1, 0");
        }
      } else if (major >= 3) {
        line("dcl_texcoord v0");
        line("dcl_texcoord1 v1");
        line("dcl_color v2");
        inputs = { "v0", "v1", "v2" };
        if (rng.chance(30)) line("dcl vFace");
        if (rng.chance(30)) { line("dcl vPos.xy"); inputs.push_back("vPos"); }
        line("dcl_2d s0");
        line("dcl_cube s1");
        line("dcl_volume s2");
        line("defi i0, 3, 1, 1, 0");
        line("defb b0, true");
      } else if (major == 2) {
        line("dcl t0");
        line("dcl t1");
        line("dcl v0");
        inputs = { "t0", "t1", "v0" };
        line("dcl_2d s0");
        line("dcl_cube s1");
        line("dcl_volume s2");
      }
    }

    void arithmetic() {
      const uint32_t k = rng.below(vertex ? 26 : 22);
      switch (k) {
        case 0: case 1: op("add", 2); break;
        case 2: op("sub", 2); break;
        case 3: case 4: op("mul", 2); break;
        case 5: case 6: op("mad", 3); break;
        case 7: op("dp3", 2); break;
        case 8: op("dp4", 2); break;
        case 9: op("min", 2); break;
        case 10: op("max", 2); break;
        case 11: if (vertex) op("slt", 2); else op("cmp", 3); break;
        case 12: if (vertex) op("sge", 2); else op("lrp", 3); break;
        case 13: op("frc", 1); break;
        case 14: scalarOp("rcp"); break;
        case 15: scalarOp("rsq"); break;
        case 16: scalarOp("exp"); break;
        case 17: scalarOp("log"); break;
        case 18: {
          instructionConstant.clear(); instructionInput.clear();
          std::string dst = temp(true);
          line("pow " + dst + mask() + ", " + scalarSource() + ", " + scalarSource());
          markWritten(dst);
          break;
        }
        case 19: if (major >= 2) op("abs", 1); else op("mov", 1); break;
        case 20: {
          if (major >= 2) {
            std::string dst = temp(true);
            line("nrm " + dst + ".xyz, " + source());
            markWritten(dst);
          } else op("mov", 1);
          break;
        }
        case 21: {
          std::string dst = temp(true);
          std::string a = source(false), b = source(false);
          if (major >= 2) {
            std::string x = temp(false), y = temp(false);
            if (x == dst || y == dst) break;
            line("crs " + dst + ".xyz, " + x + ", " + y);
            markWritten(dst);
          }
          break;
        }
        case 22: if (vertex) { std::string dst = temp(true); line("lit " + dst + ", " + source()); markWritten(dst); } break;
        case 23: if (vertex) { std::string dst = temp(true); line("dst " + dst + ", " + source() + ", " + source()); markWritten(dst); } break;
        case 24:
          if (vertex && major == 1) {
            scalarOp("expp");
          } else if (vertex && temps() >= 3) {
            instructionConstant.clear(); instructionInput.clear();
            const std::string dst = temp(true);
            std::string a = "r" + std::to_string(rng.below(uint32_t(temps())));
            std::string b = "r" + std::to_string(rng.below(uint32_t(temps())));
            if (a == b || a == dst || b == dst) break;
            line("sgn " + dst + mask() + ", " + source() + ", " + a + ", " + b);
            markWritten(dst);
          }
          break;
        case 25: if (vertex && major == 1) { scalarOp("logp"); } else if (vertex) { std::string dst = temp(true); line("m4x4 " + dst + ", " + source(false) + ", c" + std::to_string(rng.below(30))); markWritten(dst); } break;
      }
    }

    void texture() {
      if (vertex) {
        if (major >= 3 && text.find("dcl_2d s0") != std::string::npos) {
          std::string dst = temp(true);
          line("texldl " + dst + ", " + source(false) + ", s0");
          markWritten(dst);
        }
        return;
      }
      static const char* samplers[] = { "s0", "s1", "s2" };
      instructionConstant.clear(); instructionInput.clear();
      std::string dst = temp(true);
      std::string coord = !written.empty() && rng.chance(40) ? temp(false) : inputs[rng.below(uint32_t(inputs.size() < 2 ? inputs.size() : 2))];
      const uint32_t k = rng.below(major >= 3 ? 5 : 3);
      const char* s = rng.pick(samplers);
      if (k == 0) line("texld " + dst + ", " + coord + ", " + s);
      else if (k == 1) line("texldp " + dst + ", " + coord + ", " + s);
      else if (k == 2) line("texldb " + dst + ", " + coord + ", " + s);
      else if (k == 3) line("texldl " + dst + ", " + coord + ", " + s);
      else line("texldd " + dst + ", " + coord + ", " + s + ", " + source(false) + ", " + source(false));
      markWritten(dst);
    }

    void flow() {
      if (major < 3) return;
      const uint32_t k = rng.below(6);
      if (k == 0 && depth < 2) {
        line("if b0");
        depth++;
        body(3);
        if (rng.chance(50)) { line("else"); body(2); }
        line("endif");
        depth--;
      } else if (k == 1 && depth < 2) {
        static const char* cmps[] = { "_gt", "_lt", "_ge", "_le", "_eq", "_ne" };
        instructionConstant.clear(); instructionInput.clear();
        const bool face = text.find("dcl vFace") != std::string::npos && rng.chance(40);
        line("if" + std::string(rng.pick(cmps)) + " " + (face ? std::string("vFace") : scalarSource()) + ", " + scalarSource());
        depth++;
        body(3);
        line("endif");
        depth--;
      } else if (k == 2 && depth < 2) {
        line("rep i0");
        depth++;
        body(3);
        if (rng.chance(40)) {
          static const char* cmps[] = { "_gt", "_lt", "_ge" };
          instructionConstant.clear(); instructionInput.clear();
          line("break" + std::string(rng.pick(cmps)) + " " + scalarSource() + ", " + scalarSource());
        }
        line("endrep");
        depth--;
      } else if (k == 3 && depth < 2) {
        line("loop aL, i0");
        depth++;
        body(2);
        if (vertex && rng.chance(50)) {
          std::string dst = temp(true);
          line("add " + dst + ", " + dst + ", c" + std::to_string(rng.below(10)) + "[aL]");
          markWritten(dst);
        } else if (!vertex) {
          std::string dst = temp(true);
          line("add " + dst + ", " + source(false) + ", v0[aL]");
          markWritten(dst);
        }
        line("endloop");
        depth--;
      } else if (k == 4) {
        static const char* cmps[] = { "_gt", "_lt", "_ge", "_le", "_eq", "_ne" };
        instructionConstant.clear(); instructionInput.clear();
        line("setp" + std::string(rng.pick(cmps)) + " p0" + mask() + ", " + source(false) + ", " + source(false));
        usedPredicate = true;
        std::string dst = temp(true);
        if (std::find(written.begin(), written.end(), std::atoi(dst.c_str() + 1)) == written.end())
          return;
        static const char* comps[] = { "", ".x", ".y", ".w" };
        instructionConstant.clear(); instructionInput.clear();
        line(std::string(rng.chance(50) ? "(p0" : "(!p0") + rng.pick(comps) + ") mov " + dst + mask() + ", " + source());
      } else if (!vertex && depth == 0 && !written.empty()) {
        line("texkill " + temp(false));
      }
    }

    void body(int count) {
      for (int i = 0; i < count; i++) {
        const uint32_t k = rng.below(10);
        if (k < 6) arithmetic();
        else if (k < 8) texture();
        else flow();
      }
    }

    std::string finish() {
      if (written.empty()) { line("mov r0, c0"); markWritten("r0"); }
      if (vertex) {
        if (major >= 3) {
          line("mov o0, " + temp(false));
          line("mov o1, " + source());
          line("mov o2.xy, " + source());
          line("mov o3, " + source());
          if (rng.chance(30)) line("mov o2.zw, " + temp(false));
        } else {
          line("mov oPos, " + temp(false));
          line("mov oD0, " + source());
          line("mov oT0, " + source());
          if (rng.chance(50)) line("mov oT1.xy, " + temp(false));
          if (rng.chance(30)) line("mov oFog, " + temp(false) + ".x");
          if (rng.chance(30)) line("mov oPts, " + temp(false) + ".x");
        }
      } else if (major >= 2) {
        line("mov oC0, " + temp(false));
        if (rng.chance(30)) line("mov oC1, " + source());
        if (rng.chance(20)) line("mov oDepth, " + temp(false) + ".x");
      }
      return text;
    }

    std::string generate(int count) {
      declare();
      for (int r = 0; r < temps(); r++) {
        line("mov r" + std::to_string(r) + ", c" + std::to_string(r + 1));
        markWritten("r" + std::to_string(r));
      }
      if (vertex && rng.chance(40)) {
        // Relative constants through a0.
        instructionConstant.clear(); instructionInput.clear();
        line(std::string(major >= 2 ? "mova" : "mov") + " a0.x, " + scalarSource());
        std::string dst = temp(true);
        line("add " + dst + ", c" + std::to_string(rng.below(10)) + "[a0.x], " + temp(false));
        markWritten(dst);
      }
      const bool subroutine = major >= 3 && rng.chance(35);
      if (subroutine) {
        line("call l1");
        if (rng.chance(50)) line("callnz l1, b0");
      }
      body(count);
      if (vertex && major >= 3 && rng.chance(30)) {
        // Outputs addressed by aL.
        line("loop aL, i0");
        line("mov o[aL + 1], " + temp(false));
        line("endloop");
        if (text.find("defi i0") == std::string::npos)
          text.insert(text.find('\n') + 1, "defi i0, 2, 0, 1, 0\n");
      }
      std::string result = finish();
      if (subroutine) {
        // A subroutine after the main program's ret.
        text += "ret\nlabel l1\n";
        const int n = 1 + int(rng.below(3));
        for (int i = 0; i < n; i++) arithmetic();
        text += "ret\n";
        if (text.find("defb b0") == std::string::npos)
          text.insert(text.find('\n') + 1, "defb b0, true\n");
        result = text;
      }
      return result;
    }
  };

  // Pixel shader 1.x: texture stage ops, then arithmetic with the 1.x modifiers.
  std::string fuzzPs1(Random& rng) {
    const int minor = int(rng.below(4)) + 1;   // 1.1 to 1.4
    std::string t = "ps_1_" + std::to_string(minor) + "\n";
    t += "def c7, 0.5, 0.25, 0.75, 1.0\n";
    if (minor == 4) {
      t += "texld r0, t0\ntexcrd r1.xyz, t1\n";
      if (rng.chance(50)) t += "texld r2, t2\n";
      if (rng.chance(30)) t += "texkill t3\n";
      if (rng.chance(30)) t += "phase\ntexld r3, r1\n";
    } else {
      const uint32_t k = rng.below(9);
      if (k == 0) t += "tex t0\ntexcoord t1\n";
      else if (k == 1) t += "tex t0\ntexbem t1, t0\n";
      else if (k == 2) t += "tex t0\ntexbeml t1, t0\n";
      else if (k == 3) t += "tex t0\ntexreg2ar t1, t0\ntexreg2gb t2, t0\n";
      else if (k == 4) t += "tex t0\ntexm3x2pad t1, t0\ntexm3x2tex t2, t0\n";
      else if (k == 5) t += "tex t0\ntexm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3tex t3, t0\n";
      else if (k == 6) t += "tex t0\ntexm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3spec t3, t0, c0\n";
      else if (k == 7) t += "tex t0\ntexm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3vspec t3, t0\n";
      else t += minor >= 2 ? "tex t0\ntexdp3tex t1, t0\ntexreg2rgb t2, t0\n" : "tex t0\ntex t1\n";
      if (rng.chance(30)) t += "texkill t3\n";
    }
    const bool bumped = t.find("texbem") != std::string::npos;
    t += minor == 4 ? "mov r1, c7\n" : "mov r1, c7\nmov r0, c7\n";
    static const char* mods[] = { "", "_bias", "_bx2", "", "" };
    static const char* dmods[] = { "", "_x2", "_x4", "_d2", "_sat", "" };
    const char* regs14[] = { "r0", "r1", "c0", "c1", "c7", "v0", "v1" };
    const char* regs11[] = { "t0", "t1", "c0", "c1", "c7", "v0", "v1" };
    auto src = [&]() {
      std::string r = minor == 4 ? rng.pick(regs14) : rng.pick(regs11);
      if (bumped && r == "t0") r = "t1";
      if (r[0] == 'c') return r;   // no modifiers on constants
      std::string m = rng.pick(mods);
      if (rng.chance(15)) return "1-" + r;
      if (rng.chance(15)) return "-" + r + m;
      return r + m;
    };
    const char* ops[] = { "add", "mul", "mad", "lrp", "dp3", "sub", "cnd", "mov" };
    const int n = 2 + int(rng.below(4));
    for (int i = 0; i < n; i++) {
      std::string op = rng.pick(ops);
      if (op == "cnd" && minor < 4) { t += "cnd r0, r0.a, " + src() + ", " + src() + "\n"; continue; }
      const int count = op == "mad" || op == "lrp" || op == "cnd" ? 3 : op == "mov" ? 1 : 2;
      std::string line = op + rng.pick(dmods) + " r" + std::to_string(i & 1);
      for (int k = 0; k < count; k++) line += ", " + src();
      t += line + "\n";
    }
    t += "mov r0, r" + std::to_string(rng.below(2)) + "\n";
    return t;
  }

  const char* g_fuzzModel = nullptr;   // DXUP_FUZZ_MODEL: one model only

  int fuzzShaders(int count, uint64_t seed, bool verbose) {
    static const char* models[] = { "ps_2_0", "ps_2_x", "ps_3_0", "vs_1_1", "vs_2_0", "vs_3_0", "ps_1_x",
                                    "ps_3_0", "vs_3_0", "ps_1_x", "ps_1_x" };
    g_fuzzModel = std::getenv("DXUP_FUZZ_MODEL");
    std::vector<CorpusShader> shaders;
    int rejected = 0;
    Random rng(seed);
    std::vector<std::string> sources;
    while (int(shaders.size()) < count) {
      const std::string model = g_fuzzModel ? std::string(g_fuzzModel) : std::string(rng.pick(models));
      std::string text;
      if (model == "ps_1_x") {
        text = fuzzPs1(rng);
      } else {
        ShaderFuzzer f(rng, model);
        text = f.generate(4 + int(rng.below(12)));
      }
      const Blob blob = assemble(text.c_str());
      if (!blob.error.empty() || blob.tokens.empty()) {
        if (verbose && rejected < 5) std::printf("rejected:\n%s%s\n", text.c_str(), blob.error.c_str());
        rejected++;
        continue;
      }
      CorpusShader s;
      s.tokens = blob.tokens;
      s.variants.emplace_back();
      // A variant with other options: what draws ask for (shadow maps,
      // clip planes, padded varyings, NaN depth, strict float emulation).
      if (rng.chance(50)) {
        TranslateOptions9 o;
        if (model[0] == 'v') {
          o.clipPlanes = uint8_t(rng.below(64));
          o.minVaryings = rng.chance(50) ? 11 : 0;
          o.nanDepthZero = rng.chance(50);
        } else {
          o.shadowSamplers = uint16_t(rng.chance(50) ? 1 : 0);
        }
        o.floatEmulation = rng.chance(30);
        s.variants.push_back(o);
      }
      shaders.push_back(std::move(s));
      sources.push_back(text);
    }
    std::map<std::string, int> perModel;
    for (const auto& t : sources) perModel[t.substr(0, t.find('\n'))]++;
    std::printf("%d shaders generated (%d rejected by the assembler):", count, rejected);
    for (const auto& m : perModel) std::printf(" %s %d", m.first.c_str(), m.second);
    std::printf("\n");
    g_fuzzSources = &sources;
    // DXUP_FUZZ_ONE: only that shader, every output of both paths printed.
    if (const char* one = std::getenv("DXUP_FUZZ_ONE")) {
      const size_t index = size_t(std::atoi(one));
      if (index < shaders.size()) {
        std::vector<CorpusShader> single(1, shaders[index]);
        std::vector<std::string> source(1, sources[index]);
        g_fuzzSources = &source;
        g_printAll = true;
        const int r = compareShaders(single, true, false);
        g_fuzzSources = nullptr;
        return r;
      }
    }
    const int result = compareShaders(shaders, verbose, false);
    g_fuzzSources = nullptr;
    return result;
  }

}

// ---- GPU time of the pixel shaders: fxc's DXBC against the emitter's ----

namespace {

  // GPU time of drawing a full-HD quad with the shader (milliseconds per
  // draw, the median of several timed batches).
  double gpuTime(DiffRunner& r, const std::vector<uint8_t>& code, const ShaderInfo9& info, std::string& error) {
    ID3D11PixelShader* ps = nullptr;
    if (FAILED(r.device->CreatePixelShader(code.data(), code.size(), nullptr, &ps))) {
      error = "pixel shader refused";
      return -1.0;
    }
    ID3D11VertexShader* vs = r.feeder(info.varyingCount, error);
    if (!vs) { ps->Release(); return -1.0; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1920; td.Height = 1080; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* targets[4] = {};
    ID3D11RenderTargetView* rtvs[4] = {};
    for (int i = 0; i < 4; i++) {
      r.device->CreateTexture2D(&td, nullptr, &targets[i]);
      r.device->CreateRenderTargetView(targets[i], nullptr, &rtvs[i]);
    }
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    r.device->CreateTexture2D(&dd, nullptr, &depth);
    r.device->CreateDepthStencilView(depth, nullptr, &dsv);
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState* dss = nullptr;
    r.device->CreateDepthStencilState(&ds, &dss);
    auto bind = [&] {
      r.context->OMSetDepthStencilState(dss, 0);
      r.context->OMSetRenderTargets(4, rtvs, dsv);
      D3D11_VIEWPORT vp = { 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
      r.context->RSSetViewports(1, &vp);
      r.context->RSSetState(r.raster);
      r.context->IASetInputLayout(nullptr);
      r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      r.context->VSSetShader(vs, nullptr, 0);
      r.context->PSSetShader(ps, nullptr, 0);
      r.context->PSSetConstantBuffers(0, 3, r.cb);
      r.bindTextures(info, false);
    };
    D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    ID3D11Query* disjoint = nullptr;
    r.device->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    ID3D11Query* t0 = nullptr;
    ID3D11Query* t1 = nullptr;
    r.device->CreateQuery(&qd, &t0);
    r.device->CreateQuery(&qd, &t1);
    std::vector<double> samples;
    for (int batch = 0; batch < 6; batch++) {
      bind();
      r.context->Draw(4, 0);   // warm
      r.context->Begin(disjoint);
      r.context->End(t0);
      for (int i = 0; i < 10; i++)
        r.context->Draw(4, 0);
      r.context->End(t1);
      r.context->End(disjoint);
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
      while (r.context->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {}
      UINT64 a = 0, b = 0;
      while (r.context->GetData(t0, &a, sizeof(a), 0) == S_FALSE) {}
      while (r.context->GetData(t1, &b, sizeof(b), 0) == S_FALSE) {}
      if (!dj.Disjoint && dj.Frequency)
        samples.push_back(double(b - a) * 1000.0 / double(dj.Frequency) / 10.0);
    }
    disjoint->Release(); t0->Release(); t1->Release();
    for (int i = 0; i < 4; i++) { rtvs[i]->Release(); targets[i]->Release(); }
    dsv->Release(); depth->Release(); dss->Release(); vs->Release(); ps->Release();
    r.context->ClearState();
    if (samples.empty())
      return -1.0;
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
  }

  int benchShaders(const std::vector<CorpusShader>& shaders) {
    DiffRunner runner;
    if (!runner.init()) { std::printf("FAIL: Direct3D 11 setup\n"); return 1; }
    double fxcTotal = 0.0, dxbcTotal = 0.0;
    struct Row { size_t n; double fxc, dxbc; };
    std::vector<Row> rows;
    for (size_t n = 0; n < shaders.size(); n++) {
      TranslateOptions9 options = shaders[n].variants[0];
      options.dotEmulation = true;
      const auto& tokens = shaders[n].tokens;
      std::string hlsl, error;
      ShaderInfo9 info, info2;
      if (!translateShader9(tokens.data(), tokens.size(), options, hlsl, info, error) || info.stage != ShaderStage9::Pixel)
        continue;
      ID3DBlob* code = nullptr;
      if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), "r", nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, nullptr)))
        continue;
      std::vector<uint8_t> fxcCode(static_cast<const uint8_t*>(code->GetBufferPointer()),
                                   static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
      code->Release();
      std::vector<uint8_t> dxbc;
      if (!compileShader9(tokens.data(), tokens.size(), options, dxbc, info2, error))
        continue;
      const double a = gpuTime(runner, fxcCode, info, error);
      const double b = gpuTime(runner, dxbc, info, error);
      if (a < 0 || b < 0)
        continue;
      fxcTotal += a;
      dxbcTotal += b;
      rows.push_back({ n, a, b });
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.dxbc / x.fxc > y.dxbc / y.fxc; });
    for (size_t i = 0; i < rows.size() && i < 8; i++)
      std::printf("  shader %zu: fxc %.4f ms, emitter %.4f ms (%+.1f %%)\n", rows[i].n, rows[i].fxc, rows[i].dxbc,
                  (rows[i].dxbc / rows[i].fxc - 1.0) * 100.0);
    std::printf("%zu pixel shaders, 1920x1080: fxc %.3f ms, emitter %.3f ms (%+.1f %%)\n", rows.size(), fxcTotal, dxbcTotal,
                fxcTotal > 0 ? (dxbcTotal / fxcTotal - 1.0) * 100.0 : 0.0);
    return 0;
  }

}

// fxc time and size of the shaders in a d3d9_variants.bin (dxup_pso_cache)
// per float emulation mode: --compile-records FILE.
static int compileRecords(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) { std::printf("cannot open %s\n", path); return 1; }
  std::vector<std::vector<uint32_t>> shaders;
  uint32_t header[2] = {}, head[2];
  if (std::fread(header, sizeof(header), 1, f) == 1)
    while (std::fread(head, sizeof(head), 1, f) == 1 && head[1] <= (4u << 20)) {
      std::vector<uint8_t> data(head[1]);
      if (std::fread(data.data(), 1, data.size(), f) != data.size()) break;
      if (head[0] == 1 && data.size() > 8) {
        std::vector<uint32_t> tokens((data.size() - 8) / 4);
        std::memcpy(tokens.data(), data.data() + 8, tokens.size() * 4);
        shaders.push_back(std::move(tokens));
      }
    }
  std::fclose(f);
  const char* names[3] = { "IEEE", "dot products 0 * x = 0", "mul, mad, dot 0 * x = 0" };
  for (int mode = 0; mode < 3; mode++) {
    double ms = 0.0, worst = 0.0;
    size_t bytes = 0;
    int count = 0;
    for (const auto& tokens : shaders) {
      TranslateOptions9 options;
      options.dotEmulation = mode >= 1;
      options.floatEmulation = mode == 2;
      std::string hlsl, error;
      ShaderInfo9 info;
      if (!translateShader9(tokens.data(), tokens.size(), options, hlsl, info, error)) continue;
      const auto start = std::chrono::steady_clock::now();
      ID3DBlob* code = nullptr;
      ID3DBlob* errors = nullptr;
      D3DCompile(hlsl.data(), hlsl.size(), "r", nullptr, nullptr, "main", info.stage == ShaderStage9::Vertex ? "vs_5_0" : "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
      const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      ms += t; worst = (std::max)(worst, t);
      if (code) { bytes += code->GetBufferSize(); code->Release(); count++; }
      if (errors) errors->Release();
    }
    std::printf("%-26s %d shaders: %.0f ms (slowest %.1f ms), %zu bytes\n", names[mode], count, ms, worst, bytes);
  }
  return 0;
}

int main(int argc, char** argv) {
  bool dumpAll = false, compileBench = false, floatEmulation = false, dxbcBackend = false;
  for (int i = 1; i + 1 < argc; i++) {
    if (std::strcmp(argv[i], "--compile-records") == 0) return compileRecords(argv[i + 1]);
    if (std::strcmp(argv[i], "--fuzz") == 0) {
      const int count = std::atoi(argv[i + 1]);
      const uint64_t seed = i + 2 < argc ? std::strtoull(argv[i + 2], nullptr, 10) : 1;
      const bool verbose = i + 3 < argc && std::strcmp(argv[i + 3], "--verbose") == 0;
      return fuzzShaders(count, seed, verbose);
    }
    if (std::strcmp(argv[i], "--bench-records") == 0)
      return benchShaders(readRecords(argv[i + 1]));
    if (std::strcmp(argv[i], "--compare-records") == 0) {
      const bool verbose = i + 2 < argc && std::strcmp(argv[i + 2], "--verbose") == 0;
      return compareShaders(readRecords(argv[i + 1]), verbose, false);
    }
  }
  const char* filter = nullptr;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--dump") == 0) dumpAll = true;
    else if (std::strcmp(argv[i], "--compile-bench") == 0) compileBench = true;
    else if (std::strcmp(argv[i], "--float-emulation") == 0) floatEmulation = true;
    else if (std::strcmp(argv[i], "--dxbc") == 0) dxbcBackend = true;
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
    for (int pass = 0; pass < 2; pass++) {
      if (!translateShader9(vs.tokens.data(), vs.tokens.size(), options, vsHlsl, vsInfo, error))
        break;
      if (vsInfo.varyingCount >= psInfo.varyingCount)
        break;
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
    // DXUP's DXBC emitter in place of fxc (--dxbc): the same info as the HLSL.
    std::vector<uint8_t> vsDxbc, psDxbc;
    if (dxbcBackend) {
      ShaderInfo9 vsInfo2, psInfo2;
      TranslateOptions9 psOptions;
      psOptions.floatEmulation = psOptions.dotEmulation = floatEmulation;
      if (!compileShader9(ps.tokens.data(), ps.tokens.size(), psOptions, psDxbc, psInfo2, error)
       || !compileShader9(vs.tokens.data(), vs.tokens.size(), options, vsDxbc, vsInfo2, error)) {
        std::printf("FAIL %-22s DXBC: %s\n", c.name, error.c_str());
        failed++;
        continue;
      }
      if (vsInfo2.varyingCount != vsInfo.varyingCount || psInfo2.varyingCount != psInfo.varyingCount
       || psInfo2.colorOutputs != psInfo.colorOutputs) {
        std::printf("FAIL %-22s DXBC info differs\n", c.name);
        failed++;
        continue;
      }
      if (dumpAll) {
        disassemble("vertex DXBC", vsDxbc);
        disassemble("pixel DXBC", psDxbc);
      }
    }
    if (!d3d9.draw(vs, ps, psInfo, reference, error)
     || !d3d11.draw(vsHlsl, vsInfo, psHlsl, psInfo, result, error, dxbcBackend ? &vsDxbc : nullptr,
                    dxbcBackend ? &psDxbc : nullptr)) {
      if (dxbcBackend) {
        disassemble("vertex DXBC", vsDxbc);
        disassemble("pixel DXBC", psDxbc);
      }
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
