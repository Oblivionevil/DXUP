// Self-test of the formats Direct3D 7-9 games read (glitch reports).
//
// With diagnostics on (DXUP Manager's "Record a problem", the info log) the
// device fills a texture of every format Direct3D 9 offers with known texels
// when it is created, reads them back through the GPU as a shader reads
// them and compares that with what Direct3D 9 gives. A format DXUP stores
// or reads differently - fewer bits, missing channels as 0 instead of 1,
// channels swapped - shows in games as wrong or missing objects and nowhere
// else (Risen 3's terrain heights in L16 kept 8 bits). Each difference is
// logged as a glitch, with the texel.

#include "legacy_device.h"
#include "legacy_formats.h"
#include "../util/util_types.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace dxup::legacy {

  namespace {
    struct FormatName { Format format; const char* name; };
    const FormatName kTextureFormats[] = {
      { FormatA8R8G8B8, "A8R8G8B8" }, { FormatX8R8G8B8, "X8R8G8B8" }, { FormatR8G8B8, "R8G8B8" },
      { FormatR5G6B5, "R5G6B5" }, { FormatX1R5G5B5, "X1R5G5B5" }, { FormatA1R5G5B5, "A1R5G5B5" },
      { FormatA4R4G4B4, "A4R4G4B4" }, { FormatX4R4G4B4, "X4R4G4B4" }, { FormatA8, "A8" },
      { FormatA2B10G10R10, "A2B10G10R10" }, { FormatA8B8G8R8, "A8B8G8R8" }, { FormatX8B8G8R8, "X8B8G8R8" },
      { FormatG16R16, "G16R16" }, { FormatA2R10G10B10, "A2R10G10B10" }, { FormatA16B16G16R16, "A16B16G16R16" },
      { FormatL8, "L8" }, { FormatA8L8, "A8L8" }, { FormatA4L4, "A4L4" }, { FormatL16, "L16" },
      { FormatV8U8, "V8U8" }, { FormatQ8W8V8U8, "Q8W8V8U8" }, { FormatV16U16, "V16U16" },
      { FormatQ16W16V16U16, "Q16W16V16U16" }, { FormatR16F, "R16F" }, { FormatG16R16F, "G16R16F" },
      { FormatA16B16G16R16F, "A16B16G16R16F" }, { FormatR32F, "R32F" }, { FormatG32R32F, "G32R32F" },
      { FormatA32B32G32R32F, "A32B32G32R32F" }, { FormatDXT1, "DXT1" }, { FormatDXT2, "DXT2" },
      { FormatDXT3, "DXT3" }, { FormatDXT4, "DXT4" }, { FormatDXT5, "DXT5" }, { FormatATI1, "ATI1" },
      { FormatATI2, "ATI2" },
    };

    float unorm(uint32_t value, uint32_t bits) { return float(value) / float((1u << bits) - 1u); }
    float snorm(int32_t value, uint32_t bits) {
      return (std::max)(float(value) / float((1 << (bits - 1)) - 1), -1.0f);
    }

    float halfToFloat(uint16_t h) {
      const uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 31, mantissa = h & 1023;
      uint32_t bits;
      if (!exponent) {
        // Zero or denormal.
        const float value = std::ldexp(float(mantissa), -24);
        std::memcpy(&bits, &value, 4);
        bits |= sign;
      } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
      } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
      }
      float out;
      std::memcpy(&out, &bits, 4);
      return out;
    }

    uint16_t floatToHalf(float value) {
      uint32_t bits;
      std::memcpy(&bits, &value, 4);
      const uint32_t sign = (bits >> 16) & 0x8000;
      const int32_t exponent = int32_t((bits >> 23) & 255) - 112;
      if (exponent <= 0)
        return uint16_t(sign);
      if (exponent >= 31)
        return uint16_t(sign | 0x7c00);
      return uint16_t(sign | (uint32_t(exponent) << 10) | ((bits >> 13) & 1023));
    }

    template<typename T> T read(const uint8_t* p) { T value; std::memcpy(&value, p, sizeof(T)); return value; }

    // What a Direct3D 9 shader reads from a texel (block formats: the
    // block's texels are all the same here), and how far a correct result
    // may be from it. Missing color channels read 1 (DXVK's swizzles).
    bool referenceTexel(Format format, const uint8_t* p, float out[4], float& tolerance) {
      const float u8 = 0.6f / 255.0f, u16 = 0.6f / 65535.0f;
      tolerance = u8;
      auto set = [&](float r, float g, float b, float a) { out[0] = r; out[1] = g; out[2] = b; out[3] = a; };
      switch (format) {
        case FormatA8R8G8B8: set(unorm(p[2], 8), unorm(p[1], 8), unorm(p[0], 8), unorm(p[3], 8)); return true;
        case FormatX8R8G8B8: set(unorm(p[2], 8), unorm(p[1], 8), unorm(p[0], 8), 1.0f); return true;
        case FormatR8G8B8:   set(unorm(p[2], 8), unorm(p[1], 8), unorm(p[0], 8), 1.0f); return true;
        case FormatA8B8G8R8: set(unorm(p[0], 8), unorm(p[1], 8), unorm(p[2], 8), unorm(p[3], 8)); return true;
        case FormatX8B8G8R8: set(unorm(p[0], 8), unorm(p[1], 8), unorm(p[2], 8), 1.0f); return true;
        case FormatR5G6B5: {
          const uint16_t v = read<uint16_t>(p);
          set(unorm(v >> 11, 5), unorm((v >> 5) & 63, 6), unorm(v & 31, 5), 1.0f);
          tolerance = 0.6f / 31.0f;
          return true;
        }
        case FormatX1R5G5B5:
        case FormatA1R5G5B5: {
          const uint16_t v = read<uint16_t>(p);
          set(unorm((v >> 10) & 31, 5), unorm((v >> 5) & 31, 5), unorm(v & 31, 5),
              format == FormatX1R5G5B5 ? 1.0f : float(v >> 15));
          tolerance = 0.6f / 31.0f;
          return true;
        }
        case FormatA4R4G4B4:
        case FormatX4R4G4B4: {
          const uint16_t v = read<uint16_t>(p);
          set(unorm((v >> 8) & 15, 4), unorm((v >> 4) & 15, 4), unorm(v & 15, 4),
              format == FormatX4R4G4B4 ? 1.0f : unorm(v >> 12, 4));
          tolerance = 0.6f / 15.0f;
          return true;
        }
        case FormatA8:       set(0.0f, 0.0f, 0.0f, unorm(p[0], 8)); return true;
        case FormatA2B10G10R10:
        case FormatA2R10G10B10: {
          const uint32_t v = read<uint32_t>(p);
          const float low = unorm(v & 1023, 10), mid = unorm((v >> 10) & 1023, 10), high = unorm((v >> 20) & 1023, 10);
          if (format == FormatA2B10G10R10)
            set(low, mid, high, unorm(v >> 30, 2));
          else
            set(high, mid, low, unorm(v >> 30, 2));
          tolerance = 0.6f / 1023.0f;
          return true;
        }
        case FormatG16R16:
          set(unorm(read<uint16_t>(p), 16), unorm(read<uint16_t>(p + 2), 16), 1.0f, 1.0f);
          tolerance = u16;
          return true;
        case FormatA16B16G16R16:
          set(unorm(read<uint16_t>(p), 16), unorm(read<uint16_t>(p + 2), 16), unorm(read<uint16_t>(p + 4), 16),
              unorm(read<uint16_t>(p + 6), 16));
          tolerance = u16;
          return true;
        case FormatL8:       set(unorm(p[0], 8), unorm(p[0], 8), unorm(p[0], 8), 1.0f); return true;
        case FormatA8L8:     set(unorm(p[0], 8), unorm(p[0], 8), unorm(p[0], 8), unorm(p[1], 8)); return true;
        case FormatA4L4:
          set(unorm(p[0] & 15, 4), unorm(p[0] & 15, 4), unorm(p[0] & 15, 4), unorm(p[0] >> 4, 4));
          tolerance = 0.6f / 15.0f;
          return true;
        case FormatL16: {
          const float l = unorm(read<uint16_t>(p), 16);
          set(l, l, l, 1.0f);
          tolerance = u16;
          return true;
        }
        case FormatV8U8:
          set(snorm(int8_t(p[0]), 8), snorm(int8_t(p[1]), 8), 1.0f, 1.0f);
          tolerance = 0.6f / 127.0f;
          return true;
        case FormatQ8W8V8U8:
          set(snorm(int8_t(p[0]), 8), snorm(int8_t(p[1]), 8), snorm(int8_t(p[2]), 8), snorm(int8_t(p[3]), 8));
          tolerance = 0.6f / 127.0f;
          return true;
        case FormatV16U16:
          set(snorm(read<int16_t>(p), 16), snorm(read<int16_t>(p + 2), 16), 1.0f, 1.0f);
          tolerance = 0.6f / 32767.0f;
          return true;
        case FormatQ16W16V16U16:
          set(snorm(read<int16_t>(p), 16), snorm(read<int16_t>(p + 2), 16), snorm(read<int16_t>(p + 4), 16),
              snorm(read<int16_t>(p + 6), 16));
          tolerance = 0.6f / 32767.0f;
          return true;
        case FormatR16F:     set(halfToFloat(read<uint16_t>(p)), 1.0f, 1.0f, 1.0f); tolerance = 0.0f; return true;
        case FormatG16R16F:
          set(halfToFloat(read<uint16_t>(p)), halfToFloat(read<uint16_t>(p + 2)), 1.0f, 1.0f);
          tolerance = 0.0f;
          return true;
        case FormatA16B16G16R16F:
          set(halfToFloat(read<uint16_t>(p)), halfToFloat(read<uint16_t>(p + 2)), halfToFloat(read<uint16_t>(p + 4)),
              halfToFloat(read<uint16_t>(p + 6)));
          tolerance = 0.0f;
          return true;
        case FormatR32F:     set(read<float>(p), 1.0f, 1.0f, 1.0f); tolerance = 0.0f; return true;
        case FormatG32R32F:  set(read<float>(p), read<float>(p + 4), 1.0f, 1.0f); tolerance = 0.0f; return true;
        case FormatA32B32G32R32F:
          set(read<float>(p), read<float>(p + 4), read<float>(p + 8), read<float>(p + 12));
          tolerance = 0.0f;
          return true;
        default:
          break;
      }
      // Blocks with one color: endpoint 0 (and alpha) everywhere.
      const uint8_t* color = format == FormatDXT1 ? p : p + 8;
      const uint16_t c0 = read<uint16_t>(color);
      const float r = unorm(c0 >> 11, 5), g = unorm((c0 >> 5) & 63, 6), b = unorm(c0 & 31, 5);
      tolerance = 1.5f / 255.0f;
      switch (format) {
        case FormatDXT1: set(r, g, b, 1.0f); return true;
        case FormatDXT2:
        case FormatDXT3: set(r, g, b, unorm(p[0] & 15, 4)); return true;
        case FormatDXT4:
        case FormatDXT5: set(r, g, b, unorm(p[0], 8)); return true;
        case FormatATI1: set(unorm(p[0], 8), 0.0f, 0.0f, 1.0f); return true;
        case FormatATI2: set(unorm(p[8], 8), unorm(p[0], 8), 1.0f, 1.0f); return true;
        default:         return false;
      }
    }

    // Known texels of a format: varied bits, floats without NaN.
    void fillTexels(Format format, uint8_t* texels, uint32_t bytesPerTexel, bool block) {
      uint32_t seed = 0x9e3779b9u ^ uint32_t(format);
      auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
      if (block) {
        std::memset(texels, 0, 16);
        const uint16_t color = uint16_t((20u << 11) | (40u << 5) | 10u);
        switch (format) {
          case FormatDXT1:
            std::memcpy(texels, &color, 2);
            std::memcpy(texels + 2, &color, 2);
            break;
          case FormatDXT2:
          case FormatDXT3:
            std::memset(texels, 0x99, 8);   // alpha 9 / 15 everywhere
            std::memcpy(texels + 8, &color, 2);
            std::memcpy(texels + 10, &color, 2);
            break;
          case FormatDXT4:
          case FormatDXT5:
            texels[0] = texels[1] = 0xa5;   // alpha endpoints, indices 0
            std::memcpy(texels + 8, &color, 2);
            std::memcpy(texels + 10, &color, 2);
            break;
          case FormatATI1:
            texels[0] = texels[1] = 0x3c;
            break;
          case FormatATI2:
            texels[0] = texels[1] = 0x3c;   // red
            texels[8] = texels[9] = 0xd2;   // green
            break;
          default:
            break;
        }
        return;
      }
      static const float kFloats[] = { 0.25f, -1.5f, 1000.0f, 0.333333f, 3.75f, -0.0625f, 65.5f, 0.001f,
                                       12.125f, -300.0f, 0.75f, 2.0f, -7.5f, 0.5f, 100.25f, 1.0f };
      const bool floats = format == FormatR32F || format == FormatG32R32F || format == FormatA32B32G32R32F;
      const bool halves = format == FormatR16F || format == FormatG16R16F || format == FormatA16B16G16R16F;
      for (uint32_t i = 0; i < 16; i++) {
        uint8_t* texel = texels + size_t(i) * bytesPerTexel;
        if (floats) {
          for (uint32_t c = 0; c < bytesPerTexel / 4; c++)
            std::memcpy(texel + c * 4, &kFloats[(i + c * 5) & 15], 4);
        } else if (halves) {
          for (uint32_t c = 0; c < bytesPerTexel / 2; c++) {
            const uint16_t h = floatToHalf(kFloats[(i + c * 5) & 15]);
            std::memcpy(texel + c * 2, &h, 2);
          }
        } else {
          for (uint32_t b = 0; b < bytesPerTexel; b++)
            texel[b] = uint8_t(next());
        }
      }
    }
  }

  namespace {
    struct DeclTypeName { uint8_t type; const char* name; };
    const DeclTypeName kDeclTypes[] = {
      { DeclFloat1, "FLOAT1" }, { DeclFloat2, "FLOAT2" }, { DeclFloat3, "FLOAT3" }, { DeclFloat4, "FLOAT4" },
      { DeclD3DColor, "D3DCOLOR" }, { DeclUByte4, "UBYTE4" }, { DeclShort2, "SHORT2" }, { DeclShort4, "SHORT4" },
      { DeclUByte4N, "UBYTE4N" }, { DeclShort2N, "SHORT2N" }, { DeclShort4N, "SHORT4N" }, { DeclUShort2N, "USHORT2N" },
      { DeclUShort4N, "USHORT4N" }, { DeclUDec3, "UDEC3" }, { DeclDec3N, "DEC3N" }, { DeclFloat16x2, "FLOAT16_2" },
      { DeclFloat16x4, "FLOAT16_4" },
    };

    // What a Direct3D 9 vertex shader reads from an element: missing
    // components (0, 0, 0, 1), UDEC3 and DEC3N with w = 1.
    void referenceElement(uint8_t type, const uint8_t* p, float out[4], float& tolerance) {
      auto set = [&](float x, float y, float z, float w) { out[0] = x; out[1] = y; out[2] = z; out[3] = w; };
      tolerance = 1e-6f;
      auto s16 = [&](int i) { return float(read<int16_t>(p + i * 2)); };
      auto u16 = [&](int i) { return float(read<uint16_t>(p + i * 2)); };
      auto f32 = [&](int i) { return read<float>(p + i * 4); };
      auto f16 = [&](int i) { return halfToFloat(read<uint16_t>(p + i * 2)); };
      const uint32_t packed = read<uint32_t>(p);
      auto s10 = [&](int i) {
        const int32_t v = int32_t((packed >> (i * 10)) & 1023u);
        return (std::max)(float(v >= 512 ? v - 1024 : v) / 511.0f, -1.0f);
      };
      switch (type) {
        case DeclFloat1: set(f32(0), 0, 0, 1); break;
        case DeclFloat2: set(f32(0), f32(1), 0, 1); break;
        case DeclFloat3: set(f32(0), f32(1), f32(2), 1); break;
        case DeclFloat4: set(f32(0), f32(1), f32(2), f32(3)); break;
        case DeclD3DColor:
          set(unorm(p[2], 8), unorm(p[1], 8), unorm(p[0], 8), unorm(p[3], 8));
          tolerance = 0.6f / 255.0f;
          break;
        case DeclUByte4: set(p[0], p[1], p[2], p[3]); break;
        case DeclShort2: set(s16(0), s16(1), 0, 1); break;
        case DeclShort4: set(s16(0), s16(1), s16(2), s16(3)); break;
        case DeclUByte4N:
          set(unorm(p[0], 8), unorm(p[1], 8), unorm(p[2], 8), unorm(p[3], 8));
          tolerance = 0.6f / 255.0f;
          break;
        case DeclShort2N: set(s16(0) / 32767.0f, s16(1) / 32767.0f, 0, 1); tolerance = 1e-4f; break;
        case DeclShort4N:
          set(s16(0) / 32767.0f, s16(1) / 32767.0f, s16(2) / 32767.0f, s16(3) / 32767.0f);
          tolerance = 1e-4f;
          break;
        case DeclUShort2N: set(u16(0) / 65535.0f, u16(1) / 65535.0f, 0, 1); tolerance = 1e-5f; break;
        case DeclUShort4N:
          set(u16(0) / 65535.0f, u16(1) / 65535.0f, u16(2) / 65535.0f, u16(3) / 65535.0f);
          tolerance = 1e-5f;
          break;
        case DeclUDec3: set(float(packed & 1023u), float((packed >> 10) & 1023u), float((packed >> 20) & 1023u), 1); break;
        case DeclDec3N: set(s10(0), s10(1), s10(2), 1); tolerance = 1e-4f; break;
        case DeclFloat16x2: set(f16(0), f16(1), 0, 1); break;
        case DeclFloat16x4: set(f16(0), f16(1), f16(2), f16(3)); break;
        default: set(0, 0, 0, 0); break;
      }
    }
  }

  // Each vertex element type through the whole path a game's draw takes -
  // the declaration, the input layout, DXUP's shader compiler, the
  // pipeline - one point per type into a float target, read back.
  void Device::selfTestVertexTypes() {
    using Compile = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*,
      const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = module ? reinterpret_cast<Compile>(GetProcAddress(module, "D3DCompile")) : nullptr;
    if (!compile)
      return;
    static const char kVs[] =
      "struct VO { float4 p : POSITION; float4 v : TEXCOORD0; };\n"
      "VO main(float4 p : POSITION, float4 a : TEXCOORD0) { VO o; o.p = p; o.v = a; return o; }\n";
    static const char kPs[] = "float4 main(float4 v : TEXCOORD0) : COLOR { return v; }\n";
    ID3DBlob* vsCode = nullptr;
    ID3DBlob* psCode = nullptr;
    std::shared_ptr<Shader> vs, ps;
    std::string error;
    g_selfTesting = true;
    if (SUCCEEDED(compile(kVs, sizeof(kVs) - 1, "selftest", nullptr, nullptr, "main", "vs_3_0", 0, 0, &vsCode, nullptr))
     && SUCCEEDED(compile(kPs, sizeof(kPs) - 1, "selftest", nullptr, nullptr, "main", "ps_3_0", 0, 0, &psCode, nullptr))) {
      vs = Shader::create(this, static_cast<const uint32_t*>(vsCode->GetBufferPointer()), vsCode->GetBufferSize() / 4, error);
      ps = Shader::create(this, static_cast<const uint32_t*>(psCode->GetBufferPointer()), psCode->GetBufferSize() / 4, error);
    }
    if (vsCode) vsCode->Release();
    if (psCode) psCode->Release();
    const uint32_t count = uint32_t(sizeof(kDeclTypes) / sizeof(kDeclTypes[0]));
    TextureDesc desc;
    desc.width = count;
    desc.height = 1;
    desc.levels = 1;
    desc.format = FormatA32B32G32R32F;
    desc.renderTarget = true;
    Texture target(this, desc);
    if (!vs || !ps || !target.valid()) {
      g_selfTesting = false;
      return;
    }
    setRenderTarget(0, &target, 0);
    setDepthStencil(nullptr);
    setViewport({ 0, 0, count, 1, 0.0f, 1.0f });
    setRenderState(RsZEnable, 0);
    setRenderState(RsZWriteEnable, 0);
    setRenderState(RsCullMode, 1);
    setRenderState(RsLighting, 0);
    setRenderState(RsAlphaBlendEnable, 0);
    setVertexShader(vs.get());
    setPixelShader(ps.get());
    clear(0, nullptr, 1, 0, 1.0f, 0);
    struct Vertex { float position[4]; uint8_t element[16]; };
    std::vector<std::vector<uint8_t>> elements(count);
    for (uint32_t i = 0; i < count; i++) {
      Vertex v = {};
      v.position[0] = (float(i) + 0.5f) / float(count) * 2.0f - 1.0f;
      v.position[2] = 0.5f;
      v.position[3] = 1.0f;
      // Varied bits; floats and halves without NaN.
      uint32_t seed = 0x1234567u + i * 7919u;
      for (auto& b : v.element) { seed = seed * 1664525u + 1013904223u; b = uint8_t(seed >> 24); }
      const uint8_t type = kDeclTypes[i].type;
      static const float kFloats[4] = { 0.25f, -1.5f, 1000.0f, 0.333333f };
      if (type <= DeclFloat4)
        std::memcpy(v.element, kFloats, 16);
      if (type == DeclFloat16x2 || type == DeclFloat16x4) {
        for (int c = 0; c < 4; c++) {
          const uint16_t h = floatToHalf(kFloats[c]);
          std::memcpy(v.element + c * 2, &h, 2);
        }
      }
      // The two top bits of 10:10:10:2 types other than w = 1.
      if (type == DeclUDec3 || type == DeclDec3N)
        v.element[3] = uint8_t((v.element[3] & 0x3f) | 0x80);
      elements[i].assign(v.element, v.element + 16);
      const VertexElement layout[2] = { { 0, 0, DeclFloat4, 0, 0, 0 }, { 0, 16, type, 0, 5, 0 } };
      VertexDecl decl(layout, 2);
      setVertexDeclaration(&decl);
      drawPrimitiveUP(PrimitivePointList, 1, &v, sizeof(v));
      setVertexDeclaration(nullptr);
    }
    setVertexShader(nullptr);
    setPixelShader(nullptr);
    setRenderTarget(0, nullptr, 0);
    void* data = nullptr;
    uint32_t pitch = 0;
    uint32_t differing = 0;
    std::string names;
    if (target.lock(0, nullptr, true, &data, &pitch)) {
      const float* got = static_cast<const float*>(data);
      for (uint32_t i = 0; i < count; i++) {
        float expected[4], tolerance;
        referenceElement(kDeclTypes[i].type, elements[i].data(), expected, tolerance);
        const float* texel = got + size_t(i) * 4;
        for (uint32_t c = 0; c < 4; c++) {
          if (std::fabs(texel[c] - expected[c]) <= (std::max)(tolerance, std::fabs(expected[c]) * 1e-5f))
            continue;
          differing++;
          names += names.empty() ? kDeclTypes[i].name : std::string(", ") + kDeclTypes[i].name;
          logMessage(LogLevel::Warn, "glitch: self-test: vertex elements of type %s read differently from Direct3D 9: "
            "bytes %02x %02x %02x %02x %02x %02x %02x %02x give (%g, %g, %g, %g), Direct3D 9 (%g, %g, %g, %g)",
            kDeclTypes[i].name, elements[i][0], elements[i][1], elements[i][2], elements[i][3], elements[i][4],
            elements[i][5], elements[i][6], elements[i][7], texel[0], texel[1], texel[2], texel[3], expected[0],
            expected[1], expected[2], expected[3]);
          break;
        }
      }
      target.unlock(0);
    } else {
      differing = count;
      names = "the target could not be read";
    }
    g_selfTesting = false;
    if (differing)
      logMessage(LogLevel::Warn, "glitch: self-test: %u of %u vertex element types differ from Direct3D 9 (%s)", differing, count,
        names.c_str());
    else
      logMessage(LogLevel::Warn, "self-test: %u vertex element types read as with Direct3D 9", count);
  }

  void Device::selfTestFormats() {
    using Compile = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*,
      const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = module ? reinterpret_cast<Compile>(GetProcAddress(module, "D3DCompile")) : nullptr;
    if (!compile)
      return;
    static const char kVs[] =
      "float4 main(uint id : SV_VertexID) : SV_Position {\n"
      "  return float4(float2((id << 1) & 2, id & 2) * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
      "}\n";
    static const char kPs[] =
      "Texture2D<float4> t : register(t0);\n"
      "float4 main(float4 p : SV_Position) : SV_Target { return t.Load(int3(int2(p.xy), 0)); }\n";
    ID3DBlob* vsCode = nullptr;
    ID3DBlob* psCode = nullptr;
    Ref<ID3D11VertexShader> vs;
    Ref<ID3D11PixelShader> ps;
    if (FAILED(compile(kVs, sizeof(kVs) - 1, "selftest", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, nullptr))
     || FAILED(compile(kPs, sizeof(kPs) - 1, "selftest", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psCode, nullptr))
     || FAILED(m_device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs.put()))
     || FAILED(m_device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, ps.put()))) {
      if (vsCode) vsCode->Release();
      if (psCode) psCode->Release();
      return;
    }
    vsCode->Release();
    psCode->Release();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 4;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Ref<ID3D11Texture2D> target, readback;
    Ref<ID3D11RenderTargetView> rtv;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, target.put()))
     || FAILED(m_device->CreateRenderTargetView(target.get(), nullptr, rtv.put())))
      return;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, readback.put())))
      return;

    uint32_t tested = 0, differing = 0;
    std::string names;
    for (const FormatName& entry : kTextureFormats) {
      const FormatInfo info = formatInfo(entry.format);
      TextureDesc desc;
      desc.width = desc.height = 4;
      desc.levels = 1;
      desc.format = entry.format;
      Texture texture(this, desc);
      tested++;
      auto differs = [&](const std::string& what) {
        differing++;
        names += names.empty() ? entry.name : std::string(", ") + entry.name;
        logMessage(LogLevel::Warn, "glitch: self-test: textures of format %s %s", entry.name, what.c_str());
      };
      if (!texture.valid()) {
        differs("cannot be created (Direct3D 9 offers them)");
        continue;
      }
      std::vector<uint8_t> texels(16 * size_t(info.bytesPerBlock));
      fillTexels(entry.format, texels.data(), info.bytesPerBlock, info.block);
      void* data = nullptr;
      uint32_t pitch = 0;
      if (!texture.lock(0, nullptr, false, &data, &pitch)) {
        differs("cannot be locked");
        continue;
      }
      if (info.block) {
        std::memcpy(data, texels.data(), info.bytesPerBlock);
      } else {
        for (uint32_t y = 0; y < 4; y++)
          std::memcpy(static_cast<uint8_t*>(data) + size_t(y) * pitch, texels.data() + size_t(y) * 4 * info.bytesPerBlock,
                      size_t(4) * info.bytesPerBlock);
      }
      texture.unlock(0);
      texture.flush();
      ID3D11ShaderResourceView* view = texture.srv(false);
      if (!view) {
        differs("cannot be read by shaders");
        continue;
      }
      const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f };
      const D3D11_RECT scissor = { 0, 0, 4, 4 };
      ID3D11RenderTargetView* targets[1] = { rtv.get() };
      m_context->ClearState();
      m_context->OMSetRenderTargets(1, targets, nullptr);
      m_context->RSSetViewports(1, &viewport);
      m_context->RSSetScissorRects(1, &scissor);
      m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      m_context->VSSetShader(vs.get(), nullptr, 0);
      m_context->PSSetShader(ps.get(), nullptr, 0);
      m_context->PSSetShaderResources(0, 1, &view);
      m_context->Draw(3, 0);
      m_context->CopyResource(readback.get(), target.get());
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (FAILED(m_context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        continue;
      std::string first;
      for (uint32_t i = 0; i < 16 && first.empty(); i++) {
        const uint8_t* texel = info.block ? texels.data() : texels.data() + size_t(i) * info.bytesPerBlock;
        float expected[4], tolerance = 0.0f;
        if (!referenceTexel(entry.format, texel, expected, tolerance))
          break;
        const float* got = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData)
          + size_t(i / 4) * mapped.RowPitch) + size_t(i % 4) * 4;
        for (uint32_t c = 0; c < 4; c++) {
          const float allowed = (std::max)(tolerance, std::fabs(expected[c]) * 1e-6f);
          if (std::fabs(got[c] - expected[c]) <= allowed)
            continue;
          char bytes[64] = {};
          for (uint32_t b = 0; b < (std::min)(info.bytesPerBlock, 16u); b++)
            sprintf_s(bytes + b * 3, sizeof(bytes) - b * 3, "%02x ", texel[b]);
          char text[384];
          sprintf_s(text, "read differently from Direct3D 9: texel %u (bytes %s) gives (%g, %g, %g, %g), Direct3D 9 (%g, %g, %g, %g)",
            i, bytes, got[0], got[1], got[2], got[3], expected[0], expected[1], expected[2], expected[3]);
          first = text;
          break;
        }
      }
      m_context->Unmap(readback.get(), 0);
      if (!first.empty())
        differs(first);
    }
    // Depth textures shaders read (not compare): INTZ gives the depth in
    // every channel, as NVIDIA's Direct3D 9 does.
    {
      TextureDesc desc;
      desc.width = desc.height = 4;
      desc.levels = 1;
      desc.format = FormatINTZ;
      desc.depthStencil = true;
      desc.sampledDepth = true;
      Texture texture(this, desc);
      tested++;
      ID3D11ShaderResourceView* view = texture.valid() ? texture.srv(false) : nullptr;
      std::string what;
      if (!view || !texture.dsv()) {
        what = "cannot be read by shaders";
      } else {
        m_context->ClearState();
        m_context->ClearDepthStencilView(texture.dsv(), D3D11_CLEAR_DEPTH, 0.25f, 0);
        const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f };
        const D3D11_RECT scissor = { 0, 0, 4, 4 };
        ID3D11RenderTargetView* targets[1] = { rtv.get() };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        m_context->RSSetViewports(1, &viewport);
        m_context->RSSetScissorRects(1, &scissor);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(vs.get(), nullptr, 0);
        m_context->PSSetShader(ps.get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &view);
        m_context->Draw(3, 0);
        m_context->CopyResource(readback.get(), target.get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(m_context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
          const float* got = static_cast<const float*>(mapped.pData);
          bool same = true;
          for (int c = 0; c < 4; c++)
            same = same && std::fabs(got[c] - 0.25f) <= 1e-5f;
          if (!same) {
            char text[160];
            sprintf_s(text, "read differently from Direct3D 9: depth 0.25 gives (%g, %g, %g, %g), Direct3D 9 (0.25, 0.25, 0.25, 0.25)",
              got[0], got[1], got[2], got[3]);
            what = text;
          }
          m_context->Unmap(readback.get(), 0);
        }
      }
      if (!what.empty()) {
        differing++;
        names += names.empty() ? "INTZ" : ", INTZ";
        logMessage(LogLevel::Warn, "glitch: self-test: textures of format INTZ %s", what.c_str());
      }
    }
    m_context->ClearState();
    invalidateBindings();
    if (differing)
      logMessage(LogLevel::Warn, "glitch: self-test: %u of %u texture formats differ from Direct3D 9 (%s)", differing, tested,
        names.c_str());
    else
      logMessage(LogLevel::Warn, "self-test: %u texture formats read as with Direct3D 9", tested);
  }

}
