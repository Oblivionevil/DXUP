#include "d3d12_backend_device.h"
#include "d3d12_command_recorder.h"
#include "d3d12_backend_context.h"
#include "d3d12_cs_thread.h"
#include "d3d12_dxbc.h"
#include "../util/util_config.h"
#include "../util/util_types.h"
#include "../util/util_paths.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <share.h>
#include <set>

#ifndef _WIN32
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#ifdef _WIN32
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#endif

namespace dxup::d3d12 {

#ifdef _WIN32
  namespace {

    inline bool equalsIgnoreCase(const char* value, const char* expected) {
      while (*value && *expected) {
        if (std::tolower(static_cast<unsigned char>(*value))
            != std::tolower(static_cast<unsigned char>(*expected)))
          return false;

        value++;
        expected++;
      }

      return *value == '\0' && *expected == '\0';
    }

    inline bool getGpuSyncValidationEnabled() {
      char* value = nullptr;
      size_t valueSize = 0;

      if (_dupenv_s(&value, &valueSize, "DXUP_GPU_SYNC_VALIDATION") != 0 || !value)
        return false;

      bool enabled = *value != '\0'
          && std::strcmp(value, "0") != 0
          && !equalsIgnoreCase(value, "false")
          && !equalsIgnoreCase(value, "off")
          && !equalsIgnoreCase(value, "no");

      std::free(value);
      return enabled;
    }

    bool markerFileEnabled(const char* fileName) {
      return dxupSiblingExists(fileName);
    }

    bool shaderDescriptorLimitsEnabled() {
      static bool enabled = !markerFileEnabled("dxup_disable_shader_descriptor_limits.txt");
      return enabled;
    }

    bool simultaneousAccessEnabled() {
      static bool enabled = markerFileEnabled("dxup_simultaneous_access.txt");
      return enabled;
    }

    bool computeShaderDumpEnabled() {
      static bool enabled = markerFileEnabled("dxup_compute_shader_dump.txt");
      return enabled;
    }

    ShaderResourceLimits reflectShaderResourceLimits(const void* bytecode, size_t bytecodeSize) {
      ShaderResourceLimits limits = {};
      if (!shaderDescriptorLimitsEnabled() || !bytecode || !bytecodeSize)
        return limits;

      // Perf: cache reflect results by bytecode hash (D3DReflect parses bytecode,
      // must not run per PSO creation). Semantics-preserving: same bytecode -> same limits.
      uint64_t byteHash = 1469598103934665603ull;
      const auto* bbytes = static_cast<const uint8_t*>(bytecode);
      for (size_t i = 0; i < bytecodeSize; i++) {
        byteHash ^= static_cast<uint64_t>(bbytes[i]);
        byteHash = fnv64Multiply(byteHash);
      }
      uint64_t cacheKey = byteHash ^ (static_cast<uint64_t>(bytecodeSize) * 0x9e3779b97f4a7c15ull);
      static std::unordered_map<uint64_t, ShaderResourceLimits> s_reflectCache;
      static std::mutex s_reflectMutex;
      {
        std::lock_guard<std::mutex> lk(s_reflectMutex);
        auto it = s_reflectCache.find(cacheKey);
        if (it != s_reflectCache.end())
          return it->second;
      }

      // Perf: cache d3dcompiler module + D3DReflect proc (was Load/FreeLibrary per PSO).
      // Compiler threads get here at the same time: initialized once.
      using PFN_D3DReflect = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
      static const PFN_D3DReflect s_d3dReflect = [] {
        HMODULE compilerModule = LoadLibraryA("d3dcompiler_47.dll");
        if (!compilerModule)
          compilerModule = LoadLibraryA("d3dcompiler_43.dll");
        return compilerModule
          ? reinterpret_cast<PFN_D3DReflect>(GetProcAddress(compilerModule, "D3DReflect"))
          : nullptr;
      }();
      if (!s_d3dReflect)
        return limits;
      auto d3dReflect = s_d3dReflect;

      ID3D11ShaderReflection* reflection = nullptr;
      HRESULT hr = d3dReflect(bytecode, bytecodeSize, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&reflection));
      if (FAILED(hr) || !reflection) {
        return limits;
      }

      D3D11_SHADER_DESC shaderDesc = {};
      hr = reflection->GetDesc(&shaderDesc);
      if (FAILED(hr)) {
        reflection->Release();
        return limits;
      }

      // Without reflection data (stripped) the declarations in the code
      // tell the registers.
      if (shaderDesc.BoundResources == 0
       && !dxbc::declaredSlotCounts(bytecode, bytecodeSize, limits.cbvCount, limits.srvCount,
                                    limits.samplerCount, limits.uavCount)) {
        reflection->Release();
        return limits;
      }
      limits.known = true;
      for (UINT i = 0; i < shaderDesc.BoundResources; i++) {
        D3D11_SHADER_INPUT_BIND_DESC bindDesc = {};
        if (FAILED(reflection->GetResourceBindingDesc(i, &bindDesc)))
          continue;

        uint32_t endSlot = bindDesc.BindPoint + (std::max)(bindDesc.BindCount, 1u);
        switch (bindDesc.Type) {
          case D3D_SIT_CBUFFER:
            limits.cbvCount = (std::max)(limits.cbvCount, endSlot);
            break;
          case D3D_SIT_TBUFFER:   // a buffer view at t#: an SRV
          case D3D_SIT_TEXTURE:
          case D3D_SIT_STRUCTURED:
          case D3D_SIT_BYTEADDRESS:
            limits.srvCount = (std::max)(limits.srvCount, endSlot);
            break;
          case D3D_SIT_SAMPLER:
            limits.samplerCount = (std::max)(limits.samplerCount, endSlot);
            break;
          case D3D_SIT_UAV_RWTYPED:
          case D3D_SIT_UAV_RWSTRUCTURED:
          case D3D_SIT_UAV_RWBYTEADDRESS:
          case D3D_SIT_UAV_APPEND_STRUCTURED:
          case D3D_SIT_UAV_CONSUME_STRUCTURED:
          case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
            limits.uavCount = (std::max)(limits.uavCount, endSlot);
            break;
          default:
            break;
        }
      }

      reflection->Release();

      // The declared dimensions, interned: pipelines compare the pointers.
      std::array<uint8_t, 192> dimensions;
      const bool haveDimensions = dxbc::resourceDimensions(bytecode, bytecodeSize, dimensions.data(), dimensions.data() + 128);
      std::array<uint16_t, 64> uavAccess;
      const bool haveUavAccess = limits.uavCount && dxbc::uavAccess(bytecode, bytecodeSize, uavAccess.data());
      {
        std::lock_guard<std::mutex> lk(s_reflectMutex);
        if (haveDimensions) {
          static std::set<std::array<uint8_t, 192>> s_dimensions;
          limits.dimensions = s_dimensions.insert(dimensions).first->data();
        }
        if (haveUavAccess) {
          static std::set<std::array<uint16_t, 64>> s_uavAccess;
          limits.uavAccess = s_uavAccess.insert(uavAccess).first->data();
        }
        if (s_reflectCache.size() < 4096)
          s_reflectCache.emplace(cacheKey, limits);
      }
      return limits;
    }

    inline bool hasImageViewUsage(ImageViewUsage usageMask, ImageViewUsage usage) {
      return static_cast<uint32_t>(usageMask & usage) != 0;
    }

    inline uint32_t getImagePlaneCount(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
          return 2u;

        default:
          return isPlanarVideoFormat(format) ? 2u : 1u;
      }
    }

    inline uint32_t getImageSubresourceArrayCount(const ImageDesc& desc) {
      return desc.type == ImageType::Tex3D
        ? 1u
        : (desc.arraySize ? desc.arraySize : 1u);
    }

    inline uint32_t getImageSubresourceCount(const ImageDesc& desc) {
      uint32_t mipLevels = desc.mipLevels ? desc.mipLevels : 1u;
      return mipLevels * getImageSubresourceArrayCount(desc) * getImagePlaneCount(desc.format);
    }


    inline UINT getImageViewMipCount(const ImageViewDesc& desc, const ImageDesc& imageDesc) {
      if (desc.mipLevelCount)
        return desc.mipLevelCount;

      uint32_t totalMipLevels = imageDesc.mipLevels ? imageDesc.mipLevels : 1u;
      return totalMipLevels > desc.baseMipLevel ? totalMipLevels - desc.baseMipLevel : 1u;
    }

    inline UINT getImageViewArrayCount(const ImageViewDesc& desc, const ImageDesc& imageDesc) {
      if (desc.arrayLayerCount)
        return desc.arrayLayerCount;

      return imageDesc.type == ImageType::Tex3D ? imageDesc.depth : imageDesc.arraySize;
    }

    inline DXGI_FORMAT getImageViewFormat(const ImageViewDesc& desc, const ImageDesc& imageDesc) {
      return desc.format == DXGI_FORMAT_UNKNOWN ? imageDesc.format : desc.format;
    }

    inline DXGI_FORMAT getDepthStencilViewFormat(const ImageViewDesc& desc, const ImageDesc& imageDesc) {
      switch (getImageViewFormat(desc, imageDesc)) {
        case DXGI_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R16_TYPELESS:      return DXGI_FORMAT_D16_UNORM;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default:                            return getImageViewFormat(desc, imageDesc);
      }
    }

    // In D3D12, views on non-typeless resources must have the same format as the resource.
    // This function validates and fixes the view format for a D3D12 resource.
    inline DXGI_FORMAT getValidatedViewFormat(DXGI_FORMAT viewFormat, ID3D12Resource* resource) {
      if (!resource)
        return viewFormat;

      D3D12_RESOURCE_DESC resDesc = resource->GetDesc();
      DXGI_FORMAT resFormat = resDesc.Format;

      // If formats already match, no fixup needed
      if (viewFormat == resFormat)
        return viewFormat;

      // Video formats are viewed per plane (luma and chroma formats).
      if (isPlanarVideoFormat(resFormat))
        return viewFormat;

      // Typeless resources take typed views of their family.
      if (isTypelessFormat(resFormat))
        return viewFormat;

      // Non-typeless resource: view format must match resource format
      if (viewFormat != resFormat) {
        logMessage(LogLevel::Warn, "getValidatedViewFormat: fixing view format %d → %d (resource is non-typeless)",
          viewFormat, resFormat);
        return resFormat;
      }

      return viewFormat;
    }

    // Null descriptors of chroma views: a resource in the view format has
    // one plane only.
    template<typename ViewDesc>
    inline ViewDesc nullViewDesc(ViewDesc desc, DXGI_FORMAT imageFormat) {
      // TEXTURE2D and TEXTURE2DARRAY: 4 and 5 for both view types.
      if (isPlanarVideoFormat(imageFormat) && UINT(desc.ViewDimension) == 4)
        desc.Texture2D.PlaneSlice = 0;
      else if (isPlanarVideoFormat(imageFormat) && UINT(desc.ViewDimension) == 5)
        desc.Texture2DArray.PlaneSlice = 0;
      return desc;
    }

    inline void fillShaderResourceViewDesc(
        const ImageViewDesc& desc,
        const ImageDesc& imageDesc,
        D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc) {
      srvDesc = {};
      srvDesc.Format = getImageViewFormat(desc, imageDesc);
      srvDesc.Shader4ComponentMapping = desc.componentMapping ? desc.componentMapping : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      // Stencil views read the second plane of depth-stencil images (D3D12
      // removes the device for a stencil format on the depth plane), chroma
      // views the second plane of video images.
      const UINT planeSlice = getViewPlaneSlice(imageDesc.format, srvDesc.Format);

      switch (desc.type) {
        case ImageViewType::View1D:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
          srvDesc.Texture1D.MostDetailedMip = desc.baseMipLevel;
          srvDesc.Texture1D.MipLevels = getImageViewMipCount(desc, imageDesc);
          break;

        case ImageViewType::View1DArray:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
          srvDesc.Texture1DArray.MostDetailedMip = desc.baseMipLevel;
          srvDesc.Texture1DArray.MipLevels = getImageViewMipCount(desc, imageDesc);
          srvDesc.Texture1DArray.FirstArraySlice = desc.baseArrayLayer;
          srvDesc.Texture1DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          break;

        case ImageViewType::View2D:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          srvDesc.Texture2D.MostDetailedMip = desc.baseMipLevel;
          srvDesc.Texture2D.MipLevels = getImageViewMipCount(desc, imageDesc);
          srvDesc.Texture2D.PlaneSlice = planeSlice;
          break;

        case ImageViewType::View2DArray:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          srvDesc.Texture2DArray.MostDetailedMip = desc.baseMipLevel;
          srvDesc.Texture2DArray.MipLevels = getImageViewMipCount(desc, imageDesc);
          srvDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
          srvDesc.Texture2DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          srvDesc.Texture2DArray.PlaneSlice = planeSlice;
          break;

        case ImageViewType::View2DMS:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
          break;

        case ImageViewType::View2DMSArray:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
          srvDesc.Texture2DMSArray.FirstArraySlice = desc.baseArrayLayer;
          srvDesc.Texture2DMSArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          break;

        case ImageViewType::View3D:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
          srvDesc.Texture3D.MostDetailedMip = desc.baseMipLevel;
          srvDesc.Texture3D.MipLevels = getImageViewMipCount(desc, imageDesc);
          break;

        case ImageViewType::ViewCube:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
          srvDesc.TextureCube.MostDetailedMip = desc.baseMipLevel;
          srvDesc.TextureCube.MipLevels = getImageViewMipCount(desc, imageDesc);
          break;

        case ImageViewType::ViewCubeArray:
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
          srvDesc.TextureCubeArray.MostDetailedMip = desc.baseMipLevel;
          srvDesc.TextureCubeArray.MipLevels = getImageViewMipCount(desc, imageDesc);
          srvDesc.TextureCubeArray.First2DArrayFace = desc.baseArrayLayer;
          srvDesc.TextureCubeArray.NumCubes = getImageViewArrayCount(desc, imageDesc) / 6u;
          if (srvDesc.TextureCubeArray.NumCubes == 0)
            srvDesc.TextureCubeArray.NumCubes = 1;
          break;
      }
    }

    inline void fillRenderTargetViewDesc(
        const ImageViewDesc& desc,
        const ImageDesc& imageDesc,
        D3D12_RENDER_TARGET_VIEW_DESC& rtvDesc) {
      rtvDesc = {};
      rtvDesc.Format = getImageViewFormat(desc, imageDesc);

      switch (desc.type) {
        case ImageViewType::View1D:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
          rtvDesc.Texture1D.MipSlice = desc.baseMipLevel;
          break;

        case ImageViewType::View1DArray:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
          rtvDesc.Texture1DArray.MipSlice = desc.baseMipLevel;
          rtvDesc.Texture1DArray.FirstArraySlice = desc.baseArrayLayer;
          rtvDesc.Texture1DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          break;

        case ImageViewType::View2D:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
          rtvDesc.Texture2D.MipSlice = desc.baseMipLevel;
          rtvDesc.Texture2D.PlaneSlice = getViewPlaneSlice(imageDesc.format, rtvDesc.Format);
          break;

        case ImageViewType::View2DArray:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
          rtvDesc.Texture2DArray.MipSlice = desc.baseMipLevel;
          rtvDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
          rtvDesc.Texture2DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          rtvDesc.Texture2DArray.PlaneSlice = getViewPlaneSlice(imageDesc.format, rtvDesc.Format);
          break;

        case ImageViewType::View2DMS:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
          break;

        case ImageViewType::View2DMSArray:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
          rtvDesc.Texture2DMSArray.FirstArraySlice = desc.baseArrayLayer;
          rtvDesc.Texture2DMSArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          break;

        case ImageViewType::View3D:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
          rtvDesc.Texture3D.MipSlice = desc.baseMipLevel;
          rtvDesc.Texture3D.FirstWSlice = static_cast<UINT>(desc.baseArrayLayer);
          rtvDesc.Texture3D.WSize = getImageViewArrayCount(desc, imageDesc);
          break;

        default:
          rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
          rtvDesc.Texture2D.MipSlice = desc.baseMipLevel;
          break;
      }
    }

    inline void fillDepthStencilViewDesc(
        const ImageViewDesc& desc,
        const ImageDesc& imageDesc,
        D3D12_DEPTH_STENCIL_VIEW_DESC& dsvDesc) {
      dsvDesc = {};
      dsvDesc.Format = getDepthStencilViewFormat(desc, imageDesc);
      if (static_cast<uint32_t>(desc.flags & ImageViewFlags::ReadOnlyDepth))
        dsvDesc.Flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
      if (static_cast<uint32_t>(desc.flags & ImageViewFlags::ReadOnlyStencil))
        dsvDesc.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;

      switch (desc.type) {
        case ImageViewType::View1D:
          dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
          dsvDesc.Texture1D.MipSlice = desc.baseMipLevel;
          break;

        case ImageViewType::View1DArray:
          dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
          dsvDesc.Texture1DArray.MipSlice = desc.baseMipLevel;
          dsvDesc.Texture1DArray.FirstArraySlice = desc.baseArrayLayer;
          dsvDesc.Texture1DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          break;

        case ImageViewType::View2D:
          dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
          dsvDesc.Texture2D.MipSlice = desc.baseMipLevel;
          break;

        case ImageViewType::View2DArray:
          dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
          dsvDesc.Texture2DArray.MipSlice = desc.baseMipLevel;
          dsvDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
          dsvDesc.Texture2DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          break;

        case ImageViewType::View2DMS:
          dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
          break;

        case ImageViewType::View2DMSArray:
          dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
          dsvDesc.Texture2DMSArray.FirstArraySlice = desc.baseArrayLayer;
          dsvDesc.Texture2DMSArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          break;

        default:
          dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
          dsvDesc.Texture2D.MipSlice = desc.baseMipLevel;
          break;
      }
    }

    inline bool fillUnorderedAccessViewDesc(
        const ImageViewDesc& desc,
        const ImageDesc& imageDesc,
        D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc) {
      uavDesc = {};
      uavDesc.Format = getImageViewFormat(desc, imageDesc);

      switch (desc.type) {
        case ImageViewType::View1D:
          uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
          uavDesc.Texture1D.MipSlice = desc.baseMipLevel;
          return true;

        case ImageViewType::View1DArray:
          uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
          uavDesc.Texture1DArray.MipSlice = desc.baseMipLevel;
          uavDesc.Texture1DArray.FirstArraySlice = desc.baseArrayLayer;
          uavDesc.Texture1DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          return true;

        case ImageViewType::View2D:
          uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
          uavDesc.Texture2D.MipSlice = desc.baseMipLevel;
          uavDesc.Texture2D.PlaneSlice = getViewPlaneSlice(imageDesc.format, uavDesc.Format);
          return true;

        case ImageViewType::View2DArray:
          uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
          uavDesc.Texture2DArray.MipSlice = desc.baseMipLevel;
          uavDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
          uavDesc.Texture2DArray.ArraySize = getImageViewArrayCount(desc, imageDesc);
          uavDesc.Texture2DArray.PlaneSlice = getViewPlaneSlice(imageDesc.format, uavDesc.Format);
          return true;

        case ImageViewType::View3D:
          uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
          uavDesc.Texture3D.MipSlice = desc.baseMipLevel;
          uavDesc.Texture3D.FirstWSlice = static_cast<UINT>(desc.baseArrayLayer);
          uavDesc.Texture3D.WSize = getImageViewArrayCount(desc, imageDesc);
          return true;

        default:
          return false;
      }
    }

    inline bool fillBufferShaderResourceViewDesc(
        const BufferDesc& bufferDesc,
        const BufferViewDesc& desc,
        D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc) {
      if (desc.offset > UINT_MAX || desc.size > UINT_MAX)
        return false;

      srvDesc = {};
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Buffer.FirstElement = static_cast<UINT>(desc.offset);
      srvDesc.Buffer.NumElements = static_cast<UINT>(desc.size);

      if (desc.raw) {
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        return true;
      }

      if (desc.format == DXGI_FORMAT_UNKNOWN) {
        if (!bufferDesc.structStride)
          return false;

        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.Buffer.StructureByteStride = bufferDesc.structStride;
        return true;
      }

      srvDesc.Format = desc.format;
      return true;
    }

    inline bool fillBufferUnorderedAccessViewDesc(
        const BufferDesc& bufferDesc,
        const BufferViewDesc& desc,
        D3D12_UNORDERED_ACCESS_VIEW_DESC& uavDesc) {
      if (desc.offset > UINT_MAX || desc.size > UINT_MAX)
        return false;

      uavDesc = {};
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      uavDesc.Buffer.FirstElement = static_cast<UINT>(desc.offset);
      uavDesc.Buffer.NumElements = static_cast<UINT>(desc.size);

      if (desc.raw) {
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        return true;
      }

      if (desc.format == DXGI_FORMAT_UNKNOWN) {
        if (!bufferDesc.structStride)
          return false;

        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.Buffer.StructureByteStride = bufferDesc.structStride;
        if (desc.hasCounter)
          uavDesc.Buffer.CounterOffsetInBytes = 0;
        return true;
      }

      uavDesc.Format = desc.format;
      return true;
    }

  }
#endif

  // ============================================================
  // D3D12BackendBuffer
  // ============================================================

  D3D12BackendBuffer::D3D12BackendBuffer(
#ifdef _WIN32
    ComPtr<ID3D12Resource>   resource,
#endif
    const BufferDesc&        desc)
#ifdef _WIN32
  : m_resource(std::move(resource))
  , m_desc(desc) {
#else
  : m_desc(desc) {
#endif
#ifdef _WIN32
    if (m_resource)
      m_gpuAddress = m_resource->GetGPUVirtualAddress();
    const uint32_t cpuAccess = static_cast<uint32_t>(m_desc.cpuAccess);
    const bool persistentlyMappable = m_resource
      && (m_desc.usage == ResourceUsage::Dynamic
       || m_desc.usage == ResourceUsage::Staging)
      && (cpuAccess & static_cast<uint32_t>(CpuAccess::Write)) != 0
      && (cpuAccess & static_cast<uint32_t>(CpuAccess::Read)) == 0;
    if (persistentlyMappable) {
      D3D12_RANGE readRange = { 0, 0 };
      if (FAILED(m_resource->Map(0, &readRange, &m_persistentMappedData)))
        m_persistentMappedData = nullptr;
    }
    m_persistentlyMapped = m_persistentMappedData != nullptr;
#endif
  }

#ifdef _WIN32
  HRESULT D3D12BackendBuffer::createTileHeap(ID3D12Device* device, uint64_t newSize, TileHeap& heap) {
    const uint32_t existingTiles = m_reservedTiles;
    const uint32_t wantedTiles = static_cast<uint32_t>((newSize + kTileSizeInBytes - 1) / kTileSizeInBytes);
    if (!device || wantedTiles <= existingTiles)
      return S_FALSE;

    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = uint64_t(wantedTiles - existingTiles) * kTileSizeInBytes;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_NONE;
    HRESULT hr = device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap.heap));
    if (FAILED(hr))
      return hr;
    memstats::track(heap.heap.Get(), memstats::TilePool);
    heap.firstTile = existingTiles;
    heap.tileCount = wantedTiles - existingTiles;
    m_reservedTiles = wantedTiles;
    return S_OK;
  }

  void D3D12BackendBuffer::appendTileHeap(TileHeap heap) {
    // Command lists replayed again must not add a heap twice.
    if (uint64_t(heap.firstTile + heap.tileCount) * kTileSizeInBytes <= m_tilePoolSize)
      return;
    m_tilePoolSize = uint64_t(heap.firstTile + heap.tileCount) * kTileSizeInBytes;
    m_tileHeaps.push_back(std::move(heap));
  }

  ID3D12Resource* D3D12BackendBuffer::getStreamOutputCounter(ID3D12Device* device) {
    if (m_soCounter || !device)
      return m_soCounter.Get();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = sizeof(uint64_t);
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // Committed resources start zeroed: nothing written yet.
    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_soCounter)))) {
      logMessage(LogLevel::Error, "D3D12BackendBuffer: stream output counter creation failed");
      return nullptr;
    }
    m_soCounterState = D3D12_RESOURCE_STATE_COPY_DEST;
    return m_soCounter.Get();
  }
#endif

  D3D12BackendBuffer::D3D12BackendBuffer(const BufferDesc& desc, D3D12UploadSuballocator* allocator,
                                         const D3D12UploadSuballocator::Slice& slice)
  : m_resource(slice.resource)
  , m_persistentMappedData(slice.mapped)
  , m_currentState(slice.state ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_GENERIC_READ)
  , m_fixedState(!slice.state)
  , m_offset(slice.offset)
  , m_slice(slice)
  , m_sharedState(slice.state)
  , m_suballocator(allocator)
  , m_sizeClass(slice.sizeClass)
  , m_persistentlyMapped(allocator->isMapped())
  , m_desc(desc) {
    m_gpuAddress = slice.resource->GetGPUVirtualAddress() + slice.offset;
  }

  void D3D12BackendDevice::freeSlices(const std::vector<D3D12UploadSuballocator::Slice>& slices) {
    bool mixed = false;
    for (const auto& slice : slices)
      mixed |= slice.state != nullptr;
    if (!mixed) {
      m_uploadSuballocator.free(slices);
      return;
    }
    for (const auto& slice : slices)
      (slice.state ? m_defaultSuballocator : m_uploadSuballocator).free(slice);
  }

  uint32_t D3D12UploadSuballocator::allocateMany(ID3D12Device* device, uint64_t size, Slice* slices, uint32_t count) {
    if (!count || !allocate(device, size, slices[0]))
      return 0;
    // The first allocation filled the class: take more from it directly.
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint32_t sizeClass = slices[0].sizeClass;
    auto& freeSlices = m_free[sizeClass];
    uint32_t taken = 1;
    while (taken < count && !freeSlices.empty()) {
      slices[taken++] = freeSlices.back();
      freeSlices.pop_back();
    }
    const int64_t used = m_used[sizeClass].fetch_add(taken - 1, std::memory_order_relaxed) + taken - 1;
    m_peak[sizeClass] = (std::max)(m_peak[sizeClass], used);
    return taken;
  }

  bool D3D12UploadSuballocator::allocate(ID3D12Device* device, uint64_t size, Slice& slice) {
    if (!device || !size || size > kMaxSlice)
      return false;
    uint32_t sizeClass = 0;
    while ((m_minSlice << sizeClass) < size)
      sizeClass++;
    const uint64_t sliceSize = m_minSlice << sizeClass;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto& freeSlices = m_free[sizeClass];
    if (freeSlices.empty()) {
      std::lock_guard<std::mutex> returnedLock(m_returnedMutex);
      freeSlices.swap(m_returned[sizeClass]);
    }
    if (freeSlices.empty()) {
      const bool upload = m_heapType == D3D12_HEAP_TYPE_UPLOAD;
      D3D12_HEAP_PROPERTIES heapProps = {};
      heapProps.Type = m_heapType;
      if (upload)
        uploadHeapProperties(heapProps, m_gpuUploadHeap, m_writeBack);
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width            = kChunkSize;
      desc.Height           = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels        = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ComPtr<ID3D12Resource> chunk;
      if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
          upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&chunk))))
        return false;
      memstats::track(device, chunk.Get(), memstats::BlockHeap);
      void* mapped = nullptr;
      D3D12_RESOURCE_STATES* state = nullptr;
      if (upload) {
        D3D12_RANGE readRange = { 0, 0 };
        if (FAILED(chunk->Map(0, &readRange, &mapped)) || !mapped)
          return false;
      } else {
        m_chunkStates.push_back(std::make_unique<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_COMMON));
        state = m_chunkStates.back().get();
      }
      const uint64_t count = kChunkSize / sliceSize;
      freeSlices.reserve(freeSlices.size() + size_t(count));
      for (uint64_t i = count; i > 0; i--) {
        Slice entry;
        entry.resource = chunk.Get();
        entry.offset = (i - 1) * sliceSize;
        entry.mapped = mapped ? static_cast<uint8_t*>(mapped) + entry.offset : nullptr;
        entry.sizeClass = sizeClass;
        entry.state = state;
        freeSlices.push_back(entry);
      }
      m_chunks.push_back(std::move(chunk));
      m_chunkCount[sizeClass]++;
    }
    slice = freeSlices.back();
    freeSlices.pop_back();
    const int64_t used = m_used[sizeClass].fetch_add(1, std::memory_order_relaxed) + 1;
    m_peak[sizeClass] = (std::max)(m_peak[sizeClass], used);
    return true;
  }

  void D3D12UploadSuballocator::free(const Slice& slice) {
    std::lock_guard<std::mutex> lock(m_returnedMutex);
    m_returned[slice.sizeClass].push_back(slice);
    m_used[slice.sizeClass].fetch_sub(1, std::memory_order_relaxed);
  }

  void D3D12UploadSuballocator::free(const std::vector<Slice>& slices) {
    if (slices.empty())
      return;
    std::lock_guard<std::mutex> lock(m_returnedMutex);
    for (const auto& slice : slices) {
      m_returned[slice.sizeClass].push_back(slice);
      m_used[slice.sizeClass].fetch_sub(1, std::memory_order_relaxed);
    }
  }

  void D3D12UploadSuballocator::trim() {
    // Releasing a block costs the present a few hundred microseconds: two
    // a present go of those decided on.
    if (m_releasingCount.load(std::memory_order_relaxed)) {
      ComPtr<ID3D12Resource> going[2];
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& block : going) {
          if (m_releasing.empty())
            break;
          block = std::move(m_releasing.back());
          m_releasing.pop_back();
        }
        m_releasingCount.store(m_releasing.size(), std::memory_order_relaxed);
      }
    }
    // Every 10 seconds: what was needed in the last 20 to 30 seconds stays,
    // and a block more. Default blocks change their state and stay.
    const uint64_t now = GetTickCount64();
    uint64_t last = m_lastTrim.load(std::memory_order_relaxed);
    if (m_heapType != D3D12_HEAP_TYPE_UPLOAD || now - last < trimIntervalMs()
     || !m_lastTrim.compare_exchange_strong(last, now, std::memory_order_relaxed))
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::lock_guard<std::mutex> returnedLock(m_returnedMutex);
    std::vector<ID3D12Resource*> released;
    for (uint32_t c = 0; c < kMaxClassCount; c++) {
      const int64_t used = m_used[c].load(std::memory_order_relaxed);
      const int64_t peak = (std::max)({ m_peak[c], m_earlierPeaks[c][0], m_earlierPeaks[c][1], used });
      m_earlierPeaks[c][1] = m_earlierPeaks[c][0];
      m_earlierPeaks[c][0] = m_peak[c];
      m_peak[c] = used;
      const int64_t perChunk = int64_t(kChunkSize / sliceSize(c));
      int64_t total = int64_t(m_chunkCount[c]) * perChunk;
      auto& freeSlices = m_free[c];
      if (total - perChunk < peak + perChunk || freeSlices.size() + m_returned[c].size() < size_t(perChunk))
        continue;
      freeSlices.insert(freeSlices.end(), m_returned[c].begin(), m_returned[c].end());
      m_returned[c].clear();
      // The blocks all of whose slices are free.
      std::unordered_map<ID3D12Resource*, int64_t> freeCounts;
      for (const auto& slice : freeSlices)
        freeCounts[slice.resource]++;
      const size_t first = released.size();
      for (const auto& entry : freeCounts) {
        if (total - perChunk < peak + perChunk)
          break;
        if (entry.second != perChunk)
          continue;
        released.push_back(entry.first);
        total -= perChunk;
        m_chunkCount[c]--;
      }
      std::unordered_set<ID3D12Resource*> gone(released.begin() + first, released.end());
      if (!gone.empty()) {
        freeSlices.erase(std::remove_if(freeSlices.begin(), freeSlices.end(),
          [&](const Slice& slice) { return gone.count(slice.resource) != 0; }), freeSlices.end());
      }
      // The slices of the fullest blocks are handed out first (from the
      // back): the others empty, and the next trim can give them back.
      std::vector<std::pair<int64_t, Slice>> keyed;
      keyed.reserve(freeSlices.size());
      for (const auto& slice : freeSlices)
        keyed.emplace_back(freeCounts[slice.resource], slice);
      std::stable_sort(keyed.begin(), keyed.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
      for (size_t i = 0; i < keyed.size(); i++)
        freeSlices[i] = keyed[i].second;
    }
    if (released.empty())
      return;
    std::unordered_set<ID3D12Resource*> gone(released.begin(), released.end());
    for (auto& chunk : m_chunks) {
      if (gone.count(chunk.Get()))
        m_releasing.push_back(std::move(chunk));
    }
    m_chunks.erase(std::remove_if(m_chunks.begin(), m_chunks.end(),
      [](const ComPtr<ID3D12Resource>& chunk) { return !chunk; }), m_chunks.end());
    m_releasingCount.store(m_releasing.size(), std::memory_order_relaxed);
    logMessage(LogLevel::Info, "D3D12UploadSuballocator: %zu idle blocks (%llu MB) go back",
      released.size(), static_cast<unsigned long long>((released.size() * kChunkSize) >> 20));
  }

  D3D12BackendBuffer::~D3D12BackendBuffer() {
#ifdef _WIN32
    if (m_suballocator) {
      // The GPU is done with the slice: retired buffers are destroyed after
      // the commands that use them completed.
      m_suballocator->free(m_slice);
      m_persistentMappedData = nullptr;
      return;
    }
    if (m_resource && m_persistentMappedData) {
      D3D12_RANGE writtenRange = { 0, static_cast<SIZE_T>(m_desc.size) };
      m_resource->Unmap(0, &writtenRange);
      m_persistentMappedData = nullptr;
    }
#endif
  }

  // ============================================================
  // D3D12BackendImage
  // ============================================================

  D3D12BackendImage::D3D12BackendImage(
#ifdef _WIN32
    ComPtr<ID3D12Resource>   resource,
#endif
    const ImageDesc&         desc,
    bool                     simultaneousAccess)
#ifdef _WIN32
  : m_resource(std::move(resource))
  , m_subresourceStates(getImageSubresourceCount(desc), D3D12_RESOURCE_STATE_COMMON)
  , m_planeCount(getImagePlaneCount(desc.format))
  , m_simultaneousAccess(simultaneousAccess)
  , m_desc(desc) {
#else
  : m_desc(desc) {
#endif
  }

  void D3D12BackendImage::initStaging(std::shared_ptr<D3D12BackendBuffer> buffer, StagingLayout layout, uint8_t* mapped,
                                      ID3D12Device* device, std::shared_ptr<D3D12StagingSpareRegistry> spareRegistry) {
    m_stagingBuffer = std::move(buffer);
    m_staging = std::move(layout);
    m_mappedData = mapped;
    m_stagingDevice = device;
    m_spareRegistry = std::move(spareRegistry);
    switch (m_staging.textureDesc.Format) {
      case DXGI_FORMAT_R24G8_TYPELESS:
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
        m_interleavedTexelSize = 4;
        break;
      case DXGI_FORMAT_R32G8X24_TYPELESS:
      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        m_interleavedTexelSize = 8;
        break;
      default:
        m_interleavedTexelSize = 0;
        break;
    }
    if (m_interleavedTexelSize) {
      const size_t subresources = size_t(getMipLevelCount()) * getArraySubresourceCount();
      m_interleaved.resize(subresources);
      m_interleavedWritable.assign(subresources, 0);
    }
  }

  bool D3D12BackendImage::renameStaging(uint64_t readFence, uint64_t completedFence) {
    // Copying the contents costs less than waiting up to this size; the
    // spares cover the frames the GPU may lag behind. Mapped spares are
    // address space in a 32-bit process: fewer there.
    constexpr uint64_t kMaxSize = 4ull << 20;
    constexpr uint64_t kMaxSpareBytes = kSmallAddressSpace ? (4ull << 20) : (16ull << 20);
    constexpr size_t kMaxSpares = kSmallAddressSpace ? 2 : 6;
    if (!m_stagingBuffer || !m_stagingDevice || m_interleavedTexelSize || m_stagingMaps)
      return false;
    const uint64_t size = m_stagingBuffer->getDesc().size;
    if (size > kMaxSize)
      return false;

    std::unique_lock<std::mutex> lock(m_stagingMutex);
    StagingSpare next;
    for (auto it = m_stagingSpares.begin(); it != m_stagingSpares.end(); ++it) {
      if (it->fence <= completedFence) {
        next = std::move(*it);
        m_stagingSpares.erase(it);
        break;
      }
    }
    if (!next.buffer) {
      if (m_stagingSpares.size() >= kMaxSpares || (m_stagingSpares.size() + 1) * size > kMaxSpareBytes)
        return false;
      D3D12_HEAP_PROPERTIES heapProps = m_stagingDevice->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_READBACK);
      D3D12_RESOURCE_DESC bufferDesc = m_resource->GetDesc();
      ComPtr<ID3D12Resource> resource;
      void* mapped = nullptr;
      if (FAILED(m_stagingDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)))
       || FAILED(resource->Map(0, nullptr, &mapped)) || !mapped)
        return false;
      next.buffer = std::make_shared<D3D12BackendBuffer>(resource, m_stagingBuffer->getDesc());
      next.mapped = static_cast<uint8_t*>(mapped);
    }
    std::memcpy(next.mapped, m_mappedData, size_t(size));

    StagingSpare old;
    old.buffer = std::move(m_stagingBuffer);
    old.mapped = m_mappedData;
    old.fence = readFence;
    old.epoch = m_spareRegistry ? m_spareRegistry->epoch.load() : 0;
    m_stagingSpares.push_back(std::move(old));
    m_stagingBuffer = std::move(next.buffer);
    m_mappedData = next.mapped;
    m_resource = m_stagingBuffer->getResource();
    const size_t spares = m_stagingSpares.size();
    const bool registerSpares = m_spareRegistry && !m_spareRegistered;
    m_spareRegistered = m_spareRegistered || registerSpares;
    lock.unlock();
    // Not under the image's mutex: the trim takes the registry's first.
    if (registerSpares) {
      std::lock_guard<std::mutex> registryLock(m_spareRegistry->mutex);
      m_spareRegistry->images.push_back(weak_from_this());
    }
    logMessage(LogLevel::Info, "renameStaging: image %p, %llu bytes, %zu spares", static_cast<void*>(this),
      static_cast<unsigned long long>(size), spares);
    return true;
  }

  bool D3D12BackendImage::trimStagingSpares(uint64_t releaseEpoch, size_t& released) {
    std::vector<StagingSpare> going;   // freed after the lock
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    for (auto it = m_stagingSpares.begin(); it != m_stagingSpares.end();) {
      if (it->epoch <= releaseEpoch) {
        going.push_back(std::move(*it));
        it = m_stagingSpares.erase(it);
      } else {
        ++it;
      }
    }
    released += going.size();
    // The device drops an image without spares; the next rename adds it again.
    m_spareRegistered = !m_stagingSpares.empty();
    return m_spareRegistered;
  }

  uint8_t* D3D12BackendImage::mapStaging(uint32_t subresource, MapMode mode, ImageSubresourceLayout& layout) {
    const uint32_t subresources = getMipLevelCount() * getArraySubresourceCount();
    if (!m_stagingBuffer || subresource >= subresources || subresource >= m_staging.footprints.size())
      return nullptr;
    m_stagingMaps++;
    const auto& footprint = m_staging.footprints[subresource];
    if (!m_interleavedTexelSize) {
      // Planar video formats follow plane after plane, as D3D11 maps them.
      layout.offset     = 0;
      layout.rowPitch   = footprint.Footprint.RowPitch;
      layout.depthPitch = footprint.Footprint.RowPitch * m_staging.numRows[subresource];
      return m_mappedData + footprint.Offset;
    }

    const auto& stencil = m_staging.footprints[subresource + subresources];
    const uint32_t width = footprint.Footprint.Width;
    const uint32_t height = footprint.Footprint.Height;
    const uint32_t rowPitch = width * m_interleavedTexelSize;
    auto& texels = m_interleaved[subresource];
    texels.resize(size_t(rowPitch) * height);
    for (uint32_t y = 0; y < height; y++) {
      const auto* depthRow = reinterpret_cast<const uint32_t*>(m_mappedData + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
      const uint8_t* stencilRow = m_mappedData + stencil.Offset + size_t(y) * stencil.Footprint.RowPitch;
      uint8_t* out = texels.data() + size_t(y) * rowPitch;
      for (uint32_t x = 0; x < width; x++) {
        if (m_interleavedTexelSize == 4) {
          const uint32_t value = (depthRow[x] & 0x00FFFFFFu) | (uint32_t(stencilRow[x]) << 24);
          std::memcpy(out + x * 4, &value, 4);
        } else {
          const uint64_t value = uint64_t(depthRow[x]) | (uint64_t(stencilRow[x]) << 32);
          std::memcpy(out + x * 8, &value, 8);
        }
      }
    }
    m_interleavedWritable[subresource] = mode != MapMode::Read;
    layout.offset     = 0;
    layout.rowPitch   = rowPitch;
    layout.depthPitch = rowPitch * height;
    return texels.data();
  }

  void D3D12BackendImage::unmapStaging(uint32_t subresource) {
    if (m_stagingMaps)
      m_stagingMaps--;
    if (!m_interleavedTexelSize || subresource >= m_interleavedWritable.size() || !m_interleavedWritable[subresource])
      return;
    m_interleavedWritable[subresource] = 0;
    const uint32_t subresources = getMipLevelCount() * getArraySubresourceCount();
    const auto& footprint = m_staging.footprints[subresource];
    const auto& stencil = m_staging.footprints[subresource + subresources];
    const uint32_t width = footprint.Footprint.Width;
    const uint32_t height = footprint.Footprint.Height;
    const uint32_t rowPitch = width * m_interleavedTexelSize;
    const auto& texels = m_interleaved[subresource];
    for (uint32_t y = 0; y < height; y++) {
      auto* depthRow = reinterpret_cast<uint32_t*>(m_mappedData + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
      uint8_t* stencilRow = m_mappedData + stencil.Offset + size_t(y) * stencil.Footprint.RowPitch;
      const uint8_t* in = texels.data() + size_t(y) * rowPitch;
      for (uint32_t x = 0; x < width; x++) {
        if (m_interleavedTexelSize == 4) {
          uint32_t value = 0;
          std::memcpy(&value, in + x * 4, 4);
          depthRow[x] = value & 0x00FFFFFFu;
          stencilRow[x] = uint8_t(value >> 24);
        } else {
          uint64_t value = 0;
          std::memcpy(&value, in + x * 8, 8);
          depthRow[x] = uint32_t(value);
          stencilRow[x] = uint8_t(value >> 32);
        }
      }
    }
  }

  // ============================================================
  // D3D12BackendBufferView
  // ============================================================

  uint64_t nextDescriptorVersion() {
    static std::atomic<uint64_t> s_version{ 0 };
    return s_version.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  D3D12BackendBufferView::D3D12BackendBufferView(
    IBackendBuffer*          buffer,
    const BufferViewDesc&    desc
    , Rc<IBackendBuffer>     counterBuffer
    , D3D12DescriptorHeapManager* heapMgr
#ifdef _WIN32
    , ID3D12Device*          device
    , D3D12_CPU_DESCRIPTOR_HANDLE srvHandle
    , D3D12_CPU_DESCRIPTOR_HANDLE uavHandle
    , D3D12_CPU_DESCRIPTOR_HANDLE nullSrvHandle
    , D3D12_CPU_DESCRIPTOR_HANDLE nullUavHandle
#endif
  )
  : m_buffer(buffer)
  , m_bufferOwner(buffer ? buffer->retireOwner() : nullptr)
  , m_desc(desc)
  , m_counterBuffer(std::move(counterBuffer))
  , m_heapMgr(heapMgr)
#ifdef _WIN32
  , m_device(device)
  , m_srvHandle(srvHandle)
  , m_uavHandle(uavHandle)
  , m_nullSrvHandle(nullSrvHandle)
  , m_nullUavHandle(nullUavHandle)
#endif
  {
  }

  bool D3D12BackendBufferView::retargetBuffer(IBackendBuffer* buffer) {
#ifdef _WIN32
    if (!buffer || !m_device)
      return false;

    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (!d3d12Buffer || !d3d12Buffer->getResource())
      return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    if (!fillBufferShaderResourceViewDesc(d3d12Buffer->getDesc(), m_desc, srvDesc))
      return false;

    if (m_srvHandle.ptr)
      m_device->CreateShaderResourceView(d3d12Buffer->getResource(), &srvDesc, m_srvHandle);
    if (m_nullSrvHandle.ptr)
      m_device->CreateShaderResourceView(nullptr, &srvDesc, m_nullSrvHandle);

    if (m_uavHandle.ptr || m_nullUavHandle.ptr) {
      if (!static_cast<uint32_t>(d3d12Buffer->getDesc().bindFlags & BindFlags::UnorderedAccess))
        return false;

      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      if (!fillBufferUnorderedAccessViewDesc(d3d12Buffer->getDesc(), m_desc, uavDesc))
        return false;

      ID3D12Resource* counterResource = nullptr;
      if (m_counterBuffer) {
        auto* d3d12Counter = static_cast<D3D12BackendBuffer*>(m_counterBuffer.get());
        counterResource = d3d12Counter ? d3d12Counter->getResource() : nullptr;
      }

      if (m_uavHandle.ptr)
        m_device->CreateUnorderedAccessView(d3d12Buffer->getResource(), counterResource, &uavDesc, m_uavHandle);
      if (m_nullUavHandle.ptr)
        m_device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, m_nullUavHandle);
    }
    // The clear view must follow the rename too, or clears hit the old bytes.
    if (m_rawClearUavHandle.ptr)
      writeRawClearUav(d3d12Buffer);

    m_buffer = buffer;
    m_bufferOwner = buffer->retireOwner();
    m_descriptorVersion = nextDescriptorVersion();
    return true;
#else
    m_buffer = buffer;
    m_bufferOwner = buffer ? buffer->retireOwner() : nullptr;
    m_descriptorVersion = nextDescriptorVersion();
    return true;
#endif
  }

  D3D12BackendBufferView::~D3D12BackendBufferView() {
#ifdef _WIN32
    if (m_heapMgr) {
      if (m_srvHandle.ptr)     m_heapMgr->freeCbvSrvUav(m_srvHandle);
      if (m_uavHandle.ptr)     m_heapMgr->freeCbvSrvUav(m_uavHandle);
      if (m_nullSrvHandle.ptr) m_heapMgr->freeCbvSrvUav(m_nullSrvHandle);
      if (m_nullUavHandle.ptr) m_heapMgr->freeCbvSrvUav(m_nullUavHandle);
      if (m_rawClearUavHandle.ptr) m_heapMgr->freeCbvSrvUav(m_rawClearUavHandle);
    }
#endif
  }

#ifdef _WIN32
  D3D12_CPU_DESCRIPTOR_HANDLE D3D12BackendBufferView::getRawClearUavHandle() {
    if (m_rawClearUavHandle.ptr || !m_heapMgr || !m_device || !m_buffer)
      return m_rawClearUavHandle;
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(m_buffer);
    if (!d3d12Buffer->getDesc().structStride)
      return m_rawClearUavHandle;
    m_rawClearUavHandle = m_heapMgr->allocateCbvSrvUav();
    if (m_rawClearUavHandle.ptr)
      writeRawClearUav(d3d12Buffer);
    return m_rawClearUavHandle;
  }

  void D3D12BackendBufferView::writeRawClearUav(D3D12BackendBuffer* buffer) {
    // The view counts structures; the raw view counts 32-bit words.
    const uint64_t stride = buffer->getDesc().structStride;
    if (!stride)
      return;
    const uint64_t firstByte = m_desc.offset * stride;
    const uint64_t byteCount = m_desc.size ? m_desc.size * stride : buffer->getDesc().size - firstByte;
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = firstByte / 4;
    desc.Buffer.NumElements = static_cast<UINT>(byteCount / 4);
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    m_device->CreateUnorderedAccessView(buffer->getResource(), nullptr, &desc, m_rawClearUavHandle);
  }
#endif

  // ============================================================
  // D3D12BackendImageView
  // ============================================================

  D3D12BackendImageView::D3D12BackendImageView(
    IBackendImage*           image,
    const ImageViewDesc&     desc,
    D3D12DescriptorHeapManager* heapMgr
#ifdef _WIN32
    , D3D12_CPU_DESCRIPTOR_HANDLE handle
    , D3D12_CPU_DESCRIPTOR_HANDLE nullHandle
#endif
  )
  : m_image(image)
  , m_imageOwner(image ? static_cast<D3D12BackendImage*>(image)->weak_from_this().lock() : nullptr)
  , m_desc(desc)
  , m_heapMgr(heapMgr)
#ifdef _WIN32
  , m_nullHandle(nullHandle)
  , m_handle(handle)
#endif
  {
  }

#ifdef _WIN32
  namespace {
    void setSrvMinLodClamp(D3D12_SHADER_RESOURCE_VIEW_DESC& desc, float minLod) {
      switch (desc.ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE1D:        desc.Texture1D.ResourceMinLODClamp = minLod; break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:   desc.Texture1DArray.ResourceMinLODClamp = minLod; break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:        desc.Texture2D.ResourceMinLODClamp = minLod; break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:   desc.Texture2DArray.ResourceMinLODClamp = minLod; break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:        desc.Texture3D.ResourceMinLODClamp = minLod; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:      desc.TextureCube.ResourceMinLODClamp = minLod; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: desc.TextureCubeArray.ResourceMinLODClamp = minLod; break;
        default: break;
      }
    }
  }

  void D3D12BackendImageView::trackShaderResourceView(ID3D12Device* device, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    m_srvDevice = device;
    m_srvDesc = desc;
    auto* image = static_cast<D3D12BackendImage*>(m_image);
    image->m_srvViews.push_back(this);
  }

  void D3D12BackendImageView::setMinLodClamp(float minLod) {
    auto* image = static_cast<D3D12BackendImage*>(m_image);
    if (!m_srvDevice || !m_handle.ptr || !image->getResource())
      return;
    setSrvMinLodClamp(m_srvDesc, minLod);
    m_srvDevice->CreateShaderResourceView(image->getResource(), &m_srvDesc, m_handle);
    m_descriptorVersion = nextDescriptorVersion();
  }

  void D3D12BackendImageView::trackRetarget(ID3D12Device* device, const D3D12_RENDER_TARGET_VIEW_DESC* rtvDesc) {
    m_retargetDevice = device;
    if (rtvDesc) {
      m_rtvDesc = *rtvDesc;
      m_hasRtvDesc = true;
    }
    // Shader resource views are registered already.
    if (!m_srvDevice)
      static_cast<D3D12BackendImage*>(m_image)->m_srvViews.push_back(this);
  }

  void D3D12BackendImageView::rewriteForImage() {
    auto* image = static_cast<D3D12BackendImage*>(m_image);
    ID3D12Resource* resource = image->getResource();
    if (m_srvDevice && m_handle.ptr)
      m_srvDevice->CreateShaderResourceView(resource, &m_srvDesc, m_handle);
    if (m_retargetDevice && m_hasRtvDesc && m_rtvHandle.ptr)
      m_retargetDevice->CreateRenderTargetView(resource, &m_rtvDesc, m_rtvHandle);
    m_descriptorVersion = nextDescriptorVersion();
  }
#endif

  D3D12BackendImageView::~D3D12BackendImageView() {
#ifdef _WIN32
    if ((m_srvDevice || m_retargetDevice) && m_image) {
      auto* image = static_cast<D3D12BackendImage*>(m_image);
      std::lock_guard<std::mutex> lock(image->m_srvViewMutex);
      auto& views = image->m_srvViews;
      views.erase(std::remove(views.begin(), views.end(), this), views.end());
    }
    if (m_heapMgr) {
      if (m_handle.ptr)     m_heapMgr->freeCbvSrvUav(m_handle);
      if (m_nullHandle.ptr) m_heapMgr->freeCbvSrvUav(m_nullHandle);
      if (m_rtvHandle.ptr)  m_heapMgr->freeRtv(m_rtvHandle);
      if (m_dsvHandle.ptr)  m_heapMgr->freeDsv(m_dsvHandle);
      if (m_uavHandle.ptr)  m_heapMgr->freeCbvSrvUav(m_uavHandle);
      if (m_nullUavHandle.ptr) m_heapMgr->freeCbvSrvUav(m_nullUavHandle);
    }
#endif
  }

  // ============================================================
  // D3D12BackendSampler
  // ============================================================

  D3D12BackendSampler::D3D12BackendSampler(
    const SamplerDesc&       desc,
    D3D12DescriptorHeapManager* heapMgr
#ifdef _WIN32
    , D3D12_CPU_DESCRIPTOR_HANDLE handle
#endif
  )
  : m_desc(desc)
  , m_heapMgr(heapMgr)
#ifdef _WIN32
  , m_handle(handle)
#endif
  {
  }

  D3D12BackendSampler::~D3D12BackendSampler() {
#ifdef _WIN32
    if (m_heapMgr && m_handle.ptr)
      m_heapMgr->freeSampler(m_handle);
#endif
  }

  // ============================================================
  // D3D12BackendShader
  // ============================================================

  D3D12BackendShader::D3D12BackendShader(const ShaderDesc& desc)
  : m_stage(desc.stage) {
    if (desc.bytecode && desc.bytecodeSize > 0) {
      m_bytecode.resize(desc.bytecodeSize);
      std::memcpy(m_bytecode.data(), desc.bytecode, desc.bytecodeSize);
      std::vector<uint8_t> promoted;
      if (dxbc::promoteIndexableTemps(m_bytecode.data(), m_bytecode.size(), promoted))
        m_bytecode.swap(promoted);
      std::vector<uint8_t> withoutIcb;
      if (dxbc::icbToConstantBuffer(m_bytecode.data(), m_bytecode.size(), withoutIcb, m_icbSlot, m_icbData))
        m_bytecode.swap(withoutIcb);
      else
        m_icbSlot = UINT32_MAX;

      m_bytecodeHash = 1469598103934665603ull;
      for (uint8_t byte : m_bytecode) {
        m_bytecodeHash ^= static_cast<uint64_t>(byte);
        m_bytecodeHash = fnv64Multiply(m_bytecodeHash);
      }
      if (m_stage == ShaderStage::Pixel)
        m_pixelShaderTraits = dxbc::pixelShaderTraits(m_bytecode.data(), m_bytecode.size());
      m_srvSlotsKnown = dxbc::shaderResourceSlots(m_bytecode.data(), m_bytecode.size(), m_srvSlots);
    }
  }

  // ============================================================
  // D3D12BackendGraphicsPipeline
  // ============================================================

#ifdef _WIN32
  void D3D12BackendGraphicsPipeline::replacePSO(ComPtr<ID3D12PipelineState> pso) {
    std::lock_guard<std::mutex> lock(m_resolveMutex);
    if (!pso || m_replacement)
      return;
    m_replacement = std::move(pso);
    m_current.store(m_replacement.Get(), std::memory_order_release);
  }
#endif

  D3D12BackendGraphicsPipeline::D3D12BackendGraphicsPipeline(
#ifdef _WIN32
    ComPtr<ID3D12PipelineState>  pso,
    ID3D12RootSignature*         rootSig,
#endif
    PrimitiveTopology            topology,
    const PipelineResourceLimits& resourceLimits,
    uint32_t                     activeStageMask)
#ifdef _WIN32
  : m_pso(std::move(pso))
  , m_rootSignature(rootSig)
  , m_resourceLimits(resourceLimits)
  , m_activeStageMask(activeStageMask ? activeStageMask : 0x1Fu)
  , m_topology(topology) {
    m_current.store(m_pso.Get(), std::memory_order_relaxed);
#else
  : m_topology(topology) {
#endif
  }

  // ============================================================
  // D3D12BackendComputePipeline
  // ============================================================

  D3D12BackendComputePipeline::D3D12BackendComputePipeline(
#ifdef _WIN32
    ComPtr<ID3D12PipelineState>  pso,
    ID3D12RootSignature*         rootSig,
    const ShaderResourceLimits&  resourceLimits
#endif
  )
#ifdef _WIN32
  : m_pso(std::move(pso))
  , m_rootSignature(rootSig)
  , m_resourceLimits(resourceLimits) {
#else
  {
#endif
  }

  // ============================================================
  // D3D12BackendQuery
  // ============================================================

#ifdef _WIN32
  uint32_t D3D12QueryPool::resultSize(D3D12_QUERY_HEAP_TYPE type) {
    switch (type) {
      case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS: return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
      case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:       return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
      default:                                        return sizeof(uint64_t);
    }
  }

  bool D3D12QueryPool::allocate(ID3D12Device* device, D3D12_QUERY_HEAP_TYPE type, D3D12QuerySlot& slot) {
    if (!device || static_cast<uint32_t>(type) >= 4)
      return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    Pool& pool = m_pools[type];
    if (pool.freeSlots.empty()) {
      const uint32_t stride = resultSize(type);

      Page page;
      D3D12_QUERY_HEAP_DESC heapDesc = {};
      heapDesc.Type  = type;
      heapDesc.Count = kSlotsPerPage;
      if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&page.heap)))) {
        logMessage(LogLevel::Error, "D3D12QueryPool: CreateQueryHeap failed (type=%u)", static_cast<uint32_t>(type));
        return false;
      }

      D3D12_HEAP_PROPERTIES heapProps = {};
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC bufferDesc = {};
      bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
      bufferDesc.Width            = uint64_t(stride) * kSlotsPerPage;
      bufferDesc.Height           = 1;
      bufferDesc.DepthOrArraySize = 1;
      bufferDesc.MipLevels        = 1;
      bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
      bufferDesc.SampleDesc.Count = 1;
      bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&page.readback)))) {
        logMessage(LogLevel::Error, "D3D12QueryPool: readback buffer creation failed");
        return false;
      }

      void* mapped = nullptr;
      D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(bufferDesc.Width) };
      if (FAILED(page.readback->Map(0, &readRange, &mapped)) || !mapped)
        return false;
      page.mapped = static_cast<const uint8_t*>(mapped);

      for (uint32_t i = kSlotsPerPage; i > 0; i--) {
        D3D12QuerySlot freeSlot;
        freeSlot.heap           = page.heap.Get();
        freeSlot.index          = i - 1;
        freeSlot.readback       = page.readback.Get();
        freeSlot.readbackOffset = uint64_t(stride) * (i - 1);
        freeSlot.data           = page.mapped + freeSlot.readbackOffset;
        pool.freeSlots.push_back(freeSlot);
      }
      pool.pages.push_back(std::move(page));
    }

    slot = pool.freeSlots.back();
    pool.freeSlots.pop_back();
    return true;
  }

  void D3D12QueryPool::free(D3D12_QUERY_HEAP_TYPE type, const D3D12QuerySlot& slot) {
    if (static_cast<uint32_t>(type) >= 4 || !slot.heap)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pools[type].freeSlots.push_back(slot);
  }

  uint64_t D3D12BackendDevice::getTimestampFrequency() {
    uint64_t frequency = m_timestampFrequency.load(std::memory_order_relaxed);
    if (!frequency && m_commandQueue && SUCCEEDED(m_commandQueue->GetTimestampFrequency(&frequency)))
      m_timestampFrequency.store(frequency, std::memory_order_relaxed);
    return frequency;
  }
#endif

  D3D12BackendQuery::D3D12BackendQuery(D3D12BackendDevice* device, const QueryDesc& desc)
  : m_device(device)
  , m_desc(desc) {
  }

  D3D12BackendQuery::~D3D12BackendQuery() {
#ifdef _WIN32
    if (m_context)
      m_context->forgetQuery(this);
    if (m_device) {
      for (const auto& slot : m_slots)
        m_device->getQueryPool().free(heapType(), slot);
    }
#endif
  }

#ifdef _WIN32
  bool D3D12BackendQuery::hasGpuScope() const {
    return m_desc.type == QueryType::Occlusion
        || m_desc.type == QueryType::PipelineStatistics
        || m_desc.type == QueryType::StreamOutputStatistics;
  }

  D3D12_QUERY_HEAP_TYPE D3D12BackendQuery::heapType() const {
    switch (m_desc.type) {
      case QueryType::Timestamp:              return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      case QueryType::PipelineStatistics:     return D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
      case QueryType::StreamOutputStatistics: return D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
      default:                                return D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    }
  }

  D3D12_QUERY_TYPE D3D12BackendQuery::gpuQueryType(uint32_t slotInSegment) const {
    switch (m_desc.type) {
      case QueryType::Timestamp:          return D3D12_QUERY_TYPE_TIMESTAMP;
      case QueryType::PipelineStatistics: return D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
      case QueryType::StreamOutputStatistics: {
        const uint32_t stream = m_desc.stream == QueryDesc::kAllStreams ? slotInSegment : m_desc.stream;
        return static_cast<D3D12_QUERY_TYPE>(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + (stream & 3));
      }
      default:
        return m_desc.binary ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION;
    }
  }

  uint32_t D3D12BackendQuery::slotsPerSegment() const {
    return m_desc.type == QueryType::StreamOutputStatistics && m_desc.stream == QueryDesc::kAllStreams ? 4u : 1u;
  }

  bool D3D12BackendQuery::beginSegment(CommandListRecorder* list) {
    const uint32_t perSegment = slotsPerSegment();
    const uint32_t first = m_segments * perSegment;
    while (m_slots.size() < first + perSegment) {
      D3D12QuerySlot slot;
      if (!m_device->getQueryPool().allocate(m_device->getDevice(), heapType(), slot))
        return false;
      m_slots.push_back(slot);
    }

    for (uint32_t i = 0; i < perSegment; i++)
      list->BeginQuery(m_slots[first + i].heap, gpuQueryType(i), m_slots[first + i].index);
    m_segments++;
    return true;
  }

  void D3D12BackendQuery::endSegment(CommandListRecorder* list, std::vector<D3D12QueryResolve>& resolves) {
    if (!m_segments)
      return;
    const uint32_t perSegment = slotsPerSegment();
    const uint32_t first = (m_segments - 1) * perSegment;
    for (uint32_t i = 0; i < perSegment; i++) {
      const D3D12QuerySlot& slot = m_slots[first + i];
      list->EndQuery(slot.heap, gpuQueryType(i), slot.index);
      resolves.push_back({ slot.heap, gpuQueryType(i), slot.index, slot.readback, slot.readbackOffset });
    }
  }

  bool D3D12BackendQuery::writeTimestamp(CommandListRecorder* list, std::vector<D3D12QueryResolve>& resolves) {
    if (m_slots.empty()) {
      D3D12QuerySlot slot;
      if (!m_device->getQueryPool().allocate(m_device->getDevice(), heapType(), slot))
        return false;
      m_slots.push_back(slot);
    }
    const D3D12QuerySlot& slot = m_slots[0];
    list->EndQuery(slot.heap, D3D12_QUERY_TYPE_TIMESTAMP, slot.index);
    resolves.push_back({ slot.heap, D3D12_QUERY_TYPE_TIMESTAMP, slot.index, slot.readback, slot.readbackOffset });
    m_segments = 1;
    return true;
  }

  ID3D12Resource* D3D12BackendQuery::predicateBuffer() {
    if (m_predicateBuffer)
      return m_predicateBuffer.Get();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = sizeof(uint64_t);
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->getDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_predicateBuffer)))) {
      logMessage(LogLevel::Error, "D3D12BackendQuery: predicate buffer creation failed");
      return nullptr;
    }
    m_predicateState = D3D12_RESOURCE_STATE_COPY_DEST;
    return m_predicateBuffer.Get();
  }
#endif

  QueryStatus D3D12BackendQuery::getData(QueryData& data) {
#ifdef _WIN32
    if (queuedOps.load(std::memory_order_acquire))
      return QueryStatus::NotSubmitted;
    if (m_active.load(std::memory_order_acquire) || !m_issued.load(std::memory_order_acquire))
      return QueryStatus::Invalid;

    const uint64_t serial = m_issueSerial.load(std::memory_order_acquire);
    if (serial == m_cachedSerial) {
      data = m_cachedData;
      return QueryStatus::Ready;
    }

    const uint64_t target = m_fenceValue.load(std::memory_order_acquire);
    if (!m_submittedFence || m_submittedFence->load(std::memory_order_acquire) < target)
      return QueryStatus::NotSubmitted;
    if (!m_fence || m_fence->GetCompletedValue() < target)
      return QueryStatus::Pending;

    data = QueryData();
    const uint32_t perSegment = slotsPerSegment();
    const uint32_t slotCount = (std::min)(uint32_t(m_slots.size()), m_issuedSegments * perSegment);
    switch (m_desc.type) {
      case QueryType::Occlusion:
        for (uint32_t i = 0; i < slotCount; i++) {
          uint64_t samples;
          std::memcpy(&samples, m_slots[i].data, sizeof(samples));
          data.samples += samples;
        }
        break;

      case QueryType::Timestamp:
        if (slotCount)
          std::memcpy(&data.timestamp, m_slots[0].data, sizeof(data.timestamp));
        data.frequency = m_device->getTimestampFrequency();
        break;

      case QueryType::TimestampDisjoint:
        data.frequency = m_device->getTimestampFrequency();
        data.disjoint = false;
        break;

      case QueryType::PipelineStatistics:
        for (uint32_t i = 0; i < slotCount; i++) {
          uint64_t values[11];
          std::memcpy(values, m_slots[i].data, sizeof(values));
          for (uint32_t j = 0; j < 11; j++)
            data.pipelineStatistics[j] += values[j];
        }
        break;

      case QueryType::StreamOutputStatistics:
        for (uint32_t i = 0; i < slotCount; i++) {
          D3D12_QUERY_DATA_SO_STATISTICS stats;
          std::memcpy(&stats, m_slots[i].data, sizeof(stats));
          data.primitivesWritten += stats.NumPrimitivesWritten;
          data.primitivesStorageNeeded += stats.PrimitivesStorageNeeded;
        }
        break;

      case QueryType::Event:
        break;
    }
    m_cachedData = data;
    m_cachedSerial = serial;
    return QueryStatus::Ready;
#else
    (void)data;
    return QueryStatus::Ready;
#endif
  }

  // ============================================================
  // D3D12DescriptorHeapManager
  // ============================================================

#ifdef _WIN32
  void D3D12DescriptorHeapManager::init(ID3D12Device* device) {
    m_device = device;
    initPool(m_rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096);
    initPool(m_dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024);
    initPool(m_cbvSrvUav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536);
    initPool(m_sampler, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);
    logMessage(LogLevel::Info, "DescriptorHeapManager: pages of RTV=%u DSV=%u CBV/SRV/UAV=%u Sampler=%u",
      m_rtv.pageSize, m_dsv.pageSize, m_cbvSrvUav.pageSize, m_sampler.pageSize);
  }

  void D3D12DescriptorHeapManager::initPool(Pool& pool, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t pageSize) {
    pool.type = type;
    pool.pageSize = pageSize;
    pool.descriptorSize = m_device->GetDescriptorHandleIncrementSize(type);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::allocate(Pool& pool) {
    std::lock_guard<std::mutex> lock(m_mutex);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
    if (!pool.freeList.empty()) {
      handle.ptr = pool.freeList.back();
      pool.freeList.pop_back();
      return handle;
    }
    if (!pool.remaining) {
      D3D12_DESCRIPTOR_HEAP_DESC desc = {};
      desc.NumDescriptors = pool.pageSize;
      desc.Type           = pool.type;
      desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      ComPtr<ID3D12DescriptorHeap> page;
      if (!m_device || FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&page)))) {
        logMessage(LogLevel::Error, "descriptor heap page creation failed (type %d)", static_cast<int>(pool.type));
        return handle;
      }
      pool.next = page->GetCPUDescriptorHandleForHeapStart().ptr;
      pool.remaining = pool.pageSize;
      if (!pool.pages.empty())
        logMessage(LogLevel::Info, "descriptor heap type %d grows to %zu pages",
          static_cast<int>(pool.type), pool.pages.size() + 1);
      pool.pages.push_back(std::move(page));
    }
    handle.ptr = pool.next;
    pool.next += pool.descriptorSize;
    pool.remaining--;
    return handle;
  }

  void D3D12DescriptorHeapManager::free(Pool& pool, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!handle.ptr)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    pool.freeList.push_back(handle.ptr);
  }
#endif

  // ============================================================
  // D3D12BackendDevice
  // ============================================================

  D3D12BackendDevice::D3D12BackendDevice(const LUID* adapterLuid) {
#ifdef _WIN32
    if (adapterLuid) {
      m_hasAdapterLuid = true;
      m_adapterLuid = *adapterLuid;
    }
    if (const char* choice = std::getenv("DXUP_ADAPTER"); choice && *choice) {
      m_adapterChoice = choice;
      for (char& c : m_adapterChoice)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      m_adapterChoice = UserOverrides::get().adapter;
    }
    m_gpuSyncValidationEnabled = getGpuSyncValidationEnabled();
    if (markerFileEnabled("dxup_memory_stats.txt"))
      memstats::enabledFlag().store(true);
    // Deferred command lists need it too; the CS thread defers it.
    m_retireQueue = std::make_shared<BackendRetireQueue>();

    logMessage(LogLevel::Info, "D3D12BackendDevice: Initializing...");
    if (m_gpuSyncValidationEnabled)
      logMessage(LogLevel::Info, "D3D12BackendDevice: GPU sync validation enabled via DXUP_GPU_SYNC_VALIDATION");

    logMessage(LogLevel::Debug, "D3D12BackendDevice: Creating D3D12 device...");
    createDevice();
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    if (m_adapter && SUCCEEDED(m_adapter->GetDesc1(&adapterDesc)))
      m_vendorId = adapterDesc.VendorId;
    {
      D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
      if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
        const int32_t lazy = UserOverrides::get().lazyTargets;
        // Reads of tiles without memory must give 0: tier 2.
        m_lazyTargets = options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2
          && (lazy == 1 || (lazy == -1 && m_vendorId == 0x10DE));
        m_resourceHeapTier2 = options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
      }
    }
    logMessage(LogLevel::Info, "HEALTH after createDevice: 0x%08X", m_device->GetDeviceRemovedReason());

    logMessage(LogLevel::Debug, "D3D12BackendDevice: Creating command queue...");
    createCommandQueue();
    logMessage(LogLevel::Info, "HEALTH after createCommandQueue: 0x%08X", m_device->GetDeviceRemovedReason());

    logMessage(LogLevel::Debug, "D3D12BackendDevice: Creating fence...");
    createFence();
    logMessage(LogLevel::Info, "HEALTH after createFence: 0x%08X", m_device->GetDeviceRemovedReason());

    logMessage(LogLevel::Debug, "D3D12BackendDevice: Initializing descriptor heaps...");
    m_descriptorHeapManager.init(m_device.Get());
    logMessage(LogLevel::Info, "HEALTH after descriptor heaps: 0x%08X", m_device->GetDeviceRemovedReason());

    logMessage(LogLevel::Debug, "D3D12BackendDevice: Creating root signature...");
    createCommonRootSignature();
    logMessage(LogLevel::Info, "HEALTH after root signature: 0x%08X", m_device->GetDeviceRemovedReason());

    logMessage(LogLevel::Debug, "D3D12BackendDevice: Creating command signatures...");
    createCommandSignatures();
    logMessage(LogLevel::Info, "HEALTH after command signatures: 0x%08X", m_device->GetDeviceRemovedReason());

    if (m_gpuSyncValidationEnabled)
      flushAndCheckGpu("after-device-init");

    initPsoDiskCache();
    m_asyncDraws = UserOverrides::get().asyncShaders;
    if (m_asyncDraws || UserOverrides::get().shaderPrecompile)
      startCompileThreads();

    logMessage(LogLevel::Info, "D3D12BackendDevice: Initialization complete");
#endif
  }

  void D3D12BackendDevice::drainDebugMessages() {
#ifdef _WIN32
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (!m_debugLayerEnabled || !m_device || FAILED(m_device.As(&infoQueue)))
      return;
    static std::mutex s_mutex;
    static uint32_t s_logged = 0;
    std::lock_guard<std::mutex> lock(s_mutex);
    const UINT64 count = infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count && s_logged < 256; i++) {
      SIZE_T length = 0;
      if (FAILED(infoQueue->GetMessage(i, nullptr, &length)) || !length)
        continue;
      std::vector<char> storage(length);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      if (FAILED(infoQueue->GetMessage(i, message, &length)) || message->Severity > D3D12_MESSAGE_SEVERITY_ERROR)
        continue;
      s_logged++;
      logMessage(LogLevel::Error, "D3D12 debug layer: %s", message->pDescription);
    }
    infoQueue->ClearStoredMessages();
#endif
  }

  D3D12BackendDevice::~D3D12BackendDevice() {
#ifdef _WIN32
    // What is queued runs; from here on jobs run where they are handed in.
    m_submitQueue.stop();
    stopCompileThreads();
    // Its destructor waits for its uploads.
    m_initDataContext = nullptr;
    waitForGpu();
    // With the debug layer, report every error it recorded.
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (m_debugLayerEnabled && m_device && SUCCEEDED(m_device.As(&infoQueue))) {
      const UINT64 count = infoQueue->GetNumStoredMessages();
      UINT64 errors = 0;
      for (UINT64 i = 0; i < count; i++) {
        SIZE_T length = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &length)) || !length)
          continue;
        std::vector<char> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(infoQueue->GetMessage(i, message, &length)))
          continue;
        if (message->Severity > D3D12_MESSAGE_SEVERITY_ERROR)
          continue;
        if (errors++ < 64)
          logMessage(LogLevel::Error, "D3D12 debug layer: %s", message->pDescription);
      }
      logMessage(LogLevel::Warn, "D3D12 debug layer: %llu errors in %llu stored messages",
        static_cast<unsigned long long>(errors), static_cast<unsigned long long>(count));
    }
    stopPsoDiskWriter();
    m_textureAllocator->shutdown();
    m_bufferAllocator->shutdown();
    m_dynamicBufferAllocator->shutdown();
    logPsoStats("device destroyed");
#endif
  }

#ifdef _WIN32
  void D3D12BackendDevice::createDevice() {
    // Diagnostics for GPU hangs and page faults: the operations each command
    // list reached, reported when the device is removed.
    if (markerFileEnabled("dxup_dred.txt")) {
      ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        // Marker strings in the breadcrumbs (the shader of a dispatch).
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred1;
        if (SUCCEEDED(dred.As(&dred1)))
          dred1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        logMessage(LogLevel::Warn, "DRED enabled (dxup_dred.txt found)");
      }
    }

    // Try to enable debug layer if dxup_debug.txt exists next to our DLL
    m_debugLayerEnabled = false;
    {
      if (markerFileEnabled("dxup_debug.txt")) {
        ComPtr<ID3D12Debug> debugController;
        HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
        if (SUCCEEDED(hr)) {
          debugController->EnableDebugLayer();
          m_debugLayerEnabled = true;
          logMessage(LogLevel::Info, "D3D12 debug layer ENABLED (dxup_debug.txt found)");

          if (markerFileEnabled("dxup_gpu_validation.txt")) {
            ComPtr<ID3D12Debug1> debugController1;
            if (SUCCEEDED(debugController.As(&debugController1))) {
              debugController1->SetEnableGPUBasedValidation(TRUE);
              debugController1->SetEnableSynchronizedCommandQueueValidation(TRUE);
              logMessage(LogLevel::Warn, "D3D12 GPU-based validation ENABLED (dxup_gpu_validation.txt found)");
            } else {
              logMessage(LogLevel::Warn, "D3D12 GPU-based validation requested but ID3D12Debug1 is unavailable");
            }
          }
        } else {
          logMessage(LogLevel::Warn, "D3D12GetDebugInterface failed (hr=0x%08x) - debug layer not available", hr);
        }
      }
    }

    // Load system DXGI from System32 to avoid recursion into our own dxgi.dll
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string dxgiPath = std::string(sysDir) + "\\dxgi.dll";
    logMessage(LogLevel::Debug, "Loading system DXGI from: %s", dxgiPath.c_str());

    HMODULE sysDxgi = LoadLibraryA(dxgiPath.c_str());
    if (!sysDxgi) {
      logMessage(LogLevel::Error, "Failed to load system dxgi.dll from %s", dxgiPath.c_str());
      throw std::runtime_error("Failed to load system dxgi.dll");
    }

    using PFN_CreateDXGIFactory1 = HRESULT (WINAPI*)(REFIID, void**);
    auto pfnCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
      GetProcAddress(sysDxgi, "CreateDXGIFactory1"));
    if (!pfnCreateDXGIFactory1) {
      logMessage(LogLevel::Error, "CreateDXGIFactory1 not found in system dxgi.dll");
      throw std::runtime_error("CreateDXGIFactory1 not found in system dxgi.dll");
    }

    HRESULT hr = pfnCreateDXGIFactory1(IID_PPV_ARGS(&m_dxgiFactory));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "CreateDXGIFactory1 failed (hr=0x%08x)", hr);
      throw std::runtime_error("Failed to create DXGI factory");
    }
    logMessage(LogLevel::Debug, "DXGI factory created (from system dxgi.dll)");

    // Ask for the highest D3D12 feature level first; the runtime treats this
    // as the minimum required level for device creation.
    static const D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
    };

    // Tests: as on a GPU without a Direct3D 12 driver.
    if (const char* fail = std::getenv("DXUP_FAIL_D3D12"); fail && *fail == '1') {
      logMessage(LogLevel::Warn, "DXUP_FAIL_D3D12: no Direct3D 12 device");
      throw std::runtime_error("DXUP_FAIL_D3D12");
    }

    // Find hardware adapter
    auto findAdapter = [&] {
      for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, m_adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        m_adapter->GetDesc1(&desc);

        // Log adapter info (convert wide string to narrow)
        char adapterName[256];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
        logMessage(LogLevel::Info, "Adapter[%u]: %s (VRAM: %llu MB, Flags: 0x%x)",
          i, adapterName, static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)), desc.Flags);

        if (!acceptsAdapter(desc)) {
          logMessage(LogLevel::Debug, "  Skipping adapter");
          continue;
        }

        // Try each feature level
        for (auto fl : featureLevels) {
          hr = D3D12CreateDevice(m_adapter.Get(), fl, IID_PPV_ARGS(&m_device));
          if (SUCCEEDED(hr)) {
            logMessage(m_adapterChoice.empty() ? LogLevel::Info : LogLevel::Warn,
              "D3D12 device created on adapter: %s (FeatureLevel=0x%x)", adapterName, fl);
            // Immediate device health check
            HRESULT devHr = m_device->GetDeviceRemovedReason();
            logMessage(LogLevel::Info, "D3D12 device health right after creation: 0x%08X", devHr);
            break;
          } else {
            logMessage(LogLevel::Debug, "  D3D12CreateDevice failed (FeatureLevel=0x%x, hr=0x%08x)", fl, hr);
          }
        }

        if (m_device) break;
      }
    };
    findAdapter();
    if (!m_device && !m_adapterChoice.empty()) {
      logMessage(LogLevel::Warn, "dxgi.adapter: no adapter \"%s\" with Direct3D 12, using the usual one", m_adapterChoice.c_str());
      m_adapterChoice.clear();
      findAdapter();
    }

    // If debug layer was enabled and device creation failed, retry without it
    if (!m_device && m_debugLayerEnabled) {
      logMessage(LogLevel::Warn, "Device creation failed with debug layer - retrying without it...");
      m_debugLayerEnabled = false;

      // Need a fresh factory since the debug layer state is process-wide;
      // recreate factory to pick up the non-debug path
      m_dxgiFactory.Reset();
      m_adapter.Reset();
      hr = pfnCreateDXGIFactory1(IID_PPV_ARGS(&m_dxgiFactory));
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "Failed to recreate DXGI factory (hr=0x%08x)", hr);
        throw std::runtime_error("Failed to recreate DXGI factory");
      }

      for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, m_adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        m_adapter->GetDesc1(&desc);
        if (!acceptsAdapter(desc))
          continue;

        for (auto fl : featureLevels) {
          hr = D3D12CreateDevice(m_adapter.Get(), fl, IID_PPV_ARGS(&m_device));
          if (SUCCEEDED(hr)) {
            char adapterName[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
            logMessage(LogLevel::Info, "D3D12 device created (no debug layer) on: %s (FeatureLevel=0x%x)", adapterName, fl);
            break;
          }
        }
        if (m_device) break;
      }
    }

    if (!m_device) {
      logMessage(LogLevel::Error, "No suitable D3D12 adapter found");
      throw std::runtime_error("Failed to create D3D12 device");
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS19 options19 = {};
    m_anisotropicPointMip = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &options19, sizeof(options19)))
      && options19.AnisoFilterWithPointMipSupported;

    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
    // Experiment only: the CPU writes faster, but the GPU snoops every read
    // (Kena: GPU 7.9 -> 21.5 ms per frame).
    if (markerFileEnabled("dxup_writeback_upload.txt")) {
      m_uploadSuballocator.useWriteBack();
      logMessage(LogLevel::Info, "D3D12: dynamic buffers in cached system memory");
    } else if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16)))
     && options16.GPUUploadHeapSupported && !markerFileEnabled("dxup_disable_gpu_upload_heap.txt")) {
      m_uploadSuballocator.useGpuUploadHeap();
      logMessage(LogLevel::Info, "D3D12: dynamic buffers in GPU upload heaps (resizable BAR)");
    }

    // Set up info queue to capture debug messages if debug layer is active
    if (m_debugLayerEnabled) {
      ComPtr<ID3D12InfoQueue> infoQueue;
      if (SUCCEEDED(m_device.As(&infoQueue))) {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        logMessage(LogLevel::Info, "D3D12 InfoQueue configured (break-on-error disabled)");
      }
      // Report problems as they happen, a few of each kind.
      ComPtr<ID3D12InfoQueue1> infoQueue1;
      if (SUCCEEDED(m_device.As(&infoQueue1))) {
        auto callback = [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                           D3D12_MESSAGE_ID id, LPCSTR description, void*) {
          if (severity > D3D12_MESSAGE_SEVERITY_WARNING)
            return;
          static std::mutex s_mutex;
          static std::unordered_map<int, uint32_t> s_counts;
          {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (++s_counts[static_cast<int>(id)] > 5)
              return;
          }
          logMessage(LogLevel::Error, "D3D12 debug severity=%d id=%d: %s",
            static_cast<int>(severity), static_cast<int>(id), description ? description : "");
        };
        DWORD cookie = 0;
        infoQueue1->RegisterMessageCallback(callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
      }
    }
  }

  bool D3D12BackendDevice::acceptsAdapter(const DXGI_ADAPTER_DESC1& desc) const {
    // The one dxup.conf names wins over the application's choice.
    if (!m_adapterChoice.empty()) {
      if (m_adapterChoice == "warp")
        return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      char name[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
      for (char* c = name; *c; c++)
        *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
      return std::strstr(name, m_adapterChoice.c_str()) != nullptr;
    }
    // An adapter the application chose explicitly may also be WARP.
    if (m_hasAdapterLuid)
      return desc.AdapterLuid.LowPart == m_adapterLuid.LowPart
          && desc.AdapterLuid.HighPart == m_adapterLuid.HighPart;
    return !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
  }

  ComPtr<ID3D12Resource> D3D12BackendDevice::createLazyTarget(const D3D12_RESOURCE_DESC& desc,
      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clearValue, uint32_t& tiles) {
    tiles = 0;
    // Single-sampled 2D color targets of one subresource, 2 MB and more.
    if (!m_lazyTargets || m_gpuSyncValidationEnabled || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
     || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
     || (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
     || desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.DepthOrArraySize != 1)
      return nullptr;
    const D3D12_RESOURCE_ALLOCATION_INFO info = m_device->GetResourceAllocationInfo(0, 1, &desc);
    if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes < (2ull << 20))
      return nullptr;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { desc.Format };
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
     || !(support.Support2 & D3D12_FORMAT_SUPPORT2_TILED))
      return nullptr;
    D3D12_RESOURCE_DESC reserved = desc;
    reserved.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    reserved.Alignment = 0;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(m_device->CreateReservedResource(&reserved, state, clearValue, IID_PPV_ARGS(&resource))))
      return nullptr;
    UINT count = 0, subresources = 1;
    D3D12_PACKED_MIP_INFO packed = {};
    D3D12_TILE_SHAPE shape = {};
    D3D12_SUBRESOURCE_TILING tiling = {};
    m_device->GetResourceTiling(resource.Get(), &count, &packed, &shape, &subresources, 0, &tiling);
    if (!count || packed.NumPackedMips)
      return nullptr;
    tiles = count;
    return resource;
  }

  bool D3D12BackendDevice::commitLazyImage(D3D12BackendImage* image) {
    // {3b0d6e52-8a41-4c7f-9f2e-6d1a5c8b7e03}
    static constexpr GUID kLazyHeapGuid = { 0x3b0d6e52, 0x8a41, 0x4c7f, { 0x9f, 0x2e, 0x6d, 0x1a, 0x5c, 0x8b, 0x7e, 0x03 } };
    // One at a time: a second context writing the same target waits here
    // until its tiles are mapped.
    std::lock_guard<std::mutex> lock(m_lazyCommitMutex);
    const uint32_t tiles = image->m_lazyTiles.load(std::memory_order_acquire);
    if (!tiles)
      return false;
    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = uint64_t(tiles) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = m_resourceHeapTier2 ? D3D12_HEAP_FLAG_NONE : D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    HRESULT hr = m_device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "commitLazyImage: CreateHeap of %u tiles failed (hr=0x%08x)", tiles, static_cast<unsigned>(hr));
      return false;
    }
    // In queue order now, ahead of every command list still recording: a
    // list that already read the target would read the mapped memory
    // before the caller's clear, so callers commit before any list uses it.
    D3D12_TILED_RESOURCE_COORDINATE start = {};
    D3D12_TILE_REGION_SIZE region = {};
    region.NumTiles = tiles;
    const D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT heapStart = 0, rangeTiles = tiles;
    ComPtr<ID3D12Resource> resource = image->getResource();
    m_submitQueue.enqueue([resource, heap, start, region, flags, heapStart, rangeTiles](ID3D12CommandQueue* queue) {
      queue->UpdateTileMappings(resource.Get(), 1, &start, &region, heap.Get(), 1, &flags, &heapStart,
        &rangeTiles, D3D12_TILE_MAPPING_FLAG_NONE);
    });
    // The heap lives as long as the resource.
    image->getResource()->SetPrivateDataInterface(kLazyHeapGuid, heap.Get());
    memstats::track(heap.Get(), memstats::TargetTexture);
    image->m_lazyTiles.store(0, std::memory_order_release);
    return true;
  }

  void D3D12BackendDevice::createCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr))
      throw std::runtime_error("Failed to create command queue");
    m_submitQueue.init(m_device.Get(), m_commandQueue.Get());
  }

  void D3D12BackendDevice::createFence() {
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr))
      throw std::runtime_error("Failed to create fence");
  }

  void D3D12BackendDevice::createNullDescriptors() {
    for (uint8_t dimension = 0; dimension < 13; dimension++) {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
      uav.Format = DXGI_FORMAT_R32_UINT;
      switch (dimension) {
        case 1:
          srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          srv.Format = DXGI_FORMAT_R32_UINT;
          uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
          break;
        case 2:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
          srv.Texture1D.MipLevels = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
          break;
        case 4:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
          break;
        case 5:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
          srv.Texture3D.MipLevels = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
          uav.Texture3D.WSize = 1;
          break;
        case 6:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
          srv.TextureCube.MipLevels = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
          uav.Texture2DArray.ArraySize = 1;
          break;
        case 7:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
          srv.Texture1DArray.MipLevels = 1;
          srv.Texture1DArray.ArraySize = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
          uav.Texture1DArray.ArraySize = 1;
          break;
        case 8:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          srv.Texture2DArray.MipLevels = 1;
          srv.Texture2DArray.ArraySize = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
          uav.Texture2DArray.ArraySize = 1;
          break;
        case 9:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
          srv.Texture2DMSArray.ArraySize = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
          uav.Texture2DArray.ArraySize = 1;
          break;
        case 10:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
          srv.TextureCubeArray.MipLevels = 1;
          srv.TextureCubeArray.NumCubes = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
          uav.Texture2DArray.ArraySize = 1;
          break;
        case 11:
          srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          srv.Format = DXGI_FORMAT_R32_TYPELESS;
          srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
          uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
          uav.Format = DXGI_FORMAT_R32_TYPELESS;
          uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
          break;
        case 12:
          srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          srv.Format = DXGI_FORMAT_UNKNOWN;
          srv.Buffer.StructureByteStride = 4;
          uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
          uav.Format = DXGI_FORMAT_UNKNOWN;
          uav.Buffer.StructureByteStride = 4;
          break;
        default:
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          srv.Texture2D.MipLevels = 1;
          uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
          break;
      }
      m_nullSrvs[dimension] = m_descriptorHeapManager.allocateCbvSrvUav();
      m_device->CreateShaderResourceView(nullptr, &srv, m_nullSrvs[dimension]);
      m_nullUavs[dimension] = m_descriptorHeapManager.allocateCbvSrvUav();
      m_device->CreateUnorderedAccessView(nullptr, nullptr, &uav, m_nullUavs[dimension]);
    }
    // D3D11's default sampler state.
    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = sampler.BorderColor[3] = 1.0f;
    sampler.MinLOD = -D3D12_FLOAT32_MAX;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    m_defaultSampler = m_descriptorHeapManager.allocateSampler();
    m_device->CreateSampler(&sampler, m_defaultSampler);
  }

  void D3D12BackendDevice::createCommonRootSignature() {
    {
      D3D12_HEAP_PROPERTIES heapProps = {};
      heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width            = 65536;
      desc.Height           = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels        = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (SUCCEEDED(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_zeroConstantBuffer)))) {
        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        if (SUCCEEDED(m_zeroConstantBuffer->Map(0, &readRange, &mapped)) && mapped) {
          std::memset(mapped, 0, 65536);
          m_zeroConstantBuffer->Unmap(0, nullptr);
        }
        m_zeroConstantBufferAddress = m_zeroConstantBuffer->GetGPUVirtualAddress();
      }
    }

    createNullDescriptors();

    // =============================================
    // Graphics root signature (20 params, space 0)
    // Layout: 0-4 CBV, 5-9 SRV, 10-14 Sampler, 15-19 UAV
    // Per-stage visibility, all register space 0 (DXBC always uses space 0)
    // =============================================
    {
      std::vector<D3D12_ROOT_PARAMETER> rootParams(kGraphicsRootCbvBase + kGraphicsRootCbvCount);

      D3D12_SHADER_VISIBILITY shaderVis[] = {
        D3D12_SHADER_VISIBILITY_VERTEX,
        D3D12_SHADER_VISIBILITY_HULL,
        D3D12_SHADER_VISIBILITY_DOMAIN,
        D3D12_SHADER_VISIBILITY_GEOMETRY,
        D3D12_SHADER_VISIBILITY_PIXEL,
      };

      // CBV descriptor tables per stage (slots 0-4): b0-b13, space 0
      std::vector<D3D12_DESCRIPTOR_RANGE> cbvRanges(5);
      for (int i = 0; i < 5; i++) {
        cbvRanges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        cbvRanges[i].NumDescriptors     = 14 - kRootCbvSlots[i];  // D3D11 supports b0-b13
        cbvRanges[i].BaseShaderRegister = kRootCbvSlots[i];
        cbvRanges[i].RegisterSpace      = 0;
        cbvRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        rootParams[i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[i].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[i].DescriptorTable.pDescriptorRanges   = &cbvRanges[i];
        rootParams[i].ShaderVisibility                    = shaderVis[i];
      }

      // Descriptor ranges for SRV/Sampler/UAV tables
      std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges(5);
      std::vector<D3D12_DESCRIPTOR_RANGE> samplerRanges(5);
      std::vector<D3D12_DESCRIPTOR_RANGE> uavRanges(5);

      for (int i = 0; i < 5; i++) {
        // SRV tables (slots 5-9)
        srvRanges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors     = 128;
        srvRanges[i].BaseShaderRegister = 0;
        srvRanges[i].RegisterSpace      = 0;
        srvRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        rootParams[5 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[5 + i].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[5 + i].DescriptorTable.pDescriptorRanges   = &srvRanges[i];
        rootParams[5 + i].ShaderVisibility                    = shaderVis[i];

        // Sampler tables (slots 10-14)
        samplerRanges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRanges[i].NumDescriptors     = 16;
        samplerRanges[i].BaseShaderRegister = 0;
        samplerRanges[i].RegisterSpace      = 0;
        samplerRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        rootParams[10 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[10 + i].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[10 + i].DescriptorTable.pDescriptorRanges   = &samplerRanges[i];
        rootParams[10 + i].ShaderVisibility                    = shaderVis[i];

        // UAV tables (slots 15-19)
        uavRanges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[i].NumDescriptors     = 64;
        uavRanges[i].BaseShaderRegister = 0;
        uavRanges[i].RegisterSpace      = 0;
        uavRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        rootParams[15 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[15 + i].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[15 + i].DescriptorTable.pDescriptorRanges   = &uavRanges[i];
        rootParams[15 + i].ShaderVisibility                    = shaderVis[i];

        for (uint32_t slot = 0; slot < kRootCbvSlots[i]; slot++) {
          auto& root = rootParams[graphicsRootCbvIndex(i, slot)];
          root.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
          root.Descriptor.ShaderRegister = slot;
          root.Descriptor.RegisterSpace  = 0;
          root.ShaderVisibility          = shaderVis[i];
        }
      }

      D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
      rootSigDesc.NumParameters     = static_cast<UINT>(rootParams.size());
      rootSigDesc.pParameters       = rootParams.data();
      rootSigDesc.NumStaticSamplers = 0;
      rootSigDesc.pStaticSamplers   = nullptr;
      rootSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                                    | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;

      ComPtr<ID3DBlob> serializedRootSig;
      ComPtr<ID3DBlob> errorBlob;
      HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                            serializedRootSig.ReleaseAndGetAddressOf(),
                            errorBlob.ReleaseAndGetAddressOf());
      if (FAILED(hr)) {
        if (errorBlob)
          logMessage(LogLevel::Error, "Graphics root sig serialize error: %s", (const char*)errorBlob->GetBufferPointer());
        throw std::runtime_error("Failed to serialize graphics root signature");
      }

      hr = m_device->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&m_graphicsRootSignature));
      if (FAILED(hr))
        throw std::runtime_error("Failed to create graphics root signature");
    }

    // =============================================
    // Compute root signature (4 params, space 0)
    // Layout: 0 CBV, 1 SRV, 2 Sampler, 3 UAV
    // ALL visibility, register space 0
    // =============================================
    {
      constexpr uint32_t kComputeRootCbvs = kRootCbvSlots[5];
      D3D12_ROOT_PARAMETER rootParams[kComputeRootCbvBase + kComputeRootCbvs] = {};
      for (uint32_t slot = 0; slot < kComputeRootCbvs; slot++) {
        auto& root = rootParams[kComputeRootCbvBase + slot];
        root.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root.Descriptor.ShaderRegister = slot;
        root.Descriptor.RegisterSpace  = 0;
        root.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
      }

      D3D12_DESCRIPTOR_RANGE cbvRange = {};
      cbvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
      cbvRange.NumDescriptors     = 14 - kComputeRootCbvs;  // b0-b13
      cbvRange.BaseShaderRegister = kComputeRootCbvs;
      cbvRange.RegisterSpace      = 0;
      cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

      rootParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
      rootParams[0].DescriptorTable.pDescriptorRanges   = &cbvRange;
      rootParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

      D3D12_DESCRIPTOR_RANGE srvRange = {};
      srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      srvRange.NumDescriptors     = 128;
      srvRange.BaseShaderRegister = 0;
      srvRange.RegisterSpace      = 0;
      srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

      rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
      rootParams[1].DescriptorTable.pDescriptorRanges   = &srvRange;
      rootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

      D3D12_DESCRIPTOR_RANGE samplerRange = {};
      samplerRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      samplerRange.NumDescriptors     = 16;
      samplerRange.BaseShaderRegister = 0;
      samplerRange.RegisterSpace      = 0;
      samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

      rootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
      rootParams[2].DescriptorTable.pDescriptorRanges   = &samplerRange;
      rootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

      D3D12_DESCRIPTOR_RANGE uavRange = {};
      uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      uavRange.NumDescriptors     = 64;
      uavRange.BaseShaderRegister = 0;
      uavRange.RegisterSpace      = 0;
      uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

      rootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
      rootParams[3].DescriptorTable.pDescriptorRanges   = &uavRange;
      rootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

      D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
      rootSigDesc.NumParameters     = kComputeRootCbvBase + kComputeRootCbvs;
      rootSigDesc.pParameters       = rootParams;
      rootSigDesc.NumStaticSamplers = 0;
      rootSigDesc.pStaticSamplers   = nullptr;
      rootSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

      ComPtr<ID3DBlob> serializedRootSig;
      ComPtr<ID3DBlob> errorBlob;
      HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                            serializedRootSig.ReleaseAndGetAddressOf(),
                            errorBlob.ReleaseAndGetAddressOf());
      if (FAILED(hr))
        throw std::runtime_error("Failed to serialize compute root signature");

      hr = m_device->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&m_computeRootSignature));
      if (FAILED(hr))
        throw std::runtime_error("Failed to create compute root signature");
    }
  }

  void D3D12BackendDevice::createCommandSignatures() {
    // Draw indirect signature (matches D3D12_DRAW_ARGUMENTS)
    D3D12_INDIRECT_ARGUMENT_DESC drawArg = {};
    drawArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC drawSigDesc = {};
    drawSigDesc.ByteStride       = sizeof(D3D12_DRAW_ARGUMENTS);
    drawSigDesc.NumArgumentDescs = 1;
    drawSigDesc.pArgumentDescs   = &drawArg;
    m_device->CreateCommandSignature(&drawSigDesc, nullptr, IID_PPV_ARGS(&m_drawIndirectSignature));

    // Draw indexed indirect signature (matches D3D12_DRAW_INDEXED_ARGUMENTS)
    D3D12_INDIRECT_ARGUMENT_DESC drawIdxArg = {};
    drawIdxArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC drawIdxSigDesc = {};
    drawIdxSigDesc.ByteStride       = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    drawIdxSigDesc.NumArgumentDescs = 1;
    drawIdxSigDesc.pArgumentDescs   = &drawIdxArg;
    m_device->CreateCommandSignature(&drawIdxSigDesc, nullptr, IID_PPV_ARGS(&m_drawIndexedIndirectSignature));

    // Dispatch indirect signature (matches D3D12_DISPATCH_ARGUMENTS)
    D3D12_INDIRECT_ARGUMENT_DESC dispatchArg = {};
    dispatchArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC dispatchSigDesc = {};
    dispatchSigDesc.ByteStride       = sizeof(D3D12_DISPATCH_ARGUMENTS);
    dispatchSigDesc.NumArgumentDescs = 1;
    dispatchSigDesc.pArgumentDescs   = &dispatchArg;
    m_device->CreateCommandSignature(&dispatchSigDesc, nullptr, IID_PPV_ARGS(&m_dispatchIndirectSignature));

    logMessage(LogLevel::Debug, "D3D12BackendDevice: Command signatures created");
  }

  uint64_t D3D12BackendDevice::signalFence(uint64_t* ticket, HRESULT* result) {
    ID3D12Fence* fence = m_fence.Get();
    std::lock_guard<std::mutex> lock(m_fenceMutex);
    const uint64_t value = ++m_fenceValue;
    SubmitQueue* submit = &m_submitQueue;
    const uint64_t queued = m_submitQueue.enqueue([fence, value, result, submit](ID3D12CommandQueue* queue) {
      const HRESULT hr = queue->Signal(fence, value);
      if (result)
        *result = hr;
      // A value never signaled would leave its waits hanging.
      if (FAILED(hr)) {
        submit->checkResult(hr, "Signal");
        submit->removeDevice("a device fence value could not be signaled");
      }
    });
    if (ticket)
      *ticket = queued;
    return value;
  }

  bool D3D12BackendDevice::waitForFence(uint64_t value, DWORD timeoutMs) {
    if (m_fence->GetCompletedValue() >= value)
      return true;
    // An event per wait: other threads may wait at the same time.
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      return timeoutMs == INFINITE && SUCCEEDED(m_fence->SetEventOnCompletion(value, nullptr));
    const bool reached = SUCCEEDED(m_fence->SetEventOnCompletion(value, event))
      && WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    return reached || m_fence->GetCompletedValue() >= value;
  }

  void D3D12BackendDevice::waitForGpu() {
    if (!m_commandQueue || !m_fence)
      return;
    const uint64_t value = signalFence();
    // A removed device (or one DXUP gave up, IBackendDevice::lostReason)
    // may never reach the value.
    while (!waitForFence(value, 100)) {
      if (FAILED(m_submitQueue.lostReason()) || FAILED(m_device->GetDeviceRemovedReason()))
        return;
    }
  }

  HRESULT D3D12BackendDevice::flushAndCheckGpu(const char* tag) {
    if (!m_gpuSyncValidationEnabled || !m_commandQueue || !m_fence || !m_device)
      return S_OK;

    HRESULT hr = S_OK;
    uint64_t ticket = 0;
    const uint64_t value = signalFence(&ticket, &hr);
    m_submitQueue.waitFor(ticket);
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "GPU-SYNC [%s]: Signal failed hr=0x%08X", tag, hr);
      HRESULT reason = m_device->GetDeviceRemovedReason();
      logMessage(LogLevel::Error, "GPU-SYNC [%s]: DeviceRemovedReason=0x%08X", tag, reason);
      return reason;
    }

    if (!waitForFence(value, 5000)) {   // 5 second timeout
      logMessage(LogLevel::Error, "GPU-SYNC [%s]: fence wait TIMED OUT (5s)", tag);
      HRESULT reason = m_device->GetDeviceRemovedReason();
      logMessage(LogLevel::Error, "GPU-SYNC [%s]: DeviceRemovedReason=0x%08X", tag, reason);
      return reason;
    }

    HRESULT reason = m_device->GetDeviceRemovedReason();
    if (FAILED(reason)) {
      logMessage(LogLevel::Error, "GPU-SYNC [%s]: device HUNG after sync! reason=0x%08X", tag, reason);
    } else {
      logMessage(LogLevel::Info, "GPU-SYNC [%s]: OK", tag);
    }
    return reason;
  }
#endif

  // ============================================================
  // Resource Creation
  // ============================================================

#ifdef _WIN32
  bool D3D12BackendDevice::isReusableUploadBuffer(const BufferDesc& desc) {
    constexpr uint32_t reusableBindFlags =
      static_cast<uint32_t>(BindFlags::VertexBuffer) |
      static_cast<uint32_t>(BindFlags::IndexBuffer) |
      static_cast<uint32_t>(BindFlags::ConstantBuffer) |
      static_cast<uint32_t>(BindFlags::ShaderResource) |
      static_cast<uint32_t>(BindFlags::StreamOutput);
    const uint32_t cpuAccess = static_cast<uint32_t>(desc.cpuAccess);
    const uint32_t bindFlags = static_cast<uint32_t>(desc.bindFlags);
    const bool uploadUsage = desc.usage == ResourceUsage::Dynamic
                          || desc.usage == ResourceUsage::Staging;
    return desc.size != 0
        && uploadUsage
        && (cpuAccess & static_cast<uint32_t>(CpuAccess::Write)) != 0
        && (cpuAccess & static_cast<uint32_t>(CpuAccess::Read)) == 0
        && (bindFlags & ~reusableBindFlags) == 0;
  }

  D3D12BackendDevice::RecycledBufferKey D3D12BackendDevice::makeRecycledBufferKey(const BufferDesc& desc) {
    RecycledBufferKey key = {};
    key.size = desc.size;
    key.structStride = desc.structStride;
    key.usage = static_cast<uint32_t>(desc.usage);
    key.bindFlags = static_cast<uint32_t>(desc.bindFlags);
    key.cpuAccess = static_cast<uint32_t>(desc.cpuAccess);
    key.miscFlags = desc.miscFlags;
    return key;
  }

  Rc<IBackendBuffer> D3D12BackendDevice::acquireRecycledBuffer(const BufferDesc& desc) {
    if (!isReusableUploadBuffer(desc) || m_gpuSyncValidationEnabled)
      return nullptr;

    std::lock_guard<std::mutex> lock(m_recycledBufferMutex);
    auto it = m_recycledBuffers.find(makeRecycledBufferKey(desc));
    if (it == m_recycledBuffers.end() || it->second.empty())
      return nullptr;

    auto buffer = std::move(it->second.back().buffer);
    it->second.pop_back();
    m_recycledBufferCount--;
    adjustRecycledBytes(-int64_t(desc.size));
    if (it->second.empty())
      m_recycledBuffers.erase(it);
    // Its entry in the order stays behind: rebuilt once they are most of it.
    if (m_recycleOrder.size() > 2 * m_recycledBufferCount + 256)
      compactRecycleOrder();

    static_cast<D3D12BackendBuffer*>(buffer.get())->setCurrentState(D3D12_RESOURCE_STATE_GENERIC_READ);
    return buffer;
  }

  bool D3D12BackendDevice::recycleTemporaryBuffer(Rc<IBackendBuffer> buffer) {
    constexpr size_t kMaxRecycledBuffers = 4096;
    constexpr size_t kMaxRecycledBuffersPerKey = 2048;
    if (!buffer)
      return false;

    const BufferDesc desc = buffer->getDesc();
    if (buffer.use_count() != 1 || m_gpuSyncValidationEnabled
     || static_cast<D3D12BackendBuffer*>(buffer.get())->isSuballocated())
      return false;

    if (!isReusableUploadBuffer(desc) || desc.size > kMaxRecycledBytes / 4)
      return false;
    // Command lists and buffer views keep buffers through the retire owner
    // (which the reference count above does not see): one they still hold
    // is no temporary of the context's any more. Held only by the Rc's
    // deleter (and this lock), it is.
    if (const std::shared_ptr<void> owner = buffer->retireOwner(); owner && owner.use_count() > 2)
      return false;
    // Handed out again for another application buffer (this runs on the
    // recording thread, which alone touches identities).
    buffer->identity.reset();

    const RecycledBufferKey key = makeRecycledBufferKey(desc);
    Rc<IBackendBuffer> evicted;   // released after the lock
    std::lock_guard<std::mutex> lock(m_recycledBufferMutex);
    auto existing = m_recycledBuffers.find(key);
    if (existing != m_recycledBuffers.end() && existing->second.size() >= kMaxRecycledBuffersPerKey)
      return false;

    if (m_recycledBufferCount >= kMaxRecycledBuffers) {
      // Buffers left over from a load spike must not keep the sizes a
      // frame uses every time from being recycled: the one waiting
      // longest goes (which may empty and remove this buffer's bucket: it
      // is looked up again below).
      evicted = evictOldestRecycled();
      if (!evicted)
        return false;
    }

    const uint64_t sequence = ++m_recycleSequence;
    m_recycledBuffers[key].push_back({ std::move(buffer), sequence });
    m_recycleOrder.emplace_back(sequence, key);
    m_recycledBufferCount++;
    adjustRecycledBytes(int64_t(desc.size));
    return true;
  }

  Rc<IBackendBuffer> D3D12BackendDevice::evictOldestRecycled(uint64_t maxSequence) {
    // Each entry comes up once: entries of buffers taken again are dropped
    // on the way.
    while (!m_recycleOrder.empty() && m_recycleOrder.front().first <= maxSequence) {
      const auto [sequence, key] = m_recycleOrder.front();
      m_recycleOrder.pop_front();
      auto it = m_recycledBuffers.find(key);
      // The oldest of its bucket, as the order is the order they came back.
      if (it == m_recycledBuffers.end() || it->second.empty() || it->second.front().sequence != sequence)
        continue;
      Rc<IBackendBuffer> buffer = std::move(it->second.front().buffer);
      it->second.pop_front();
      m_recycledBufferCount--;
      adjustRecycledBytes(-int64_t(key.size));
      if (it->second.empty())
        m_recycledBuffers.erase(it);
      return buffer;
    }
    return nullptr;
  }

  void D3D12BackendDevice::compactRecycleOrder() {
    m_recycleOrder.clear();
    for (const auto& [key, bucket] : m_recycledBuffers) {
      for (const auto& entry : bucket)
        m_recycleOrder.emplace_back(entry.sequence, key);
    }
    std::sort(m_recycleOrder.begin(), m_recycleOrder.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
  }

  void D3D12BackendDevice::trimRecycledBuffers() {
    // At present: what a load (texture uploads) recycled between two
    // presents comes down to kMaxRecycledBytes, the buffers waiting longest
    // first; and every trim interval, the buffers not taken again since
    // the last one go.
    const uint64_t now = GetTickCount64();
    uint64_t last = m_lastRecycleTrim.load(std::memory_order_relaxed);
    const bool idleTrim = now - last >= trimIntervalMs()
      && m_lastRecycleTrim.compare_exchange_strong(last, now, std::memory_order_relaxed);
    if (!idleTrim && m_recycledBufferBytes.load(std::memory_order_relaxed) <= kMaxRecycledBytes)
      return;
    std::vector<Rc<IBackendBuffer>> released;   // after the lock
    std::lock_guard<std::mutex> lock(m_recycledBufferMutex);
    if (idleTrim) {
      // Those back before the last trim lead the order.
      while (auto buffer = evictOldestRecycled(m_recycleTrimMark))
        released.push_back(std::move(buffer));
      m_recycleTrimMark = m_recycleSequence;
    }
    while (m_recycledBufferBytes.load(std::memory_order_relaxed) > kMaxRecycledBytes) {
      auto buffer = evictOldestRecycled();
      if (!buffer)
        break;
      released.push_back(std::move(buffer));
    }
  }

  void D3D12BackendDevice::trimStagingSpares() {
    // Every trim interval. A spare made in round r is memory of a staging
    // image the GPU used in a command list submitted by the present that
    // ran round r at the latest, so before the fence signaled in round
    // r + 1: once that fence passed, the spares of round r and before go,
    // which nothing took again for a whole interval.
    const uint64_t now = GetTickCount64();
    uint64_t last = m_lastStagingTrim.load(std::memory_order_relaxed);
    if (now - last < trimIntervalMs()
     || !m_lastStagingTrim.compare_exchange_strong(last, now, std::memory_order_relaxed))
      return;
    D3D12StagingSpareRegistry& registry = *m_stagingSpareRegistry;
    std::vector<std::weak_ptr<D3D12BackendImage>> images;
    uint64_t releaseEpoch = 0;
    {
      std::lock_guard<std::mutex> lock(registry.mutex);
      if (registry.images.empty() || !m_fence || !m_commandQueue)
        return;
      // The GPU is that far behind: the round waits.
      if (m_stagingTrimFence && m_fence->GetCompletedValue() < m_stagingTrimFence)
        return;
      const uint64_t round = registry.epoch.fetch_add(1);
      m_stagingTrimFence = signalFence();
      // The fence of the round before passed: the spares of the round
      // before that go.
      if (round < 3)
        return;
      releaseEpoch = round - 2;
      images.swap(registry.images);
    }
    std::vector<std::weak_ptr<D3D12BackendImage>> kept;
    size_t released = 0;
    for (auto& weak : images) {
      if (auto image = weak.lock()) {
        if (image->trimStagingSpares(releaseEpoch, released))
          kept.push_back(std::move(weak));
      }
    }
    {
      std::lock_guard<std::mutex> lock(registry.mutex);
      registry.images.insert(registry.images.end(), kept.begin(), kept.end());
    }
    if (released)
      logMessage(LogLevel::Info, "trimStagingSpares: %zu spares of %zu staging images go back", released, images.size());
  }
#endif

  Rc<IBackendBuffer> D3D12BackendDevice::createBuffer(const BufferDesc& desc) {
    // Detail: size, bind flags << 32, usage << 48.
    HitchScope hitch("createBuffer", desc.size | uint64_t(static_cast<uint32_t>(desc.bindFlags)) << 32
      | uint64_t(static_cast<uint32_t>(desc.usage)) << 48);
#ifdef _WIN32
    if (desc.miscFlags & kResourceMiscTilePool) {
      auto pool = std::make_shared<D3D12BackendBuffer>(ComPtr<ID3D12Resource>(), desc);
      D3D12BackendBuffer::TileHeap heap;
      if (FAILED(pool->createTileHeap(m_device.Get(), desc.size, heap)))
        return nullptr;
      pool->appendTileHeap(std::move(heap));
      return BackendRetireQueue::wrap<IBackendBuffer>(m_retireQueue, std::move(pool));
    }

    // Small CPU-written buffers without views are slices of upload blocks.
    const uint32_t sliceBindFlags =
      static_cast<uint32_t>(BindFlags::VertexBuffer) |
      static_cast<uint32_t>(BindFlags::IndexBuffer) |
      static_cast<uint32_t>(BindFlags::ConstantBuffer);
    if (desc.usage == ResourceUsage::Dynamic && !desc.miscFlags && !m_gpuSyncValidationEnabled
     && desc.size && desc.size <= D3D12UploadSuballocator::kMaxSlice
     && (static_cast<uint32_t>(desc.bindFlags) & ~sliceBindFlags) == 0
     && !(static_cast<uint32_t>(desc.cpuAccess) & static_cast<uint32_t>(CpuAccess::Read))) {
      D3D12UploadSuballocator::Slice slice;
      hitch.rename("createBuffer (upload slice)");
      if (m_uploadSuballocator.allocate(m_device.Get(), desc.size, slice)) {
        // Initial data or the first map write the data; the rest reads 0.
        D3D12UploadSuballocator::prepareSlice(slice, desc.dataSize ? desc.dataSize : desc.size,
          (desc.bindFlags & BindFlags::ConstantBuffer) != BindFlags::None);
        auto buffer = std::make_shared<D3D12BackendBuffer>(desc, &m_uploadSuballocator, slice);
        return BackendRetireQueue::wrap<IBackendBuffer>(m_retireQueue, std::move(buffer));
      }
    }

    // Small buffers the GPU only reads, written by copies: slices of
    // default blocks. Shader views and GPU writes need buffers of their own.
    if ((desc.usage == ResourceUsage::Default || desc.usage == ResourceUsage::Immutable)
     && !desc.miscFlags && !m_gpuSyncValidationEnabled
     && desc.size && desc.size <= D3D12UploadSuballocator::kMaxSlice
     && (static_cast<uint32_t>(desc.bindFlags) & ~sliceBindFlags) == 0) {
      D3D12UploadSuballocator::Slice slice;
      hitch.rename("createBuffer (default slice)");
      if (m_defaultSuballocator.allocate(m_device.Get(), desc.size, slice)) {
        auto buffer = std::make_shared<D3D12BackendBuffer>(desc, &m_defaultSuballocator, slice);
        if (desc.usage == ResourceUsage::Default && desc.bindFlags == BindFlags::ConstantBuffer
         && desc.cpuAccess == CpuAccess::None)
          buffer->setRenamableConstantBuffer();
        return BackendRetireQueue::wrap<IBackendBuffer>(m_retireQueue, std::move(buffer));
      }
    }

    hitch.rename("createBuffer (recycled)");
    if (auto recycled = acquireRecycledBuffer(desc))
      return recycled;
    hitch.rename("createBuffer (new resource)");

    D3D12_HEAP_PROPERTIES heapProps = {};
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

    switch (desc.usage) {
      case ResourceUsage::Dynamic:
        // WRITE_DISCARD renames dynamic buffers, so the GPU only reads them.
        D3D12UploadSuballocator::uploadHeapProperties(heapProps, false, m_uploadSuballocator.usesWriteBack());
        initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        break;
      case ResourceUsage::Staging:
        // CPU-visible memory the GPU may copy both to and from, as D3D11
        // allows for staging resources whatever their CPU access: cached
        // for CPU reads, write-combined otherwise.
        heapProps = m_device->GetCustomHeapProperties(0,
          (static_cast<uint32_t>(desc.cpuAccess) & static_cast<uint32_t>(CpuAccess::Read))
            ? D3D12_HEAP_TYPE_READBACK
            : D3D12_HEAP_TYPE_UPLOAD);
        initialState = D3D12_RESOURCE_STATE_COMMON;
        break;
      default:
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    }

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width              = desc.size;
    resourceDesc.Height             = 1;
    resourceDesc.DepthOrArraySize   = 1;
    resourceDesc.MipLevels          = 1;
    resourceDesc.Format             = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (static_cast<uint32_t>(desc.bindFlags & BindFlags::UnorderedAccess))
      resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> resource;
    // Shared buffers live in shared heaps; CPU-visible heaps cannot be shared.
    const bool sharedBuffer = (desc.miscFlags & kResourceMiscShared) && heapProps.Type == D3D12_HEAP_TYPE_DEFAULT
      && !(desc.miscFlags & kResourceMiscTiled);
    // Tiled buffers are reserved resources whose memory comes from tile pools.
    HRESULT hr = E_FAIL;
    if (desc.miscFlags & kResourceMiscTiled) {
      hr = m_device->CreateReservedResource(&resourceDesc, initialState, nullptr, IID_PPV_ARGS(&resource));
    } else {
      // Default and dynamic buffers go into shared heaps. Small ones take a
      // whole 64 KB page there as a committed resource would, but without
      // the kernel allocation: Kena creates 16 and 112 byte structured
      // buffers and 4 and 20 byte dynamic vertex/shader resource buffers
      // while it runs, and every few seconds one took 4-13 ms committed.
      D3D12PlacedAllocator* allocator = nullptr;
      if (!(desc.miscFlags & kResourceMiscShared) && !m_gpuSyncValidationEnabled) {
        if (heapProps.Type == D3D12_HEAP_TYPE_DEFAULT)
          allocator = m_bufferAllocator.get();
        else if (heapProps.Type == D3D12_HEAP_TYPE_UPLOAD && desc.usage == ResourceUsage::Dynamic)
          allocator = m_dynamicBufferAllocator.get();
      }
      hitch.rename(allocator ? "createBuffer (placed)" : "createBuffer (committed)");
      if (allocator && allocator->createPlaced(m_device.Get(), resourceDesc, initialState, nullptr, resource))
        hr = S_OK;
      else {
        hr = m_device->CreateCommittedResource(
          &heapProps, sharedBuffer ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
          &resourceDesc, initialState,
          nullptr, IID_PPV_ARGS(&resource));
        if (SUCCEEDED(hr))
          memstats::track(m_device.Get(), resource.Get(), memstats::Buffer);
      }
    }

    if (FAILED(hr)) {
      HRESULT removedReason = m_device->GetDeviceRemovedReason();
      logMessage(LogLevel::Error,
        "D3D12BackendDevice::createBuffer failed hr=0x%08X reason=0x%08X size=%llu bind=0x%x",
        hr,
        removedReason,
        static_cast<unsigned long long>(desc.size),
        static_cast<uint32_t>(desc.bindFlags));
      return nullptr;
    }

    auto buffer = std::make_shared<D3D12BackendBuffer>(std::move(resource), desc);
    buffer->setCurrentState(initialState);
    buffer->setFixedState(heapProps.Type == D3D12_HEAP_TYPE_UPLOAD || heapProps.Type == D3D12_HEAP_TYPE_GPU_UPLOAD
      || heapProps.Type == D3D12_HEAP_TYPE_READBACK
      || (heapProps.Type == D3D12_HEAP_TYPE_CUSTOM && desc.usage == ResourceUsage::Dynamic));
    if (sharedBuffer)
      buffer->setSharedSync(std::make_shared<D3D12SharedSync>());

    if (m_gpuSyncValidationEnabled) {
      // Resources are created on any thread.
      static std::atomic<uint32_t> s_bufCount{ 0 };
      const uint32_t bufferId = ++s_bufCount;
      char tag[64];
      snprintf(tag, sizeof(tag), "createBuffer#%u-%lluB-flags0x%x", bufferId, static_cast<unsigned long long>(desc.size), static_cast<uint32_t>(desc.bindFlags));
      flushAndCheckGpu(tag);
    }

    return BackendRetireQueue::wrap<IBackendBuffer>(m_retireQueue, std::move(buffer));
#else
    return std::make_shared<D3D12BackendBuffer>(desc);
#endif
  }

  Rc<IBackendImage> D3D12BackendDevice::createImage(const ImageDesc& desc) {
    HitchScope hitch("createImage", uint64_t(desc.width) * desc.height);
#ifdef _WIN32
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc = {};
    switch (desc.type) {
      case ImageType::Tex1D:
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        break;
      case ImageType::Tex3D:
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        break;
      default:
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        break;
    }

    resourceDesc.Width              = desc.width;
    resourceDesc.Height             = desc.height;
    resourceDesc.DepthOrArraySize   = (desc.type == ImageType::Tex3D) ? desc.depth : desc.arraySize;
    resourceDesc.MipLevels          = desc.mipLevels;
    resourceDesc.Format             = desc.format;
    resourceDesc.SampleDesc.Count   = desc.sampleCount;
    resourceDesc.SampleDesc.Quality = desc.sampleQuality;

    // In D3D12, if a depth buffer also needs shader resource access,
    // the resource must be created with the typeless equivalent format.
    bool isDepthSRV = static_cast<uint32_t>(desc.bindFlags & BindFlags::DepthStencil)
                   && static_cast<uint32_t>(desc.bindFlags & BindFlags::ShaderResource);
    if (isDepthSRV) {
      switch (resourceDesc.Format) {
        case DXGI_FORMAT_D32_FLOAT:           resourceDesc.Format = DXGI_FORMAT_R32_TYPELESS;     break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:   resourceDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;   break;
        case DXGI_FORMAT_D16_UNORM:           resourceDesc.Format = DXGI_FORMAT_R16_TYPELESS;     break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:resourceDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;break;
        default: break;
      }
    }

    // Views in the sRGB or UNORM format of the family (swap chain proxies):
    // the resource is typeless; the clear value keeps the format itself.
    if (desc.miscFlags & kResourceMiscCastable) {
      switch (resourceDesc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: resourceDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: resourceDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; break;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: resourceDesc.Format = DXGI_FORMAT_B8G8R8X8_TYPELESS; break;
        default: break;
      }
    }

    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CLEAR_VALUE* clearValue = nullptr;
    D3D12_CLEAR_VALUE optimizedClear = {};

    // Typeless formats cannot be used directly in clear values.
    // Check if the format is typeless to decide whether to provide a clear value.
    bool isTypeless = false;
    switch (desc.format) {
      case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      case DXGI_FORMAT_R32G32B32_TYPELESS:
      case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      case DXGI_FORMAT_R32G32_TYPELESS:
      case DXGI_FORMAT_R32G8X24_TYPELESS:
      case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      case DXGI_FORMAT_R16G16_TYPELESS:
      case DXGI_FORMAT_R32_TYPELESS:
      case DXGI_FORMAT_R24G8_TYPELESS:
      case DXGI_FORMAT_R8G8_TYPELESS:
      case DXGI_FORMAT_R16_TYPELESS:
      case DXGI_FORMAT_R8_TYPELESS:
      case DXGI_FORMAT_BC1_TYPELESS:
      case DXGI_FORMAT_BC2_TYPELESS:
      case DXGI_FORMAT_BC3_TYPELESS:
      case DXGI_FORMAT_BC4_TYPELESS:
      case DXGI_FORMAT_BC5_TYPELESS:
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      case DXGI_FORMAT_BC6H_TYPELESS:
      case DXGI_FORMAT_BC7_TYPELESS:
        isTypeless = true;
        break;
      default:
        break;
    }

    if (static_cast<uint32_t>(desc.bindFlags & BindFlags::RenderTarget)) {
      resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      if (!isTypeless) {
        optimizedClear.Format = desc.format;
        clearValue = &optimizedClear;
      }
    }
    if (static_cast<uint32_t>(desc.bindFlags & BindFlags::DepthStencil)) {
      resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
      // Depth-stencil resources always need an optimized clear value.
      // For typeless resources, use the typed depth equivalent format.
      DXGI_FORMAT clearFmt = desc.format;
      if (isTypeless) {
        switch (resourceDesc.Format) {
          case DXGI_FORMAT_R32_TYPELESS:      clearFmt = DXGI_FORMAT_D32_FLOAT;            break;
          case DXGI_FORMAT_R16_TYPELESS:      clearFmt = DXGI_FORMAT_D16_UNORM;            break;
          case DXGI_FORMAT_R24G8_TYPELESS:    clearFmt = DXGI_FORMAT_D24_UNORM_S8_UINT;    break;
          case DXGI_FORMAT_R32G8X24_TYPELESS: clearFmt = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; break;
          default: break;
        }
      }
      optimizedClear.Format = clearFmt;
      optimizedClear.DepthStencil.Depth = 1.0f;
      optimizedClear.DepthStencil.Stencil = 0;
      clearValue = &optimizedClear;
    }
    if (static_cast<uint32_t>(desc.bindFlags & BindFlags::UnorderedAccess))
      resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // D3D12 multisamples only render targets and depth buffers; D3D11 also
    // takes images that are only read.
    const D3D12_RESOURCE_FLAGS attachmentFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
      | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (desc.sampleCount > 1 && !(resourceDesc.Flags & attachmentFlags)) {
      D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { resourceDesc.Format };
      const bool queried = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
      const bool depthFormat = resourceDesc.Format == DXGI_FORMAT_R24G8_TYPELESS
        || resourceDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS
        || (queried && (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL));
      resourceDesc.Flags |= depthFormat ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }

    const bool canUseSimultaneousAccess =
      simultaneousAccessEnabled()
      && static_cast<uint32_t>(desc.bindFlags & BindFlags::UnorderedAccess)
      && !(static_cast<uint32_t>(desc.bindFlags & BindFlags::DepthStencil))
      && desc.sampleCount <= 1;
    if (canUseSimultaneousAccess)
      resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    // Shared images live in shared heaps. Like D3D11 shared textures, other
    // devices access them at the same time where D3D12 allows it.
    const bool shared = (desc.miscFlags & kResourceMiscShared) && !(desc.miscFlags & kResourceMiscTiled)
      && desc.usage != ResourceUsage::Staging;
    if (shared && !(resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && desc.sampleCount <= 1)
      resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (static_cast<uint32_t>(desc.bindFlags & BindFlags::DepthStencil)
     && static_cast<uint32_t>(desc.bindFlags & BindFlags::UnorderedAccess)) {
      logMessage(LogLevel::Warn,
        "createImage depth+uav: %ux%u fmt=%u resFmt=%u bind=0x%x flags=0x%x",
        desc.width,
        desc.height,
        static_cast<uint32_t>(desc.format),
        static_cast<uint32_t>(resourceDesc.Format),
        static_cast<uint32_t>(desc.bindFlags),
        static_cast<uint32_t>(resourceDesc.Flags));
    }

    if (desc.usage == ResourceUsage::Staging && !(desc.miscFlags & kResourceMiscTiled))
      return createStagingImage(desc, resourceDesc);

    // Pre-check: is device already removed?
    {
      HRESULT preHr = m_device->GetDeviceRemovedReason();
      if (FAILED(preHr)) {
        logMessage(LogLevel::Error, "D3D12BackendDevice::createImage - device already removed BEFORE creation! reason=0x%08X, %ux%u, origFormat=%d, resFormat=%d",
          preHr, desc.width, desc.height, desc.format, resourceDesc.Format);
        return nullptr;
      }
    }

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = E_FAIL;
    uint32_t lazyTiles = 0;
    if (desc.miscFlags & kResourceMiscTiled) {
      // Tiled textures are reserved resources in the standard tile layout.
      resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
      resourceDesc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
      hr = m_device->CreateReservedResource(&resourceDesc, initialState, clearValue, IID_PPV_ARGS(&resource));
    } else {
      // Textures that are not render targets go into shared heaps; shared
      // textures need a resource of their own.
      // D3D11_RESOURCE_MISC_SHARED, _GDI_COMPATIBLE, _SHARED_KEYEDMUTEX, _SHARED_NTHANDLE
      constexpr uint32_t kSharedMisc = 0x2 | 0x200 | 0x100 | 0x800;
      const bool placeable = heapProps.Type == D3D12_HEAP_TYPE_DEFAULT
        && !(resourceDesc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        && resourceDesc.SampleDesc.Count == 1
        && !(desc.miscFlags & kSharedMisc)
        && !m_gpuSyncValidationEnabled;
      if (placeable && m_textureAllocator->createPlaced(m_device.Get(), resourceDesc, initialState, clearValue, resource))
        hr = S_OK;
      else if (!shared && (resource = createLazyTarget(resourceDesc, initialState, clearValue, lazyTiles)))
        hr = S_OK;
      else {
        hr = m_device->CreateCommittedResource(
          &heapProps, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
          &resourceDesc, initialState,
          clearValue, IID_PPV_ARGS(&resource));
        if (SUCCEEDED(hr))
          memstats::track(m_device.Get(), resource.Get(),
            (resourceDesc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
              ? memstats::TargetTexture : memstats::Texture);
      }
    }

    if (FAILED(hr)) {
      HRESULT removedReason = m_device->GetDeviceRemovedReason();
      logMessage(LogLevel::Error, "D3D12BackendDevice::createImage failed: %ux%u, origFormat=%d, resFormat=%d, Flags=0x%x, isTypeless=%d, isDepthSRV=%d, hr=0x%08X, removedReason=0x%08X",
        desc.width, desc.height, desc.format, resourceDesc.Format, resourceDesc.Flags, isTypeless, isDepthSRV, hr, removedReason);
      return nullptr;
    }

    auto image = std::make_shared<D3D12BackendImage>(
      std::move(resource),
      desc,
      (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0);
    image->setCurrentState(initialState);
    image->m_lazyTiles.store(lazyTiles, std::memory_order_release);
    if (shared)
      image->setSharedSync(std::make_shared<D3D12SharedSync>());

    auto isTrackedDebugFormat = [](DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
          return true;

        default:
          return false;
      }
    };

    // Images are created on any thread: each takes a number of its own.
    static std::atomic<uint32_t> s_imageCreateLogCount{ 0 };
    const uint32_t imageLogId = ++s_imageCreateLogCount;
    if (imageLogId <= 256
     || isTrackedDebugFormat(desc.format)
     || isTrackedDebugFormat(resourceDesc.Format)) {
      logMessage(LogLevel::Info,
        "createImage-ok: id=%u img=%p res=%p %ux%ux%u mip=%u array=%u sample=%u origFmt=%u resFmt=%u bind=0x%x usage=%u cpu=0x%x misc=0x%x flags=0x%x state=0x%x",
        imageLogId,
        image.get(),
        image->getResource(),
        desc.width,
        desc.height,
        desc.depth,
        desc.mipLevels,
        desc.arraySize,
        desc.sampleCount,
        static_cast<uint32_t>(desc.format),
        static_cast<uint32_t>(resourceDesc.Format),
        static_cast<uint32_t>(desc.bindFlags),
        static_cast<uint32_t>(desc.usage),
        static_cast<uint32_t>(desc.cpuAccess),
        desc.miscFlags,
        static_cast<uint32_t>(resourceDesc.Flags),
        static_cast<uint32_t>(initialState));
    }

    // Post-creation sync check: force GPU completion every 10th image
    // to catch async hangs with precise timing.
    static std::atomic<uint32_t> s_imageCount{ 0 };
    const uint32_t imageId = ++s_imageCount;
    {
      HRESULT postHr = m_device->GetDeviceRemovedReason();
      if (FAILED(postHr)) {
        logMessage(LogLevel::Error, "createImage #%u - device removed AFTER creation! reason=0x%08X, %ux%u, Format=%d, resFormat=%d, Flags=0x%x",
          imageId, postHr, desc.width, desc.height, desc.format, resourceDesc.Format, resourceDesc.Flags);
      } else if (m_gpuSyncValidationEnabled && imageId % 10 == 0) {
        char tag[64];
        snprintf(tag, sizeof(tag), "createImage#%u-%ux%u-fmt%d", imageId, desc.width, desc.height, desc.format);
        flushAndCheckGpu(tag);
      }
    }

    return BackendRetireQueue::wrap<IBackendImage>(m_retireQueue, std::move(image));
#else
    return std::make_shared<D3D12BackendImage>(desc);
#endif
  }

  Rc<IBackendImage> D3D12BackendDevice::createStagingImage(const ImageDesc& desc, D3D12_RESOURCE_DESC textureDesc) {
#ifdef _WIN32
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    // Video formats have more planes in D3D12 than DXUP tracks; each plane
    // follows the previous one as in a mapped D3D11 texture.
    D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = {};
    formatInfo.Format = textureDesc.Format;
    UINT planes = 1;
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo)))
     && formatInfo.PlaneCount > 1)
      planes = formatInfo.PlaneCount;
    const UINT arrayLayers = textureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
      ? 1u : textureDesc.DepthOrArraySize;
    const UINT subresources = UINT(textureDesc.MipLevels) * arrayLayers * planes;

    D3D12BackendImage::StagingLayout layout;
    layout.textureDesc = textureDesc;
    layout.footprints.resize(subresources);
    layout.numRows.resize(subresources);
    layout.rowSizes.resize(subresources);
    UINT64 totalSize = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, subresources, 0,
      layout.footprints.data(), layout.numRows.data(), layout.rowSizes.data(), &totalSize);
    if (!totalSize || totalSize == UINT64_MAX) {
      logMessage(LogLevel::Error, "createStagingImage: no copyable layout for %ux%u format %d",
        desc.width, desc.height, static_cast<int>(desc.format));
      return nullptr;
    }

    // CPU-cached system memory the GPU can copy to and from.
    D3D12_HEAP_PROPERTIES heapProps = m_device->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = totalSize;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
    if (SUCCEEDED(hr))
      memstats::track(m_device.Get(), resource.Get(), memstats::Internal);
    void* mapped = nullptr;
    if (SUCCEEDED(hr))
      hr = resource->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
      logMessage(LogLevel::Error, "createStagingImage failed hr=0x%08X size=%llu", hr,
        static_cast<unsigned long long>(totalSize));
      return nullptr;
    }

    BufferDesc stagingBufferDesc = {};
    stagingBufferDesc.size = totalSize;
    auto buffer = std::make_shared<D3D12BackendBuffer>(resource, stagingBufferDesc);
    auto image = std::make_shared<D3D12BackendImage>(std::move(resource), desc, false);
    image->initStaging(std::move(buffer), std::move(layout), static_cast<uint8_t*>(mapped), m_device.Get(),
      m_stagingSpareRegistry);
    return BackendRetireQueue::wrap<IBackendImage>(m_retireQueue, std::move(image));
#else
    (void)textureDesc;
    return std::make_shared<D3D12BackendImage>(desc);
#endif
  }

  Rc<IBackendSampler> D3D12BackendDevice::createSampler(const SamplerDesc& desc) {
#ifdef _WIN32
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_descriptorHeapManager.allocateSampler();

    D3D12_SAMPLER_DESC d3d12Desc = {};
    d3d12Desc.Filter         = static_cast<D3D12_FILTER>(desc.filter);
    if ((d3d12Desc.Filter & ~0x180) == D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT && !m_anisotropicPointMip)
      d3d12Desc.Filter = static_cast<D3D12_FILTER>((d3d12Desc.Filter & 0x180) | D3D12_FILTER_ANISOTROPIC);
    d3d12Desc.AddressU       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(static_cast<int>(desc.addressU) + 1);
    d3d12Desc.AddressV       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(static_cast<int>(desc.addressV) + 1);
    d3d12Desc.AddressW       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(static_cast<int>(desc.addressW) + 1);
    d3d12Desc.MipLODBias     = desc.mipLodBias;
    d3d12Desc.MaxAnisotropy  = desc.maxAnisotropy;
    d3d12Desc.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(static_cast<int>(desc.comparisonFunc) + 1);
    std::memcpy(d3d12Desc.BorderColor, desc.borderColor, sizeof(float) * 4);
    d3d12Desc.MinLOD         = desc.minLod;
    d3d12Desc.MaxLOD         = desc.maxLod;

    m_device->CreateSampler(&d3d12Desc, handle);

    return BackendRetireQueue::wrap<IBackendSampler>(m_retireQueue, std::make_shared<D3D12BackendSampler>(desc, &m_descriptorHeapManager, handle));
#else
    return std::make_shared<D3D12BackendSampler>(desc);
#endif
  }

  Rc<IBackendBufferView> D3D12BackendDevice::createBufferView(
    IBackendBuffer* buffer, const BufferViewDesc& desc) {
#ifdef _WIN32
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    Rc<IBackendBuffer> counterBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE nullUavHandle = {};

    // Everything that can fail comes before the descriptors are taken.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    if (!fillBufferShaderResourceViewDesc(buffer->getDesc(), desc, srvDesc))
      return nullptr;
    const bool unorderedAccess = static_cast<uint32_t>(buffer->getDesc().bindFlags & BindFlags::UnorderedAccess) != 0;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    ID3D12Resource* counterResource = nullptr;
    if (unorderedAccess) {
      if (!fillBufferUnorderedAccessViewDesc(buffer->getDesc(), desc, uavDesc))
        return nullptr;
      if (desc.hasCounter) {
        BufferDesc counterDesc = {};
        counterDesc.size = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
        counterDesc.usage = ResourceUsage::Default;
        counterDesc.bindFlags = BindFlags::UnorderedAccess;
        counterBuffer = createBuffer(counterDesc);
        if (!counterBuffer)
          return nullptr;

        counterResource = static_cast<D3D12BackendBuffer*>(counterBuffer.get())->getResource();
      }
    }

    srvHandle = m_descriptorHeapManager.allocateCbvSrvUav();
    m_device->CreateShaderResourceView(d3d12Buffer->getResource(), &srvDesc, srvHandle);
    nullSrvHandle = m_descriptorHeapManager.allocateCbvSrvUav();
    m_device->CreateShaderResourceView(nullptr, &srvDesc, nullSrvHandle);

    if (unorderedAccess) {
      uavHandle = m_descriptorHeapManager.allocateCbvSrvUav();
      m_device->CreateUnorderedAccessView(d3d12Buffer->getResource(), counterResource, &uavDesc, uavHandle);
      nullUavHandle = m_descriptorHeapManager.allocateCbvSrvUav();
      m_device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, nullUavHandle);
    }

    return BackendRetireQueue::wrap<IBackendBufferView>(m_retireQueue, std::make_shared<D3D12BackendBufferView>(
      buffer,
      desc,
      std::move(counterBuffer),
      &m_descriptorHeapManager,
      m_device.Get(),
      srvHandle,
      uavHandle,
      nullSrvHandle,
      nullUavHandle));
#else
    return std::make_shared<D3D12BackendBufferView>(buffer, desc);
#endif
  }

  Rc<IBackendImageView> D3D12BackendDevice::createImageView(
    IBackendImage* image, const ImageViewDesc& desc) {
#ifdef _WIN32
    auto* d3d12Image = static_cast<D3D12BackendImage*>(image);
    const auto& imageDesc = image->getDesc();
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrvHandle = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    // Holds off min LOD changes until the view is registered with the image;
    // a retargetable image keeps its resource until all views are made.
    std::unique_lock<std::mutex> srvViewLock(d3d12Image->m_srvViewMutex, std::defer_lock);
    const bool retargetable = d3d12Image->isRetargetable();
    if (retargetable)
      srvViewLock.lock();
    if (hasImageViewUsage(desc.usage, ImageViewUsage::ShaderResource)) {
      if (!srvViewLock.owns_lock())
        srvViewLock.lock();
      srvHandle = m_descriptorHeapManager.allocateCbvSrvUav();
      fillShaderResourceViewDesc(desc, imageDesc, srvDesc);
      srvDesc.Format = getValidatedViewFormat(srvDesc.Format, d3d12Image->getResource());
      setSrvMinLodClamp(srvDesc, image->minLod.load(std::memory_order_relaxed));
      m_device->CreateShaderResourceView(d3d12Image->getResource(), &srvDesc, srvHandle);
      nullSrvHandle = m_descriptorHeapManager.allocateCbvSrvUav();
      const D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = nullViewDesc(srvDesc, imageDesc.format);
      m_device->CreateShaderResourceView(nullptr, &nullSrvDesc, nullSrvHandle);
      {
        HRESULT chk = m_device->GetDeviceRemovedReason();
        if (FAILED(chk))
          logMessage(LogLevel::Error, "Device removed after CreateSRV! reason=0x%08X, imgFmt=%d, viewFmt=%d", chk, imageDesc.format, srvDesc.Format);
      }
    }

    auto view = std::make_shared<D3D12BackendImageView>(image, desc, &m_descriptorHeapManager, srvHandle, nullSrvHandle);
    if (srvHandle.ptr)
      view->trackShaderResourceView(m_device.Get(), srvDesc);
    if (srvViewLock.owns_lock() && !retargetable)
      srvViewLock.unlock();

    if (hasImageViewUsage(desc.usage, ImageViewUsage::RenderTarget)) {
      D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_descriptorHeapManager.allocateRtv();
      D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
      fillRenderTargetViewDesc(desc, imageDesc, rtvDesc);
      // Swap chain buffers take render target views in the other format of
      // their family (sRGB views of UNORM buffers).
      if (!retargetable)
        rtvDesc.Format = getValidatedViewFormat(rtvDesc.Format, d3d12Image->getResource());
      m_device->CreateRenderTargetView(d3d12Image->getResource(), &rtvDesc, rtvHandle);
      view->setRtvHandle(rtvHandle);
      if (retargetable)
        view->trackRetarget(m_device.Get(), &rtvDesc);
      {
        HRESULT chk = m_device->GetDeviceRemovedReason();
        if (FAILED(chk))
          logMessage(LogLevel::Error, "Device removed after CreateRTV! reason=0x%08X, imgFmt=%d, viewFmt=%d", chk, imageDesc.format, rtvDesc.Format);
      }
    }

    if (hasImageViewUsage(desc.usage, ImageViewUsage::DepthStencil)) {
      D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_descriptorHeapManager.allocateDsv();
      D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
      fillDepthStencilViewDesc(desc, imageDesc, dsvDesc);
      dsvDesc.Format = getValidatedViewFormat(dsvDesc.Format, d3d12Image->getResource());
      m_device->CreateDepthStencilView(d3d12Image->getResource(), &dsvDesc, dsvHandle);
      view->setDsvHandle(dsvHandle);
      {
        HRESULT chk = m_device->GetDeviceRemovedReason();
        if (FAILED(chk))
          logMessage(LogLevel::Error, "Device removed after CreateDSV! reason=0x%08X, imgFmt=%d, viewFmt=%d", chk, imageDesc.format, dsvDesc.Format);
      }
    }

    if (hasImageViewUsage(desc.usage, ImageViewUsage::UnorderedAccess)) {
      // Check if the D3D12 resource actually supports UAV
      D3D12_RESOURCE_DESC resDesc = d3d12Image->getResource()->GetDesc();
      if (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        if (!fillUnorderedAccessViewDesc(desc, imageDesc, uavDesc)) {
          logMessage(LogLevel::Warn,
            "Skipping UAV creation - unsupported image UAV view type=%u fmt=%d samples=%u array=%u mip=%u",
            static_cast<uint32_t>(desc.type),
            static_cast<int>(imageDesc.format),
            imageDesc.sampleCount,
            imageDesc.arraySize,
            imageDesc.mipLevels);
        } else {
          // Taken only for a view that is made: the view frees what it holds.
          const D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = m_descriptorHeapManager.allocateCbvSrvUav();
          const D3D12_CPU_DESCRIPTOR_HANDLE nullUavHandle = m_descriptorHeapManager.allocateCbvSrvUav();
          uavDesc.Format = getValidatedViewFormat(uavDesc.Format, d3d12Image->getResource());
          m_device->CreateUnorderedAccessView(d3d12Image->getResource(), nullptr, &uavDesc, uavHandle);
          const D3D12_UNORDERED_ACCESS_VIEW_DESC nullUavDesc = nullViewDesc(uavDesc, imageDesc.format);
          m_device->CreateUnorderedAccessView(nullptr, nullptr, &nullUavDesc, nullUavHandle);
          view->setUavHandle(uavHandle);
          view->setNullUavHandle(nullUavHandle);
          {
            HRESULT chk = m_device->GetDeviceRemovedReason();
            if (FAILED(chk))
              logMessage(LogLevel::Error, "Device removed after CreateUAV! reason=0x%08X, imgFmt=%d, viewFmt=%d", chk, imageDesc.format, uavDesc.Format);
          }
        }
      } else {
        logMessage(LogLevel::Warn, "Skipping UAV creation - resource doesn't have ALLOW_UNORDERED_ACCESS (fmt=%d)", imageDesc.format);
      }
    }

    if (retargetable)
      view->trackRetarget(m_device.Get(), nullptr);
    if (srvViewLock.owns_lock())
      srvViewLock.unlock();
    return BackendRetireQueue::wrap<IBackendImageView>(m_retireQueue, std::move(view));
#else
    return std::make_shared<D3D12BackendImageView>(image, desc, nullptr);
#endif
  }

  void D3D12BackendDevice::attachIcb(ShaderResourceLimits& limits, const void* bytecode, size_t size, uint64_t hash) {
#ifdef _WIN32
    if (!bytecode || !size)
      return;
    std::lock_guard<std::mutex> lock(m_icbMutex);
    if (m_icbBuffers.empty())
      return;
    if (!hash) {
      hash = 1469598103934665603ull;
      const auto* bytes = static_cast<const uint8_t*>(bytecode);
      for (size_t i = 0; i < size; i++)
        hash = fnv64Multiply(hash ^ bytes[i]);
    }
    auto it = m_icbBuffers.find(hash);
    if (it == m_icbBuffers.end() || !it->second.buffer)
      return;
    limits.icbSlot = it->second.slot;
    limits.icbAddress = it->second.address;
    limits.icbSize = it->second.size;
#else
    (void)limits; (void)bytecode; (void)size; (void)hash;
#endif
  }

  Rc<IBackendShader> D3D12BackendDevice::createShader(const ShaderDesc& desc) {
    auto shader = std::make_shared<D3D12BackendShader>(desc);
#ifdef _WIN32
    // Diagnostics (dxup_dump_shaders.txt): the application's bytecode of
    // every shader, for benchmarks outside the game.
    static const bool dumpShaders = markerFileEnabled("dxup_dump_shaders.txt");
    if (dumpShaders && desc.bytecode && desc.bytecodeSize) {
      static const char* const stages[] = { "vs", "hs", "ds", "gs", "ps", "cs" };
      const uint32_t stage = static_cast<uint32_t>(desc.stage);
      char name[80];
      snprintf(name, sizeof(name), "dxup_shaders\\%s_%016llx.dxbc", stage < 6 ? stages[stage] : "xs",
        static_cast<unsigned long long>(shader->getBytecodeHash()));
      const std::wstring folder = dxupSiblingPathW("dxup_shaders");
      if (!folder.empty()) {
        CreateDirectoryW(folder.c_str(), nullptr);
        const std::wstring path = dxupSiblingPathW(name);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
          if (FILE* file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO)) {
            fwrite(desc.bytecode, 1, desc.bytecodeSize, file);
            fclose(file);
          }
        }
      }
    }
    // The immediate constant buffer's contents in video memory, once per
    // shader code.
    if (shader->getIcbSlot() != UINT32_MAX) {
      std::lock_guard<std::mutex> lock(m_icbMutex);
      auto& entry = m_icbBuffers[shader->getBytecodeHash()];
      if (!entry.buffer) {
        const auto& data = shader->getIcbData();
        std::vector<uint8_t> padded(((data.size() * 4) + 255) & ~size_t(255), 0);
        std::memcpy(padded.data(), data.data(), data.size() * 4);
        BufferDesc bufferDesc;
        bufferDesc.size = padded.size();
        bufferDesc.usage = ResourceUsage::Default;
        bufferDesc.bindFlags = BindFlags::ConstantBuffer;
        auto buffer = createBuffer(bufferDesc);
        if (buffer) {
          if (auto ctx = beginInitialData()) {
            // The initial data (and its lock) ends also when the upload throws.
            try {
              ctx->updateBuffer(buffer.get(), 0, padded.size(), padded.data());
            } catch (...) {
              endInitialData(ctx.get());
              throw;
            }
            endInitialData(ctx.get());
          }
          entry.slot = shader->getIcbSlot();
          entry.address = static_cast<D3D12BackendBuffer*>(buffer.get())->getGpuAddress();
          entry.size = uint32_t(padded.size());
          entry.buffer = std::move(buffer);
        }
      }
    }
    // Stored pipelines whose shaders all exist now compile in the background.
    // A pixel shader whose stored pipelines all shade per sample, or all per
    // pixel (SGSSAA switched, or other draws supersampled since a change),
    // gets the other variant warmed on its own.
    bool stored = false, perPixelMissing = false, perSampleMissing = false;
    if (!m_storedPipelines.empty() && desc.stage != ShaderStage::Compute && shader->getBytecodeSize())
      stored = noteShaderCreated(shader, perPixelMissing, perSampleMissing);
    if (!stored)
      warmShaderStage(shader);
    else if (perPixelMissing || perSampleMissing)
      warmShaderStage(shader, perPixelMissing, perSampleMissing);
#endif
    return shader;
  }


#ifdef _WIN32
  // SEH wrapper for CreateGraphicsPipelineState - must be in a plain function
  // without C++ objects that need unwinding on the stack
  __declspec(noinline)
  static HRESULT safeCreateGraphicsPSO(
      ID3D12Device* device,
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC* psoDesc,
      ID3D12PipelineState** ppPSO) {
    // Check device health first — if removed, calling CreateGraphicsPipelineState crashes the driver
    HRESULT removeReason = device->GetDeviceRemovedReason();
    if (FAILED(removeReason)) {
      logMessage(LogLevel::Error, "safeCreateGraphicsPSO: device already removed (0x%08X), skipping PSO creation", removeReason);
      *ppPSO = nullptr;
      return removeReason;
    }
    logMessage(LogLevel::Debug, "safeCreateGraphicsPSO: ENTERING tid=%lu device=%p numRT=%u",
      static_cast<unsigned long>(GetCurrentThreadId()), device, psoDesc->NumRenderTargets);
    HRESULT hr = E_FAIL;
    __try {
      hr = device->CreateGraphicsPipelineState(psoDesc, IID_PPV_ARGS(ppPSO));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
      logMessage(LogLevel::Error, "CreateGraphicsPipelineState CRASHED (SEH exception 0x%08X)", GetExceptionCode());
      *ppPSO = nullptr;
      hr = E_FAIL;
    }
    logMessage(LogLevel::Debug, "safeCreateGraphicsPSO: EXITING hr=0x%08X pso=%p", hr, *ppPSO);
    return hr;
  }

  // The same for compute pipelines, which also start from cached blobs a
  // driver may choke on.
  __declspec(noinline)
  static HRESULT safeCreateComputePSO(
      ID3D12Device* device,
      const D3D12_COMPUTE_PIPELINE_STATE_DESC* psoDesc,
      ID3D12PipelineState** ppPSO) {
    *ppPSO = nullptr;
    HRESULT removeReason = device->GetDeviceRemovedReason();
    if (FAILED(removeReason)) {
      logMessage(LogLevel::Error, "safeCreateComputePSO: device already removed (0x%08X), skipping PSO creation", removeReason);
      return removeReason;
    }
    HRESULT hr = E_FAIL;
    __try {
      hr = device->CreateComputePipelineState(psoDesc, IID_PPV_ARGS(ppPSO));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
      logMessage(LogLevel::Error, "CreateComputePipelineState CRASHED (SEH exception 0x%08X)", GetExceptionCode());
      *ppPSO = nullptr;
      hr = E_FAIL;
    }
    return hr;
  }
#endif

#ifdef _WIN32
  // ============================================================
  // Shader stage warm-up
  // ============================================================
  // Direct3D 11 drivers compile a shader when the application creates it;
  // Direct3D 12 compiles pipelines, on the first draw that needs one (NVIDIA:
  // ~8 ms each, a stutter for every new shader). The driver keeps what it
  // compiled per stage: once a pipeline of the shader with any partner and
  // state exists, the real one takes ~0.2 ms. New vertex and pixel shaders
  // get such a pipeline on a compiler thread, behind the urgent work.
  void D3D12BackendDevice::warmShaderStage(const std::shared_ptr<D3D12BackendShader>& shader, bool perPixel, bool perSample) {
    const ShaderStage stage = shader->getStage();
    if ((stage != ShaderStage::Vertex && stage != ShaderStage::Pixel) || !shader->getBytecodeSize()
     || m_compileThreads.empty() || !m_graphicsRootSignature)
      return;
    const auto* code = static_cast<const uint8_t*>(shader->getBytecode());
    auto bytecode = std::make_shared<std::vector<uint8_t>>(code, code + shader->getBytecodeSize());
    const uint32_t traits = shader->getPixelShaderTraits();
    const UserOverrides& overrides = UserOverrides::get();
    if (stage != ShaderStage::Pixel || overrides.sgssaa == SgssaaMode::Off || overrides.msaaSamples <= 1)
      perSample = false;
    if (!perPixel && !perSample)
      return;
    auto job = std::make_shared<CompileJob>();
    job->run = [this, stage, bytecode, traits, perPixel, perSample] {
      compileStageProbe(stage, *bytecode, traits, perPixel, perSample);
    };
    enqueueCompile(std::move(job), false);
  }

  D3D12BackendDevice::ProbeBytecode D3D12BackendDevice::compileProbe(const char* source, size_t size,
                                                                     const char* name, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3DCompile(source, size, name, nullptr, nullptr, "main", target, 0, 0,
          blob.GetAddressOf(), errors.GetAddressOf())) || !blob)
      return nullptr;
    const auto* code = static_cast<const uint8_t*>(blob->GetBufferPointer());
    return std::make_shared<const std::vector<uint8_t>>(code, code + blob->GetBufferSize());
  }

  // A vertex shader that writes what the pixel shader reads, in the same
  // order (the compiler packs both sides alike); null when an input needs
  // more than plain values.
  D3D12BackendDevice::ProbeBytecode D3D12BackendDevice::probePartnerVs(const dxbc::Signature& pixelInputs) {
    std::string source = "struct O {\n";
    bool position = false;
    uint32_t index = 0;
    for (const auto& element : pixelInputs.elements) {
      switch (element.systemValue) {
        case D3D_NAME_UNDEFINED: break;
        case D3D_NAME_POSITION: position = true; source += "  float4 p : SV_Position;\n"; continue;
        case D3D_NAME_PRIMITIVE_ID: case D3D_NAME_IS_FRONT_FACE: case D3D_NAME_SAMPLE_INDEX:
        case D3D_NAME_COVERAGE: case D3D_NAME_INNER_COVERAGE: continue;
        default: return nullptr;
      }
      uint32_t components = 0;
      for (uint32_t bit = 0; bit < 4; bit++)
        components += (element.mask >> bit) & 1;
      const char* type = element.componentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint"
                       : element.componentType == D3D_REGISTER_COMPONENT_SINT32 ? "int" : "float";
      char line[160];
      snprintf(line, sizeof(line), "  %s%u e%u : %s%u;\n", type, components ? components : 4u, index++,
        element.name.c_str(), element.semanticIndex);
      source += line;
    }
    if (!position)
      source += "  float4 p : SV_Position;\n";
    source += "};\nO main() { O o = (O)0; return o; }\n";

    std::lock_guard<std::mutex> lock(m_probeMutex);
    auto it = m_probeVs.find(source);
    if (it != m_probeVs.end())
      return it->second;
    ProbeBytecode code = compileProbe(source.data(), source.size(), "probe_vs", "vs_5_0");
    if (m_probeVs.size() < 4096)
      m_probeVs.emplace(source, code);
    return code;
  }

  void D3D12BackendDevice::compileStageProbe(ShaderStage stage, const std::vector<uint8_t>& bytecode,
                                             uint32_t pixelShaderTraits, bool perPixel, bool perSample) {
    if (m_stopCompileThreads || FAILED(m_device->GetDeviceRemovedReason()))
      return;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_graphicsRootSignature.Get();
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc.Count = 1;
    for (auto& target : desc.BlendState.RenderTarget)
      target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    dxbc::Signature signature;
    ProbeBytecode partner;

    if (stage == ShaderStage::Vertex) {
      // Every attribute from one buffer as four 32-bit components.
      if (dxbc::readSignature(bytecode.data(), bytecode.size(), false, signature)) {
        for (const auto& element : signature.elements) {
          if (element.systemValue != D3D_NAME_UNDEFINED)
            continue;
          D3D12_INPUT_ELEMENT_DESC input = {};
          input.SemanticName = element.name.c_str();
          input.SemanticIndex = element.semanticIndex;
          input.Format = element.componentType == D3D_REGISTER_COMPONENT_UINT32 ? DXGI_FORMAT_R32G32B32A32_UINT
                       : element.componentType == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                       : DXGI_FORMAT_R32G32B32A32_FLOAT;
          input.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
          inputs.push_back(input);
        }
      }
      {
        std::lock_guard<std::mutex> lock(m_probeMutex);
        if (!m_probePsCompiled) {
          static const char kSource[] = "float4 main() : SV_Target { return 0; }";
          m_probePs = compileProbe(kSource, sizeof(kSource) - 1, "probe_ps", "ps_5_0");
          m_probePsCompiled = true;
        }
        partner = m_probePs;
      }
      if (!partner)
        return;
      desc.VS = { bytecode.data(), bytecode.size() };
      desc.PS = { partner->data(), partner->size() };
      desc.InputLayout = { inputs.data(), UINT(inputs.size()) };
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else {
      if (!dxbc::readSignature(bytecode.data(), bytecode.size(), false, signature)
       || !(partner = probePartnerVs(signature)))
        return;
      desc.VS = { partner->data(), partner->size() };
      desc.PS = { bytecode.data(), bytecode.size() };
      // Targets of the output types the shader writes, a depth buffer for
      // depth and stencil reference outputs.
      dxbc::Signature outputs;
      if (dxbc::readSignature(bytecode.data(), bytecode.size(), true, outputs)) {
        for (const auto& element : outputs.elements) {
          // The signature names targets SV_Target with no system value.
          const bool target = element.systemValue == D3D_NAME_TARGET
            || (element.systemValue == D3D_NAME_UNDEFINED && _stricmp(element.name.c_str(), "SV_Target") == 0);
          if (target && element.reg < 8) {
            desc.NumRenderTargets = (std::max)(desc.NumRenderTargets, element.reg + 1);
            desc.RTVFormats[element.reg] = element.componentType == D3D_REGISTER_COMPONENT_UINT32
                ? DXGI_FORMAT_R32G32B32A32_UINT
              : element.componentType == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
              : DXGI_FORMAT_R8G8B8A8_UNORM;
          } else if (element.systemValue == D3D_NAME_DEPTH || element.systemValue == D3D_NAME_DEPTH_GREATER_EQUAL
                  || element.systemValue == D3D_NAME_DEPTH_LESS_EQUAL || element.systemValue == D3D_NAME_STENCIL_REF
                  || _strnicmp(element.name.c_str(), "SV_Depth", 8) == 0 || _stricmp(element.name.c_str(), "SV_StencilRef") == 0) {
            desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            desc.DepthStencilState.DepthEnable = TRUE;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
          }
        }
      }
      for (uint32_t i = 0; i < desc.NumRenderTargets; i++) {
        if (desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN)
          desc.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
      }
    }

    ComPtr<ID3D12PipelineState> pso;
    if (perPixel && FAILED(safeCreateGraphicsPSO(m_device.Get(), &desc, pso.GetAddressOf())))
      return;
    if (perPixel)
      noteDriverStages(stage == ShaderStage::Vertex ? desc.VS : D3D12_SHADER_BYTECODE{},
                       stage == ShaderStage::Pixel ? desc.PS : D3D12_SHADER_BYTECODE{});
    // Supersampled draws run the pixel shader per sample: that variant too.
    const UserOverrides& overrides = UserOverrides::get();
    std::vector<uint8_t> sampleFrequency;
    if (perSample && stage == ShaderStage::Pixel && overrides.sgssaa != SgssaaMode::Off && overrides.msaaSamples > 1
     && (pixelShaderTraits & PixelShaderInterpolates)
     && !(pixelShaderTraits & (PixelShaderWritesUavs | PixelShaderReadsCoverage))
     && (overrides.sgssaa == SgssaaMode::Full || (pixelShaderTraits & PixelShaderDiscards))
     && dxbc::makeSampleFrequency(bytecode.data(), bytecode.size(), sampleFrequency)) {
      desc.PS = { sampleFrequency.data(), sampleFrequency.size() };
      desc.SampleDesc.Count = overrides.msaaSamples;
      if (SUCCEEDED(safeCreateGraphicsPSO(m_device.Get(), &desc, pso.ReleaseAndGetAddressOf())))
        noteDriverStages({}, desc.PS);
    }
    m_psoStats.warmed.fetch_add(1, std::memory_order_relaxed);
  }
#endif
#ifdef _WIN32
  D3D12BackendDevice::PsoCacheKey D3D12BackendDevice::graphicsPipelineKey(const GraphicsPipelineDesc& desc) const {
    // Build a stable cache key from pipeline content. Pointer-based keys can
    // alias after a D3D11 shader or input layout is destroyed and its address
    // is reused for a different object.
    auto hashMem = [](const void* data, size_t sz) -> uint64_t {
      const uint8_t* p = static_cast<const uint8_t*>(data);
      uint64_t h = 1469598103934665603ull;
      for (size_t i = 0; i < sz; i++) { h ^= p[i]; h = fnv64Multiply(h); }
      return h;
    };

    auto appendHash = [](uint64_t& hash, const void* data, size_t size) {
      const auto* bytes = static_cast<const uint8_t*>(data);
      for (size_t i = 0; i < size; i++) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash = fnv64Multiply(hash);
      }
    };

    auto bytecodeHash = [&](const void* bytecode, size_t bytecodeSize, uint64_t suppliedHash) {
      return suppliedHash || !bytecode || !bytecodeSize
        ? suppliedHash
        : hashMem(bytecode, bytecodeSize);
    };

    auto appendBool = [&](uint64_t& hash, bool value) {
      const uint8_t normalized = value ? 1u : 0u;
      appendHash(hash, &normalized, sizeof(normalized));
    };

    auto appendUint32 = [&](uint64_t& hash, uint32_t value) {
      appendHash(hash, &value, sizeof(value));
    };


    uint64_t inputLayoutHash = desc.inputLayoutHash;
    if (!inputLayoutHash && desc.inputLayout.elements && desc.inputLayout.numElements) {
      inputLayoutHash = 1469598103934665603ull;
      appendHash(inputLayoutHash, &desc.inputLayout.numElements, sizeof(desc.inputLayout.numElements));
      for (uint32_t i = 0; i < desc.inputLayout.numElements; i++) {
        const auto& element = desc.inputLayout.elements[i];
        const size_t semanticLength = element.semanticName ? std::strlen(element.semanticName) : 0;
        appendHash(inputLayoutHash, &semanticLength, sizeof(semanticLength));
        if (semanticLength)
          appendHash(inputLayoutHash, element.semanticName, semanticLength);
        appendHash(inputLayoutHash, &element.semanticIndex, sizeof(element.semanticIndex));
        appendHash(inputLayoutHash, &element.format, sizeof(element.format));
        appendHash(inputLayoutHash, &element.inputSlot, sizeof(element.inputSlot));
        appendHash(inputLayoutHash, &element.byteOffset, sizeof(element.byteOffset));
        appendHash(inputLayoutHash, &element.inputClass, sizeof(element.inputClass));
        appendHash(inputLayoutHash, &element.instanceDataStepRate, sizeof(element.instanceDataStepRate));
      }
    }

    uint64_t soHash = 1469598103934665603ull;
    appendHash(soHash, &desc.numSOEntries, sizeof(desc.numSOEntries));
    for (uint32_t i = 0; i < desc.numSOEntries && desc.soEntries; i++) {
      const auto& entry = desc.soEntries[i];
      const size_t semanticLength = entry.semanticName ? std::strlen(entry.semanticName) : 0;
      appendHash(soHash, &semanticLength, sizeof(semanticLength));
      if (semanticLength)
        appendHash(soHash, entry.semanticName, semanticLength);
      appendHash(soHash, &entry.stream, sizeof(entry.stream));
      appendHash(soHash, &entry.semanticIndex, sizeof(entry.semanticIndex));
      appendHash(soHash, &entry.startComponent, sizeof(entry.startComponent));
      appendHash(soHash, &entry.componentCount, sizeof(entry.componentCount));
      appendHash(soHash, &entry.outputSlot, sizeof(entry.outputSlot));
    }
    appendHash(soHash, &desc.numSOStrides, sizeof(desc.numSOStrides));
    if (desc.soBufferStrides && desc.numSOStrides)
      appendHash(soHash, desc.soBufferStrides, desc.numSOStrides * sizeof(desc.soBufferStrides[0]));
    appendHash(soHash, &desc.rasterizedStream, sizeof(desc.rasterizedStream));

    PsoCacheKey cacheKey = {};
    cacheKey.vsHash = bytecodeHash(desc.vsBytecode, desc.vsBytecodeSize, desc.vsBytecodeHash);
    cacheKey.psHash = bytecodeHash(desc.psBytecode, desc.psBytecodeSize, desc.psBytecodeHash);
    cacheKey.gsHash = bytecodeHash(desc.gsBytecode, desc.gsBytecodeSize, desc.gsBytecodeHash);
    cacheKey.hsHash = bytecodeHash(desc.hsBytecode, desc.hsBytecodeSize, desc.hsBytecodeHash);
    cacheKey.dsHash = bytecodeHash(desc.dsBytecode, desc.dsBytecodeSize, desc.dsBytecodeHash);
    cacheKey.vsSize = desc.vsBytecodeSize;
    cacheKey.psSize = desc.psBytecodeSize;
    cacheKey.gsSize = desc.gsBytecodeSize;
    cacheKey.hsSize = desc.hsBytecodeSize;
    cacheKey.dsSize = desc.dsBytecodeSize;
    cacheKey.inputLayoutHash = inputLayoutHash;
    cacheKey.numInputElements = desc.inputLayout.numElements;
    cacheKey.blendHash = desc.blendStateHash
      ? desc.blendStateHash : state_hash::blend(desc.blendState);
    cacheKey.rsHash = desc.rasterizerStateHash
      ? desc.rasterizerStateHash : state_hash::rasterizer(desc.rasterizerState);
    cacheKey.dssHash = desc.depthStencilStateHash
      ? desc.depthStencilStateHash : state_hash::depthStencil(desc.depthStencilState);
    cacheKey.topology = desc.topology;
    cacheKey.stripCutIndexType = desc.stripCutIndexType;
    cacheKey.numRenderTargets = desc.numRenderTargets;
    for (uint32_t i = 0; i < 8; i++) cacheKey.rtvFormats[i] = desc.rtvFormats[i];
    cacheKey.dsvFormat = desc.dsvFormat;
    cacheKey.sampleCount = desc.sampleCount;
    cacheKey.sampleMask = desc.sampleMask;
    cacheKey.soHash = soHash;
    cacheKey.sampleShading = desc.sampleShading ? 1u : 0u;
    cacheKey.sampleQuality = desc.sampleCount > 1 ? desc.sampleQuality : 0u;
    return cacheKey;
  }

  namespace {
    // The channels of a render target format a pixel shader output reaches.
    uint8_t renderTargetChannels(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT:
          return 0x1;
        case DXGI_FORMAT_A8_UNORM:
          return 0x8;
        case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT:
          return 0x3;
        case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
          return 0x7;
        default:
          return 0xf;
      }
    }

    // What of each pixel shader output (o0-o7) reaches its target: the
    // format's channels the write mask lets through, alpha also when the
    // blend or alpha to coverage reads it; nothing without a target (depth
    // only passes whose shader also computes a color: 2.6x the GPU time of
    // Direct3D 11 on NVIDIA). False when every output stays whole
    // (dual-source blending reads o1 whole).
    bool pixelOutputMasks(const GraphicsPipelineDesc& desc, uint8_t masks[8]) {
      const RenderTargetBlendDesc& first = desc.blendState.renderTargets[0];
      auto dualSource = [](BlendFactor factor) {
        return factor == BlendFactor::Src1Color || factor == BlendFactor::InvSrc1Color
          || factor == BlendFactor::Src1Alpha || factor == BlendFactor::InvSrc1Alpha;
      };
      auto readsAlpha = [](BlendFactor factor) {
        return factor == BlendFactor::SrcAlpha || factor == BlendFactor::InvSrcAlpha || factor == BlendFactor::SrcAlphaSat;
      };
      if (first.blendEnable && (dualSource(first.srcBlend) || dualSource(first.dstBlend)
       || dualSource(first.srcBlendAlpha) || dualSource(first.dstBlendAlpha)))
        return false;
      bool any = false;
      for (uint32_t i = 0; i < 8; i++) {
        if (i >= desc.numRenderTargets || desc.rtvFormats[i] == DXGI_FORMAT_UNKNOWN) {
          masks[i] = i == 0 && desc.blendState.alphaToCoverageEnable ? 0x8 : 0x0;
          any = true;
          continue;
        }
        const RenderTargetBlendDesc& target = desc.blendState.independentBlendEnable ? desc.blendState.renderTargets[i] : first;
        uint8_t mask = renderTargetChannels(desc.rtvFormats[i]) & target.writeMask;
        if ((target.blendEnable && (readsAlpha(target.srcBlend) || readsAlpha(target.dstBlend)))
         || (i == 0 && desc.blendState.alphaToCoverageEnable))
          mask |= 0x8;
        masks[i] = mask;
        any = any || mask != 0xf;
      }
      return any;
    }

    // A pipeline description that owns what it points to: the application
    // may destroy shaders and input layouts while the pipeline compiles.
    struct OwnedGraphicsPipelineDesc {
      GraphicsPipelineDesc desc;
      std::vector<uint8_t> bytecode[5];
      std::vector<InputElementDesc> elements;
      std::vector<GraphicsPipelineDesc::SOEntry> soEntries;
      std::vector<uint32_t> soStrides;
      std::deque<std::string> names;

      explicit OwnedGraphicsPipelineDesc(const GraphicsPipelineDesc& source)
      : desc(source) {
        auto own = [](const void*& code, size_t size, std::vector<uint8_t>& storage) {
          if (!code || !size)
            return;
          const auto* bytes = static_cast<const uint8_t*>(code);
          storage.assign(bytes, bytes + size);
          code = storage.data();
        };
        own(desc.vsBytecode, desc.vsBytecodeSize, bytecode[0]);
        own(desc.psBytecode, desc.psBytecodeSize, bytecode[1]);
        own(desc.gsBytecode, desc.gsBytecodeSize, bytecode[2]);
        own(desc.hsBytecode, desc.hsBytecodeSize, bytecode[3]);
        own(desc.dsBytecode, desc.dsBytecodeSize, bytecode[4]);
        auto ownName = [this](const char* name) -> const char* {
          if (!name)
            return nullptr;
          names.emplace_back(name);
          return names.back().c_str();
        };
        if (desc.inputLayout.elements && desc.inputLayout.numElements) {
          elements.assign(desc.inputLayout.elements, desc.inputLayout.elements + desc.inputLayout.numElements);
          for (auto& element : elements)
            element.semanticName = ownName(element.semanticName);
          desc.inputLayout.elements = elements.data();
        }
        if (desc.soEntries && desc.numSOEntries) {
          soEntries.assign(desc.soEntries, desc.soEntries + desc.numSOEntries);
          for (auto& entry : soEntries)
            entry.semanticName = ownName(entry.semanticName);
          desc.soEntries = soEntries.data();
        }
        if (desc.soBufferStrides && desc.numSOStrides) {
          soStrides.assign(desc.soBufferStrides, desc.soBufferStrides + desc.numSOStrides);
          desc.soBufferStrides = soStrides.data();
        }
      }
    };
  }
#endif

  Rc<IBackendPipeline> D3D12BackendDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    HitchScope hitch("createGraphicsPipeline");
#ifdef _WIN32
    const PsoCacheKey cacheKey = graphicsPipelineKey(desc);
    int64_t waitStart = 0;
    {
      std::unique_lock<std::mutex> lock(m_psoCacheMutex);
      for (;;) {
        auto it = m_psoCache.find(cacheKey);
        if (it != m_psoCache.end()) {
          if (waitStart)
            notePsoWait(waitStart);
          return it->second;
        }
        if (!waitStart && hitchTraceEnabled())
          waitStart = hitchTimestamp();
        if (!m_compilingPsos.count(cacheKey))
          break;
        // Queued and not started: compile it here rather than wait behind
        // the queue. Otherwise another thread compiles it: wait for that.
        auto queued = m_queuedPsoJobs.find(cacheKey);
        if (queued != m_queuedPsoJobs.end() && !queued->second->claimed.exchange(true)) {
          auto job = queued->second;
          lock.unlock();
          job->run();
          lock.lock();
          continue;
        }
        m_psoCompiledCv.wait(lock);
      }
      m_compilingPsos.insert(cacheKey);
    }
    Rc<IBackendPipeline> pipeline = compileGraphicsPipeline(desc, cacheKey);
    if (waitStart)
      notePsoWait(waitStart);
    return pipeline;
#else
    return std::make_shared<D3D12BackendGraphicsPipeline>(desc.topology);
#endif
  }

  Rc<IBackendPipeline> D3D12BackendDevice::requestGraphicsPipeline(const GraphicsPipelineDesc& desc, bool& pending) {
    pending = false;
#ifdef _WIN32
    if (m_compileThreads.empty())
      return createGraphicsPipeline(desc);
    const PsoCacheKey cacheKey = graphicsPipelineKey(desc);
    std::shared_ptr<CompileJob> queuedJob;
    {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      auto it = m_psoCache.find(cacheKey);
      if (it != m_psoCache.end())
        return it->second;
      pending = true;
      if (!m_compilingPsos.count(cacheKey)) {
        auto owned = std::make_shared<OwnedGraphicsPipelineDesc>(desc);
        auto job = std::make_shared<CompileJob>();
        job->run = [this, owned, cacheKey] { compileGraphicsPipeline(owned->desc, cacheKey); };
        m_compilingPsos.insert(cacheKey);
        m_queuedPsoJobs.emplace(cacheKey, job);
        queuedJob = std::move(job);
      } else {
        // A precompiled pipeline a draw waits for now goes first.
        auto queued = m_queuedPsoJobs.find(cacheKey);
        if (queued != m_queuedPsoJobs.end() && !queued->second->claimed.load() && !queued->second->bumped.exchange(true))
          enqueueCompile(queued->second);
        return nullptr;
      }
    }
    if (enqueueCompile(queuedJob) || queuedJob->claimed.exchange(true))
      return nullptr;
    // No compiler thread takes it any more.
    pending = false;
    return compileGraphicsPipeline(desc, cacheKey);
#else
    return createGraphicsPipeline(desc);
#endif
  }

  Rc<IBackendPipeline> D3D12BackendDevice::createGraphicsPipelineDeferred(const GraphicsPipelineDesc& desc) {
#ifdef _WIN32
    if (m_compileThreads.empty())
      return createGraphicsPipeline(desc);
    const PsoCacheKey cacheKey = graphicsPipelineKey(desc);
    std::shared_ptr<CompileJob> queuedJob;
    Rc<IBackendPipeline> pending;
    {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      auto cached = m_psoCache.find(cacheKey);
      if (cached != m_psoCache.end())
        return cached->second;
      auto known = m_pendingPsos.find(cacheKey);
      if (known != m_pendingPsos.end())
        return known->second;

      auto owned = std::make_shared<OwnedGraphicsPipelineDesc>(desc);
      auto pipeline = std::make_shared<D3D12BackendGraphicsPipeline>(
        ComPtr<ID3D12PipelineState>(), m_graphicsRootSignature.Get(), desc.topology);
      pipeline->m_pending = true;
      pipeline->m_pendingDesc = owned;
      pending = pipeline;
      m_pendingPsos.emplace(cacheKey, pending);
      if (!m_compilingPsos.count(cacheKey)) {
        // In front of the precompiled pipelines and warm-ups: a draw needs it.
        auto job = std::make_shared<CompileJob>();
        job->run = [this, owned, cacheKey] { compileGraphicsPipeline(owned->desc, cacheKey); };
        m_compilingPsos.insert(cacheKey);
        m_queuedPsoJobs.emplace(cacheKey, job);
        queuedJob = std::move(job);
      } else {
        auto queued = m_queuedPsoJobs.find(cacheKey);
        if (queued != m_queuedPsoJobs.end() && !queued->second->claimed.load() && !queued->second->bumped.exchange(true))
          enqueueCompile(queued->second);
      }
    }
    // Without compiler threads (the device goes away) the bind compiles it.
    if (queuedJob)
      enqueueCompile(queuedJob);
    return pending;
#else
    return createGraphicsPipeline(desc);
#endif
  }

#ifdef _WIN32
  D3D12BackendGraphicsPipeline* D3D12BackendDevice::resolvePipeline(D3D12BackendGraphicsPipeline* pipeline) {
    if (auto* compiled = pipeline->m_resolved.load(std::memory_order_acquire))
      return compiled;
    if (pipeline->m_failed.load(std::memory_order_acquire))
      return nullptr;
    std::lock_guard<std::mutex> lock(pipeline->m_resolveMutex);
    if (!pipeline->m_pendingDesc)
      return pipeline->m_failed.load(std::memory_order_acquire) ? nullptr : pipeline->m_resolved.load(std::memory_order_acquire);
    // Waits for the compiler thread, or compiles it here if none started it.
    auto owned = std::static_pointer_cast<OwnedGraphicsPipelineDesc>(pipeline->m_pendingDesc);
    Rc<IBackendPipeline> compiled = createGraphicsPipeline(owned->desc);
    auto* result = static_cast<D3D12BackendGraphicsPipeline*>(compiled.get());
    if (result)
      pipeline->m_resolved.store(result, std::memory_order_release);
    else
      pipeline->m_failed.store(true, std::memory_order_release);
    pipeline->m_pendingDesc.reset();
    return result;
  }

  Rc<IBackendPipeline> D3D12BackendDevice::compileGraphicsPipeline(const GraphicsPipelineDesc& source, const PsoCacheKey& key) {
    // D3D11 streams out of the vertex shader through a "geometry shader"
    // made of vertex shader code (CreateGeometryShaderWithStreamOutput);
    // D3D12 takes the declaration without a geometry shader.
    GraphicsPipelineDesc withoutGs;
    const bool vertexStreamOut = source.gsBytecode && dxbc::programType(source.gsBytecode, source.gsBytecodeSize) == 1;
    if (vertexStreamOut) {
      withoutGs = source;
      withoutGs.gsBytecode = nullptr;
      withoutGs.gsBytecodeSize = 0;
    }
    const GraphicsPipelineDesc& desc = vertexStreamOut ? withoutGs : source;
    const int64_t start = hitchTraceEnabled() ? hitchTimestamp() : 0;
    Rc<IBackendPipeline> pipeline;
    try {
      pipeline = buildGraphicsPipeline(desc, key);
    } catch (const std::exception& e) {
      logMessage(LogLevel::Error, "createGraphicsPipeline: %s", e.what());
    }
    if (start) {
      const uint64_t us = notePsoCompile(start);
      if (us >= 4000)
        logSlowPipeline(desc, us);
    }
    // The description as the application made it, which the key stands for:
    // a later run then finds the record and precompiles the same pipeline.
    if (pipeline)
      storePipelineRecord(source, key);
    {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      // Failures stay cached too: the state is not tried again for every draw.
      auto [cached, inserted] = m_psoCache.emplace(key, pipeline);
      if (!inserted)
        pipeline = cached->second;
      m_compilingPsos.erase(key);
      m_queuedPsoJobs.erase(key);
    }
    m_psoCompiledCv.notify_all();
    return pipeline;
  }

  Rc<IBackendPipeline> D3D12BackendDevice::buildGraphicsPipeline(const GraphicsPipelineDesc& desc, const PsoCacheKey& cacheKey,
                                                                 bool masksLater) {
    // Compiler threads count too.
    static std::atomic<int> gfxPipeCreateLogCount{0};
    if (gfxPipeCreateLogCount < 128) {
      size_t cacheSize = 0;
      {
        std::lock_guard<std::mutex> lock(m_psoCacheMutex);
        cacheSize = m_psoCache.size();
      }
      logMessage(LogLevel::Info,
        "createGfxPipeline: CACHE MISS rootSig=%p vs=%p(%zu) ps=%p(%zu) numRT=%u dsvFmt=%u topo=%u sample=%u cacheSize=%zu",
        m_graphicsRootSignature.Get(),
        desc.vsBytecode, desc.vsBytecodeSize,
        desc.psBytecode, desc.psBytecodeSize,
        desc.numRenderTargets,
        static_cast<uint32_t>(desc.dsvFormat),
        static_cast<uint32_t>(desc.topology),
        desc.sampleCount,
        cacheSize);
      gfxPipeCreateLogCount++;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_graphicsRootSignature.Get();

    // Shaders - DXBC directly usable
    if (desc.vsBytecode)
      psoDesc.VS = { desc.vsBytecode, desc.vsBytecodeSize };
    if (desc.psBytecode)
      psoDesc.PS = { desc.psBytecode, desc.psBytecodeSize };
    if (desc.gsBytecode)
      psoDesc.GS = { desc.gsBytecode, desc.gsBytecodeSize };
    if (desc.hsBytecode)
      psoDesc.HS = { desc.hsBytecode, desc.hsBytecodeSize };
    if (desc.dsBytecode)
      psoDesc.DS = { desc.dsBytecode, desc.dsBytecodeSize };

    // Input Layout
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    std::deque<std::string> missingInputNames;
    uint32_t nullVertexSlot = UINT32_MAX;
    std::vector<uint8_t> linkedPs, linkedVs;
    if (desc.inputLayout.elements && desc.inputLayout.numElements > 0) {
      inputElements.resize(desc.inputLayout.numElements);
      for (uint32_t i = 0; i < desc.inputLayout.numElements; i++) {
        auto& src = desc.inputLayout.elements[i];
        auto& dst = inputElements[i];
        dst.SemanticName         = src.semanticName;
        dst.SemanticIndex        = src.semanticIndex;
        dst.Format               = src.format;
        dst.InputSlot            = src.inputSlot;
        dst.AlignedByteOffset    = src.byteOffset;
        dst.InputSlotClass       = (src.inputClass == InputClassification::PerVertex)
          ? D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
          : D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        dst.InstanceDataStepRate = src.instanceDataStepRate;
      }
    }

    // Vertex shader inputs the input layout lacks read (0, 0, 0, 1) in
    // D3D11; D3D12 refuses such pipelines. They read a vertex buffer slot
    // the layout leaves free, which the context keeps unbound for them.
    dxbc::Signature vsInputs;
    if (desc.vsBytecode && dxbc::readSignature(desc.vsBytecode, desc.vsBytecodeSize, false, vsInputs)) {
      uint32_t usedSlots = 0;
      for (const auto& element : inputElements)
        usedSlots |= 1u << (element.InputSlot & 31);
      for (const auto& input : vsInputs.elements) {
        if (input.systemValue != 0)
          continue;
        bool provided = false;
        for (const auto& element : inputElements) {
          provided |= element.SemanticIndex == input.semanticIndex
                   && element.SemanticName && _stricmp(element.SemanticName, input.name.c_str()) == 0;
        }
        if (provided)
          continue;
        for (uint32_t slot = 32; nullVertexSlot == UINT32_MAX && slot-- > 0;) {
          if (!(usedSlots & (1u << slot)))
            nullVertexSlot = slot;
        }
        if (nullVertexSlot == UINT32_MAX)
          break;
        missingInputNames.push_back(input.name);
        D3D12_INPUT_ELEMENT_DESC element = {};
        element.SemanticName = missingInputNames.back().c_str();
        element.SemanticIndex = input.semanticIndex;
        element.Format = input.componentType == 1 ? DXGI_FORMAT_R32_UINT
                       : input.componentType == 2 ? DXGI_FORMAT_R32_SINT
                       : DXGI_FORMAT_R32_FLOAT;
        element.InputSlot = nullVertexSlot;
        element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        inputElements.push_back(element);
      }
    }
    if (!inputElements.empty()) {
      psoDesc.InputLayout.pInputElementDescs = inputElements.data();
      psoDesc.InputLayout.NumElements        = static_cast<UINT>(inputElements.size());
    }

    // D3D11 hands interpolants to the pixel shader by register: an input
    // reads what the stage before wrote to its register, whatever the
    // semantic. D3D12 links by semantic and refuses pipelines where they
    // differ; the pixel shader's inputs take the semantics of the outputs
    // in their registers.
    const void* lastStageCode = desc.gsBytecode ? desc.gsBytecode : desc.dsBytecode ? desc.dsBytecode : desc.vsBytecode;
    const size_t lastStageSize = desc.gsBytecode ? desc.gsBytecodeSize : desc.dsBytecode ? desc.dsBytecodeSize : desc.vsBytecodeSize;
    dxbc::Signature psInputs, lastOutputs;
    if (desc.psBytecode && lastStageCode
     && dxbc::readSignature(desc.psBytecode, desc.psBytecodeSize, false, psInputs)
     && dxbc::readSignature(lastStageCode, lastStageSize, true, lastOutputs)) {
      const uint32_t stream = desc.gsBytecode && desc.rasterizedStream != D3D12_SO_NO_RASTERIZED_STREAM
        ? desc.rasterizedStream : 0u;
      dxbc::Signature linked = psInputs;
      bool changed = false;
      std::vector<dxbc::SignatureElement> constantOutputs;
      for (auto& input : linked.elements) {
        if (input.systemValue != 0)
          continue;
        const dxbc::SignatureElement* source = nullptr;
        bool registerWritten = false;
        for (const auto& output : lastOutputs.elements) {
          registerWritten |= output.stream == stream && output.reg == input.reg;
          if (!source && output.stream == stream && output.reg == input.reg && (output.mask & input.mask))
            source = &output;
        }
        // A register nothing writes reads (0, 0, 0, 1): the vertex shader
        // writes that to a new output the input is linked to.
        if (!registerWritten && !desc.gsBytecode && !desc.dsBytecode) {
          input.name = "DXUP_CONSTANT";
          input.semanticIndex = input.reg;
          dxbc::SignatureElement output = input;
          output.rwMask = 0;
          output.stream = 0;
          output.minPrecision = 0;
          constantOutputs.push_back(output);
          changed = true;
          continue;
        }
        if (!source || source->systemValue != 0)
          continue;
        if (source->semanticIndex == input.semanticIndex && _stricmp(source->name.c_str(), input.name.c_str()) == 0)
          continue;
        input.name = source->name;
        input.semanticIndex = source->semanticIndex;
        changed = true;
      }
      // Inputs packed into one register would end up with one name.
      for (size_t i = 0; changed && i < linked.elements.size(); i++) {
        for (size_t j = i + 1; j < linked.elements.size(); j++) {
          if (linked.elements[i].semanticIndex == linked.elements[j].semanticIndex
           && _stricmp(linked.elements[i].name.c_str(), linked.elements[j].name.c_str()) == 0)
            changed = false;
        }
      }
      if (changed && !constantOutputs.empty()
       && !dxbc::addConstantOutputs(desc.vsBytecode, desc.vsBytecodeSize, constantOutputs, linkedVs))
        changed = false;
      if (changed && dxbc::replaceChunk(desc.psBytecode, desc.psBytecodeSize, linked.chunk,
                                        dxbc::writeSignature(linked), linkedPs)) {
        psoDesc.PS = { linkedPs.data(), linkedPs.size() };
        if (!linkedVs.empty())
          psoDesc.VS = { linkedVs.data(), linkedVs.size() };
      }
    }

    // Supersampling: the pixel shader interpolates at and runs for each
    // sample. Shaders with nothing interpolated stay as they are.
    std::vector<uint8_t> sampleFrequencyPs;
    if (desc.sampleShading && psoDesc.PS.pShaderBytecode
     && dxbc::makeSampleFrequency(psoDesc.PS.pShaderBytecode, psoDesc.PS.BytecodeLength, sampleFrequencyPs))
      psoDesc.PS = { sampleFrequencyPs.data(), sampleFrequencyPs.size() };

    // The stages without masked outputs: what warm-ups compiled.
    const D3D12_SHADER_BYTECODE unmaskedVs = psoDesc.VS, unmaskedPs = psoDesc.PS;

    // What no target takes leaves the pixel shader (d3d12.pixelOutputMasks,
    // see dxbc::maskOutputWrites).
    std::vector<uint8_t> maskedPs;
    bool outputsMasked = false;
    uint8_t outputMasks[8] = {};
    if (UserOverrides::get().pixelOutputMasks && psoDesc.PS.pShaderBytecode && pixelOutputMasks(desc, outputMasks)
     && dxbc::maskOutputWrites(psoDesc.PS.pShaderBytecode, psoDesc.PS.BytecodeLength, outputMasks, 8, maskedPs)) {
      psoDesc.PS = { maskedPs.data(), maskedPs.size() };
      outputsMasked = true;
    }
    // Likewise vertex shader outputs the pixel shader does not read (all but
    // the system values without one: shadow passes), when the vertex shader
    // feeds the rasterizer and nothing streams out.
    std::vector<uint8_t> maskedVs;
    uint8_t vertexMasks[32] = {};
    if (UserOverrides::get().pixelOutputMasks && psoDesc.VS.pShaderBytecode && !desc.gsBytecode && !desc.hsBytecode
     && !desc.dsBytecode && !desc.numSOEntries
     && dxbc::vertexOutputMasks(psoDesc.VS.pShaderBytecode, psoDesc.VS.BytecodeLength, psoDesc.PS.pShaderBytecode,
                                psoDesc.PS.BytecodeLength, vertexMasks)
     && dxbc::maskOutputWrites(psoDesc.VS.pShaderBytecode, psoDesc.VS.BytecodeLength, vertexMasks, 32, maskedVs)) {
      psoDesc.VS = { maskedVs.data(), maskedVs.size() };
      outputsMasked = true;
    }

    // Blend State
    psoDesc.BlendState.AlphaToCoverageEnable  = desc.blendState.alphaToCoverageEnable;
    psoDesc.BlendState.IndependentBlendEnable = desc.blendState.independentBlendEnable;
    for (int i = 0; i < 8; i++) {
      auto& src = desc.blendState.renderTargets[i];
      auto& dst = psoDesc.BlendState.RenderTarget[i];
      dst.BlendEnable           = src.blendEnable;
      dst.SrcBlend              = static_cast<D3D12_BLEND>(static_cast<int>(src.srcBlend) + 1);
      dst.DestBlend             = static_cast<D3D12_BLEND>(static_cast<int>(src.dstBlend) + 1);
      dst.BlendOp               = static_cast<D3D12_BLEND_OP>(static_cast<int>(src.blendOp) + 1);
      dst.SrcBlendAlpha         = static_cast<D3D12_BLEND>(static_cast<int>(src.srcBlendAlpha) + 1);
      dst.DestBlendAlpha        = static_cast<D3D12_BLEND>(static_cast<int>(src.dstBlendAlpha) + 1);
      dst.BlendOpAlpha          = static_cast<D3D12_BLEND_OP>(static_cast<int>(src.blendOpAlpha) + 1);
      dst.RenderTargetWriteMask = src.writeMask;
      dst.LogicOpEnable         = src.logicOpEnable;
      dst.LogicOp               = static_cast<D3D12_LOGIC_OP>(src.logicOp);
    }

    // Rasterizer State
    psoDesc.RasterizerState.FillMode              = (desc.rasterizerState.fillMode == FillMode::Wireframe)
      ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = static_cast<D3D12_CULL_MODE>(static_cast<int>(desc.rasterizerState.cullMode) + 1);
    psoDesc.RasterizerState.FrontCounterClockwise = desc.rasterizerState.frontCounterClockwise;
    psoDesc.RasterizerState.DepthBias             = desc.rasterizerState.depthBias;
    psoDesc.RasterizerState.DepthBiasClamp        = desc.rasterizerState.depthBiasClamp;
    psoDesc.RasterizerState.SlopeScaledDepthBias  = desc.rasterizerState.slopeScaledDepthBias;
    psoDesc.RasterizerState.DepthClipEnable       = desc.rasterizerState.depthClipEnable;
    psoDesc.RasterizerState.MultisampleEnable     = desc.rasterizerState.multisampleEnable;
    psoDesc.RasterizerState.AntialiasedLineEnable = desc.rasterizerState.antialiasedLineEnable;
    psoDesc.RasterizerState.ForcedSampleCount     = desc.rasterizerState.forcedSampleCount;
    psoDesc.RasterizerState.ConservativeRaster    = desc.rasterizerState.conservativeRaster
      ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Depth Stencil State
    psoDesc.DepthStencilState.DepthEnable    = desc.depthStencilState.depthEnable;
    psoDesc.DepthStencilState.DepthWriteMask = desc.depthStencilState.depthWriteEnable
      ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc      = static_cast<D3D12_COMPARISON_FUNC>(
      static_cast<int>(desc.depthStencilState.depthFunc) + 1);
    psoDesc.DepthStencilState.StencilEnable  = desc.depthStencilState.stencilEnable;
    psoDesc.DepthStencilState.StencilReadMask  = desc.depthStencilState.stencilReadMask;
    psoDesc.DepthStencilState.StencilWriteMask = desc.depthStencilState.stencilWriteMask;

    auto convertStencilOp = [](const StencilOpDesc& s) -> D3D12_DEPTH_STENCILOP_DESC {
      D3D12_DEPTH_STENCILOP_DESC r = {};
      r.StencilFailOp      = static_cast<D3D12_STENCIL_OP>(static_cast<int>(s.stencilFailOp) + 1);
      r.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(static_cast<int>(s.stencilDepthFailOp) + 1);
      r.StencilPassOp      = static_cast<D3D12_STENCIL_OP>(static_cast<int>(s.stencilPassOp) + 1);
      r.StencilFunc        = static_cast<D3D12_COMPARISON_FUNC>(static_cast<int>(s.stencilFunc) + 1);
      return r;
    };

    psoDesc.DepthStencilState.FrontFace = convertStencilOp(desc.depthStencilState.frontFace);
    psoDesc.DepthStencilState.BackFace  = convertStencilOp(desc.depthStencilState.backFace);

    logMessage(LogLevel::Info, "createGfxPSO-step6: render targets numRT=%u rtvFmt[0]=%u rtvFmt[1]=%u rtvFmt[2]=%u rtvFmt[3]=%u",
      desc.numRenderTargets,
      desc.numRenderTargets > 0 ? static_cast<uint32_t>(desc.rtvFormats[0]) : 0u,
      desc.numRenderTargets > 1 ? static_cast<uint32_t>(desc.rtvFormats[1]) : 0u,
      desc.numRenderTargets > 2 ? static_cast<uint32_t>(desc.rtvFormats[2]) : 0u,
      desc.numRenderTargets > 3 ? static_cast<uint32_t>(desc.rtvFormats[3]) : 0u);
    // Render targets
    psoDesc.NumRenderTargets = desc.numRenderTargets;
    for (uint32_t i = 0; i < desc.numRenderTargets; i++)
      psoDesc.RTVFormats[i] = desc.rtvFormats[i];
    psoDesc.DSVFormat = desc.dsvFormat;

    psoDesc.SampleDesc.Count   = desc.sampleCount;
    psoDesc.SampleDesc.Quality = desc.sampleCount > 1 ? desc.sampleQuality : 0u;
    psoDesc.SampleMask         = desc.sampleMask;

    // Stream output
    std::vector<D3D12_SO_DECLARATION_ENTRY> soDecl;
    std::vector<std::string> soSemanticNames;
    if (desc.numSOEntries > 0 && desc.soEntries) {
      soDecl.resize(desc.numSOEntries);
      soSemanticNames.resize(desc.numSOEntries);
      for (uint32_t i = 0; i < desc.numSOEntries; i++) {
        soSemanticNames[i] = desc.soEntries[i].semanticName ? desc.soEntries[i].semanticName : "";
        soDecl[i].Stream         = desc.soEntries[i].stream;
        // A NULL name is a gap of componentCount components.
        soDecl[i].SemanticName   = desc.soEntries[i].semanticName ? soSemanticNames[i].c_str() : nullptr;
        soDecl[i].SemanticIndex  = desc.soEntries[i].semanticIndex;
        soDecl[i].StartComponent = desc.soEntries[i].startComponent;
        soDecl[i].ComponentCount = desc.soEntries[i].componentCount;
        soDecl[i].OutputSlot     = desc.soEntries[i].outputSlot;
      }
      psoDesc.StreamOutput.pSODeclaration   = soDecl.data();
      psoDesc.StreamOutput.NumEntries       = static_cast<UINT>(soDecl.size());
      psoDesc.StreamOutput.pBufferStrides   = desc.soBufferStrides;
      psoDesc.StreamOutput.NumStrides       = desc.numSOStrides;
      psoDesc.StreamOutput.RasterizedStream = desc.rasterizedStream;
      // A rasterized stream without a position has nothing to rasterize;
      // Direct3D 11 takes it, D3D12 wants no rasterized stream then.
      const D3D12_SHADER_BYTECODE& last = psoDesc.GS.BytecodeLength ? psoDesc.GS
                                        : psoDesc.DS.BytecodeLength ? psoDesc.DS : psoDesc.VS;
      dxbc::Signature outputs;
      if (desc.rasterizedStream != D3D12_SO_NO_RASTERIZED_STREAM && last.pShaderBytecode
       && dxbc::readSignature(last.pShaderBytecode, last.BytecodeLength, true, outputs)) {
        bool position = false;
        for (const auto& element : outputs.elements)
          position |= element.systemValue == D3D_NAME_POSITION && element.stream == desc.rasterizedStream;
        if (!position)
          psoDesc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
      }
    }

    // Topology type
    if (desc.topology >= PrimitiveTopology::PatchList1
     && desc.topology <= PrimitiveTopology::PatchList32) {
      psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    } else {
      switch (desc.topology) {
        case PrimitiveTopology::PointList:
          psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
          break;
        case PrimitiveTopology::LineList:
        case PrimitiveTopology::LineStrip:
        case PrimitiveTopology::LineListAdj:
        case PrimitiveTopology::LineStripAdj:
          psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
          break;
        default:
          psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
          break;
      }
    }

    switch (desc.topology) {
      case PrimitiveTopology::LineStrip:
      case PrimitiveTopology::TriangleStrip:
      case PrimitiveTopology::LineStripAdj:
      case PrimitiveTopology::TriangleStripAdj:
        psoDesc.IBStripCutValue = desc.stripCutIndexType == IndexType::UInt32
          ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
          : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
        break;
      default:
        psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        break;
    }

    ComPtr<ID3D12PipelineState> pso;
    logMessage(LogLevel::Info, "createGfxPSO-step9: calling CreateGraphicsPipelineState device=%p rootSig=%p sampleMask=0x%X",
      m_device.Get(), psoDesc.pRootSignature, psoDesc.SampleMask);
    {
      // The whole fixed state, to compare pipelines of two programs.
      const D3D12_RENDER_TARGET_BLEND_DESC& b = psoDesc.BlendState.RenderTarget[0];
      const D3D12_RASTERIZER_DESC& r = psoDesc.RasterizerState;
      const D3D12_DEPTH_STENCIL_DESC& z = psoDesc.DepthStencilState;
      logMessage(LogLevel::Info, "createGfxPSO-state: blend0 %d %d/%d/%d %d/%d/%d mask 0x%x logic %d a2c %d independent %d | "
        "raster fill %d cull %d ccw %d bias %d/%g/%g clip %d ms %d aaline %d forced %u conservative %d | depth %d write %d func %d "
        "stencil %d | topo %d cut %d | vs %zu ps %zu gs %zu inputs %u samples %u",
        b.BlendEnable, b.SrcBlend, b.DestBlend, b.BlendOp, b.SrcBlendAlpha, b.DestBlendAlpha, b.BlendOpAlpha, b.RenderTargetWriteMask,
        b.LogicOpEnable, psoDesc.BlendState.AlphaToCoverageEnable, psoDesc.BlendState.IndependentBlendEnable,
        r.FillMode, r.CullMode, r.FrontCounterClockwise, r.DepthBias, r.DepthBiasClamp, r.SlopeScaledDepthBias, r.DepthClipEnable,
        r.MultisampleEnable, r.AntialiasedLineEnable, r.ForcedSampleCount, r.ConservativeRaster, z.DepthEnable, z.DepthWriteMask,
        z.DepthFunc, z.StencilEnable, psoDesc.PrimitiveTopologyType, psoDesc.IBStripCutValue, psoDesc.VS.BytecodeLength,
        psoDesc.PS.BytecodeLength, psoDesc.GS.BytecodeLength, psoDesc.InputLayout.NumElements, psoDesc.SampleDesc.Count);
    }

    // The in-memory key is entirely content-based and therefore also provides
    // a stable cross-run key for the driver PSO blob cache.
    // Pipelines with masked pixel shader outputs get blobs of their own.
    uint64_t psoKeyHash = PsoDiskKeyHash{}(cacheKey) ^ kRootSignatureLayoutVersion
      ^ (outputsMasked ? kPixelOutputMaskVersion : 0);

    const std::vector<uint8_t> diskBlob = takePsoDiskBlob(psoKeyHash);
    bool hadDiskCache = !diskBlob.empty();
    if (hadDiskCache) {
      psoDesc.CachedPSO.pCachedBlob = diskBlob.data();
      psoDesc.CachedPSO.CachedBlobSizeInBytes = diskBlob.size();
    }

    // Masked outputs make shaders the driver has not seen: such a pipeline
    // compiles from scratch (Trine 2: 5-17 ms on the first draws) although a
    // warm-up compiled the shaders as they are. It starts without the masks,
    // which the driver finds compiled (~0.2 ms), and a compiler thread builds
    // the masked one to replace it (as DXVK links pipeline libraries first
    // and compiles the optimized pipeline in the background).
    masksLater = masksLater && outputsMasked && !hadDiskCache && !m_compileThreads.empty()
      && driverHasStages(unmaskedVs, unmaskedPs) && !driverHasStages(psoDesc.VS, psoDesc.PS);
    if (masksLater) {
      psoDesc.VS = unmaskedVs;
      psoDesc.PS = unmaskedPs;
    }

    ID3D12PipelineState* rawPso = nullptr;
    HRESULT hr = safeCreateGraphicsPSO(m_device.Get(), &psoDesc, &rawPso);

    // If cached blob was stale (driver update etc.), retry without it
    if (FAILED(hr) && hadDiskCache) {
      logMessage(LogLevel::Warn, "PSO disk cache: cached blob failed, retrying without cache hash=%llx",
        static_cast<unsigned long long>(psoKeyHash));
      psoDesc.CachedPSO = {};
      rawPso = nullptr;
      hr = safeCreateGraphicsPSO(m_device.Get(), &psoDesc, &rawPso);
      DeleteFileW(psoDiskPath(psoKeyHash).c_str());
      hadDiskCache = false;
    }

    logMessage(LogLevel::Debug, "createGfxPSO-postCall: hr=0x%08X rawPso=%p numRT=%u", hr, rawPso, desc.numRenderTargets);
    if (rawPso) pso.Attach(rawPso);
    logMessage(LogLevel::Debug, "createGfxPSO-postAttach: pso=%p", pso.Get());
    if (FAILED(hr) || !pso) {
      logMessage(LogLevel::Error, "CreateGraphicsPipelineState FAILED hr=0x%08X vs=%016llx ps=%016llx gs=%016llx",
        hr, static_cast<unsigned long long>(cacheKey.vsHash), static_cast<unsigned long long>(cacheKey.psHash),
        static_cast<unsigned long long>(cacheKey.gsHash));
      // What the stages and the layout declare, for finding out why.
      auto describe = [](const dxbc::Signature& signature) {
        std::string text;
        for (const auto& e : signature.elements) {
          char item[96];
          snprintf(item, sizeof(item), " %s%u@%u.%x/sv%u/t%u", e.name.c_str(), e.semanticIndex, e.reg, e.mask,
            e.systemValue, e.componentType);
          text += item;
        }
        return text;
      };
      std::string layoutText;
      for (const auto& element : inputElements) {
        char item[96];
        snprintf(item, sizeof(item), " %s%u/slot%u/fmt%u", element.SemanticName ? element.SemanticName : "?",
          element.SemanticIndex, element.InputSlot, static_cast<uint32_t>(element.Format));
        layoutText += item;
      }
      logMessage(LogLevel::Error, "  layout:%s", layoutText.c_str());
      logMessage(LogLevel::Error, "  vs inputs:%s", describe(vsInputs).c_str());
      logMessage(LogLevel::Error, "  last stage outputs:%s", describe(lastOutputs).c_str());
      logMessage(LogLevel::Error, "  ps inputs:%s (linked %d)", describe(psInputs).c_str(), linkedPs.empty() ? 0 : 1);
      logMessage(LogLevel::Error, "  rt %u fmt0 %u dsv %u samples %u gs %d hs %d ds %d", desc.numRenderTargets,
        static_cast<uint32_t>(desc.rtvFormats[0]), static_cast<uint32_t>(desc.dsvFormat), desc.sampleCount,
        desc.gsBytecode ? 1 : 0, desc.hsBytecode ? 1 : 0, desc.dsBytecode ? 1 : 0);
      return nullptr;
    }
    logMessage(LogLevel::Debug, "createGfxPSO-returning: OK rootSig=%p topo=%u", m_graphicsRootSignature.Get(), static_cast<uint32_t>(desc.topology));
    noteDriverStages(psoDesc.VS, psoDesc.PS);

    PipelineResourceLimits resourceLimits = {};
    uint32_t activeStageMask = 0;
    if (desc.vsBytecode)
      activeStageMask |= 1u << static_cast<uint32_t>(ShaderStage::Vertex);
    if (desc.hsBytecode)
      activeStageMask |= 1u << static_cast<uint32_t>(ShaderStage::Hull);
    if (desc.dsBytecode)
      activeStageMask |= 1u << static_cast<uint32_t>(ShaderStage::Domain);
    if (desc.gsBytecode)
      activeStageMask |= 1u << static_cast<uint32_t>(ShaderStage::Geometry);
    if (desc.psBytecode)
      activeStageMask |= 1u << static_cast<uint32_t>(ShaderStage::Pixel);

    if (shaderDescriptorLimitsEnabled()) {
      ShaderResourceLimits unusedStageLimits = {};
      unusedStageLimits.known = true;
      resourceLimits.fill(unusedStageLimits);
      if (desc.vsBytecode)
        resourceLimits[static_cast<uint32_t>(ShaderStage::Vertex)] = reflectShaderResourceLimits(desc.vsBytecode, desc.vsBytecodeSize);
      if (desc.hsBytecode)
        resourceLimits[static_cast<uint32_t>(ShaderStage::Hull)] = reflectShaderResourceLimits(desc.hsBytecode, desc.hsBytecodeSize);
      if (desc.dsBytecode)
        resourceLimits[static_cast<uint32_t>(ShaderStage::Domain)] = reflectShaderResourceLimits(desc.dsBytecode, desc.dsBytecodeSize);
      if (desc.gsBytecode)
        resourceLimits[static_cast<uint32_t>(ShaderStage::Geometry)] = reflectShaderResourceLimits(desc.gsBytecode, desc.gsBytecodeSize);
      if (desc.psBytecode)
        resourceLimits[static_cast<uint32_t>(ShaderStage::Pixel)] = reflectShaderResourceLimits(desc.psBytecode, desc.psBytecodeSize);
    }
    attachIcb(resourceLimits[static_cast<uint32_t>(ShaderStage::Vertex)], desc.vsBytecode, desc.vsBytecodeSize, desc.vsBytecodeHash);
    attachIcb(resourceLimits[static_cast<uint32_t>(ShaderStage::Hull)], desc.hsBytecode, desc.hsBytecodeSize, desc.hsBytecodeHash);
    attachIcb(resourceLimits[static_cast<uint32_t>(ShaderStage::Domain)], desc.dsBytecode, desc.dsBytecodeSize, desc.dsBytecodeHash);
    attachIcb(resourceLimits[static_cast<uint32_t>(ShaderStage::Geometry)], desc.gsBytecode, desc.gsBytecodeSize, desc.gsBytecodeHash);
    attachIcb(resourceLimits[static_cast<uint32_t>(ShaderStage::Pixel)], desc.psBytecode, desc.psBytecodeSize, desc.psBytecodeHash);

    auto pipeline = std::make_shared<D3D12BackendGraphicsPipeline>(
      std::move(pso), m_graphicsRootSignature.Get(), desc.topology, resourceLimits, activeStageMask);
    pipeline->setNullVertexSlot(nullVertexSlot);

    // GetCachedBlob and file writes can stall the render thread. Keep the PSO
    // alive and let the low-priority cache worker serialize it instead.
    // Pipelines that get their masks later store the masked one.
    if (masksLater)
      queueMaskedPipeline(desc, cacheKey, pipeline);
    else if (!hadDiskCache)
      deferPsoBlobWrite(psoKeyHash, pipeline->getPSO());
    else
      m_psoStats.diskHits.fetch_add(1, std::memory_order_relaxed);

    return pipeline;
  }

  void D3D12BackendDevice::queueMaskedPipeline(const GraphicsPipelineDesc& desc, const PsoCacheKey& key,
                                               const std::shared_ptr<D3D12BackendGraphicsPipeline>& pipeline) {
    auto owned = std::make_shared<OwnedGraphicsPipelineDesc>(desc);
    std::weak_ptr<D3D12BackendGraphicsPipeline> target = pipeline;
    auto job = std::make_shared<CompileJob>();
    // Behind the pipelines draws wait for and the warm-ups.
    job->run = [this, owned, key, target] {
      if (m_stopCompileThreads || target.expired())
        return;
      Rc<IBackendPipeline> masked;
      try {
        masked = buildGraphicsPipeline(owned->desc, key, false);
      } catch (const std::exception& e) {
        logMessage(LogLevel::Error, "masked pipeline: %s", e.what());
      }
      auto unmasked = target.lock();
      if (!masked || !unmasked)
        return;
      unmasked->replacePSO(static_cast<D3D12BackendGraphicsPipeline*>(masked.get())->m_pso);
      m_psoStats.masksLater.fetch_add(1, std::memory_order_relaxed);
    };
    enqueueCompile(std::move(job), false);
  }
#endif

  Rc<IBackendPipeline> D3D12BackendDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    HitchScope hitch("createComputePipeline");
#ifdef _WIN32
    // Several threads may create compute pipelines: each dump takes a
    // number of its own.
    static std::atomic<int> csCreateLogs{0};
    int csCreateLogCount = 0;
    if (computeShaderDumpEnabled()
     && desc.csBytecode
     && desc.csBytecodeSize >= 28
     && (csCreateLogCount = csCreateLogs.fetch_add(1, std::memory_order_relaxed)) < 3) {
      const uint8_t* bc = static_cast<const uint8_t*>(desc.csBytecode);
      const size_t bcSize = desc.csBytecodeSize;
      auto readU32 = [bc](size_t at) {
        uint32_t value = 0;
        std::memcpy(&value, bc + at, sizeof(value));
        return value;
      };
      logMessage(LogLevel::Info, "createComputePSO #%d: bytecodeSize=%zu magic=%c%c%c%c totalSize=%u",
        csCreateLogCount,
        bcSize,
        bc[0], bc[1], bc[2], bc[3],
        readU32(24));

      // Save first shader to file for offline analysis
      if (csCreateLogCount == 0) {
        if (FILE* sf = dxupOpenSibling("dxup_cs0.dxbc", "wb")) {
          fwrite(desc.csBytecode, 1, desc.csBytecodeSize, sf);
          fclose(sf);
          logMessage(LogLevel::Info, "  Saved shader to dxup_cs0.dxbc");
        }
      }

      // Find and dump SHEX chunk tokens (resource declarations). The
      // application's container is checked like any input: 64-bit offsets,
      // chunks cut to the bytes there are.
      if (bcSize >= 32) {
        const uint32_t chunkCount = readU32(28);
        if (32 + uint64_t(chunkCount) * 4 <= bcSize) {
          for (uint32_t i = 0; i < chunkCount && i < 20; i++) {
            const uint32_t offset = readU32(32 + size_t(i) * 4);
            if (uint64_t(offset) + 8 <= bcSize) {
              char tag[5] = {};
              memcpy(tag, bc + offset, 4);
              const uint32_t chunkSize = readU32(offset + 4);
              logMessage(LogLevel::Info, "  chunk[%u]: '%s' offset=%u size=%u", i, tag, offset, chunkSize);

              // Dump SHEX tokens
              if (memcmp(tag, "SHEX", 4) == 0 || memcmp(tag, "SHDR", 4) == 0) {
                const uint64_t available = (std::min)(uint64_t(chunkSize), uint64_t(bcSize) - offset - 8);
                const uint32_t tokenCount = uint32_t(available / 4);
                // Dump first 60 tokens (includes version, length, and all declarations)
                uint32_t dumpCount = tokenCount < 60 ? tokenCount : 60;
                for (uint32_t t = 0; t < dumpCount; t += 4) {
                  uint32_t end = t + 4 < dumpCount ? t + 4 : dumpCount;
                  char hexBuf[256] = {};
                  int pos = 0;
                  for (uint32_t tt = t; tt < end; tt++)
                    pos += snprintf(hexBuf + pos, sizeof(hexBuf) - size_t(pos), " %08X", readU32(offset + 8 + size_t(tt) * 4));
                  logMessage(LogLevel::Info, "  SHEX[%u]:%s", t, hexBuf);
                }
              }
            }
          }
        }
      }
    }

    if (!desc.csBytecode || !desc.csBytecodeSize)
      return nullptr;

    const ComputePsoCacheKey cacheKey = computePipelineKey(desc);
    {
      std::unique_lock<std::mutex> lock(m_psoCacheMutex);
      for (;;) {
        auto cached = m_computePsoCache.find(cacheKey);
        if (cached != m_computePsoCache.end())
          return cached->second;
        if (!m_compilingComputePsos.count(cacheKey))
          break;
        // As for graphics pipelines: run it here if no thread started it.
        auto queued = m_queuedComputeJobs.find(cacheKey);
        if (queued != m_queuedComputeJobs.end() && !queued->second->claimed.exchange(true)) {
          auto job = queued->second;
          lock.unlock();
          job->run();
          lock.lock();
          continue;
        }
        m_psoCompiledCv.wait(lock);
      }
      m_compilingComputePsos.insert(cacheKey);
    }
    return compileComputePipeline(desc, cacheKey);
#else
    return std::make_shared<D3D12BackendComputePipeline>();
#endif
  }

  void D3D12BackendDevice::requestComputePipeline(const ComputePipelineDesc& desc) {
#ifdef _WIN32
    if (m_compileThreads.empty() || !desc.csBytecode || !desc.csBytecodeSize)
      return;
    const ComputePsoCacheKey cacheKey = computePipelineKey(desc);
    // The application may destroy the shader while it compiles.
    const auto* code = static_cast<const uint8_t*>(desc.csBytecode);
    auto bytecode = std::make_shared<std::vector<uint8_t>>(code, code + desc.csBytecodeSize);
    ComputePipelineDesc owned = desc;
    owned.csBytecode = bytecode->data();
    auto job = std::make_shared<CompileJob>();
    job->run = [this, bytecode, owned, cacheKey] { compileComputePipeline(owned, cacheKey); };
    {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      if (m_computePsoCache.count(cacheKey) || !m_compilingComputePsos.insert(cacheKey).second)
        return;
      m_queuedComputeJobs.emplace(cacheKey, job);
    }
    if (!enqueueCompile(job) && !job->claimed.exchange(true))
      job->run();
#else
    (void)desc;
#endif
  }

#ifdef _WIN32
  D3D12BackendDevice::ComputePsoCacheKey D3D12BackendDevice::computePipelineKey(const ComputePipelineDesc& desc) const {
    uint64_t csHash = desc.csBytecodeHash;
    if (!csHash) {
      csHash = 1469598103934665603ull;
      const auto* bytes = static_cast<const uint8_t*>(desc.csBytecode);
      for (size_t i = 0; i < desc.csBytecodeSize; i++) {
        csHash ^= static_cast<uint64_t>(bytes[i]);
        csHash = fnv64Multiply(csHash);
      }
    }

    ComputePsoCacheKey cacheKey = {};
    cacheKey.csHash = csHash;
    cacheKey.csSize = static_cast<uint64_t>(desc.csBytecodeSize);
    return cacheKey;
  }

  Rc<IBackendPipeline> D3D12BackendDevice::compileComputePipeline(const ComputePipelineDesc& desc, const ComputePsoCacheKey& key) {
    const int64_t start = hitchTraceEnabled() ? hitchTimestamp() : 0;
    Rc<IBackendPipeline> pipeline;
    try {
      pipeline = buildComputePipeline(desc, key);
    } catch (const std::exception& e) {
      logMessage(LogLevel::Error, "createComputePipeline: %s", e.what());
    }
    if (start)
      notePsoCompile(start);
    {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      auto [cached, inserted] = m_computePsoCache.emplace(key, pipeline);
      if (!inserted)
        pipeline = cached->second;
      m_compilingComputePsos.erase(key);
      m_queuedComputeJobs.erase(key);
    }
    m_psoCompiledCv.notify_all();
    return pipeline;
  }

  Rc<IBackendPipeline> D3D12BackendDevice::buildComputePipeline(const ComputePipelineDesc& desc, const ComputePsoCacheKey& cacheKey) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_computeRootSignature.Get();
    psoDesc.CS = { desc.csBytecode, desc.csBytecodeSize };

    // Namespace compute entries away from the graphics-key hash while keeping
    // the existing on-disk filename format stable.
    uint64_t psoKeyHash = ComputePsoCacheHash::hash64(cacheKey)
      ^ 0xC04E50534F434143ull ^ kRootSignatureLayoutVersion;
    const std::vector<uint8_t> diskBlob = takePsoDiskBlob(psoKeyHash);
    bool hadDiskCache = !diskBlob.empty();
    if (hadDiskCache) {
      psoDesc.CachedPSO.pCachedBlob = diskBlob.data();
      psoDesc.CachedPSO.CachedBlobSizeInBytes = diskBlob.size();
    }

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = safeCreateComputePSO(m_device.Get(), &psoDesc, pso.ReleaseAndGetAddressOf());
    if (FAILED(hr) && hadDiskCache) {
      logMessage(LogLevel::Warn,
        "Compute PSO disk cache: cached blob failed, retrying without cache hash=%llx",
        static_cast<unsigned long long>(psoKeyHash));
      psoDesc.CachedPSO = {};
      hr = safeCreateComputePSO(m_device.Get(), &psoDesc, pso.ReleaseAndGetAddressOf());

      DeleteFileW(psoDiskPath(psoKeyHash).c_str());
      hadDiskCache = false;
    }
    if (FAILED(hr) || !pso) {
      logMessage(LogLevel::Error, "CreateComputePipelineState FAILED hr=0x%08X size=%zu", hr, desc.csBytecodeSize);
      return nullptr;
    }
    logMessage(LogLevel::Debug, "CreateComputePSO: OK pso=%p", pso.Get());

    ShaderResourceLimits resourceLimits = {};
    if (shaderDescriptorLimitsEnabled())
      resourceLimits = reflectShaderResourceLimits(desc.csBytecode, desc.csBytecodeSize);
    attachIcb(resourceLimits, desc.csBytecode, desc.csBytecodeSize, desc.csBytecodeHash);
    auto pipeline = std::make_shared<D3D12BackendComputePipeline>(
      std::move(pso), m_computeRootSignature.Get(), resourceLimits);
    pipeline->setShaderHash(cacheKey.csHash);
    // With DRED the shaders a hang report names can be looked at.
    if (markerFileEnabled("dxup_dred.txt")) {
      char name[64];
      snprintf(name, sizeof(name), "dxup_shaders\\cs_%016llx.dxbc", static_cast<unsigned long long>(cacheKey.csHash));
      const std::wstring folder = dxupSiblingPathW("dxup_shaders");
      if (!folder.empty()) {
        CreateDirectoryW(folder.c_str(), nullptr);
        if (FILE* file = _wfsopen(dxupSiblingPathW(name).c_str(), L"wb", _SH_DENYNO)) {
          fwrite(desc.csBytecode, 1, desc.csBytecodeSize, file);
          fclose(file);
        }
      }
    }

    if (!hadDiskCache)
      deferPsoBlobWrite(psoKeyHash, pipeline->getPSO());
    else
      m_psoStats.diskHits.fetch_add(1, std::memory_order_relaxed);

    return pipeline;
  }

  namespace {
    uint64_t microsecondsSince(int64_t start) {
      static const int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
      }();
      return uint64_t(hitchTimestamp() - start) * 1000000ull / uint64_t(frequency);
    }
  }

  uint64_t D3D12BackendDevice::notePsoCompile(int64_t start) {
    const uint64_t us = microsecondsSince(start);
    m_psoStats.compileUs.fetch_add(us, std::memory_order_relaxed);
    uint64_t max = m_psoStats.maxUs.load(std::memory_order_relaxed);
    while (us > max && !m_psoStats.maxUs.compare_exchange_weak(max, us, std::memory_order_relaxed)) {}
    m_psoStats.compiles.fetch_add(1, std::memory_order_relaxed);
    // While pipelines compile, the totals every two seconds (hitch trace):
    // games are often ended without destroying their device.
    if (hitchTraceEnabled()) {
      static const int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
      }();
      const int64_t now = hitchTimestamp();
      int64_t last = m_psoStats.loggedAt.load(std::memory_order_relaxed);
      if (now - last >= 2 * frequency && m_psoStats.loggedAt.compare_exchange_strong(last, now, std::memory_order_relaxed))
        logPsoStats("running");
    }
    return us;
  }

  uint64_t D3D12BackendDevice::stageHash(const void* bytecode, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++)
      hash = fnv64Multiply(hash ^ bytes[i]);
    return hash;
  }

  void D3D12BackendDevice::noteDriverStages(const D3D12_SHADER_BYTECODE& vs, const D3D12_SHADER_BYTECODE& ps) {
    const uint64_t vsHash = vs.pShaderBytecode ? stageHash(vs.pShaderBytecode, vs.BytecodeLength) : 0;
    const uint64_t psHash = ps.pShaderBytecode ? stageHash(ps.pShaderBytecode, ps.BytecodeLength) : 0;
    std::lock_guard<std::mutex> lock(m_probeMutex);
    if (m_warmedStages.size() >= 65536)
      return;
    if (vsHash)
      m_warmedStages.insert(vsHash);
    if (psHash)
      m_warmedStages.insert(psHash);
  }

  // Whether the driver compiled the given stages (absent ones count as
  // compiled).
  bool D3D12BackendDevice::driverHasStages(const D3D12_SHADER_BYTECODE& vs, const D3D12_SHADER_BYTECODE& ps) {
    const uint64_t vsHash = vs.pShaderBytecode ? stageHash(vs.pShaderBytecode, vs.BytecodeLength) : 0;
    const uint64_t psHash = ps.pShaderBytecode ? stageHash(ps.pShaderBytecode, ps.BytecodeLength) : 0;
    std::lock_guard<std::mutex> lock(m_probeMutex);
    return (!vsHash || m_warmedStages.count(vsHash)) && (!psHash || m_warmedStages.count(psHash));
  }

  // What a pipeline that compiled slowly had, and which of its stages a
  // warm-up had compiled before: tells which pipelines the warm-up misses.
  void D3D12BackendDevice::logSlowPipeline(const GraphicsPipelineDesc& desc, uint64_t us) {
    auto stage = [this](const void* code, size_t size) -> std::string {
      if (!code || !size)
        return "-";
      const uint64_t hash = stageHash(code, size);
      bool warmed;
      {
        std::lock_guard<std::mutex> lock(m_probeMutex);
        warmed = m_warmedStages.count(hash) != 0;
      }
      char text[40];
      snprintf(text, sizeof(text), "%016llx%s", static_cast<unsigned long long>(hash), warmed ? " warm" : " cold");
      return text;
    };
    std::string formats;
    for (uint32_t i = 0; i < desc.numRenderTargets && i < 8; i++)
      formats += (i ? "," : "") + std::to_string(uint32_t(desc.rtvFormats[i]));
    logMessage(LogLevel::Warn, "slow pipeline %.1f ms: vs %s, ps %s, gs %s, hs %s, ds %s, inputs %u, targets [%s], depth %u, samples %u%s, so %u",
      double(us) / 1000.0, stage(desc.vsBytecode, desc.vsBytecodeSize).c_str(), stage(desc.psBytecode, desc.psBytecodeSize).c_str(),
      stage(desc.gsBytecode, desc.gsBytecodeSize).c_str(), stage(desc.hsBytecode, desc.hsBytecodeSize).c_str(),
      stage(desc.dsBytecode, desc.dsBytecodeSize).c_str(), desc.inputLayout.numElements, formats.c_str(),
      uint32_t(desc.dsvFormat), desc.sampleCount, desc.sampleShading ? " per sample" : "", desc.numSOEntries);
  }

  void D3D12BackendDevice::notePsoWait(int64_t start) {
    m_psoStats.waitUs.fetch_add(microsecondsSince(start), std::memory_order_relaxed);
    m_psoStats.waits.fetch_add(1, std::memory_order_relaxed);
  }

  void D3D12BackendDevice::logPsoStats(const char* when) {
    if (!hitchTraceEnabled())
      return;
    logMessageImpl(LogLevel::Warn,
      "pso stats (%s): %u compiled (%u from disk cache, %u precompiled, %u shader stages warmed, %u masked later) in %.1f ms, slowest %.1f ms; draws waited %u times for %.1f ms",
      when, m_psoStats.compiles.load(), m_psoStats.diskHits.load(), m_psoStats.precompiled.load(), m_psoStats.warmed.load(),
      m_psoStats.masksLater.load(),
      m_psoStats.compileUs.load() / 1000.0,
      m_psoStats.maxUs.load() / 1000.0, m_psoStats.waits.load(), m_psoStats.waitUs.load() / 1000.0);
  }

  // ============================================================
  // Background compilation (d3d11.asyncShaders)
  // ============================================================

  void D3D12BackendDevice::startCompileThreads() {
    // A quarter of the CPU's threads: the application keeps the rest. While
    // more than kBurstBacklog jobs wait (a game creating its shaders while it
    // loads), up to half: the stages warm before the draws that need them.
    const unsigned cores = (std::max)(1u, std::thread::hardware_concurrency());
    const unsigned count = (std::min)(6u, (std::max)(1u, cores / 4));
    const unsigned burst = (std::min)(12u, (std::max)(count, cores / 2));
    for (unsigned i = 0; i < burst; i++) {
      try {
        const bool extra = i >= count;
        m_compileThreads.emplace_back([this, extra] { runCompileThread(extra); });
      } catch (const std::exception& e) {
        logMessage(LogLevel::Warn, "Async shaders: failed to start a compiler thread: %s", e.what());
        break;
      }
    }
    logMessage(LogLevel::Info, "Async shaders: %zu compiler threads", m_compileThreads.size());
  }

  void D3D12BackendDevice::runCompileThread(bool burst) {
    SetThreadDescription(GetCurrentThread(), burst ? L"dxup-compile-burst" : L"dxup-compile");
    // Below the application's threads: pipelines take longer, frames do not.
    SetThreadPriority(GetCurrentThread(), burst ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
      std::shared_ptr<CompileJob> job;
      {
        std::unique_lock<std::mutex> lock(m_compileMutex);
        (burst ? m_burstCv : m_compileCv).wait(lock, [this, burst] {
          return m_stopCompileThreads || (!m_compileJobs.empty() && (!burst || m_compileJobs.size() > kBurstBacklog));
        });
        if (m_stopCompileThreads)
          return;
        job = std::move(m_compileJobs.front());
        m_compileJobs.pop_front();
      }
      if (!job->claimed.exchange(true))
        job->run();
    }
  }

  bool D3D12BackendDevice::enqueueCompile(std::shared_ptr<CompileJob> job, bool urgent) {
    bool backlog = false;
    {
      std::lock_guard<std::mutex> lock(m_compileMutex);
      if (m_compileThreads.empty() || m_stopCompileThreads)
        return false;
      if (urgent)
        m_compileJobs.push_front(std::move(job));
      else
        m_compileJobs.push_back(std::move(job));
      backlog = m_compileJobs.size() > kBurstBacklog;
    }
    m_compileCv.notify_one();
    if (backlog)
      m_burstCv.notify_one();
    return true;
  }

  void D3D12BackendDevice::stopCompileThreads() {
    {
      std::lock_guard<std::mutex> lock(m_compileMutex);
      m_stopCompileThreads = true;
      m_compileJobs.clear();
    }
    m_compileCv.notify_all();
    m_burstCv.notify_all();
    for (auto& thread : m_compileThreads) {
      if (thread.joinable())
        thread.join();
    }
    // The dropped jobs never finish; whoever needs their pipelines compiles them.
    {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      m_compilingPsos.clear();
      m_compilingComputePsos.clear();
      m_queuedPsoJobs.clear();
      m_queuedComputeJobs.clear();
    }
    m_psoCompiledCv.notify_all();
  }
#endif

  Rc<IBackendQuery> D3D12BackendDevice::createQuery(const QueryDesc& desc) {
    return BackendRetireQueue::wrap<IBackendQuery>(m_retireQueue, std::make_shared<D3D12BackendQuery>(this, desc));
  }

  Rc<IBackendContext> D3D12BackendDevice::createContext() {
#ifdef _WIN32
    return std::make_shared<D3D12BackendContext>(this, false);
#else
    return nullptr;
#endif
  }

  Rc<IBackendContext> D3D12BackendDevice::createCopyContext() {
#ifdef _WIN32
    return std::make_shared<D3D12BackendContext>(this, true);
#else
    return nullptr;
#endif
  }

  Rc<IBackendContext> D3D12BackendDevice::beginInitialData() {
    // Held until endInitialData, which callers only call with a context:
    // without one (its creation threw or failed) the lock goes now.
    std::unique_lock<std::mutex> lock(m_initDataMutex);
    if (!m_initDataContext)
      m_initDataContext = createCopyContext();
    if (!m_initDataContext)
      return nullptr;
    lock.release();
    return m_initDataContext;
  }

  void D3D12BackendDevice::endInitialData(IBackendContext* context) {
    (void)context;
    // Taken over from beginInitialData; released also when the flush throws.
    std::unique_lock<std::mutex> lock(m_initDataMutex, std::adopt_lock);
    m_initDataPending.store(true, std::memory_order_release);
    // Loading many resources at once still submits now and then.
    if (++m_initDataRecorded >= 128 && m_initDataContext) {
      m_initDataContext->flush();
      m_initDataRecorded = 0;
      m_initDataPending.store(false, std::memory_order_release);
    }
  }

  void D3D12BackendDevice::submitInitialData() {
    if (!m_initDataPending.load(std::memory_order_acquire))
      return;
    std::lock_guard<std::mutex> lock(m_initDataMutex);
    if (m_initDataPending.load(std::memory_order_relaxed) && m_initDataContext) {
      m_initDataContext->flush();
      m_initDataRecorded = 0;
      m_initDataPending.store(false, std::memory_order_release);
    }
  }

  bool D3D12BackendDevice::checkFeatureSupport(BackendFeature feature) {
    switch (feature) {
      case BackendFeature::GeometryShader:
      case BackendFeature::TessellationShader:
      case BackendFeature::ComputeShader:
      case BackendFeature::MultiViewport:
      case BackendFeature::OcclusionQuery:
      case BackendFeature::TimestampQuery:
      case BackendFeature::PipelineStatisticsQuery:
      case BackendFeature::IndirectDraw:
        return true;
      case BackendFeature::StreamOutput:
        return true;
      default:
        return false;
    }
  }

  BackendLimits D3D12BackendDevice::getLimits() {
    BackendLimits limits = {};
    limits.maxTexture1DSize     = 16384;
    limits.maxTexture2DSize     = 16384;
    limits.maxTexture3DSize     = 2048;
    limits.maxTextureCubeSize   = 16384;
    limits.maxTextureArraySize  = 2048;
    limits.maxConstantBufferSize = 65536;
    limits.maxVertexBuffers     = 32;
    limits.maxRenderTargets     = 8;
    limits.maxViewports         = 16;
    limits.maxComputeThreadGroupSize[0] = 1024;
    limits.maxComputeThreadGroupSize[1] = 1024;
    limits.maxComputeThreadGroupSize[2] = 64;
    limits.maxComputeThreadGroupCount[0] = 65535;
    limits.maxComputeThreadGroupCount[1] = 65535;
    limits.maxComputeThreadGroupCount[2] = 65535;
    return limits;
  }

  uint32_t D3D12BackendDevice::checkMultisampleQualityLevels(DXGI_FORMAT format, uint32_t sampleCount) {
#ifdef _WIN32
    if (!m_device || sampleCount == 0) return 0;
    if (sampleCount == 1) return 1;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaaLevels = {};
    msaaLevels.Format = format;
    msaaLevels.SampleCount = sampleCount;
    HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                &msaaLevels, sizeof(msaaLevels));
    if (SUCCEEDED(hr))
      return msaaLevels.NumQualityLevels;
    return 0;
#else
    return (sampleCount == 1) ? 1 : 0;
#endif
  }

  // ============================================================
  // PSO Disk Cache
  // ============================================================

#ifdef _WIN32
  void D3D12BackendDevice::initPsoDiskCache() {
#ifdef _WIN32
    // dxup_pso_cache next to the DLL.
    // Wide paths: a game folder may hold any character.
    m_psoDiskCacheDir = dxupSiblingPathW("dxup_pso_cache");

    // Create directory if it doesn't exist
    CreateDirectoryW(m_psoDiskCacheDir.c_str(), nullptr);
    DWORD attr = m_psoDiskCacheDir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(m_psoDiskCacheDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      logMessage(LogLevel::Warn, "PSO disk cache: failed to create dir '%ls'",
        m_psoDiskCacheDir.c_str());
      m_psoDiskCacheDir.clear();
      return;
    }

    m_deferredPsoDiskWriteKeys.reserve(256);
    dropPsoBlobsOfOtherDrivers();
    if (!m_psoBlobsOff)
      loadPsoDiskCache();
    loadPipelineStateCache();

    try {
      m_psoDiskWriter = Thread([this] { runPsoDiskWriter(); });
    } catch (const std::exception& e) {
      logMessage(LogLevel::Warn, "PSO disk cache: failed to start background writer: %s", e.what());
    }

    logMessage(LogLevel::Info, "PSO disk cache: loaded %zu and listed %zu cached blobs from '%ls'",
      m_psoDiskBlobs.size(), m_psoDiskFiles.size(), m_psoDiskCacheDir.c_str());
#endif
  }

  static bool readPsoFile(const std::wstring& path, std::vector<uint8_t>& blob) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
      return false;
    auto sz = f.tellg();
    if (sz <= 0)
      return false;
    f.seekg(0);
    blob.resize(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(blob.data()), sz);
    if (f.gcount() == static_cast<std::streamsize>(sz))
      return true;
    blob.clear();
    return false;
  }

  std::wstring D3D12BackendDevice::psoDiskPath(uint64_t keyHash) const {
    wchar_t filename[64];
    swprintf_s(filename, L"%llx.pso", static_cast<unsigned long long>(keyHash));
    return m_psoDiskCacheDir + L"\\" + filename;
  }

  std::vector<uint8_t> D3D12BackendDevice::takePsoDiskBlob(uint64_t keyHash) {
    // Each key is compiled once and then served from the in-memory PSO cache,
    // so the blob is moved out rather than copied.
    {
      std::lock_guard<std::mutex> lock(m_psoDiskBlobMutex);
      auto it = m_psoDiskBlobs.find(keyHash);
      if (it != m_psoDiskBlobs.end()) {
        std::vector<uint8_t> blob = std::move(it->second);
        m_psoDiskBlobs.erase(it);
        return blob;
      }
      if (!m_psoDiskFiles.erase(keyHash))
        return {};
    }
    // Listed at start, read now (on a compiler thread, mostly).
    std::vector<uint8_t> blob;
    readPsoFile(psoDiskPath(keyHash), blob);
    return blob;
  }

  void D3D12BackendDevice::deferPsoBlobWrite(uint64_t keyHash, ID3D12PipelineState* pso) {
#ifdef _WIN32
    if (m_psoDiskCacheDir.empty() || !pso)
      return;

    if (!m_psoDiskWriter.joinable())
      return;

    std::lock_guard<std::mutex> lock(m_psoDiskWriteMutex);
    if (!m_deferredPsoDiskWriteKeys.insert(keyHash).second)
      return;

    DeferredPsoDiskWrite write = {};
    write.keyHash = keyHash;
    write.pso = pso;
    m_deferredPsoDiskWrites.push_back(std::move(write));
    m_psoDiskWriteCv.notify_one();
#else
    (void)keyHash;
    (void)pso;
#endif
  }

  void D3D12BackendDevice::runPsoDiskWriter() {
#ifdef _WIN32
    // Below the frame-critical threads, but not in background mode: its very
    // low I/O priority took ~250 ms per file while anything else used the
    // disk, and the device's destruction waits for the queue (a minute and
    // more after a session with a few hundred new pipelines).
    SetThreadDescription(GetCurrentThread(), L"dxup-pso-writer");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    for (;;) {
      DeferredPsoDiskWrite write;
      {
        std::unique_lock<std::mutex> lock(m_psoDiskWriteMutex);
        m_psoDiskWriteCv.wait(lock, [this] {
          return m_stopPsoDiskWriter || !m_deferredPsoDiskWrites.empty();
        });

        if (m_deferredPsoDiskWrites.empty()) {
          if (m_stopPsoDiskWriter)
            break;
          continue;
        }

        write = std::move(m_deferredPsoDiskWrites.front());
        m_deferredPsoDiskWrites.pop_front();
      }

      if (!write.record.empty())
        appendPipelineRecord(write.record);
      else
        savePsoBlobToDisk(write.keyHash, write.pso.Get());
    }
    closePipelineStateCache();
#endif
  }

  void D3D12BackendDevice::stopPsoDiskWriter() {
#ifdef _WIN32
    {
      std::lock_guard<std::mutex> lock(m_psoDiskWriteMutex);
      m_stopPsoDiskWriter = true;
    }
    m_psoDiskWriteCv.notify_one();

    if (m_psoDiskWriter.joinable())
      m_psoDiskWriter.join();
#endif
  }

  void D3D12BackendDevice::dropPsoBlobsOfOtherDrivers() {
    DXGI_ADAPTER_DESC1 desc = {};
    LARGE_INTEGER driver = {};
    if (!m_adapter || FAILED(m_adapter->GetDesc1(&desc))
     || FAILED(m_adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
      return;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      m_psoBlobsOff = true;
      return;
    }
    char id[96];
    snprintf(id, sizeof(id), "%04x %04x %08x %02x %llu", desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
             static_cast<unsigned long long>(driver.QuadPart));
    const std::wstring path = m_psoDiskCacheDir + L"\\driver.txt";
    std::string previous;
    {
      std::ifstream f(path, std::ios::binary);
      if (f)
        previous.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (previous == id)
      return;
    // Without the file: blobs of an earlier DXUP, from this driver mostly.
    if (!previous.empty()) {
      uint32_t removed = 0;
      WIN32_FIND_DATAW fd = {};
      HANDLE find = FindFirstFileW((m_psoDiskCacheDir + L"\\*.pso").c_str(), &fd);
      if (find != INVALID_HANDLE_VALUE) {
        do {
          if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileW((m_psoDiskCacheDir + L"\\" + fd.cFileName).c_str()))
            removed++;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
      }
      logMessage(LogLevel::Info, "PSO disk cache: another GPU or driver (%s), its %u pipeline blobs removed", id, removed);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f)
      f.write(id, std::streamsize(std::strlen(id)));
  }

  void D3D12BackendDevice::loadPsoDiskCache() {
#ifdef _WIN32
    if (m_psoDiskCacheDir.empty()) return;
    // Reading every file would hold up the start; the files are read when
    // their pipeline compiles, mostly on a compiler thread.
    const bool listOnly = true;

    std::wstring searchPath = m_psoDiskCacheDir + L"\\*.pso";
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

      // Filename is "hexhash.pso"
      std::wstring name(fd.cFileName);
      auto dotPos = name.rfind(L'.');
      if (dotPos == std::wstring::npos) continue;
      std::wstring stem = name.substr(0, dotPos);

      uint64_t hash = 0;
      try { hash = std::stoull(stem, nullptr, 16); }
      catch (...) { continue; }

      if (listOnly) {
        m_psoDiskFiles.insert(hash);
        continue;
      }
      std::vector<uint8_t> blob;
      if (readPsoFile(m_psoDiskCacheDir + L"\\" + fd.cFileName, blob))
        m_psoDiskBlobs[hash] = std::move(blob);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
#endif
  }

  void D3D12BackendDevice::savePsoBlobToDisk(uint64_t keyHash, ID3D12PipelineState* pso) {
#ifdef _WIN32
    if (m_psoDiskCacheDir.empty() || !pso || m_psoBlobsOff) return;

    ID3DBlob* rawBlob = nullptr;
    HRESULT hr = pso->GetCachedBlob(&rawBlob);
    if (FAILED(hr) || !rawBlob || rawBlob->GetBufferSize() == 0) {
      logMessage(LogLevel::Debug, "PSO disk cache: GetCachedBlob failed hr=0x%08X", hr);
      if (rawBlob) rawBlob->Release();
      return;
    }

    // Written beside it and renamed: a run that ends meanwhile, or another
    // process reading the cache, never finds a partial blob under the name.
    const std::wstring path = psoDiskPath(keyHash);
    const std::wstring temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_"
      + std::to_wstring(GetCurrentThreadId());
    bool written = false;
    {
      std::ofstream f(temp, std::ios::binary | std::ios::trunc);
      if (f) {
        f.write(static_cast<const char*>(rawBlob->GetBufferPointer()),
                static_cast<std::streamsize>(rawBlob->GetBufferSize()));
        f.close();
        written = !f.fail();
      }
    }
    if (written && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      logMessage(LogLevel::Debug, "PSO disk cache: saved %zu bytes as %016llx.pso",
        rawBlob->GetBufferSize(), static_cast<unsigned long long>(keyHash));
    } else {
      DeleteFileW(temp.c_str());
    }
    rawBlob->Release();
#endif
  }
#endif

}
