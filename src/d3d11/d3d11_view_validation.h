#pragma once

// View descriptions as D3D11 resolves and checks them: omitted descriptions
// become the view of the whole resource, and descriptions the native runtime
// rejects fail with E_INVALIDARG before they reach D3D12, where they would
// remove the device.

#include "d3d11_d3d12_bridge.h"

#include <d3d11_4.h>

#include <functional>

namespace dxup {

  // Private data that marks swap chain buffers: DXGI creates them typeless,
  // so their views may take any format of the buffer format's family.
  // {4B6C9A1E-2D7F-4E83-A5C1-9F0B3D2E6A74}
  static constexpr GUID kDxupSwapChainBufferGuid =
    { 0x4b6c9a1e, 0x2d7f, 0x4e83, { 0xa5, 0xc1, 0x9f, 0x0b, 0x3d, 0x2e, 0x6a, 0x74 } };

  struct ViewResourceInfo {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    DXGI_FORMAT format     = DXGI_FORMAT_UNKNOWN;
    UINT        width      = 1;
    UINT        height     = 1;
    UINT        depth      = 1;
    UINT        mipLevels  = 1;
    UINT        arraySize  = 1;
    UINT        samples    = 1;
    UINT        bindFlags  = 0;
    UINT        miscFlags  = 0;
    UINT        byteWidth  = 0;
    UINT        stride     = 0;
    bool        swapChainBuffer = false;
  };

  inline bool getViewResourceInfo(ID3D11Resource* resource, ViewResourceInfo& info) {
    resource->GetType(&info.dimension);
    switch (info.dimension) {
      case D3D11_RESOURCE_DIMENSION_BUFFER: {
        D3D11_BUFFER_DESC desc = {};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        info.bindFlags = desc.BindFlags;
        info.miscFlags = desc.MiscFlags;
        info.byteWidth = desc.ByteWidth;
        info.stride    = desc.StructureByteStride;
        return true;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC desc = {};
        static_cast<ID3D11Texture1D*>(resource)->GetDesc(&desc);
        info.format = desc.Format;
        info.width = desc.Width;
        info.mipLevels = desc.MipLevels;
        info.arraySize = desc.ArraySize;
        info.bindFlags = desc.BindFlags;
        info.miscFlags = desc.MiscFlags;
        return true;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC desc = {};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        info.format = desc.Format;
        info.width = desc.Width;
        info.height = desc.Height;
        info.mipLevels = desc.MipLevels;
        info.arraySize = desc.ArraySize;
        info.samples = desc.SampleDesc.Count;
        info.bindFlags = desc.BindFlags;
        info.miscFlags = desc.MiscFlags;
        UINT marker = 0, markerSize = sizeof(marker);
        info.swapChainBuffer = SUCCEEDED(resource->GetPrivateData(kDxupSwapChainBufferGuid, &markerSize, &marker)) && marker;
        return true;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC desc = {};
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
        info.format = desc.Format;
        info.width = desc.Width;
        info.height = desc.Height;
        info.depth = desc.Depth;
        info.mipLevels = desc.MipLevels;
        info.bindFlags = desc.BindFlags;
        info.miscFlags = desc.MiscFlags;
        return true;
      }
      default:
        return false;
    }
  }

  // The typeless format a format can be cast from; itself for formats
  // without a family.
  inline DXGI_FORMAT getFormatFamily(DXGI_FORMAT format) {
    const UINT f = UINT(format);
    struct Range { UINT first, last; DXGI_FORMAT family; };
    static const Range kRanges[] = {
      {  1,  4, DXGI_FORMAT_R32G32B32A32_TYPELESS },
      {  5,  8, DXGI_FORMAT_R32G32B32_TYPELESS },
      {  9, 14, DXGI_FORMAT_R16G16B16A16_TYPELESS },
      { 15, 18, DXGI_FORMAT_R32G32_TYPELESS },
      { 19, 22, DXGI_FORMAT_R32G8X24_TYPELESS },
      { 23, 25, DXGI_FORMAT_R10G10B10A2_TYPELESS },
      { 27, 32, DXGI_FORMAT_R8G8B8A8_TYPELESS },
      { 33, 38, DXGI_FORMAT_R16G16_TYPELESS },
      { 39, 43, DXGI_FORMAT_R32_TYPELESS },
      { 44, 47, DXGI_FORMAT_R24G8_TYPELESS },
      { 48, 52, DXGI_FORMAT_R8G8_TYPELESS },
      { 53, 59, DXGI_FORMAT_R16_TYPELESS },
      { 60, 64, DXGI_FORMAT_R8_TYPELESS },
      { 70, 72, DXGI_FORMAT_BC1_TYPELESS },
      { 73, 75, DXGI_FORMAT_BC2_TYPELESS },
      { 76, 78, DXGI_FORMAT_BC3_TYPELESS },
      { 79, 81, DXGI_FORMAT_BC4_TYPELESS },
      { 82, 84, DXGI_FORMAT_BC5_TYPELESS },
      { 94, 96, DXGI_FORMAT_BC6H_TYPELESS },
      { 97, 99, DXGI_FORMAT_BC7_TYPELESS },
    };
    for (const auto& range : kRanges) {
      if (f >= range.first && f <= range.last)
        return range.family;
    }
    switch (format) {
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
      case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
      default:
        return format;
    }
  }

  // Formats that only name a family and are no view format.
  inline bool isFamilyOnlyFormat(DXGI_FORMAT format) {
    return format != DXGI_FORMAT_UNKNOWN && getFormatFamily(format) == format
      && format != DXGI_FORMAT_R11G11B10_FLOAT && UINT(format) >= 1
      && (UINT(format) <= 99 || format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8X8_TYPELESS)
      && [&] {
        switch (format) {
          case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS:
          case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_TYPELESS:
          case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
          case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R16G16_TYPELESS:
          case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS:
          case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R16_TYPELESS:
          case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_BC1_TYPELESS:
          case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC3_TYPELESS:
          case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC5_TYPELESS:
          case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC7_TYPELESS:
          case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return true;
          default:
            return false;
        }
      }();
  }

  // The format a texture view has: its own, or the resource's for UNKNOWN.
  // Typed resources are viewed in their format, typeless ones in a format
  // of their family.
  inline HRESULT resolveViewFormat(const ViewResourceInfo& info, DXGI_FORMAT& format) {
    if (format == DXGI_FORMAT_UNKNOWN)
      format = info.format;
    if (isFamilyOnlyFormat(format))
      return E_INVALIDARG;
    if (format == info.format)
      return S_OK;
    if (info.swapChainBuffer)
      return getFormatFamily(format) == getFormatFamily(info.format) ? S_OK : E_INVALIDARG;
    return isFamilyOnlyFormat(info.format) && getFormatFamily(format) == info.format ? S_OK : E_INVALIDARG;
  }

  inline bool checkMipRange(UINT total, UINT first, UINT& count) {
    if (first >= total)
      return false;
    if (count == UINT(-1))
      count = total - first;
    return count && count <= total - first;
  }

  inline bool checkArrayRange(UINT total, UINT first, UINT count) {
    return count && first < total && count <= total - first;
  }

  using FormatSupportQuery = std::function<UINT(DXGI_FORMAT)>;

  inline UINT getBufferElementSize(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_UNKNOWN ? 0u : computeRowPitch(1, format);
  }

  inline bool checkBufferRange(const ViewResourceInfo& info, UINT elementSize, UINT first, UINT count) {
    if (!elementSize || !count)
      return false;
    const UINT64 elements = info.byteWidth / elementSize;
    return UINT64(first) + count <= elements;
  }

  // ---- Shader resource views ----

  inline HRESULT resolveSrvDesc(ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                const FormatSupportQuery& formatSupport, D3D11_SHADER_RESOURCE_VIEW_DESC& out) {
    ViewResourceInfo info;
    if (!getViewResourceInfo(resource, info) || !(info.bindFlags & D3D11_BIND_SHADER_RESOURCE))
      return E_INVALIDARG;
    const bool structured = (info.miscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;

    if (info.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      if (!pDesc) {
        // Only structured buffers have a view without a description.
        if (!structured || !info.stride)
          return E_INVALIDARG;
        out = {};
        out.Format = DXGI_FORMAT_UNKNOWN;
        out.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        out.Buffer.FirstElement = 0;
        out.Buffer.NumElements = info.byteWidth / info.stride;
        return S_OK;
      }
      out = *pDesc;
      UINT first = 0, count = 0;
      bool raw = false;
      if (out.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
        first = out.Buffer.FirstElement;
        count = out.Buffer.NumElements;
      } else if (out.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
        first = out.BufferEx.FirstElement;
        count = out.BufferEx.NumElements;
        if (out.BufferEx.Flags & ~UINT(D3D11_BUFFEREX_SRV_FLAG_RAW))
          return E_INVALIDARG;
        raw = (out.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0;
      } else {
        return E_INVALIDARG;
      }
      UINT elementSize = 0;
      if (raw) {
        if (out.Format != DXGI_FORMAT_R32_TYPELESS || !(info.miscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS))
          return E_INVALIDARG;
        elementSize = 4;
      } else if (structured) {
        if (out.Format != DXGI_FORMAT_UNKNOWN)
          return E_INVALIDARG;
        elementSize = info.stride;
      } else {
        if (out.Format == DXGI_FORMAT_UNKNOWN || isFamilyOnlyFormat(out.Format)
         || !(formatSupport(out.Format) & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
          return E_INVALIDARG;
        elementSize = getBufferElementSize(out.Format);
      }
      return checkBufferRange(info, elementSize, first, count) ? S_OK : E_INVALIDARG;
    }

    const bool cube = (info.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
    if (!pDesc) {
      out = {};
      out.Format = info.format;
      switch (info.dimension) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
          if (info.arraySize > 1) {
            out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
            out.Texture1DArray = { 0, info.mipLevels, 0, info.arraySize };
          } else {
            out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            out.Texture1D = { 0, info.mipLevels };
          }
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
          if (info.samples > 1) {
            if (info.arraySize > 1) {
              out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
              out.Texture2DMSArray = { 0, info.arraySize };
            } else {
              out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            }
          } else if (cube && info.arraySize > 6) {
            out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            out.TextureCubeArray = { 0, info.mipLevels, 0, info.arraySize / 6 };
          } else if (cube) {
            out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            out.TextureCube = { 0, info.mipLevels };
          } else if (info.arraySize > 1) {
            out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            out.Texture2DArray = { 0, info.mipLevels, 0, info.arraySize };
          } else {
            out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            out.Texture2D = { 0, info.mipLevels };
          }
          break;
        default:
          out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
          out.Texture3D = { 0, info.mipLevels };
          break;
      }
    } else {
      out = *pDesc;
    }

    if (FAILED(resolveViewFormat(info, out.Format))
     || !(formatSupport(out.Format) & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
      return E_INVALIDARG;

    switch (info.dimension) {
      case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        if (out.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE1D)
          return checkMipRange(info.mipLevels, out.Texture1D.MostDetailedMip, out.Texture1D.MipLevels) ? S_OK : E_INVALIDARG;
        if (out.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE1DARRAY)
          return checkMipRange(info.mipLevels, out.Texture1DArray.MostDetailedMip, out.Texture1DArray.MipLevels)
              && checkArrayRange(info.arraySize, out.Texture1DArray.FirstArraySlice, out.Texture1DArray.ArraySize)
            ? S_OK : E_INVALIDARG;
        return E_INVALIDARG;
      case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        switch (out.ViewDimension) {
          case D3D11_SRV_DIMENSION_TEXTURE2D:
            return info.samples == 1 && checkMipRange(info.mipLevels, out.Texture2D.MostDetailedMip, out.Texture2D.MipLevels)
              ? S_OK : E_INVALIDARG;
          case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            return info.samples == 1
                && checkMipRange(info.mipLevels, out.Texture2DArray.MostDetailedMip, out.Texture2DArray.MipLevels)
                && checkArrayRange(info.arraySize, out.Texture2DArray.FirstArraySlice, out.Texture2DArray.ArraySize)
              ? S_OK : E_INVALIDARG;
          case D3D11_SRV_DIMENSION_TEXTURE2DMS:
            return S_OK;
          case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
            return checkArrayRange(info.arraySize, out.Texture2DMSArray.FirstArraySlice, out.Texture2DMSArray.ArraySize)
              ? S_OK : E_INVALIDARG;
          case D3D11_SRV_DIMENSION_TEXTURECUBE:
            return cube && info.samples == 1
                && checkMipRange(info.mipLevels, out.TextureCube.MostDetailedMip, out.TextureCube.MipLevels)
              ? S_OK : E_INVALIDARG;
          case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
            return cube && info.samples == 1
                && checkMipRange(info.mipLevels, out.TextureCubeArray.MostDetailedMip, out.TextureCubeArray.MipLevels)
                && out.TextureCubeArray.NumCubes && out.TextureCubeArray.First2DArrayFace < info.arraySize
                && UINT64(out.TextureCubeArray.NumCubes) * 6 <= info.arraySize - out.TextureCubeArray.First2DArrayFace
              ? S_OK : E_INVALIDARG;
          default:
            return E_INVALIDARG;
        }
      default:
        return out.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D
            && checkMipRange(info.mipLevels, out.Texture3D.MostDetailedMip, out.Texture3D.MipLevels)
          ? S_OK : E_INVALIDARG;
    }
  }

  // Multisample views of single-sample textures read their only sample:
  // D3D12 takes them as plain views of the first mip.
  inline D3D11_SHADER_RESOURCE_VIEW_DESC getBackendSrvDesc(ID3D11Resource* resource,
                                                           const D3D11_SHADER_RESOURCE_VIEW_DESC& desc) {
    ViewResourceInfo info;
    if (!getViewResourceInfo(resource, info) || info.samples > 1)
      return desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC result = desc;
    if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS) {
      result.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      result.Texture2D = { 0, 1 };
    } else if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY) {
      result.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      result.Texture2DArray = { 0, 1, desc.Texture2DMSArray.FirstArraySlice, desc.Texture2DMSArray.ArraySize };
    }
    return result;
  }

  // ---- Render target views ----

  inline HRESULT resolveRtvDesc(ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
                                const FormatSupportQuery& formatSupport, D3D11_RENDER_TARGET_VIEW_DESC& out) {
    ViewResourceInfo info;
    if (!getViewResourceInfo(resource, info) || !(info.bindFlags & D3D11_BIND_RENDER_TARGET))
      return E_INVALIDARG;

    if (info.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      if (!pDesc || pDesc->ViewDimension != D3D11_RTV_DIMENSION_BUFFER
       || pDesc->Format == DXGI_FORMAT_UNKNOWN || isFamilyOnlyFormat(pDesc->Format)
       || !(formatSupport(pDesc->Format) & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
        return E_INVALIDARG;
      out = *pDesc;
      return checkBufferRange(info, getBufferElementSize(out.Format), out.Buffer.FirstElement, out.Buffer.NumElements)
        ? S_OK : E_INVALIDARG;
    }

    if (!pDesc) {
      out = {};
      out.Format = info.format;
      switch (info.dimension) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
          if (info.arraySize > 1) {
            out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
            out.Texture1DArray = { 0, 0, info.arraySize };
          } else {
            out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
          }
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
          if (info.samples > 1) {
            if (info.arraySize > 1) {
              out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
              out.Texture2DMSArray = { 0, info.arraySize };
            } else {
              out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            }
          } else if (info.arraySize > 1) {
            out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            out.Texture2DArray = { 0, 0, info.arraySize };
          } else {
            out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
          }
          break;
        default:
          out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
          out.Texture3D = { 0, 0, info.depth };
          break;
      }
    } else {
      out = *pDesc;
    }

    if (FAILED(resolveViewFormat(info, out.Format))
     || !(formatSupport(out.Format) & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
      return E_INVALIDARG;

    switch (out.ViewDimension) {
      case D3D11_RTV_DIMENSION_TEXTURE1D:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D && out.Texture1D.MipSlice < info.mipLevels
          ? S_OK : E_INVALIDARG;
      case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D && out.Texture1DArray.MipSlice < info.mipLevels
            && checkArrayRange(info.arraySize, out.Texture1DArray.FirstArraySlice, out.Texture1DArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      case D3D11_RTV_DIMENSION_TEXTURE2D:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && info.samples == 1
            && out.Texture2D.MipSlice < info.mipLevels
          ? S_OK : E_INVALIDARG;
      case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && info.samples == 1
            && out.Texture2DArray.MipSlice < info.mipLevels
            && checkArrayRange(info.arraySize, out.Texture2DArray.FirstArraySlice, out.Texture2DArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      case D3D11_RTV_DIMENSION_TEXTURE2DMS:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && info.samples > 1 ? S_OK : E_INVALIDARG;
      case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && info.samples > 1
            && checkArrayRange(info.arraySize, out.Texture2DMSArray.FirstArraySlice, out.Texture2DMSArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      case D3D11_RTV_DIMENSION_TEXTURE3D: {
        if (info.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D || out.Texture3D.MipSlice >= info.mipLevels)
          return E_INVALIDARG;
        const UINT depth = (std::max)(info.depth >> out.Texture3D.MipSlice, 1u);
        if (out.Texture3D.WSize == UINT(-1) && out.Texture3D.FirstWSlice < depth)
          out.Texture3D.WSize = depth - out.Texture3D.FirstWSlice;
        return checkArrayRange(depth, out.Texture3D.FirstWSlice, out.Texture3D.WSize) ? S_OK : E_INVALIDARG;
      }
      default:
        return E_INVALIDARG;
    }
  }

  // ---- Depth-stencil views ----

  inline HRESULT resolveDsvDesc(ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                const FormatSupportQuery& formatSupport, D3D11_DEPTH_STENCIL_VIEW_DESC& out) {
    ViewResourceInfo info;
    if (!getViewResourceInfo(resource, info) || !(info.bindFlags & D3D11_BIND_DEPTH_STENCIL)
     || info.dimension == D3D11_RESOURCE_DIMENSION_BUFFER || info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
      return E_INVALIDARG;

    if (!pDesc) {
      out = {};
      out.Format = info.format;
      if (info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        if (info.arraySize > 1) {
          out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
          out.Texture1DArray = { 0, 0, info.arraySize };
        } else {
          out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
        }
      } else if (info.samples > 1) {
        if (info.arraySize > 1) {
          out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
          out.Texture2DMSArray = { 0, info.arraySize };
        } else {
          out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        }
      } else if (info.arraySize > 1) {
        out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        out.Texture2DArray = { 0, 0, info.arraySize };
      } else {
        out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
      }
    } else {
      out = *pDesc;
    }

    if (out.Flags & ~UINT(D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL))
      return E_INVALIDARG;
    if (FAILED(resolveViewFormat(info, out.Format))
     || !(formatSupport(out.Format) & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL))
      return E_INVALIDARG;

    const bool is1D = info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    switch (out.ViewDimension) {
      case D3D11_DSV_DIMENSION_TEXTURE1D:
        return is1D && out.Texture1D.MipSlice < info.mipLevels ? S_OK : E_INVALIDARG;
      case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
        return is1D && out.Texture1DArray.MipSlice < info.mipLevels
            && checkArrayRange(info.arraySize, out.Texture1DArray.FirstArraySlice, out.Texture1DArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      case D3D11_DSV_DIMENSION_TEXTURE2D:
        return !is1D && info.samples == 1 && out.Texture2D.MipSlice < info.mipLevels ? S_OK : E_INVALIDARG;
      case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        return !is1D && info.samples == 1 && out.Texture2DArray.MipSlice < info.mipLevels
            && checkArrayRange(info.arraySize, out.Texture2DArray.FirstArraySlice, out.Texture2DArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        return !is1D && info.samples > 1 ? S_OK : E_INVALIDARG;
      case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        return !is1D && info.samples > 1
            && checkArrayRange(info.arraySize, out.Texture2DMSArray.FirstArraySlice, out.Texture2DMSArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      default:
        return E_INVALIDARG;
    }
  }

  // ---- Unordered access views ----

  inline HRESULT resolveUavDesc(ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                const FormatSupportQuery& formatSupport, D3D11_UNORDERED_ACCESS_VIEW_DESC& out) {
    ViewResourceInfo info;
    if (!getViewResourceInfo(resource, info) || !(info.bindFlags & D3D11_BIND_UNORDERED_ACCESS))
      return E_INVALIDARG;
    const bool structured = (info.miscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;

    if (info.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      if (!pDesc) {
        if (!structured || !info.stride)
          return E_INVALIDARG;
        out = {};
        out.Format = DXGI_FORMAT_UNKNOWN;
        out.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        out.Buffer.NumElements = info.byteWidth / info.stride;
        return S_OK;
      }
      out = *pDesc;
      if (out.ViewDimension != D3D11_UAV_DIMENSION_BUFFER)
        return E_INVALIDARG;
      const UINT flags = out.Buffer.Flags;
      const UINT counterFlags = D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER;
      if ((flags & ~UINT(D3D11_BUFFER_UAV_FLAG_RAW | counterFlags))
       || (flags & counterFlags) == counterFlags)
        return E_INVALIDARG;
      UINT elementSize = 0;
      if (flags & D3D11_BUFFER_UAV_FLAG_RAW) {
        if ((flags & counterFlags) || out.Format != DXGI_FORMAT_R32_TYPELESS
         || !(info.miscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS))
          return E_INVALIDARG;
        elementSize = 4;
      } else if (structured) {
        if (out.Format != DXGI_FORMAT_UNKNOWN)
          return E_INVALIDARG;
        elementSize = info.stride;
      } else {
        if ((flags & counterFlags) || out.Format == DXGI_FORMAT_UNKNOWN || isFamilyOnlyFormat(out.Format)
         || !(formatSupport(out.Format) & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
          return E_INVALIDARG;
        elementSize = getBufferElementSize(out.Format);
      }
      return checkBufferRange(info, elementSize, out.Buffer.FirstElement, out.Buffer.NumElements)
        ? S_OK : E_INVALIDARG;
    }

    if (info.samples > 1)
      return E_INVALIDARG;
    if (!pDesc) {
      out = {};
      out.Format = info.format;
      switch (info.dimension) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
          if (info.arraySize > 1) {
            out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
            out.Texture1DArray = { 0, 0, info.arraySize };
          } else {
            out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
          }
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
          if (info.arraySize > 1) {
            out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            out.Texture2DArray = { 0, 0, info.arraySize };
          } else {
            out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
          }
          break;
        default:
          out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
          out.Texture3D = { 0, 0, info.depth };
          break;
      }
    } else {
      out = *pDesc;
    }

    if (FAILED(resolveViewFormat(info, out.Format))
     || !(formatSupport(out.Format) & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
      return E_INVALIDARG;

    switch (out.ViewDimension) {
      case D3D11_UAV_DIMENSION_TEXTURE1D:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D && out.Texture1D.MipSlice < info.mipLevels
          ? S_OK : E_INVALIDARG;
      case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D && out.Texture1DArray.MipSlice < info.mipLevels
            && checkArrayRange(info.arraySize, out.Texture1DArray.FirstArraySlice, out.Texture1DArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      case D3D11_UAV_DIMENSION_TEXTURE2D:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && out.Texture2D.MipSlice < info.mipLevels
          ? S_OK : E_INVALIDARG;
      case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
        return info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && out.Texture2DArray.MipSlice < info.mipLevels
            && checkArrayRange(info.arraySize, out.Texture2DArray.FirstArraySlice, out.Texture2DArray.ArraySize)
          ? S_OK : E_INVALIDARG;
      case D3D11_UAV_DIMENSION_TEXTURE3D: {
        if (info.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D || out.Texture3D.MipSlice >= info.mipLevels)
          return E_INVALIDARG;
        const UINT depth = (std::max)(info.depth >> out.Texture3D.MipSlice, 1u);
        if (out.Texture3D.WSize == UINT(-1) && out.Texture3D.FirstWSlice < depth)
          out.Texture3D.WSize = depth - out.Texture3D.FirstWSlice;
        return checkArrayRange(depth, out.Texture3D.FirstWSlice, out.Texture3D.WSize) ? S_OK : E_INVALIDARG;
      }
      default:
        return E_INVALIDARG;
    }
  }

}
