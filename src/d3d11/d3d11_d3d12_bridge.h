#pragma once

#include "../directx/directx_backend.h"
#include <cstring>

#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_1.h>
#ifdef DXUP_D3D12_BACKEND
#include "../d3d12/d3d12_backend_device.h"
#endif
#endif

namespace dxup {

#ifdef _WIN32
  // dxup_final_pass_trace.txt next to the DLL: the state of every
  // full-screen pass in the log, with CPU copies of all constant buffers.
  // For developers only: copying each mapped constant buffer back costs a
  // D3D9 game 98 % of its frame rate, so the detailed log does not do it.
  inline bool d3d11FinalPassTraceEnabled() {
    static bool enabled = [] {
      char markerPath[MAX_PATH] = {};
      HMODULE module = nullptr;
      GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&d3d11FinalPassTraceEnabled),
        &module);
      if (module)
        GetModuleFileNameA(module, markerPath, MAX_PATH);
      char* lastSlash = strrchr(markerPath, '\\');
      if (lastSlash)
        *(lastSlash + 1) = '\0';
      else
        markerPath[0] = '\0';
      strcat_s(markerPath, MAX_PATH, "dxup_final_pass_trace.txt");
      DWORD attributes = GetFileAttributesA(markerPath);
      return attributes != INVALID_FILE_ATTRIBUTES
          && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }();
    return enabled;
  }

  inline bool d3d11InfoLoggingEnabled() {
    static bool enabled = [] {
      char markerPath[MAX_PATH] = {};
      HMODULE module = nullptr;
      GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&d3d11InfoLoggingEnabled),
        &module);

      if (module)
        GetModuleFileNameA(module, markerPath, MAX_PATH);

      char* lastSlash = strrchr(markerPath, '\\');
      if (lastSlash) {
        *(lastSlash + 1) = '\0';
        strcat_s(markerPath, MAX_PATH, "dxup_info_log.txt");
      } else {
        strcpy_s(markerPath, MAX_PATH, "dxup_info_log.txt");
      }

      DWORD attributes = GetFileAttributesA(markerPath);
      return attributes != INVALID_FILE_ATTRIBUTES
          && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }();
    return enabled;
  }
#endif

  // ============================================================
  // D3D11 to Backend Type Conversion Functions
  // ============================================================

  inline ResourceUsage convertUsage(uint32_t d3d11Usage) {
    switch (d3d11Usage) {
      case 0: return ResourceUsage::Default;    // D3D11_USAGE_DEFAULT
      case 1: return ResourceUsage::Immutable;  // D3D11_USAGE_IMMUTABLE
      case 2: return ResourceUsage::Dynamic;    // D3D11_USAGE_DYNAMIC
      case 3: return ResourceUsage::Staging;    // D3D11_USAGE_STAGING
      default: return ResourceUsage::Default;
    }
  }

  inline BindFlags convertBindFlags(uint32_t d3d11Flags) {
    BindFlags result = BindFlags::None;
    if (d3d11Flags & 0x01) result = result | BindFlags::VertexBuffer;     // D3D11_BIND_VERTEX_BUFFER
    if (d3d11Flags & 0x02) result = result | BindFlags::IndexBuffer;      // D3D11_BIND_INDEX_BUFFER
    if (d3d11Flags & 0x04) result = result | BindFlags::ConstantBuffer;   // D3D11_BIND_CONSTANT_BUFFER
    if (d3d11Flags & 0x08) result = result | BindFlags::ShaderResource;   // D3D11_BIND_SHADER_RESOURCE
    if (d3d11Flags & 0x10) result = result | BindFlags::StreamOutput;     // D3D11_BIND_STREAM_OUTPUT
    if (d3d11Flags & 0x20) result = result | BindFlags::RenderTarget;     // D3D11_BIND_RENDER_TARGET
    if (d3d11Flags & 0x40) result = result | BindFlags::DepthStencil;     // D3D11_BIND_DEPTH_STENCIL
    if (d3d11Flags & 0x80) result = result | BindFlags::UnorderedAccess;  // D3D11_BIND_UNORDERED_ACCESS
    return result;
  }

  inline CpuAccess convertCpuAccess(uint32_t d3d11CpuFlags) {
    if ((d3d11CpuFlags & 0x20000) && (d3d11CpuFlags & 0x10000))  // READ + WRITE
      return CpuAccess::Read;  // Both
    if (d3d11CpuFlags & 0x20000)  // D3D11_CPU_ACCESS_READ
      return CpuAccess::Read;
    if (d3d11CpuFlags & 0x10000)  // D3D11_CPU_ACCESS_WRITE
      return CpuAccess::Write;
    return CpuAccess::None;
  }

  // Returns bytes per pixel for common DXGI formats (0 if block-compressed)
  inline uint32_t getFormatBpp(DXGI_FORMAT format) {
    switch (format) {
      case 1:   // DXGI_FORMAT_R32G32B32A32_TYPELESS
      case 2:   // DXGI_FORMAT_R32G32B32A32_FLOAT
      case 3:   // DXGI_FORMAT_R32G32B32A32_UINT
      case 4:   return 16;  // DXGI_FORMAT_R32G32B32A32_SINT
      case 5:   // DXGI_FORMAT_R32G32B32_TYPELESS
      case 6:   // DXGI_FORMAT_R32G32B32_FLOAT
      case 7:   // DXGI_FORMAT_R32G32B32_UINT
      case 8:   return 12;  // DXGI_FORMAT_R32G32B32_SINT
      case 9:   // DXGI_FORMAT_R16G16B16A16_TYPELESS
      case 10:  // DXGI_FORMAT_R16G16B16A16_FLOAT
      case 11:  // DXGI_FORMAT_R16G16B16A16_UNORM
      case 12:  // DXGI_FORMAT_R16G16B16A16_UINT
      case 13:  // DXGI_FORMAT_R16G16B16A16_SNORM
      case 14:  // DXGI_FORMAT_R16G16B16A16_SINT
      case 15:  // DXGI_FORMAT_R32G32_TYPELESS
      case 16:  // DXGI_FORMAT_R32G32_FLOAT
      case 17:  // DXGI_FORMAT_R32G32_UINT
      case 18:  return 8;  // DXGI_FORMAT_R32G32_SINT
      case 19:  // DXGI_FORMAT_R32G8X24_TYPELESS
      case 20:  // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
      case 21:  // DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
      case 22:  return 8;  // DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
      case 23:  // DXGI_FORMAT_R11G11B10_FLOAT
      case 24:  // DXGI_FORMAT_R10G10B10A2_TYPELESS
      case 25:  // DXGI_FORMAT_R10G10B10A2_UNORM
      case 26:  // DXGI_FORMAT_R10G10B10A2_UINT
      case 27:  // DXGI_FORMAT_R8G8B8A8_TYPELESS
      case 28:  // DXGI_FORMAT_R8G8B8A8_UNORM
      case 29:  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
      case 30:  // DXGI_FORMAT_R8G8B8A8_UINT
      case 31:  // DXGI_FORMAT_R8G8B8A8_SNORM
      case 32:  // DXGI_FORMAT_R8G8B8A8_SINT
      case 33:  // DXGI_FORMAT_R16G16_TYPELESS
      case 34:  // DXGI_FORMAT_R16G16_FLOAT
      case 35:  // DXGI_FORMAT_R16G16_UNORM
      case 36:  // DXGI_FORMAT_R16G16_UINT
      case 37:  // DXGI_FORMAT_R16G16_SNORM
      case 38:  // DXGI_FORMAT_R16G16_SINT
      case 39:  // DXGI_FORMAT_R32_TYPELESS
      case 40:  // DXGI_FORMAT_D32_FLOAT
      case 41:  // DXGI_FORMAT_R32_FLOAT
      case 42:  // DXGI_FORMAT_R32_UINT
      case 43:  // DXGI_FORMAT_R32_SINT
      case 44:  // DXGI_FORMAT_R24G8_TYPELESS
      case 45:  // DXGI_FORMAT_D24_UNORM_S8_UINT
      case 46:  // DXGI_FORMAT_R24_UNORM_X8_TYPELESS
      case 47:  // DXGI_FORMAT_X24_TYPELESS_G8_UINT
      case 67:  // DXGI_FORMAT_R9G9B9E5_SHAREDEXP
      case 87:  // DXGI_FORMAT_B8G8R8A8_UNORM
      case 88:  // DXGI_FORMAT_B8G8R8X8_UNORM
      case 89:  // DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM
      case 90:  // DXGI_FORMAT_B8G8R8A8_TYPELESS
      case 91:  // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
      case 92:  // DXGI_FORMAT_B8G8R8X8_TYPELESS
      case 93:  return 4;  // DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
      case 48:  // DXGI_FORMAT_R8G8_TYPELESS
      case 49:  // DXGI_FORMAT_R8G8_UNORM
      case 50:  // DXGI_FORMAT_R8G8_UINT
      case 51:  // DXGI_FORMAT_R8G8_SNORM
      case 52:  // DXGI_FORMAT_R8G8_SINT
      case 53:  // DXGI_FORMAT_R16_TYPELESS
      case 54:  // DXGI_FORMAT_R16_FLOAT
      case 55:  // DXGI_FORMAT_D16_UNORM
      case 56:  // DXGI_FORMAT_R16_UNORM
      case 57:  // DXGI_FORMAT_R16_UINT
      case 58:  // DXGI_FORMAT_R16_SNORM
      case 59:  // DXGI_FORMAT_R16_SINT
      case 85:  // DXGI_FORMAT_B5G6R5_UNORM
      case 86:  // DXGI_FORMAT_B5G5R5A1_UNORM
      case 114: // DXGI_FORMAT_A8P8
      case 115: return 2;  // DXGI_FORMAT_B4G4R4A4_UNORM
      case 113: return 1;  // DXGI_FORMAT_P8
      case 60:  // DXGI_FORMAT_R8_TYPELESS
      case 61:  // DXGI_FORMAT_R8_UNORM
      case 62:  // DXGI_FORMAT_R8_UINT
      case 63:  // DXGI_FORMAT_R8_SNORM
      case 64:  // DXGI_FORMAT_R8_SINT
      case 65:  return 1;  // DXGI_FORMAT_A8_UNORM
      default:  return 0;  // Block-compressed or unknown
    }
  }

  // Returns block size for block-compressed formats, 0 if uncompressed
  inline uint32_t getFormatBlockSize(DXGI_FORMAT format) {
    switch (format) {
      case 70: case 71: case 72: case 73: case 74:  // BC1
      case 75: case 76: case 77: case 78: case 79:  // BC2
      case 80: case 81: case 82: case 83: case 84:  // BC3
      case 94: case 95: case 96: case 97: case 98:  // BC4, BC5, BC6H, BC7
      case 99:
        return 4;
      default:
        return 1;
    }
  }

  // Returns bytes per block for block-compressed formats
  inline uint32_t getFormatBlockBytes(DXGI_FORMAT format) {
    switch (format) {
      case 70: case 71: case 72:  // BC1
      case 79: case 80: case 81:  // BC4
        return 8;
      case 73: case 74: case 75: case 76: case 77: case 78:  // BC2, BC3
      case 82: case 83: case 84:  // BC5
      case 94: case 95: case 96: case 97: case 98: case 99:  // BC6H, BC7
        return 16;
      default:
        return 0;
    }
  }

  // Compute row pitch for a given width and format
  inline uint32_t computeRowPitch(uint32_t width, DXGI_FORMAT format) {
    uint32_t bpp = getFormatBpp(format);
    if (bpp > 0) {
      return width * bpp;
    }
    uint32_t blockSize = getFormatBlockSize(format);
    uint32_t blockBytes = getFormatBlockBytes(format);
    if (blockSize > 0 && blockBytes > 0) {
      uint32_t numBlocksWide = (width + blockSize - 1) / blockSize;
      return numBlocksWide * blockBytes;
    }
    return width * 4;  // fallback
  }

  // One row of an upload. Rows the application's pitch makes shorter than the
  // row (God of War gives block-compressed levels whose width is no multiple
  // of 4 a pitch of the whole blocks only) are copied as far as they go, the
  // rest repeats their last block or pixel: D3D11 drivers read on into the
  // next row there.
  inline void copyUploadRow(uint8_t* dst, const uint8_t* src, uint32_t rowBytes, uint32_t srcPitch, DXGI_FORMAT format) {
    if (srcPitch >= rowBytes) {
      std::memcpy(dst, src, rowBytes);
      return;
    }
    const uint32_t unit = (std::max)(computeRowPitch(1, format), 1u);
    const uint32_t given = srcPitch / unit * unit;
    if (!given) {
      std::memset(dst, 0, rowBytes);
      return;
    }
    std::memcpy(dst, src, given);
    for (uint32_t offset = given; offset < rowBytes; offset += unit)
      std::memcpy(dst + offset, dst + given - unit, (std::min)(unit, rowBytes - offset));
  }

  // Compute the number of rows for a given height and format
  inline uint32_t computeNumRows(uint32_t height, DXGI_FORMAT format) {
    uint32_t blockSize = getFormatBlockSize(format);
    if (blockSize > 1) {
      return (height + blockSize - 1) / blockSize;
    }
    return height;
  }

  struct UploadLayout {
    uint32_t copyRowBytes = 0;
    uint32_t rowPitch     = 0;
    uint32_t numRows      = 1;
    uint32_t depth        = 1;
    uint64_t totalSize    = 0;
  };

  inline UploadLayout getImageUploadLayout(
      BackendType     backendType,
      IBackendDevice* backendDevice,
      IBackendImage*  image,
      uint32_t        subresourceIndex,
      uint32_t        fallbackCopyRowBytes,
      uint32_t        fallbackNumRows,
      uint32_t        fallbackDepth,
      bool            preserveCallerExtent = false) {
    constexpr uint32_t PitchAlignment = 256;

    UploadLayout layout;
    layout.copyRowBytes = fallbackCopyRowBytes;
    layout.rowPitch     = (fallbackCopyRowBytes + PitchAlignment - 1) & ~(PitchAlignment - 1);
    layout.numRows      = fallbackNumRows ? fallbackNumRows : 1;
    layout.depth        = fallbackDepth ? fallbackDepth : 1;
    layout.totalSize    = uint64_t(layout.rowPitch) * layout.numRows * layout.depth;

#if defined(DXUP_D3D12_BACKEND) && defined(_WIN32)
    if (backendType != BackendType::D3D12 || !backendDevice || !image)
      return layout;

    auto* d3d12Device = static_cast<d3d12::D3D12BackendDevice*>(backendDevice)->getDevice();
    auto* d3d12Image  = static_cast<d3d12::D3D12BackendImage*>(image);
    if (!d3d12Device || !d3d12Image || !d3d12Image->getResource())
      return layout;

    // A box is copied with the footprint of its own extent (aligned rows of
    // the box), not with the rows of the whole subresource.
    if (preserveCallerExtent)
      return layout;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    D3D12_RESOURCE_DESC resourceDesc = d3d12Image->getTextureDesc();
    d3d12Device->GetCopyableFootprints(
      &resourceDesc,
      subresourceIndex,
      1,
      0,
      &footprint,
      &numRows,
      &rowSizeBytes,
      &totalBytes);

    if (footprint.Footprint.RowPitch)
      layout.rowPitch = footprint.Footprint.RowPitch;

    if (rowSizeBytes && rowSizeBytes <= UINT32_MAX)
      layout.copyRowBytes = static_cast<uint32_t>(rowSizeBytes);
    if (numRows)
      layout.numRows = numRows;
    if (footprint.Footprint.Depth)
      layout.depth = footprint.Footprint.Depth;
    if (totalBytes)
      layout.totalSize = totalBytes;
#endif

    return layout;
  }

#ifdef _WIN32
  inline uint64_t alignConstantBufferSize(uint64_t size) {
    return (size + 255u) & ~255ull;
  }

  inline BufferDesc convertBufferDesc(const D3D11_BUFFER_DESC* d3d11Desc) {
    BufferDesc desc = {};
    desc.size        = d3d11Desc->ByteWidth;
    if (d3d11Desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
      desc.size = alignConstantBufferSize(desc.size);
      desc.dataSize = d3d11Desc->ByteWidth;
    }
    desc.structStride = d3d11Desc->StructureByteStride;
    desc.usage       = convertUsage(d3d11Desc->Usage);
    desc.bindFlags   = convertBindFlags(d3d11Desc->BindFlags);
    desc.cpuAccess   = convertCpuAccess(d3d11Desc->CPUAccessFlags);
    desc.miscFlags   = d3d11Desc->MiscFlags;
    return desc;
  }

  inline uint32_t computeFullMipLevelCount(uint32_t width, uint32_t height = 1u, uint32_t depth = 1u) {
    uint32_t maxExtent = width;
    if (height > maxExtent)
      maxExtent = height;
    if (depth > maxExtent)
      maxExtent = depth;

    uint32_t mipLevels = 1u;
    while (maxExtent > 1u) {
      maxExtent = maxExtent >> 1u;
      mipLevels++;
    }

    return mipLevels;
  }

  inline ImageDesc convertTexture2DDesc(const D3D11_TEXTURE2D_DESC* d3d11Desc) {
    ImageDesc desc = {};
    desc.type          = ImageType::Tex2D;
    desc.width         = d3d11Desc->Width;
    desc.height        = d3d11Desc->Height;
    desc.depth         = 1;
    desc.mipLevels     = d3d11Desc->MipLevels
      ? d3d11Desc->MipLevels
      : computeFullMipLevelCount(d3d11Desc->Width, d3d11Desc->Height, 1u);
    desc.arraySize     = d3d11Desc->ArraySize;
    desc.format        = d3d11Desc->Format;
    desc.sampleCount   = d3d11Desc->SampleDesc.Count;
    desc.sampleQuality = d3d11Desc->SampleDesc.Quality;
    desc.usage         = convertUsage(d3d11Desc->Usage);
    desc.bindFlags     = convertBindFlags(d3d11Desc->BindFlags);
    desc.cpuAccess     = convertCpuAccess(d3d11Desc->CPUAccessFlags);
    desc.miscFlags     = d3d11Desc->MiscFlags;
    return desc;
  }

  inline ImageDesc convertTexture1DDesc(const D3D11_TEXTURE1D_DESC* d3d11Desc) {
    ImageDesc desc = {};
    desc.type          = ImageType::Tex1D;
    desc.width         = d3d11Desc->Width;
    desc.height        = 1;
    desc.depth         = 1;
    desc.mipLevels     = d3d11Desc->MipLevels
      ? d3d11Desc->MipLevels
      : computeFullMipLevelCount(d3d11Desc->Width, 1u, 1u);
    desc.arraySize     = d3d11Desc->ArraySize;
    desc.format        = d3d11Desc->Format;
    desc.sampleCount   = 1;
    desc.usage         = convertUsage(d3d11Desc->Usage);
    desc.bindFlags     = convertBindFlags(d3d11Desc->BindFlags);
    desc.cpuAccess     = convertCpuAccess(d3d11Desc->CPUAccessFlags);
    desc.miscFlags     = d3d11Desc->MiscFlags;
    return desc;
  }

  inline ImageDesc convertTexture3DDesc(const D3D11_TEXTURE3D_DESC* d3d11Desc) {
    ImageDesc desc = {};
    desc.type          = ImageType::Tex3D;
    desc.width         = d3d11Desc->Width;
    desc.height        = d3d11Desc->Height;
    desc.depth         = d3d11Desc->Depth;
    desc.mipLevels     = d3d11Desc->MipLevels
      ? d3d11Desc->MipLevels
      : computeFullMipLevelCount(d3d11Desc->Width, d3d11Desc->Height, d3d11Desc->Depth);
    desc.arraySize     = 1;
    desc.format        = d3d11Desc->Format;
    desc.sampleCount   = 1;
    desc.usage         = convertUsage(d3d11Desc->Usage);
    desc.bindFlags     = convertBindFlags(d3d11Desc->BindFlags);
    desc.cpuAccess     = convertCpuAccess(d3d11Desc->CPUAccessFlags);
    desc.miscFlags     = d3d11Desc->MiscFlags;
    return desc;
  }

  inline MapMode convertMapType(D3D11_MAP d3d11Map) {
    switch (d3d11Map) {
      case D3D11_MAP_READ:               return MapMode::Read;
      case D3D11_MAP_WRITE:              return MapMode::Write;
      case D3D11_MAP_READ_WRITE:         return MapMode::ReadWrite;
      case D3D11_MAP_WRITE_DISCARD:      return MapMode::WriteDiscard;
      case D3D11_MAP_WRITE_NO_OVERWRITE: return MapMode::WriteNoOverwrite;
      default:                           return MapMode::Read;
    }
  }

  inline ComparisonFunc convertComparisonFunc(D3D11_COMPARISON_FUNC d3d11Func) {
    switch (d3d11Func) {
      case D3D11_COMPARISON_NEVER:         return ComparisonFunc::Never;
      case D3D11_COMPARISON_LESS:          return ComparisonFunc::Less;
      case D3D11_COMPARISON_EQUAL:         return ComparisonFunc::Equal;
      case D3D11_COMPARISON_LESS_EQUAL:    return ComparisonFunc::LessEqual;
      case D3D11_COMPARISON_GREATER:       return ComparisonFunc::Greater;
      case D3D11_COMPARISON_NOT_EQUAL:     return ComparisonFunc::NotEqual;
      case D3D11_COMPARISON_GREATER_EQUAL: return ComparisonFunc::GreaterEqual;
      case D3D11_COMPARISON_ALWAYS:        return ComparisonFunc::Always;
      default:                             return ComparisonFunc::Never;
    }
  }

  inline BlendState convertBlendDesc(const D3D11_BLEND_DESC* d3d11Desc) {
    BlendState state = {};
    state.alphaToCoverageEnable  = d3d11Desc->AlphaToCoverageEnable;
    state.independentBlendEnable = d3d11Desc->IndependentBlendEnable;

    for (int i = 0; i < 8; i++) {
      auto& src = d3d11Desc->RenderTarget[i];
      auto& dst = state.renderTargets[i];
      dst.blendEnable    = src.BlendEnable;
      dst.srcBlend       = static_cast<BlendFactor>(src.SrcBlend - 1);
      dst.dstBlend       = static_cast<BlendFactor>(src.DestBlend - 1);
      dst.blendOp        = static_cast<BlendOp>(src.BlendOp - 1);
      dst.srcBlendAlpha  = static_cast<BlendFactor>(src.SrcBlendAlpha - 1);
      dst.dstBlendAlpha  = static_cast<BlendFactor>(src.DestBlendAlpha - 1);
      dst.blendOpAlpha   = static_cast<BlendOp>(src.BlendOpAlpha - 1);
      dst.writeMask      = src.RenderTargetWriteMask;
    }

    return state;
  }

  inline D3D11_BLEND_DESC downgradeBlendDesc1(const D3D11_BLEND_DESC1* d3d11Desc) {
    D3D11_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable  = d3d11Desc->AlphaToCoverageEnable;
    desc.IndependentBlendEnable = d3d11Desc->IndependentBlendEnable;

    for (int i = 0; i < 8; i++) {
      desc.RenderTarget[i].BlendEnable           = d3d11Desc->RenderTarget[i].BlendEnable;
      desc.RenderTarget[i].SrcBlend              = d3d11Desc->RenderTarget[i].SrcBlend;
      desc.RenderTarget[i].DestBlend             = d3d11Desc->RenderTarget[i].DestBlend;
      desc.RenderTarget[i].BlendOp               = d3d11Desc->RenderTarget[i].BlendOp;
      desc.RenderTarget[i].SrcBlendAlpha         = d3d11Desc->RenderTarget[i].SrcBlendAlpha;
      desc.RenderTarget[i].DestBlendAlpha        = d3d11Desc->RenderTarget[i].DestBlendAlpha;
      desc.RenderTarget[i].BlendOpAlpha          = d3d11Desc->RenderTarget[i].BlendOpAlpha;
      desc.RenderTarget[i].RenderTargetWriteMask = d3d11Desc->RenderTarget[i].RenderTargetWriteMask;
    }

    return desc;
  }

  inline D3D11_BLEND_DESC1 upgradeBlendDesc1(const D3D11_BLEND_DESC* d3d11Desc) {
    D3D11_BLEND_DESC1 desc = {};
    desc.AlphaToCoverageEnable  = d3d11Desc->AlphaToCoverageEnable;
    desc.IndependentBlendEnable = d3d11Desc->IndependentBlendEnable;

    for (int i = 0; i < 8; i++) {
      desc.RenderTarget[i].BlendEnable           = d3d11Desc->RenderTarget[i].BlendEnable;
      desc.RenderTarget[i].LogicOpEnable         = FALSE;
      desc.RenderTarget[i].SrcBlend              = d3d11Desc->RenderTarget[i].SrcBlend;
      desc.RenderTarget[i].DestBlend             = d3d11Desc->RenderTarget[i].DestBlend;
      desc.RenderTarget[i].BlendOp               = d3d11Desc->RenderTarget[i].BlendOp;
      desc.RenderTarget[i].SrcBlendAlpha         = d3d11Desc->RenderTarget[i].SrcBlendAlpha;
      desc.RenderTarget[i].DestBlendAlpha        = d3d11Desc->RenderTarget[i].DestBlendAlpha;
      desc.RenderTarget[i].BlendOpAlpha          = d3d11Desc->RenderTarget[i].BlendOpAlpha;
      desc.RenderTarget[i].LogicOp               = D3D11_LOGIC_OP_NOOP;
      desc.RenderTarget[i].RenderTargetWriteMask = d3d11Desc->RenderTarget[i].RenderTargetWriteMask;
    }

    return desc;
  }

  inline RasterizerState convertRasterizerDesc(const D3D11_RASTERIZER_DESC* d3d11Desc) {
    RasterizerState state = {};
    state.fillMode             = (d3d11Desc->FillMode == D3D11_FILL_WIREFRAME)
      ? FillMode::Wireframe : FillMode::Solid;
    state.cullMode             = static_cast<CullMode>(d3d11Desc->CullMode - 1);
    state.frontCounterClockwise = d3d11Desc->FrontCounterClockwise;
    state.depthBias            = d3d11Desc->DepthBias;
    state.depthBiasClamp       = d3d11Desc->DepthBiasClamp;
    state.slopeScaledDepthBias = d3d11Desc->SlopeScaledDepthBias;
    state.depthClipEnable      = d3d11Desc->DepthClipEnable;
    state.scissorEnable        = d3d11Desc->ScissorEnable;
    state.multisampleEnable    = d3d11Desc->MultisampleEnable;
    state.antialiasedLineEnable = d3d11Desc->AntialiasedLineEnable;
    return state;
  }

  inline D3D11_RASTERIZER_DESC downgradeRasterizerDesc1(const D3D11_RASTERIZER_DESC1* d3d11Desc) {
    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode              = d3d11Desc->FillMode;
    desc.CullMode              = d3d11Desc->CullMode;
    desc.FrontCounterClockwise = d3d11Desc->FrontCounterClockwise;
    desc.DepthBias             = d3d11Desc->DepthBias;
    desc.DepthBiasClamp        = d3d11Desc->DepthBiasClamp;
    desc.SlopeScaledDepthBias  = d3d11Desc->SlopeScaledDepthBias;
    desc.DepthClipEnable       = d3d11Desc->DepthClipEnable;
    desc.ScissorEnable         = d3d11Desc->ScissorEnable;
    desc.MultisampleEnable     = d3d11Desc->MultisampleEnable;
    desc.AntialiasedLineEnable = d3d11Desc->AntialiasedLineEnable;
    return desc;
  }

  inline D3D11_RASTERIZER_DESC1 upgradeRasterizerDesc1(const D3D11_RASTERIZER_DESC* d3d11Desc) {
    D3D11_RASTERIZER_DESC1 desc = {};
    desc.FillMode              = d3d11Desc->FillMode;
    desc.CullMode              = d3d11Desc->CullMode;
    desc.FrontCounterClockwise = d3d11Desc->FrontCounterClockwise;
    desc.DepthBias             = d3d11Desc->DepthBias;
    desc.DepthBiasClamp        = d3d11Desc->DepthBiasClamp;
    desc.SlopeScaledDepthBias  = d3d11Desc->SlopeScaledDepthBias;
    desc.DepthClipEnable       = d3d11Desc->DepthClipEnable;
    desc.ScissorEnable         = d3d11Desc->ScissorEnable;
    desc.MultisampleEnable     = d3d11Desc->MultisampleEnable;
    desc.AntialiasedLineEnable = d3d11Desc->AntialiasedLineEnable;
    desc.ForcedSampleCount     = 0;
    return desc;
  }

  inline DepthStencilState convertDepthStencilDesc(const D3D11_DEPTH_STENCIL_DESC* d3d11Desc) {
    DepthStencilState state = {};
    state.depthEnable      = d3d11Desc->DepthEnable;
    state.depthWriteEnable = (d3d11Desc->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL);
    state.depthFunc        = convertComparisonFunc(d3d11Desc->DepthFunc);
    state.stencilEnable    = d3d11Desc->StencilEnable;
    state.stencilReadMask  = d3d11Desc->StencilReadMask;
    state.stencilWriteMask = d3d11Desc->StencilWriteMask;

    auto convertStencilOp = [](const D3D11_DEPTH_STENCILOP_DESC& s) -> StencilOpDesc {
      StencilOpDesc r = {};
      r.stencilFailOp      = static_cast<StencilOp>(s.StencilFailOp - 1);
      r.stencilDepthFailOp = static_cast<StencilOp>(s.StencilDepthFailOp - 1);
      r.stencilPassOp      = static_cast<StencilOp>(s.StencilPassOp - 1);
      r.stencilFunc        = convertComparisonFunc(s.StencilFunc);
      return r;
    };

    state.frontFace = convertStencilOp(d3d11Desc->FrontFace);
    state.backFace  = convertStencilOp(d3d11Desc->BackFace);
    return state;
  }

  // Anisotropic filtering forced by dxup.conf. Samplers that filter linearly
  // when minifying and magnifying become anisotropic with that many samples,
  // or trilinear with 0 or 1; point sampling and the reduction (comparison,
  // minimum, maximum) stay as the application asked. The filter holds D3D11
  // filter bits.
  inline void overrideSamplerAnisotropy(SamplerDesc& desc, int32_t anisotropy) {
    if (anisotropy < 0)
      return;
    constexpr uint32_t kReductionBits = 0x180u;
    constexpr uint32_t kMinMagLinear = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    const uint32_t filter = static_cast<uint32_t>(desc.filter);
    const uint32_t reduction = filter & kReductionBits;
    const uint32_t base = filter & ~kReductionBits;
    if ((base & kMinMagLinear) != kMinMagLinear)
      return;
    if (anisotropy > 1) {
      desc.filter = static_cast<FilterMode>(reduction | D3D11_FILTER_ANISOTROPIC);
      desc.maxAnisotropy = static_cast<uint32_t>(anisotropy);
    } else {
      if (base == D3D11_FILTER_ANISOTROPIC)
        desc.filter = static_cast<FilterMode>(reduction | D3D11_FILTER_MIN_MAG_MIP_LINEAR);
      desc.maxAnisotropy = 1;
    }
  }

  // A LOD bias dxup.conf adds, within the range D3D11 allows.
  inline void overrideSamplerLodBias(SamplerDesc& desc, float bias) {
    if (bias == 0.0f)
      return;
    desc.mipLodBias = (std::min)((std::max)(desc.mipLodBias + bias, D3D11_MIP_LOD_BIAS_MIN), D3D11_MIP_LOD_BIAS_MAX);
  }

  inline SamplerDesc convertSamplerDesc(const D3D11_SAMPLER_DESC* d3d11Desc) {
    SamplerDesc desc = {};
    desc.filter         = static_cast<FilterMode>(d3d11Desc->Filter);
    desc.addressU       = static_cast<TextureAddressMode>(d3d11Desc->AddressU - 1);
    desc.addressV       = static_cast<TextureAddressMode>(d3d11Desc->AddressV - 1);
    desc.addressW       = static_cast<TextureAddressMode>(d3d11Desc->AddressW - 1);
    desc.mipLodBias     = d3d11Desc->MipLODBias;
    // D3D11 takes an anisotropy of 0 and no comparison where the filter
    // does not compare; D3D12 needs values in range.
    desc.maxAnisotropy  = (std::max)(d3d11Desc->MaxAnisotropy, 1u);
    desc.comparisonFunc = convertComparisonFunc(d3d11Desc->ComparisonFunc
      ? d3d11Desc->ComparisonFunc : D3D11_COMPARISON_NEVER);
    std::memcpy(desc.borderColor, d3d11Desc->BorderColor, sizeof(float) * 4);
    desc.minLod         = d3d11Desc->MinLOD;
    desc.maxLod         = d3d11Desc->MaxLOD;
    return desc;
  }
#endif

}
