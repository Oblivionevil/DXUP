#pragma once

// Read/write hazards between bindings, as the D3D11 runtime resolves them
// (measured on the native runtime):
// - A view or buffer bound for reading while a part of its resource is
//   bound for writing (render target, writable depth or stencil, unordered
//   access, stream output) is bound as NULL instead.
// - Binding something for writing unbinds the reads and the other writes of
//   the parts of the resource it covers, in every stage.
// Textures are compared by mip level, array layer and plane (depth or
// stencil), buffers as a whole. A read-only depth or stencil plane is no
// write.

#include "d3d11_device.h"

namespace dxup {

  struct D3D11ViewRange {
    ID3D11Resource* resource = nullptr;
    bool            whole = true;
    uint32_t        mipBegin = 0;
    uint32_t        mipEnd = 0;
    uint32_t        layerBegin = 0;
    uint32_t        layerEnd = 0;
    // 1: color or depth, 2: stencil.
    uint32_t        planes = 1;
  };

  inline bool viewRangesOverlap(const D3D11ViewRange& a, const D3D11ViewRange& b) {
    if (!a.resource || a.resource != b.resource || !(a.planes & b.planes))
      return false;
    if (a.whole || b.whole)
      return true;
    return a.mipBegin < b.mipEnd && b.mipBegin < a.mipEnd
        && a.layerBegin < b.layerEnd && b.layerBegin < a.layerEnd;
  }

  namespace hazard_detail {

    struct Extent {
      uint32_t mips = 1;
      uint32_t layers = 1;
    };

    inline Extent resourceExtent(ID3D11Resource* resource) {
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);
      Extent extent;
      switch (dimension) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
          D3D11_TEXTURE1D_DESC desc;
          static_cast<D3D11Texture1D*>(static_cast<ID3D11Texture1D*>(resource))->GetDesc(&desc);
          extent = { desc.MipLevels, desc.ArraySize };
          break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
          D3D11_TEXTURE2D_DESC desc;
          static_cast<D3D11Texture2D*>(static_cast<ID3D11Texture2D*>(resource))->GetDesc(&desc);
          extent = { desc.MipLevels, desc.ArraySize };
          break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
          D3D11_TEXTURE3D_DESC desc;
          static_cast<D3D11Texture3D*>(static_cast<ID3D11Texture3D*>(resource))->GetDesc(&desc);
          extent = { desc.MipLevels, 1 };
          break;
        }
        default:
          break;
      }
      return extent;
    }

    inline D3D11ViewRange textureRange(ID3D11Resource* resource, uint32_t mip, uint32_t mipCount,
                                       uint32_t layer, uint32_t layerCount) {
      const Extent extent = resourceExtent(resource);
      D3D11ViewRange range;
      range.resource = resource;
      range.whole = false;
      range.mipBegin = mip;
      range.mipEnd = mipCount == UINT32_MAX || mip + mipCount > extent.mips ? extent.mips : mip + mipCount;
      range.layerBegin = layer;
      range.layerEnd = layerCount == UINT32_MAX || layer + layerCount > extent.layers ? extent.layers : layer + layerCount;
      return range;
    }

    inline bool isStencilViewFormat(DXGI_FORMAT format) {
      return format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    }

    inline bool hasStencil(DXGI_FORMAT format) {
      return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }

  }

  inline D3D11ViewRange shaderResourceRange(ID3D11ShaderResourceView* view) {
    using namespace hazard_detail;
    auto* impl = static_cast<D3D11ShaderResourceView*>(view);
    ID3D11Resource* resource = impl->GetResourceRaw();
    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    impl->GetDesc(&desc);
    D3D11ViewRange range;
    switch (desc.ViewDimension) {
      case D3D11_SRV_DIMENSION_TEXTURE1D:
        range = textureRange(resource, desc.Texture1D.MostDetailedMip, desc.Texture1D.MipLevels, 0, 1);
        break;
      case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
        range = textureRange(resource, desc.Texture1DArray.MostDetailedMip, desc.Texture1DArray.MipLevels,
          desc.Texture1DArray.FirstArraySlice, desc.Texture1DArray.ArraySize);
        break;
      case D3D11_SRV_DIMENSION_TEXTURE2D:
        range = textureRange(resource, desc.Texture2D.MostDetailedMip, desc.Texture2D.MipLevels, 0, 1);
        break;
      case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        range = textureRange(resource, desc.Texture2DArray.MostDetailedMip, desc.Texture2DArray.MipLevels,
          desc.Texture2DArray.FirstArraySlice, desc.Texture2DArray.ArraySize);
        break;
      case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        range = textureRange(resource, 0, 1, 0, 1);
        break;
      case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
        range = textureRange(resource, 0, 1, desc.Texture2DMSArray.FirstArraySlice, desc.Texture2DMSArray.ArraySize);
        break;
      case D3D11_SRV_DIMENSION_TEXTURE3D:
        range = textureRange(resource, desc.Texture3D.MostDetailedMip, desc.Texture3D.MipLevels, 0, 1);
        break;
      case D3D11_SRV_DIMENSION_TEXTURECUBE:
        range = textureRange(resource, desc.TextureCube.MostDetailedMip, desc.TextureCube.MipLevels, 0, 6);
        break;
      case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
        range = textureRange(resource, desc.TextureCubeArray.MostDetailedMip, desc.TextureCubeArray.MipLevels,
          desc.TextureCubeArray.First2DArrayFace, desc.TextureCubeArray.NumCubes * 6);
        break;
      default:
        range.resource = resource;
        break;
    }
    range.planes = isStencilViewFormat(desc.Format) ? 2u : 1u;
    return range;
  }

  inline D3D11ViewRange renderTargetRange(ID3D11RenderTargetView* view) {
    using namespace hazard_detail;
    auto* impl = static_cast<D3D11RenderTargetView*>(view);
    ID3D11Resource* resource = impl->GetResourceRaw();
    D3D11_RENDER_TARGET_VIEW_DESC desc;
    impl->GetDesc(&desc);
    switch (desc.ViewDimension) {
      case D3D11_RTV_DIMENSION_TEXTURE1D:
        return textureRange(resource, desc.Texture1D.MipSlice, 1, 0, 1);
      case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
        return textureRange(resource, desc.Texture1DArray.MipSlice, 1, desc.Texture1DArray.FirstArraySlice,
          desc.Texture1DArray.ArraySize);
      case D3D11_RTV_DIMENSION_TEXTURE2D:
        return textureRange(resource, desc.Texture2D.MipSlice, 1, 0, 1);
      case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        return textureRange(resource, desc.Texture2DArray.MipSlice, 1, desc.Texture2DArray.FirstArraySlice,
          desc.Texture2DArray.ArraySize);
      case D3D11_RTV_DIMENSION_TEXTURE2DMS:
        return textureRange(resource, 0, 1, 0, 1);
      case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
        return textureRange(resource, 0, 1, desc.Texture2DMSArray.FirstArraySlice, desc.Texture2DMSArray.ArraySize);
      case D3D11_RTV_DIMENSION_TEXTURE3D:
        return textureRange(resource, desc.Texture3D.MipSlice, 1, 0, 1);
      default: {
        D3D11ViewRange range;
        range.resource = resource;
        return range;
      }
    }
  }

  // The planes a depth-stencil view writes; none when it is read-only.
  inline D3D11ViewRange depthStencilWriteRange(ID3D11DepthStencilView* view) {
    using namespace hazard_detail;
    auto* impl = static_cast<D3D11DepthStencilView*>(view);
    ID3D11Resource* resource = impl->GetResourceRaw();
    D3D11_DEPTH_STENCIL_VIEW_DESC desc;
    impl->GetDesc(&desc);
    D3D11ViewRange range;
    switch (desc.ViewDimension) {
      case D3D11_DSV_DIMENSION_TEXTURE1D:
        range = textureRange(resource, desc.Texture1D.MipSlice, 1, 0, 1);
        break;
      case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
        range = textureRange(resource, desc.Texture1DArray.MipSlice, 1, desc.Texture1DArray.FirstArraySlice,
          desc.Texture1DArray.ArraySize);
        break;
      case D3D11_DSV_DIMENSION_TEXTURE2D:
        range = textureRange(resource, desc.Texture2D.MipSlice, 1, 0, 1);
        break;
      case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        range = textureRange(resource, desc.Texture2DArray.MipSlice, 1, desc.Texture2DArray.FirstArraySlice,
          desc.Texture2DArray.ArraySize);
        break;
      case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        range = textureRange(resource, 0, 1, 0, 1);
        break;
      case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        range = textureRange(resource, 0, 1, desc.Texture2DMSArray.FirstArraySlice, desc.Texture2DMSArray.ArraySize);
        break;
      default:
        range.resource = resource;
        break;
    }
    range.planes = 0;
    if (!(desc.Flags & D3D11_DSV_READ_ONLY_DEPTH))
      range.planes |= 1u;
    if (hasStencil(desc.Format) && !(desc.Flags & D3D11_DSV_READ_ONLY_STENCIL))
      range.planes |= 2u;
    return range;
  }

  inline D3D11ViewRange unorderedAccessRange(ID3D11UnorderedAccessView* view) {
    using namespace hazard_detail;
    auto* impl = static_cast<D3D11UnorderedAccessView*>(view);
    ID3D11Resource* resource = impl->GetResourceRaw();
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
    impl->GetDesc(&desc);
    switch (desc.ViewDimension) {
      case D3D11_UAV_DIMENSION_TEXTURE1D:
        return textureRange(resource, desc.Texture1D.MipSlice, 1, 0, 1);
      case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        return textureRange(resource, desc.Texture1DArray.MipSlice, 1, desc.Texture1DArray.FirstArraySlice,
          desc.Texture1DArray.ArraySize);
      case D3D11_UAV_DIMENSION_TEXTURE2D:
        return textureRange(resource, desc.Texture2D.MipSlice, 1, 0, 1);
      case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
        return textureRange(resource, desc.Texture2DArray.MipSlice, 1, desc.Texture2DArray.FirstArraySlice,
          desc.Texture2DArray.ArraySize);
      case D3D11_UAV_DIMENSION_TEXTURE3D:
        return textureRange(resource, desc.Texture3D.MipSlice, 1, 0, 1);
      default: {
        D3D11ViewRange range;
        range.resource = resource;
        return range;
      }
    }
  }

  inline D3D11ViewRange wholeBufferRange(ID3D11Buffer* buffer) {
    D3D11ViewRange range;
    range.resource = buffer;
    return range;
  }

}
