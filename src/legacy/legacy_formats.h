#pragma once

// Formats of the legacy APIs on Direct3D 11: which DXGI format holds a
// legacy one, and the conversions where none matches its memory layout.

#include "legacy_types.h"

#include <dxgiformat.h>

#include <cstdint>
#include <cstring>

namespace dxup::legacy {

  struct FormatInfo {
    uint32_t bytesPerBlock;   // bytes per texel, or per 4x4 block
    bool     block;           // block compressed (DXT)
    bool     depth;
    bool     stencil;
  };

  inline FormatInfo formatInfo(Format format) {
    switch (format) {
      case FormatR8G8B8:   return { 3, false, false, false };
      case FormatA8R8G8B8:
      case FormatX8R8G8B8:
      case FormatA8B8G8R8:
      case FormatX8B8G8R8: return { 4, false, false, false };
      case FormatR5G6B5:
      case FormatX1R5G5B5:
      case FormatA1R5G5B5:
      case FormatA4R4G4B4:
      case FormatX4R4G4B4:
      case FormatA8L8:
      case FormatV8U8:     return { 2, false, false, false };
      case FormatR3G3B2:
      case FormatA8:
      case FormatP8:
      case FormatL8:
      case FormatA4L4:     return { 1, false, false, false };
      case FormatD16Lockable:
      case FormatD16:      return { 2, false, true, false };
      case FormatD15S1:    return { 2, false, true, true };
      case FormatD32:
      case FormatD24X8:    return { 4, false, true, false };
      case FormatD24S8:
      case FormatD24X4S4:  return { 4, false, true, true };
      case FormatDXT1:
      case FormatATI1:     return { 8, true, false, false };
      case FormatDXT2:
      case FormatDXT3:
      case FormatDXT4:
      case FormatDXT5:
      case FormatATI2:     return { 16, true, false, false };
      case FormatL16:
      case FormatR16F:     return { 2, false, false, false };
      case FormatDF16:     return { 2, false, true, false };
      case FormatD32FLockable:
      case FormatDF24:     return { 4, false, true, false };
      case FormatD24FS8:
      case FormatINTZ:     return { 4, false, true, true };
      case FormatA16B16G16R16:
      case FormatA16B16G16R16F:
      case FormatQ16W16V16U16:
      case FormatG32R32F:  return { 8, false, false, false };
      case FormatA32B32G32R32F: return { 16, false, false, false };
      default:             return { 4, false, false, false };
    }
  }

  // Bytes per row of a subresource in the application's layout (rows of
  // blocks for compressed formats), whole DWORDs as Direct3D 9 drivers
  // give them (an L8 surface 6 pixels wide has a pitch of 8).
  inline uint32_t legacyPitch(Format format, uint32_t width) {
    const FormatInfo info = formatInfo(format);
    const uint32_t bytes = info.block ? ((width + 3) / 4) * info.bytesPerBlock : width * info.bytesPerBlock;
    return (bytes + 3) & ~3u;
  }

  // The pitch a lock reports. Direct3D 9 drivers give ATI1 and ATI2 (BC4,
  // BC5) the pitch of an 8-bit format, the width in whole DWORDs, and games
  // write height rows of it (Trine 2 its ATI2 normal maps); the blocks lie
  // in the same bytes for ATI2 as with a pitch of block rows.
  inline uint32_t legacyLockPitch(Format format, uint32_t width) {
    if (format == FormatATI1 || format == FormatATI2)
      return (width + 3) & ~3u;
    return legacyPitch(format, width);
  }

  inline uint32_t legacyRows(Format format, uint32_t height) {
    return formatInfo(format).block ? (height + 3) / 4 : height;
  }

  // The DXGI format with the same memory layout, or UNKNOWN when the data
  // converts (to B8G8R8A8).
  inline DXGI_FORMAT directDxgiFormat(Format format) {
    switch (format) {
      case FormatA8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
      case FormatX8R8G8B8: return DXGI_FORMAT_B8G8R8X8_UNORM;
      case FormatA8B8G8R8: return DXGI_FORMAT_R8G8B8A8_UNORM;
      case FormatR5G6B5:   return DXGI_FORMAT_B5G6R5_UNORM;
      case FormatA1R5G5B5: return DXGI_FORMAT_B5G5R5A1_UNORM;
      case FormatA4R4G4B4: return DXGI_FORMAT_B4G4R4A4_UNORM;
      case FormatA8:       return DXGI_FORMAT_A8_UNORM;
      case FormatV8U8:     return DXGI_FORMAT_R8G8_SNORM;
      case FormatDXT1:     return DXGI_FORMAT_BC1_UNORM;
      case FormatDXT2:
      case FormatDXT3:     return DXGI_FORMAT_BC2_UNORM;
      case FormatDXT4:
      case FormatDXT5:     return DXGI_FORMAT_BC3_UNORM;
      case FormatATI1:     return DXGI_FORMAT_BC4_UNORM;
      case FormatATI2:     return DXGI_FORMAT_BC5_UNORM;
      case FormatD16Lockable:
      case FormatD16:
      case FormatDF16:     return DXGI_FORMAT_D16_UNORM;
      case FormatD15S1:
      case FormatD24S8:
      case FormatD24X8:
      case FormatD24X4S4:
      case FormatD24FS8:
      case FormatINTZ:
      case FormatDF24:
      case FormatD32:      return DXGI_FORMAT_D24_UNORM_S8_UINT;
      case FormatD32FLockable: return DXGI_FORMAT_D32_FLOAT;
      case FormatA2B10G10R10:
      case FormatA2R10G10B10: return DXGI_FORMAT_R10G10B10A2_UNORM;
      case FormatG16R16:   return DXGI_FORMAT_R16G16_UNORM;
      case FormatA16B16G16R16: return DXGI_FORMAT_R16G16B16A16_UNORM;
      case FormatV16U16:   return DXGI_FORMAT_R16G16_SNORM;
      case FormatQ8W8V8U8: return DXGI_FORMAT_R8G8B8A8_SNORM;
      case FormatQ16W16V16U16: return DXGI_FORMAT_R16G16B16A16_SNORM;
      // Read as (L, L, L, 1) through the view (Texture::srv): all 16 bits
      // (heightmaps: Risen 3's terrain), not converted to 8.
      case FormatL16:      return DXGI_FORMAT_R16_UNORM;
      case FormatR16F:     return DXGI_FORMAT_R16_FLOAT;
      case FormatG16R16F:  return DXGI_FORMAT_R16G16_FLOAT;
      case FormatA16B16G16R16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case FormatR32F:     return DXGI_FORMAT_R32_FLOAT;
      case FormatG32R32F:  return DXGI_FORMAT_R32G32_FLOAT;
      case FormatA32B32G32R32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
      default:             return DXGI_FORMAT_UNKNOWN;
    }
  }

  // How a shader view reads the DXGI format to give what Direct3D 9 gives
  // (D3D12 shader component mapping; 0: as it is). Direct3D 9 reads missing
  // color channels as 1, Direct3D 10+ as 0; ATI2 is (green, red, 1, 1), L16
  // (L, L, L, 1). DXVK swizzles its Vulkan views the same way. The device's
  // self-test (legacy_selftest.cpp) checks every format against it.
  inline uint32_t shaderComponentMapping(Format format) {
    auto map = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 3) | (b << 6) | (a << 9) | (1u << 12); };
    constexpr uint32_t R = 0, G = 1, One = 5;
    switch (format) {
      case FormatATI2:     return map(G, R, One, One);
      case FormatL16:      return map(R, R, R, One);
      case FormatG16R16:
      case FormatV8U8:
      case FormatV16U16:
      case FormatG16R16F:
      case FormatG32R32F:  return map(R, G, One, One);
      case FormatR16F:
      case FormatR32F:     return map(R, One, One, One);
      default:             return 0;
    }
  }

  // A2R10G10B10 keeps red in the high bits, DXGI's R10G10B10A2 in the low:
  // swapped when written to and read from the GPU (so shaders and render
  // targets see red as red).
  inline void swapRedBlue1010102(uint8_t* data, size_t texels) {
    for (size_t i = 0; i < texels; i++) {
      uint32_t v;
      std::memcpy(&v, data + i * 4, 4);
      v = (v & 0xc00ffc00u) | ((v & 0x3ffu) << 20) | ((v >> 20) & 0x3ffu);
      std::memcpy(data + i * 4, &v, 4);
    }
  }

  // Formats with an sRGB twin: created typeless, viewed both ways.
  struct SrgbFormats { DXGI_FORMAT typeless, linear, srgb; };
  inline bool srgbFormats(DXGI_FORMAT format, SrgbFormats& out) {
    switch (format) {
      case DXGI_FORMAT_B8G8R8A8_UNORM: out = { DXGI_FORMAT_B8G8R8A8_TYPELESS, format, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB }; return true;
      case DXGI_FORMAT_B8G8R8X8_UNORM: out = { DXGI_FORMAT_B8G8R8X8_TYPELESS, format, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB }; return true;
      case DXGI_FORMAT_R8G8B8A8_UNORM: out = { DXGI_FORMAT_R8G8B8A8_TYPELESS, format, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB }; return true;
      case DXGI_FORMAT_BC1_UNORM: out = { DXGI_FORMAT_BC1_TYPELESS, format, DXGI_FORMAT_BC1_UNORM_SRGB }; return true;
      case DXGI_FORMAT_BC2_UNORM: out = { DXGI_FORMAT_BC2_TYPELESS, format, DXGI_FORMAT_BC2_UNORM_SRGB }; return true;
      case DXGI_FORMAT_BC3_UNORM: out = { DXGI_FORMAT_BC3_TYPELESS, format, DXGI_FORMAT_BC3_UNORM_SRGB }; return true;
      default: return false;
    }
  }

  // Depth formats a shader samples: the resource, depth view and shader view.
  struct DepthFormats { DXGI_FORMAT typeless, dsv, srv; };
  inline bool depthFormats(DXGI_FORMAT format, DepthFormats& out) {
    switch (format) {
      case DXGI_FORMAT_D24_UNORM_S8_UINT: out = { DXGI_FORMAT_R24G8_TYPELESS, format, DXGI_FORMAT_R24_UNORM_X8_TYPELESS }; return true;
      case DXGI_FORMAT_D16_UNORM: out = { DXGI_FORMAT_R16_TYPELESS, format, DXGI_FORMAT_R16_UNORM }; return true;
      case DXGI_FORMAT_D32_FLOAT: out = { DXGI_FORMAT_R32_TYPELESS, format, DXGI_FORMAT_R32_FLOAT }; return true;
      default: return false;
    }
  }

  // Depth textures sampled with a comparison (shadow maps of NVIDIA's
  // Direct3D 9) rather than read: all depth formats but INTZ, DF16, DF24.
  inline bool shadowFormat(Format format) {
    return formatInfo(format).depth && format != FormatINTZ && format != FormatDF16 && format != FormatDF24
      && format != FormatRAWZ;
  }

  inline uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
  inline uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
  inline uint32_t expand4(uint32_t v) { return v * 17u; }

  // One texel of a legacy format as 0xAARRGGBB.
  inline uint32_t toArgb(Format format, const uint8_t* src, const uint32_t* palette) {
    uint32_t v16 = 0;
    switch (format) {
      case FormatA8R8G8B8: { uint32_t v; std::memcpy(&v, src, 4); return v; }
      case FormatX8R8G8B8: { uint32_t v; std::memcpy(&v, src, 4); return v | 0xff000000u; }
      case FormatA8B8G8R8: { uint32_t v; std::memcpy(&v, src, 4);
        return (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu); }
      case FormatX8B8G8R8: { uint32_t v; std::memcpy(&v, src, 4);
        return 0xff000000u | (v & 0x0000ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu); }
      case FormatR8G8B8:   return 0xff000000u | (uint32_t(src[2]) << 16) | (uint32_t(src[1]) << 8) | src[0];
      case FormatR5G6B5:   std::memcpy(&v16, src, 2);
        return 0xff000000u | (expand5(v16 >> 11) << 16) | (expand6((v16 >> 5) & 63) << 8) | expand5(v16 & 31);
      case FormatX1R5G5B5:
      case FormatA1R5G5B5: std::memcpy(&v16, src, 2);
        return ((format == FormatX1R5G5B5 || (v16 & 0x8000)) ? 0xff000000u : 0u)
          | (expand5((v16 >> 10) & 31) << 16) | (expand5((v16 >> 5) & 31) << 8) | expand5(v16 & 31);
      case FormatA4R4G4B4:
      case FormatX4R4G4B4: std::memcpy(&v16, src, 2);
        return ((format == FormatX4R4G4B4 ? 15u : (v16 >> 12)) * 17u << 24)
          | (expand4((v16 >> 8) & 15) << 16) | (expand4((v16 >> 4) & 15) << 8) | expand4(v16 & 15);
      case FormatR3G3B2: {
        const uint32_t r = (src[0] >> 5) & 7, g = (src[0] >> 2) & 7, b = src[0] & 3;
        return 0xff000000u | ((r * 255 / 7) << 16) | ((g * 255 / 7) << 8) | (b * 85);
      }
      case FormatA8:       return uint32_t(src[0]) << 24;
      case FormatL8:       return 0xff000000u | uint32_t(src[0]) * 0x010101u;
      case FormatA8L8:     return (uint32_t(src[1]) << 24) | uint32_t(src[0]) * 0x010101u;
      case FormatA4L4:     return (expand4(src[0] >> 4) << 24) | expand4(src[0] & 15) * 0x010101u;
      case FormatP8:       return palette ? palette[src[0]] : 0xff000000u;
      case FormatL16:      return 0xff000000u | uint32_t(src[1]) * 0x010101u;
      default:             return 0xffff00ffu;
    }
  }

  // And back (render targets read back in a converted format).
  inline void fromArgb(Format format, uint32_t argb, uint8_t* dst) {
    const uint32_t a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    uint16_t v16 = 0;
    switch (format) {
      case FormatA8R8G8B8:
      case FormatX8R8G8B8: std::memcpy(dst, &argb, 4); return;
      case FormatA8B8G8R8:
      case FormatX8B8G8R8: { const uint32_t v = (argb & 0xff00ff00u) | (r) | (b << 16); std::memcpy(dst, &v, 4); return; }
      case FormatR8G8B8:   dst[0] = uint8_t(b); dst[1] = uint8_t(g); dst[2] = uint8_t(r); return;
      case FormatR5G6B5:   v16 = uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); break;
      case FormatX1R5G5B5:
      case FormatA1R5G5B5: v16 = uint16_t((a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); break;
      case FormatA4R4G4B4:
      case FormatX4R4G4B4: v16 = uint16_t(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); break;
      case FormatR3G3B2:   dst[0] = uint8_t(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6)); return;
      case FormatA8:       dst[0] = uint8_t(a); return;
      case FormatL8:       dst[0] = uint8_t(r); return;
      case FormatA8L8:     dst[0] = uint8_t(r); dst[1] = uint8_t(a); return;
      case FormatA4L4:     dst[0] = uint8_t(((a >> 4) << 4) | (r >> 4)); return;
      case FormatL16:      dst[0] = uint8_t(r); dst[1] = uint8_t(r); return;
      default: return;
    }
    std::memcpy(dst, &v16, 2);
  }

}
