#include "d3d12_backend_context.h"
#include "d3d12_dxbc.h"
#include "d3d12_cs_thread.h"
#include "../util/util_types.h"
#include "../util/util_paths.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#endif

// Suppress unused parameter warnings on non-Windows builds
// where the #ifdef _WIN32 blocks leave parameters unused by design
#ifndef _WIN32
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

namespace dxup::d3d12 {

  // Diagnostic: skip GPU draw/dispatch ops beyond this limit per frame.
  // -1 = unlimited (default). Set to N to only execute the first N ops.
  static int g_gpuOpLimit = -1;
  static int g_gpuOpCounter = 0;  // reset at each flush()

#ifdef _WIN32
  namespace {

    bool dredEnabled() {
      static bool enabled = dxupSiblingExists("dxup_dred.txt");
      return enabled;
    }

    bool perfStatsEnabled() {
      static bool enabled = dxupSiblingExists("dxup_perf_stats.txt");
      return enabled;
    }

    bool infoDiagnosticsEnabled() {
      static bool enabled = dxupSiblingExists("dxup_info_log.txt");
      return enabled;
    }

    bool recentOpsEnabled() {
      static bool enabled = [] {
        char* value = nullptr;
        size_t valueSize = 0;
        bool result = _dupenv_s(&value, &valueSize, "DXUP_RECORD_RECENT_OPS") == 0
          && value
          && value[0] != '\0'
          && value[0] != '0';
        if (value)
          free(value);
        return result;
      }();
      return enabled;
    }

    bool asyncResetEnabled() {
      static bool enabled = !dxupSiblingExists("dxup_disable_async_reset.txt");
      return enabled;
    }

    bool immediateSmallUpdatesEnabled() {
      static bool enabled = dxupSiblingExists("dxup_immediate_small_updates.txt");
      return enabled;
    }

    bool barrierStatsEnabled() {
      static bool enabled = dxupSiblingExists("dxup_barrier_stats.txt");
      return enabled;
    }

    bool perfTimingEnabled() {
      static bool enabled = dxupSiblingExists("dxup_perf_timing.txt");
      return enabled;
    }

    inline double qpcElapsedMs(LARGE_INTEGER start, LARGE_INTEGER end, int64_t frequency) {
      return frequency > 0
        ? static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency)
        : 0.0;
    }

    bool commonPromotionEnabled() {
      static bool enabled = dxupSiblingExists("dxup_common_promotion.txt");
      return enabled;
    }

    inline uint64_t mixDescriptorTableFingerprint(uint64_t fingerprint, uint64_t value) {
      return fnv64Multiply(fingerprint ^ value);
    }

    inline uint64_t descriptorTableFingerprintSeed(uint32_t descriptorCount) {
      return mixDescriptorTableFingerprint(1469598103934665603ull, descriptorCount);
    }

    inline bool sameShaderResourceLimits(
        const ShaderResourceLimits& a,
        const ShaderResourceLimits& b) {
      return a.known == b.known
          && a.cbvCount == b.cbvCount
          && a.srvCount == b.srvCount
          && a.samplerCount == b.samplerCount
          && a.uavCount == b.uavCount
          && a.dimensions == b.dimensions
          && a.icbAddress == b.icbAddress;
    }

    constexpr D3D12_RESOURCE_STATES kReadOnlyBufferStates =
      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
      D3D12_RESOURCE_STATE_INDEX_BUFFER |
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
      D3D12_RESOURCE_STATE_COPY_SOURCE |
      D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
      D3D12_RESOURCE_STATE_DEPTH_READ |
      D3D12_RESOURCE_STATE_PREDICATION;

    inline bool isReadOnlyBufferState(D3D12_RESOURCE_STATES state) {
      return state != D3D12_RESOURCE_STATE_COMMON
        && state != D3D12_RESOURCE_STATE_PRESENT
        && (state & ~kReadOnlyBufferStates) == 0;
    }

    inline D3D12_RESOURCE_STATES mergeBufferReadStates(
        D3D12_RESOURCE_STATES currentState,
        D3D12_RESOURCE_STATES newState) {
      if (!isReadOnlyBufferState(currentState) || !isReadOnlyBufferState(newState))
        return newState;

      return static_cast<D3D12_RESOURCE_STATES>(currentState | newState);
    }

    // States in which an image may be written (memory statistics).
    constexpr D3D12_RESOURCE_STATES kImageWriteStates = D3D12_RESOURCE_STATE_RENDER_TARGET
      | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST
      | D3D12_RESOURCE_STATE_RESOLVE_DEST;

    inline bool isImageReadOnlyPromotableState(D3D12_RESOURCE_STATES state) {
      constexpr D3D12_RESOURCE_STATES kReadOnlyTextureStates =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_COPY_SOURCE;
      return state != D3D12_RESOURCE_STATE_COMMON
        && (state & ~kReadOnlyTextureStates) == 0;
    }

    inline bool isImageCommonPromotableState(
        const D3D12BackendImage* image,
        D3D12_RESOURCE_STATES    state) {
      if (!commonPromotionEnabled() || !image)
        return false;

      if (isImageReadOnlyPromotableState(state))
        return true;

      if (image->allowsSimultaneousAccess()) {
        constexpr D3D12_RESOURCE_STATES kSimultaneousTextureStates =
          D3D12_RESOURCE_STATE_RENDER_TARGET |
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
          D3D12_RESOURCE_STATE_COPY_DEST |
          D3D12_RESOURCE_STATE_RESOLVE_DEST |
          D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        return state != D3D12_RESOURCE_STATE_COMMON
          && (state & ~kSimultaneousTextureStates) == 0;
      }

      return state == D3D12_RESOURCE_STATE_COPY_DEST;
    }

    inline UINT getImageMipLevelCount(const ImageDesc& imageDesc) {
      return imageDesc.mipLevels ? imageDesc.mipLevels : 1u;
    }

    inline UINT getImageArrayCountForSubresources(const ImageDesc& imageDesc) {
      return imageDesc.type == ImageType::Tex3D
        ? 1u
        : (imageDesc.arraySize ? imageDesc.arraySize : 1u);
    }

    inline UINT getImageViewMipCountForTransition(const ImageViewDesc& viewDesc, const ImageDesc& imageDesc) {
      UINT totalMipLevels = getImageMipLevelCount(imageDesc);
      if (viewDesc.baseMipLevel >= totalMipLevels)
        return 0;

      UINT remainingMipLevels = totalMipLevels - viewDesc.baseMipLevel;
      UINT mipCount = viewDesc.mipLevelCount ? viewDesc.mipLevelCount : remainingMipLevels;
      return mipCount > remainingMipLevels ? remainingMipLevels : mipCount;
    }

    inline UINT getImageViewArrayCountForTransition(const ImageViewDesc& viewDesc, const ImageDesc& imageDesc) {
      if (imageDesc.type == ImageType::Tex3D)
        return 1u;

      UINT totalArrayLayers = getImageArrayCountForSubresources(imageDesc);
      if (viewDesc.baseArrayLayer >= totalArrayLayers)
        return 0;

      UINT remainingArrayLayers = totalArrayLayers - viewDesc.baseArrayLayer;
      UINT arrayCount = viewDesc.arrayLayerCount ? viewDesc.arrayLayerCount : remainingArrayLayers;
      return arrayCount > remainingArrayLayers ? remainingArrayLayers : arrayCount;
    }

    inline DXGI_FORMAT getImageViewFormatForTransition(const ImageViewDesc& viewDesc, const ImageDesc& imageDesc) {
      return viewDesc.format == DXGI_FORMAT_UNKNOWN ? imageDesc.format : viewDesc.format;
    }

    inline UINT getImageViewPlaneStartForTransition(const ImageViewDesc& viewDesc, const ImageDesc& imageDesc) {
      return getViewPlaneSlice(imageDesc.format, getImageViewFormatForTransition(viewDesc, imageDesc));
    }

    inline UINT getImageViewPlaneCountForTransition(
        const D3D12BackendImage* image,
        const ImageViewDesc&     viewDesc,
        const ImageDesc&         imageDesc) {
      UINT planeCount = image->getPlaneCount();
      if (planeCount <= 1)
        return 1u;

      return static_cast<uint32_t>(viewDesc.usage & ImageViewUsage::DepthStencil)
        ? planeCount
        : 1u;
    }

    inline UINT calcImageSubresourceIndex(
        const D3D12BackendImage* image,
        const ImageSubresource&  subresource) {
      return image->calcSubresource(subresource.mipLevel, subresource.arrayLayer);
    }

    inline uint32_t getMipExtent(uint32_t size, uint32_t mipLevel) {
      return (std::max)(size >> mipLevel, 1u);
    }

    inline ImageCopyRegion getFullSubresourceRegion(
        const D3D12BackendImage* image,
        const ImageSubresource&  subresource) {
      const auto& imageDesc = image->getDesc();

      ImageCopyRegion region = {};
      region.width = getMipExtent(imageDesc.width, subresource.mipLevel);
      region.height = getMipExtent(imageDesc.height, subresource.mipLevel);
      region.depth = imageDesc.type == ImageType::Tex3D
        ? getMipExtent(imageDesc.depth, subresource.mipLevel)
        : 1u;
      return region;
    }

    // Texel blocks of a format as copies address them.
    inline void getCopyBlockExtent(DXGI_FORMAT format, uint32_t& width, uint32_t& height) {
      switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
          width = 4;
          height = 4;
          return;
        case DXGI_FORMAT_R8G8_B8G8_UNORM: case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_YUY2: case DXGI_FORMAT_Y210: case DXGI_FORMAT_Y216:
          width = 2;
          height = 1;
          return;
        default:
          width = 1;
          height = 1;
          return;
      }
    }

    // D3D11 boxes of block formats may end at a mip edge that is not
    // block-aligned (mips smaller than a block); D3D12 copies whole blocks.
    inline void alignCopyBox(DXGI_FORMAT format, D3D12_BOX& box) {
      uint32_t blockWidth = 1, blockHeight = 1;
      getCopyBlockExtent(format, blockWidth, blockHeight);
      box.right = (box.right + blockWidth - 1) / blockWidth * blockWidth;
      box.bottom = (box.bottom + blockHeight - 1) / blockHeight * blockHeight;
    }

    inline D3D12_RESOURCE_DESC getCopyableRegionDesc(
        const D3D12BackendImage* image,
        const ImageCopyRegion&   region) {
      D3D12_RESOURCE_DESC desc = image->getTextureDesc();
      desc.Width = (std::max)(static_cast<uint64_t>(region.width), uint64_t(1));
      desc.Height = (std::max)(static_cast<uint32_t>(region.height), uint32_t(1));
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.SampleDesc.Quality = 0;
      if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        desc.DepthOrArraySize = static_cast<UINT16>((std::max)(region.depth, 1u));
      else
        desc.DepthOrArraySize = 1;
      return desc;
    }

    inline D3D12_RESOURCE_STATES getImageViewCurrentState(
        const D3D12BackendImage* image,
        const ImageViewDesc&     viewDesc) {
      const auto& imageDesc = image->getDesc();
      UINT mipCount = getImageViewMipCountForTransition(viewDesc, imageDesc);
      UINT arrayCount = getImageViewArrayCountForTransition(viewDesc, imageDesc);
      UINT planeStart = getImageViewPlaneStartForTransition(viewDesc, imageDesc);
      UINT planeCount = getImageViewPlaneCountForTransition(image, viewDesc, imageDesc);
      UINT planeEnd = planeStart + planeCount;

      if (!mipCount || !arrayCount || planeStart >= image->getPlaneCount())
        return image->getCurrentState();

      if (planeEnd > image->getPlaneCount())
        planeEnd = image->getPlaneCount();

      D3D12_RESOURCE_STATES currentState = image->getCurrentState();
      bool first = true;

      for (UINT plane = planeStart; plane < planeEnd; plane++) {
        for (UINT arrayLayer = 0; arrayLayer < arrayCount; arrayLayer++) {
          UINT subresource = image->calcSubresource(
            viewDesc.baseMipLevel,
            imageDesc.type == ImageType::Tex3D ? 0u : viewDesc.baseArrayLayer + arrayLayer,
            plane);

          D3D12_RESOURCE_STATES subresourceState = image->getSubresourceState(subresource);
          if (first) {
            currentState = subresourceState;
            first = false;
          }
        }
      }

      return currentState;
    }

    void logD3D12DebugMessages(ID3D12Device* device, const char* tag) {
      if (!device)
        return;

      ComPtr<ID3D12InfoQueue> infoQueue;
      HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&infoQueue));
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "D3D12 info queue unavailable for %s (hr=0x%08X)", tag, hr);
        return;
      }

      UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
      logMessage(LogLevel::Error, "D3D12 info queue message count for %s: %llu",
        tag,
        static_cast<unsigned long long>(messageCount));

      if (!messageCount)
        return;

      UINT64 firstMessage = messageCount > 32 ? messageCount - 32 : 0;
      for (UINT64 i = firstMessage; i < messageCount; i++) {
        SIZE_T messageLength = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &messageLength)) || !messageLength)
          continue;

        std::vector<char> messageData(messageLength);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
        if (FAILED(infoQueue->GetMessage(i, message, &messageLength)))
          continue;

        logMessage(LogLevel::Error, "D3D12 debug [%s] #%llu severity=%d id=%d: %s",
          tag,
          static_cast<unsigned long long>(i),
          static_cast<int>(message->Severity),
          static_cast<int>(message->ID),
          message->pDescription ? message->pDescription : "<no description>");
      }

      infoQueue->ClearStoredMessages();
    }

    bool safeCopyCbvSrvUavDescriptor(
        ID3D12Device*                device,
        D3D12_CPU_DESCRIPTOR_HANDLE  dstHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE  srcHandle) {
      __try {
        device->CopyDescriptorsSimple(1, dstHandle, srcHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error,
          "CopyDescriptorsSimple CRASHED (SEH=0x%08X) dst=%p src=%p",
          GetExceptionCode(),
          reinterpret_cast<void*>(dstHandle.ptr),
          reinterpret_cast<void*>(srcHandle.ptr));
        return false;
      }
    }

    bool safeSetGraphicsRootDescriptorTable(
        CommandListRecorder*         cmdList,
        UINT                         rootIndex,
        D3D12_GPU_DESCRIPTOR_HANDLE  gpuHandle) {
      __try {
        cmdList->SetGraphicsRootDescriptorTable(rootIndex, gpuHandle);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error,
          "SetGraphicsRootDescriptorTable CRASHED (SEH=0x%08X) root=%u gpu=%p",
          GetExceptionCode(),
          rootIndex,
          reinterpret_cast<void*>(gpuHandle.ptr));
        return false;
      }
    }

    bool safeSetComputeRootDescriptorTable(
        CommandListRecorder*         cmdList,
        UINT                         rootIndex,
        D3D12_GPU_DESCRIPTOR_HANDLE  gpuHandle) {
      __try {
        cmdList->SetComputeRootDescriptorTable(rootIndex, gpuHandle);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error,
          "SetComputeRootDescriptorTable CRASHED (SEH=0x%08X) root=%u gpu=%p",
          GetExceptionCode(),
          rootIndex,
          reinterpret_cast<void*>(gpuHandle.ptr));
        return false;
      }
    }

    __declspec(noinline) bool safeExecuteIndirect(
        ID3D12GraphicsCommandList*  cmdList,
        ID3D12CommandSignature*     cmdSig,
        ID3D12Resource*             argResource,
        UINT64                      offset) {
      __try {
        cmdList->ExecuteIndirect(cmdSig, 1, argResource, offset, nullptr, 0);
        return true;
      } __except(EXCEPTION_EXECUTE_HANDLER) {
        logMessage(dxup::LogLevel::Error,
          "ExecuteIndirect CRASHED (SEH=0x%08X) cmdSig=%p argRes=%p offset=%llu",
          GetExceptionCode(), cmdSig, argResource,
          static_cast<unsigned long long>(offset));
        return false;
      }
    }

    bool safeCopyTextureRegion(
        CommandListRecorder*                    cmdList,
        const D3D12_TEXTURE_COPY_LOCATION*      dstLoc,
        UINT                                    dstX,
        UINT                                    dstY,
        UINT                                    dstZ,
        const D3D12_TEXTURE_COPY_LOCATION*      srcLoc,
        const D3D12_BOX*                        srcBox,
      const dxup::d3d12::D3D12BackendBuffer*  srcBuffer,
        const dxup::ImageCopyRegion*            region,
        UINT                                    subresourceIdx,
        UINT                                    numRows,
        UINT64                                  rowSizeBytes) {
      __try {
        cmdList->CopyTextureRegion(dstLoc, dstX, dstY, dstZ, srcLoc, srcBox);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        const auto& footprint = srcLoc->PlacedFootprint;
        const auto& bufferDesc = srcBuffer->getDesc();
        logMessage(LogLevel::Error,
          "CopyTextureRegion CRASHED (SEH=0x%08X) sub=%u dstXYZ=%u,%u,%u srcOffset=%llu fmt=%u fp=%ux%ux%u rowPitch=%u rowSize=%llu numRows=%u srcUsage=%u srcCpu=%u srcState=0x%x region=%u,%u,%u %ux%ux%u srcBox=%u,%u,%u-%u,%u,%u",
          GetExceptionCode(),
          subresourceIdx,
          dstX,
          dstY,
          dstZ,
          static_cast<unsigned long long>(footprint.Offset),
          static_cast<uint32_t>(footprint.Footprint.Format),
          footprint.Footprint.Width,
          footprint.Footprint.Height,
          footprint.Footprint.Depth,
          footprint.Footprint.RowPitch,
          static_cast<unsigned long long>(rowSizeBytes),
          numRows,
          static_cast<uint32_t>(bufferDesc.usage),
          static_cast<uint32_t>(bufferDesc.cpuAccess),
          static_cast<uint32_t>(srcBuffer->getCurrentState()),
          region ? region->x : 0,
          region ? region->y : 0,
          region ? region->z : 0,
          region ? region->width : 0,
          region ? region->height : 0,
          region ? region->depth : 0,
          srcBox ? srcBox->left : 0,
          srcBox ? srcBox->top : 0,
          srcBox ? srcBox->front : 0,
          srcBox ? srcBox->right : 0,
          srcBox ? srcBox->bottom : 0,
          srcBox ? srcBox->back : 0);
        return false;
      }
    }

    __declspec(noinline) void safeCopyTextureRegionSimple(
        CommandListRecorder*                    cmdList,
        const D3D12_TEXTURE_COPY_LOCATION*      dstLoc,
        UINT                                    dstX,
        UINT                                    dstY,
        UINT                                    dstZ,
        const D3D12_TEXTURE_COPY_LOCATION*      srcLoc,
        const D3D12_BOX*                        srcBox,
        dxup::d3d12::D3D12BackendDevice*        device) {
      __try {
        cmdList->CopyTextureRegion(dstLoc, dstX, dstY, dstZ, srcLoc, srcBox);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error,
          "copyImageRegion: CopyTextureRegion CRASHED (SEH=0x%08X) dstRes=%p dstSub=%u srcRes=%p srcSub=%u",
          GetExceptionCode(),
          dstLoc->pResource, dstLoc->SubresourceIndex,
          srcLoc->pResource, srcLoc->SubresourceIndex);
        HRESULT devHr = device->getDevice()->GetDeviceRemovedReason();
        logMessage(LogLevel::Error,
          "copyImageRegion: device removed reason after crash=0x%08x", devHr);
      }
    }

    const char* shaderStageName(ShaderStage stage) {
      switch (stage) {
        case ShaderStage::Vertex:   return "VS";
        case ShaderStage::Hull:     return "HS";
        case ShaderStage::Domain:   return "DS";
        case ShaderStage::Geometry: return "GS";
        case ShaderStage::Pixel:    return "PS";
        case ShaderStage::Compute:  return "CS";
        default:                    return "?";
      }
    }

    // DRED (dxup_dred.txt): the command lists the GPU did not finish, with
    // the operations around where it stopped, and a page fault address.
    void logDredOutput(ID3D12Device* device) {
      ComPtr<ID3D12DeviceRemovedExtendedData> dred;
      if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;
      // The markers before the operations where the GPU stopped.
      ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
      D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs1 = {};
      if (SUCCEEDED(dred.As(&dred1)) && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs1))) {
        uint32_t lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs1.pHeadAutoBreadcrumbNode;
             node && lists < 16; node = node->pNext) {
          const uint32_t count = node->BreadcrumbCount;
          const uint32_t reached = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
          if (reached >= count)
            continue;
          lists++;
          logMessage(LogLevel::Error, "DRED: list %p stopped after %u of %u operations, %u texts",
            static_cast<void*>(node->pCommandList), reached, count, node->BreadcrumbContextsCount);
          for (uint32_t i = 0; i < node->BreadcrumbContextsCount; i++) {
            const auto& context = node->pBreadcrumbContexts[i];
            if (context.BreadcrumbIndex + 8 < reached || context.BreadcrumbIndex > reached + 2 || !context.pContextString)
              continue;
            char text[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, context.pContextString, -1, text, sizeof(text) - 1, nullptr, nullptr);
            logMessage(LogLevel::Error, "DRED: list %p operation %u%s: %s", static_cast<void*>(node->pCommandList),
              context.BreadcrumbIndex, context.BreadcrumbIndex + 1 == reached ? " (stopped after)" : "", text);
          }
        }
      }
      D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
      if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
        uint32_t lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
             node && lists < 16; node = node->pNext) {
          const uint32_t count = node->BreadcrumbCount;
          const uint32_t reached = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
          if (reached >= count)
            continue;
          lists++;
          logMessage(LogLevel::Error, "DRED: command list %p stopped after %u of %u operations",
            static_cast<void*>(node->pCommandList), reached, count);
          const uint32_t first = reached > 12 ? reached - 12 : 0;
          const uint32_t last = (std::min)(count, reached + 4);
          std::string ops;
          for (uint32_t i = first; i < last; i++) {
            char entry[32];
            sprintf_s(entry, "%s%u%s", i == reached ? " [" : " ", static_cast<unsigned>(node->pCommandHistory[i]),
              i == reached ? "]" : "");
            ops += entry;
          }
          logMessage(LogLevel::Error, "DRED: operations %u..%u:%s", first, last, ops.c_str());
        }
      }
      D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
      if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA) {
        logMessage(LogLevel::Error, "DRED: page fault at GPU address 0x%llx",
          static_cast<unsigned long long>(pageFault.PageFaultVA));
        for (auto* node = pageFault.pHeadExistingAllocationNode; node; node = node->pNext)
          logMessage(LogLevel::Error, "DRED:   existing allocation type %d", static_cast<int>(node->AllocationType));
        for (auto* node = pageFault.pHeadRecentFreedAllocationNode; node; node = node->pNext)
          logMessage(LogLevel::Error, "DRED:   recently freed allocation type %d", static_cast<int>(node->AllocationType));
      }
    }

    void logDeviceRemovedReason(D3D12BackendDevice* device, const char* tag) {
      if (!device || !device->getDevice())
        return;

      HRESULT hr = device->getDevice()->GetDeviceRemovedReason();
      if (FAILED(hr)) {
        const char* reasonStr = "UNKNOWN";
        switch (hr) {
          case 0x887A0006L: reasonStr = "DEVICE_HUNG (TDR)"; break;
          case 0x887A0005L: reasonStr = "DEVICE_REMOVED"; break;
          case 0x887A0007L: reasonStr = "DEVICE_RESET"; break;
          case 0x887A0020L: reasonStr = "DRIVER_INTERNAL_ERROR"; break;
          case 0x887A0001L: reasonStr = "INVALID_CALL"; break;
          case 0x887A002BL: reasonStr = "ACCESS_DENIED (possible memory exhaustion)"; break;
        }
        logMessage(LogLevel::Error,
          "D3D12 device removed after %s: hr=0x%08X (%s)",
          tag,
          static_cast<uint32_t>(hr),
          reasonStr);
        logD3D12DebugMessages(device->getDevice(), tag);
        logDredOutput(device->getDevice());
      }
    }

    bool safeResourceBarrier(
        CommandListRecorder*                    cmdList,
        UINT                                    barrierCount,
        const D3D12_RESOURCE_BARRIER*           barriers) {
      __try {
        cmdList->ResourceBarrier(barrierCount, barriers);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error,
          "ResourceBarrier CRASHED (SEH=0x%08X) count=%u",
          GetExceptionCode(),
          barrierCount);

        for (UINT i = 0; i < barrierCount; i++) {
          const auto& barrier = barriers[i];
          if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            logMessage(LogLevel::Error,
              "  barrier[%u]: type=transition res=%p before=0x%x after=0x%x sub=%u flags=0x%x",
              i,
              barrier.Transition.pResource,
              static_cast<uint32_t>(barrier.Transition.StateBefore),
              static_cast<uint32_t>(barrier.Transition.StateAfter),
              barrier.Transition.Subresource,
              static_cast<uint32_t>(barrier.Flags));
          } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            logMessage(LogLevel::Error,
              "  barrier[%u]: type=uav res=%p flags=0x%x",
              i,
              barrier.UAV.pResource,
              static_cast<uint32_t>(barrier.Flags));
          } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            logMessage(LogLevel::Error,
              "  barrier[%u]: type=aliasing before=%p after=%p flags=0x%x",
              i,
              barrier.Aliasing.pResourceBefore,
              barrier.Aliasing.pResourceAfter,
              static_cast<uint32_t>(barrier.Flags));
          } else {
            logMessage(LogLevel::Error,
              "  barrier[%u]: type=%u flags=0x%x",
              i,
              static_cast<uint32_t>(barrier.Type),
              static_cast<uint32_t>(barrier.Flags));
          }
        }

        return false;
      }
    }

    // safeCloseAndExecute's halves with the submit thread: the list closes
    // on the thread that recorded it, the thread executes it.
    __declspec(noinline)
    bool safeClose(CommandListRecorder* cmdList, int flushCount, int64_t* closedAt) {
      __try {
        HRESULT hr = cmdList->Close();
        if (closedAt)
          *closedAt = hitchTimestamp();
        if (FAILED(hr)) {
          logMessage(LogLevel::Error, "safeClose: Close failed hr=0x%08x flush=%d", hr, flushCount);
          return false;
        }
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error, "safeClose CRASHED (SEH=0x%08X) flush=%d", GetExceptionCode(), flushCount);
        return false;
      }
    }

    // A list that faults in ExecuteCommandLists is lost. Its fence value still
    // comes (as flush()'s recovery signals a lost batch's), so waits for it
    // end, but the device counts as lost: the worker recorded later lists
    // on the states the lost one's barriers would have left, and unlike
    // flush() without the thread it cannot take them back.
    __declspec(noinline)
    bool safeExecute(ID3D12CommandQueue* queue, ID3D12CommandList* list) {
      __try {
        queue->ExecuteCommandLists(1, &list);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error, "submit thread: ExecuteCommandLists CRASHED (SEH=0x%08X); the list is lost", GetExceptionCode());
        return false;
      }
    }

    __declspec(noinline)
    void safeExecuteAndSignal(ID3D12CommandQueue* queue, ID3D12CommandList* list, ID3D12Fence* fence, uint64_t value,
                              SubmitQueue* submit) {
      if (!safeExecute(queue, list))
        submit->checkResult(DXGI_ERROR_DRIVER_INTERNAL_ERROR, "ExecuteCommandLists (resource states unknown)");
      HRESULT hr = E_FAIL;
      __try {
        hr = queue->Signal(fence, value);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error, "submit thread: Signal CRASHED (SEH=0x%08X)", GetExceptionCode());
      }
      submit->checkResult(hr, "Signal");
      submit->checkDevice("ExecuteCommandLists");
    }

    __declspec(noinline)
    bool safeCloseAndExecute(
        CommandListRecorder*       cmdList,
        ID3D12CommandQueue* cmdQueue,
        int flushCount,
        bool collectTiming,
        int64_t* closedAt = nullptr) {
      __try {
        LARGE_INTEGER closeStart = {}, closeEnd = {}, execEnd = {}, freq = {};
        if (collectTiming) {
          QueryPerformanceFrequency(&freq);
          QueryPerformanceCounter(&closeStart);
        }
        HRESULT hr = cmdList->Close();
        if (closedAt)
          *closedAt = hitchTimestamp();
        if (collectTiming)
          QueryPerformanceCounter(&closeEnd);
        if (FAILED(hr)) {
          logMessage(LogLevel::Error, "safeCloseAndExecute: Close failed hr=0x%08x flush=%d", hr, flushCount);
          return false;
        }
        ID3D12CommandList* cmdLists[] = { cmdList->raw() };
        cmdQueue->ExecuteCommandLists(1, cmdLists);
        if (collectTiming) {
          QueryPerformanceCounter(&execEnd);
          double closeMs = (double)(closeEnd.QuadPart - closeStart.QuadPart) * 1000.0 / freq.QuadPart;
          double execMs = (double)(execEnd.QuadPart - closeEnd.QuadPart) * 1000.0 / freq.QuadPart;
          if (closeMs > 5.0) {
            logMessage(LogLevel::Warn, "safeCloseAndExecute: flush=%d Close=%.1fms Execute=%.1fms",
              flushCount, closeMs, execMs);
          }
        }
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error, "safeCloseAndExecute CRASHED (SEH=0x%08X) flush=%d",
          GetExceptionCode(), flushCount);
        return false;
      }
    }

    bool safeOMSetRenderTargets(
        CommandListRecorder*                    cmdList,
        UINT                                    renderTargetCount,
        const D3D12_CPU_DESCRIPTOR_HANDLE*      rtvHandles,
        BOOL                                    rtvSingleHandle,
        const D3D12_CPU_DESCRIPTOR_HANDLE*      dsvHandle) {
      __try {
        cmdList->OMSetRenderTargets(renderTargetCount, rtvHandles, rtvSingleHandle, dsvHandle);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        logMessage(LogLevel::Error,
          "OMSetRenderTargets CRASHED (SEH=0x%08X) count=%u rtv0=%p dsv=%p single=%u",
          GetExceptionCode(),
          renderTargetCount,
          renderTargetCount && rtvHandles ? reinterpret_cast<void*>(rtvHandles[0].ptr) : nullptr,
          dsvHandle ? reinterpret_cast<void*>(dsvHandle->ptr) : nullptr,
          rtvSingleHandle ? 1u : 0u);
        return false;
      }
    }

  }
#endif

  D3D12BackendContext::D3D12BackendContext(D3D12BackendDevice* device, bool copyOnly)
  : m_device(device), m_copyOnly(copyOnly) {
#ifdef _WIN32
    m_perfStatsEnabled = perfStatsEnabled();
    m_perfTimingEnabled = perfTimingEnabled();
    m_infoDiagnosticsEnabled = infoDiagnosticsEnabled();
    m_recentOpsEnabled = recentOpsEnabled();
    m_barrierStatsEnabled = barrierStatsEnabled();

    logMessage(LogLevel::Debug, "D3D12BackendContext: Initializing (copyOnly=%d)...", copyOnly);

    // Read diagnostic GPU op limit from dxup_gpu_limit.txt next to our DLL
    {
      static bool limitRead = false;
      if (!limitRead) {
        limitRead = true;
        // The file next to d3d11.dll.
        FILE* f = fopen(dxupSiblingPath("dxup_gpu_limit.txt").c_str(), "r");
        if (f) {
          int val = -1;
          if (fscanf(f, "%d", &val) == 1) {
            g_gpuOpLimit = val;
            logMessage(LogLevel::Info, "dxup_gpu_limit.txt: limit=%d (will skip draw/dispatch after this count)", g_gpuOpLimit);
          }
          fclose(f);
        }
      }
    }

    auto* d3d12Device = device->getDevice();

    { HRESULT chk = d3d12Device->GetDeviceRemovedReason();
      if (FAILED(chk)) logMessage(LogLevel::Error, "Device already hung at context init start! reason=0x%08X", chk); }

    // Use a small allocator ring so CPU recording can overlap GPU execution.
    // dxup_disable_async_reset.txt restores the single-allocator fallback.
    {
      // The initial data context too: with one allocator, every submission
      // of new resources' data waited for the GPU to finish all work.
      m_commandAllocatorCount = asyncResetEnabled() ? kCommandAllocatorRingSize : 1u;
      if (m_commandAllocatorCount > 1)
        logMessage(LogLevel::Info, "D3D12BackendContext: async allocator ring enabled (%u slots)", m_commandAllocatorCount);

      for (uint32_t i = 0; i < m_commandAllocatorCount; i++) {
        HRESULT hr = d3d12Device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&m_commandAllocators[i]));
        if (FAILED(hr)) {
          logMessage(LogLevel::Error, "Failed to create command allocator[%u] (hr=0x%08x)", i, hr);
          throw std::runtime_error("Failed to create command allocator");
        }
      }
      m_currentAllocatorIndex = 0;
      m_currentAllocator = m_commandAllocators[0];
    }

    // Create command list
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    HRESULT hr = d3d12Device->CreateCommandList(0,
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      m_currentAllocator.Get(), nullptr,
      IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "Failed to create command list (hr=0x%08x)", hr);
      throw std::runtime_error("Failed to create command list");
    }
    m_commandLists[0] = cmdList;
    m_cmdList.setList(std::move(cmdList));

    { HRESULT chk = d3d12Device->GetDeviceRemovedReason();
      if (FAILED(chk)) logMessage(LogLevel::Error, "Device hung after CreateCommandList! reason=0x%08X", chk); }

    // Create fence for synchronization
    hr = d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Get descriptor sizes
    m_cbvSrvUavDescSize = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_samplerDescSize   = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    m_cbvDescriptorCache.reserve(kMaxCachedCbvDescriptors);
    m_pendingBarriers.reserve(64);
    m_emittedBarriers.reserve(256);
    m_commonDecaySubresources.reserve(64);
    m_stateRollback.reserve(256);
    m_bufferStateRollback.reserve(128);
    m_batchResourceRefs.reserve(256);
    m_batchResourceRefOccupiedSlots.reserve(1024);
    m_pendingUploads.reserve(64);
    m_inFlightUploads.reserve(128);
    for (uint32_t stage = 0; stage < kShaderStages; stage++) {
      m_cbvTableCacheLookup[stage].reserve(128);
      m_srvTableCacheLookup[stage].reserve(128);
      m_uavTableCacheLookup[stage].reserve(64);
    }

    if (!copyOnly) {
      // Create shader-visible CBV/SRV/UAV heap: small while partitions
      // recycle it (it grows when they do not), whole without them.
      createShaderVisibleCbvSrvUavHeap(descriptorPartitionCount() > 1
        ? kInitialCbvSrvUavHeapSize : kShaderVisibleCbvSrvUavHeapSize);

      // Create shader-visible sampler heap
      D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
      samplerHeapDesc.NumDescriptors = kShaderVisibleSamplerHeapSize;
      samplerHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
      samplerHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      d3d12Device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_shaderVisibleSamplerHeap));
      if (!m_shaderVisibleCbvSrvUavHeap || !m_shaderVisibleSamplerHeap)
        throw std::runtime_error("Failed to create shader-visible descriptor heaps");
      initSamplerTables();

      // Set descriptor heaps
      ID3D12DescriptorHeap* heaps[] = {
        m_shaderVisibleCbvSrvUavHeap.Get(),
        m_shaderVisibleSamplerHeap.Get()
      };
      m_cmdList->SetDescriptorHeaps(2, heaps);

      // Set root signatures for both graphics and compute
      m_dredMarkers = dredEnabled();
      if (m_dredMarkers)
        initDispatchBreadcrumbs();
      m_resetRootOnModeSwitch = device->getVendorId() == 0x1002;
      m_currentRootSignature = device->getGraphicsRootSignature();
      m_computeRootSignature = device->getComputeRootSignature();
      m_cmdList->SetGraphicsRootSignature(m_currentRootSignature);
      m_cmdList->SetComputeRootSignature(m_computeRootSignature);
      invalidateRootTableCache();

      // Create query heaps
      D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
      queryHeapDesc.Count = kMaxQueriesPerHeap;

      queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
      d3d12Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&m_occlusionQueryHeap));

      queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      d3d12Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&m_timestampQueryHeap));

      queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
      d3d12Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&m_pipelineStatsQueryHeap));

      queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
      d3d12Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&m_streamOutputQueryHeap));

      char profileMarker[MAX_PATH] = {}, profileOut[MAX_PATH] = {};
      if (dxupSiblingPathTo(profileMarker, MAX_PATH, "dxup_gpu_profile.txt")
       && dxupSiblingPathTo(profileOut, MAX_PATH, "dxup_gpu_profile_out.txt"))
        m_gpuProfiler.init(d3d12Device, device->getCommandQueue(), profileMarker, profileOut);

    }

    logMessage(LogLevel::Debug, "D3D12BackendContext: Initialization complete");
#endif
  }

  D3D12BackendContext::~D3D12BackendContext() {
#ifdef _WIN32
    m_cmdList.stopThread();
    // Queued submissions use the lists, allocators and the fence: they run
    // first (synchronize() below stops early on a removed device).
    m_device->submitQueue().drain();
    // With ring allocator, GPU may still be executing command lists using our
    // allocators even when upload lists are empty. Must always sync before
    // destroying allocators, since all contexts share one command queue.
    if (m_fence && m_fenceValue > 0)
      synchronize();
    // The upscaler's resources go while the device lives, one still being
    // made as well.
    m_upscaler.reset();
    if (m_upscalerPending.valid())
      m_upscalerPending.get();
    if (m_upscalerThread.joinable())
      m_upscalerThread.join();
    // The context's own render target descriptors (read when recorded).
    if (m_nullRtv.ptr)
      m_device->getDescriptorHeapManager().freeRtv(m_nullRtv);
    if (m_lazyClearRtv.ptr)
      m_device->getDescriptorHeapManager().freeRtv(m_lazyClearRtv);

    for (auto* query : m_activeQueries)
      query->m_context = nullptr;
    m_activeQueries.clear();
    if (m_predicate)
      m_predicate->m_context = nullptr;
    m_predicate = nullptr;

    // Release any remaining batch resource references (GPU is synced above)
    releaseAllResourceBatches();

    for (uint32_t i = 0; i < kCommandAllocatorRingSize; i++) {
      if (m_uploadScratchBuffers[i] && m_uploadScratchMapped[i]) {
        D3D12_RANGE writtenRange = { 0, static_cast<SIZE_T>(m_uploadScratchOffsets[i]) };
        m_uploadScratchBuffers[i]->Unmap(0, &writtenRange);
        m_uploadScratchMapped[i] = nullptr;
      }
    }

    if (m_fenceEvent)
      CloseHandle(m_fenceEvent);
    if (m_frameLatencyEvent)
      CloseHandle(m_frameLatencyEvent);
#endif
  }

  // ============================================================
  // Resource Binding
  // ============================================================

  void D3D12BackendContext::bindVertexBuffer(uint32_t slot, IBackendBuffer* buffer, uint32_t offset, uint32_t stride) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxVertexBuffers) return;

    if (!buffer) {
      D3D12_VERTEX_BUFFER_VIEW vbv = {};
      m_trackedVBs[slot].view = vbv;
      m_trackedVBs[slot].bound = false;
      m_cmdList->IASetVertexBuffers(slot, 1, &vbv);
      return;
    }

    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    holdBatchResourceOnce(d3d12Buffer->getResource(),
      m_trackedVBs[slot].resourceHeld,
      m_trackedVBs[slot].resourceHeldGeneration);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    // An offset past the end reads zeros, as in D3D11, instead of a size
    // that wraps around.
    const uint64_t bufferSize = d3d12Buffer->getDesc().size;
    vbv.BufferLocation = d3d12Buffer->getGpuAddress() + (std::min)(uint64_t(offset), bufferSize);
    vbv.SizeInBytes    = static_cast<UINT>(offset < bufferSize ? bufferSize - offset : 0);
    vbv.StrideInBytes  = stride;

    if (m_trackedVBs[slot].bound
     && m_trackedVBs[slot].view.BufferLocation == vbv.BufferLocation
     && m_trackedVBs[slot].view.SizeInBytes == vbv.SizeInBytes
     && m_trackedVBs[slot].view.StrideInBytes == vbv.StrideInBytes)
      return;

    m_trackedVBs[slot].view = vbv;
    m_trackedVBs[slot].bound = true;
    // The slot the current pipeline reads its missing inputs from stays
    // unbound; the buffer is set when another pipeline is bound.
    if (slot != m_nullVertexSlot)
      m_cmdList->IASetVertexBuffers(slot, 1, &vbv);
#endif
  }

  void D3D12BackendContext::applyNullVertexSlot(uint32_t slot) {
#ifdef _WIN32
    if (m_nullVertexSlot != slot && m_nullVertexSlot < kMaxVertexBuffers && m_trackedVBs[m_nullVertexSlot].bound)
      m_cmdList->IASetVertexBuffers(m_nullVertexSlot, 1, &m_trackedVBs[m_nullVertexSlot].view);
    m_nullVertexSlot = slot;
    if (slot < kMaxVertexBuffers && m_trackedVBs[slot].bound) {
      const D3D12_VERTEX_BUFFER_VIEW none = {};
      m_cmdList->IASetVertexBuffers(slot, 1, &none);
    }
#else
    (void)slot;
#endif
  }

  void D3D12BackendContext::bindIndexBuffer(IBackendBuffer* buffer, uint32_t offset, IndexType type) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!buffer) {
      m_trackedIB = {};
      m_trackedIBBound = false;
      m_cmdList->IASetIndexBuffer(nullptr);
      return;
    }

    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    holdBatchResourceOnce(d3d12Buffer->getResource(),
      m_trackedIBResourceHeld,
      m_trackedIBResourceHeldGeneration);

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    const uint64_t bufferSize = d3d12Buffer->getDesc().size;
    ibv.BufferLocation = d3d12Buffer->getGpuAddress() + (std::min)(uint64_t(offset), bufferSize);
    ibv.SizeInBytes    = static_cast<UINT>(offset < bufferSize ? bufferSize - offset : 0);
    ibv.Format         = (type == IndexType::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    if (m_trackedIBBound
     && m_trackedIB.BufferLocation == ibv.BufferLocation
     && m_trackedIB.SizeInBytes == ibv.SizeInBytes
     && m_trackedIB.Format == ibv.Format)
      return;

    m_trackedIB = ibv;
    m_trackedIBBound = true;
    m_cmdList->IASetIndexBuffer(&ibv);
#endif
  }

  void D3D12BackendContext::bindConstantBuffer(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxConstantBuffers) return;

    uint32_t stageIdx = static_cast<uint32_t>(stage);
    if (stageIdx >= kShaderStages) return;

    auto& binding = m_cbvBindings[stageIdx][slot];
    binding.buffer = static_cast<D3D12BackendBuffer*>(buffer);
    binding.firstConstant = 0;
    binding.numConstants = 0;
    if (buffer && slot >= m_cbvBindingEnd[stageIdx])
      m_cbvBindingEnd[stageIdx] = slot + 1;

    if (buffer) {
      auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
      transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
      holdBatchResourceOnce(d3d12Buffer->getResource(),
        binding.resourceHeld,
        binding.resourceHeldGeneration);

      UINT size = static_cast<UINT>(d3d12Buffer->getDesc().size);
      size = (size + 255) & ~255u;  // CBV size must be 256-aligned
      // Shaders see the first 4096 constants of larger buffers (D3D11), and
      // a D3D12 CBV holds no more.
      size = (std::min)(size, UINT(D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16));

      D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = d3d12Buffer->getGpuAddress();
      if (binding.bound && binding.gpuAddress == gpuAddress && binding.sizeInBytes == size)
        return;

      static int cbvBindLogCount = 0;
      if (cbvBindLogCount < 20 && stage == ShaderStage::Compute) {
        logMessage(LogLevel::Info, "bindCB: CS slot=%u gpu=%p size=%u buf=%p",
          slot, reinterpret_cast<void*>(static_cast<uintptr_t>(gpuAddress)), size, d3d12Buffer);
        cbvBindLogCount++;
      }

      binding.gpuAddress = gpuAddress;
      binding.sizeInBytes = size;
      binding.bound = true;
    } else {
      if (!binding.bound && !binding.gpuAddress && !binding.sizeInBytes)
        return;

      binding.gpuAddress = 0;
      binding.sizeInBytes = 0;
      binding.bound = false;
    }
    m_cbvDirty = true;
    m_cbvStageDirty[stageIdx] = true;
    m_descriptorDirtyStageMask |= 1u << stageIdx;
#endif
  }

  void D3D12BackendContext::bindConstantBufferRange(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer,
                                                     uint32_t firstConstant, uint32_t numConstants) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxConstantBuffers) return;

    uint32_t stageIdx = static_cast<uint32_t>(stage);
    if (stageIdx >= kShaderStages) return;

    auto& binding = m_cbvBindings[stageIdx][slot];
    binding.buffer = static_cast<D3D12BackendBuffer*>(buffer);
    binding.firstConstant = firstConstant;
    binding.numConstants = numConstants;
    if (buffer && slot >= m_cbvBindingEnd[stageIdx])
      m_cbvBindingEnd[stageIdx] = slot + 1;

    if (buffer) {
      auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
      transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
      holdBatchResourceOnce(d3d12Buffer->getResource(),
        binding.resourceHeld,
        binding.resourceHeldGeneration);

      // firstConstant is in 16-byte units, D3D12 needs byte offset (must be 256-aligned)
      uint32_t byteOffset = firstConstant * 16;
      // A range past the end of the buffer is cut there: D3D11 reads zeros
      // beyond it.
      const uint32_t bufferSize = static_cast<uint32_t>(d3d12Buffer->getDesc().size);
      const uint32_t available = byteOffset < bufferSize ? bufferSize - byteOffset : 0;
      uint32_t byteSize = numConstants * 16;
      if (byteSize == 0 || byteSize > available)
        byteSize = available;
      if (!byteSize) {
        bindConstantBuffer(stage, slot, nullptr);
        return;
      }
      byteSize = (byteSize + 255) & ~255u;  // CBV size must be 256-aligned
      byteSize = (std::min)(byteSize, uint32_t(D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16));

      D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = d3d12Buffer->getGpuAddress() + byteOffset;
      if (binding.bound && binding.gpuAddress == gpuAddress && binding.sizeInBytes == byteSize)
        return;

      binding.gpuAddress = gpuAddress;
      binding.sizeInBytes = byteSize;
      binding.bound = true;
    } else {
      if (!binding.bound && !binding.gpuAddress && !binding.sizeInBytes)
        return;

      binding.gpuAddress = 0;
      binding.sizeInBytes = 0;
      binding.bound = false;
    }
    m_cbvDirty = true;
    m_cbvStageDirty[stageIdx] = true;
    m_descriptorDirtyStageMask |= 1u << stageIdx;
#endif
  }

#ifdef _WIN32
  void D3D12BackendContext::invalidateRootTableCache() {
#ifdef _WIN32
    invalidateRootCache(false);
    invalidateRootCache(true);
#endif
  }

  void D3D12BackendContext::invalidateRootCache(bool compute) {
#ifdef _WIN32
    if (compute) {
      m_currentComputeRootTableValid.fill(false);
      m_currentComputeRootCbvs.fill(0);
    } else {
      m_currentGraphicsRootTableValid.fill(false);
      m_currentGraphicsRootCbvs.fill(0);
    }
#else
    (void)compute;
#endif
  }

  void D3D12BackendContext::restoreAfterInternalPass() {
#ifdef _WIN32
    m_trackedOmBindingValid = false;
    m_outputMergerRebindRequired.store(true, std::memory_order_relaxed);
    if (m_trackedViewportCount)
      m_cmdList->RSSetViewports(m_trackedViewportCount, m_trackedViewports);
    if (m_trackedScissorCount)
      m_cmdList->RSSetScissorRects(m_trackedScissorCount, m_trackedScissors);
    if (m_trackedTopology != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
      m_cmdList->IASetPrimitiveTopology(m_trackedTopology);
    // The pass's root signature dropped the application's root arguments.
    // The tracked root signature stays: a draw with the same pipeline binds
    // nothing, and a command list reset sets the tracked one again (a
    // forgotten one left the next list without any: the driver crashed in
    // SetGraphicsRootConstantBufferView after a scaled Present).
    if (m_currentRootSignature)
      m_cmdList->SetGraphicsRootSignature(m_currentRootSignature);
    markAllDescriptorStagesDirty();
    invalidateRootTableCache();
    m_psoDirtyAfterFlush = true;
#endif
  }

  bool D3D12BackendContext::graphicsRootTableNeedsSet(uint32_t rootIndex,
                                                       D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) const {
#ifdef _WIN32
    return rootIndex >= m_currentGraphicsRootTables.size()
        || !m_currentGraphicsRootTableValid[rootIndex]
        || m_currentGraphicsRootTables[rootIndex].ptr != gpuHandle.ptr;
#else
    (void)rootIndex;
    (void)gpuHandle;
    return true;
#endif
  }

  bool D3D12BackendContext::computeRootTableNeedsSet(uint32_t rootIndex,
                                                      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) const {
#ifdef _WIN32
    return rootIndex >= m_currentComputeRootTables.size()
        || !m_currentComputeRootTableValid[rootIndex]
        || m_currentComputeRootTables[rootIndex].ptr != gpuHandle.ptr;
#else
    (void)rootIndex;
    (void)gpuHandle;
    return true;
#endif
  }

  void D3D12BackendContext::noteGraphicsRootTable(uint32_t rootIndex,
                                                   D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
#ifdef _WIN32
    if (rootIndex < m_currentGraphicsRootTables.size()) {
      m_currentGraphicsRootTables[rootIndex] = gpuHandle;
      m_currentGraphicsRootTableValid[rootIndex] = true;
    }
#else
    (void)rootIndex;
    (void)gpuHandle;
#endif
  }

  void D3D12BackendContext::noteComputeRootTable(uint32_t rootIndex,
                                                  D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
#ifdef _WIN32
    if (rootIndex < m_currentComputeRootTables.size()) {
      m_currentComputeRootTables[rootIndex] = gpuHandle;
      m_currentComputeRootTableValid[rootIndex] = true;
    }
#else
    (void)rootIndex;
    (void)gpuHandle;
#endif
  }

  uint32_t D3D12BackendContext::clampDescriptorCountToShader(
      uint32_t stageIdx,
      uint32_t descriptorCount,
      uint32_t ShaderResourceLimits::*member) const {
#ifdef _WIN32
    // Every register the shader declares, bound or not: a table shorter than
    // that leaves the shader reading other descriptors.
    const ShaderResourceLimits* limits = activeResourceLimits(stageIdx);
    if (!limits || !limits->known)
      return descriptorCount;

    return limits->*member;
#else
    (void)stageIdx;
    (void)member;
    return descriptorCount;
#endif
  }

  D3D12_CPU_DESCRIPTOR_HANDLE D3D12BackendContext::getOrCreateCachedCbvDescriptor(
      D3D12_GPU_VIRTUAL_ADDRESS gpuAddress,
      UINT sizeInBytes) {
#ifdef _WIN32
    CbvDescriptorKey key = {};
    key.gpuAddress = gpuAddress;
    key.sizeInBytes = sizeInBytes;

    auto cached = m_cbvDescriptorCache.find(key);
    if (cached != m_cbvDescriptorCache.end()) {
      if (perfStatsEnabled() && !m_copyOnly)
        m_perfIntervalDescriptorCacheHits++;
      return cached->second;
    }

    if (m_cbvDescriptorCache.size() >= kMaxCachedCbvDescriptors)
      return {};

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_device->getDescriptorHeapManager().allocateCbvSrvUav();
    if (!cpuHandle.ptr)
      return {};

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = gpuAddress;
    cbvDesc.SizeInBytes = sizeInBytes;
    m_device->getDevice()->CreateConstantBufferView(&cbvDesc, cpuHandle);

    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCbvCreates++;

    m_cbvDescriptorCache.emplace(key, cpuHandle);
    return cpuHandle;
#else
    (void)gpuAddress;
    (void)sizeInBytes;
    return {};
#endif
  }

  void D3D12BackendContext::commitCbvBindings() {
#ifdef _WIN32
    auto hasActiveDirty = [&]() {
      if (m_isComputePipeline)
        return m_cbvStageDirty[static_cast<uint32_t>(ShaderStage::Compute)];
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if ((m_currentGraphicsStageMask & (1u << stage)) && m_cbvStageDirty[stage])
          return true;
      return false;
    };
    if (!hasActiveDirty()) return;

    auto* d3d12Device = m_device->getDevice();
    static int cbvCommitLogCount = 0;

    auto commitStage = [&](uint32_t stageIdx, uint32_t rootIndex, bool compute) {
      if (!m_cbvStageDirty[stageIdx])
        return;
      m_cbvStageDirty[stageIdx] = false;

      // Unbound slots read zeros, as in D3D11.
      const D3D12_GPU_VIRTUAL_ADDRESS zeroAddress = m_device->getZeroConstantBufferAddress();
      CbvSlot fallback = {};
      fallback.gpuAddress = zeroAddress;
      fallback.sizeInBytes = 65536;
      // The shader's immediate constant buffer, moved to a slot of its own.
      const ShaderResourceLimits* stageLimits = activeResourceLimits(stageIdx);
      const uint32_t icbSlot = stageLimits && stageLimits->icbAddress ? stageLimits->icbSlot : UINT32_MAX;
      CbvSlot icb = {};
      if (icbSlot != UINT32_MAX) {
        icb.bound = true;
        icb.gpuAddress = stageLimits->icbAddress;
        icb.sizeInBytes = stageLimits->icbSize;
      }

      // Root descriptors for the first slots.
      const uint32_t rootSlots = kRootCbvSlots[stageIdx];
      for (uint32_t slot = 0; slot < rootSlots; slot++) {
        const auto& binding = slot == icbSlot ? icb : m_cbvBindings[stageIdx][slot];
        const D3D12_GPU_VIRTUAL_ADDRESS address = binding.bound && binding.gpuAddress ? binding.gpuAddress : zeroAddress;
        if (compute) {
          if (m_currentComputeRootCbvs[slot] == address)
            continue;
          m_currentComputeRootCbvs[slot] = address;
          m_cmdList->SetComputeRootConstantBufferView(kComputeRootCbvBase + slot, address);
        } else {
          const uint32_t index = graphicsRootCbvIndex(stageIdx, slot);
          if (m_currentGraphicsRootCbvs[index - kGraphicsRootCbvBase] == address)
            continue;
          m_currentGraphicsRootCbvs[index - kGraphicsRootCbvBase] = address;
          m_cmdList->SetGraphicsRootConstantBufferView(index, address);
        }
      }

      uint32_t descriptorCount = 0;
      for (uint32_t slot = rootSlots; slot < kMaxConstantBuffers; slot++) {
        const auto& binding = m_cbvBindings[stageIdx][slot];
        if (binding.bound || binding.gpuAddress || binding.sizeInBytes)
          descriptorCount = slot + 1;
      }
      descriptorCount = (std::min)(clampDescriptorCountToShader(stageIdx, descriptorCount, &ShaderResourceLimits::cbvCount),
                                   uint32_t(kMaxConstantBuffers));
      if (icbSlot != UINT32_MAX && icbSlot < kMaxConstantBuffers)
        descriptorCount = (std::max)(descriptorCount, icbSlot + 1);
      // The table covers the slots after the root descriptors.
      if (descriptorCount <= rootSlots)
        return;
      const uint32_t tableFirst = rootSlots;
      descriptorCount -= tableFirst;

      const bool logThis = cbvCommitLogCount < 8;
      if (logThis) {
        logMessage(LogLevel::Info,
          "commitCbvBindings: stage=%u root=%u compute=%u count=%u fallbackGpu=%p fallbackSize=%u",
          stageIdx, rootIndex, compute ? 1u : 0u, descriptorCount,
          reinterpret_cast<void*>(static_cast<uintptr_t>(fallback.gpuAddress)),
          fallback.sizeInBytes);
        cbvCommitLogCount++;
      }

      if (!descriptorCount || !fallback.gpuAddress)
        return;

      std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxConstantBuffers> cbvGpuAddresses;
      std::array<UINT, kMaxConstantBuffers> cbvSizes;
      uint64_t fingerprint = descriptorTableFingerprintSeed(descriptorCount);
      for (uint32_t slot = 0; slot < descriptorCount; slot++) {
        const auto& srcBinding = tableFirst + slot == icbSlot ? icb
          : m_cbvBindings[stageIdx][tableFirst + slot].bound ? m_cbvBindings[stageIdx][tableFirst + slot]
          : fallback;

        cbvGpuAddresses[slot] = srcBinding.gpuAddress;
        cbvSizes[slot] = srcBinding.sizeInBytes;
        fingerprint = mixDescriptorTableFingerprint(fingerprint, static_cast<uint64_t>(cbvGpuAddresses[slot]));
        fingerprint = mixDescriptorTableFingerprint(fingerprint, static_cast<uint64_t>(cbvSizes[slot]));

        if (logThis && slot < 4) {
          logMessage(LogLevel::Info,
            "commitCbvBindings: stage=%u slot=%u bound=%u gpu=%p size=%u",
            stageIdx, slot, srcBinding.bound ? 1u : 0u,
            reinterpret_cast<void*>(static_cast<uintptr_t>(srcBinding.gpuAddress)),
            srcBinding.sizeInBytes);
        }
      }

      auto bindGpuHandle = [&](D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
        if (compute) {
          if (!computeRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          m_cmdList->SetComputeRootDescriptorTable(rootIndex, gpuHandle);
          noteComputeRootTable(rootIndex, gpuHandle);
        } else {
          if (!graphicsRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          m_cmdList->SetGraphicsRootDescriptorTable(rootIndex, gpuHandle);
          noteGraphicsRootTable(rootIndex, gpuHandle);
        }
      };

      auto& cache = m_cbvTableCache[stageIdx];
      auto& cacheLookup = m_cbvTableCacheLookup[stageIdx];
      auto cachedIndex = cacheLookup.find(fingerprint);
      if (cachedIndex != cacheLookup.end() && cachedIndex->second < cache.size()) {
        const auto& entry = cache[cachedIndex->second];
        bool match = true;
        if (entry.descriptorCount != descriptorCount || entry.fingerprint != fingerprint) {
          match = false;
        } else {
          for (uint32_t slot = 0; slot < descriptorCount; slot++) {
            if (entry.gpuAddresses[slot] != cbvGpuAddresses[slot]
             || entry.sizes[slot] != cbvSizes[slot]) {
              match = false;
              break;
            }
          }
        }

        if (match) {
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalDescriptorCacheHits++;
          bindGpuHandle(entry.gpuHandle);
          return;
        }
      }

      D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = allocateShaderVisibleCbvSrvUav();
      uint32_t startOffset = m_cbvSrvUavHeapOffset - 1;
      for (uint32_t i = 1; i < descriptorCount; i++)
        allocateShaderVisibleCbvSrvUav();

      for (uint32_t slot = 0; slot < descriptorCount; slot++) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        cpuHandle.ptr = cpuStart.ptr + slot * m_cbvSrvUavDescSize;

        D3D12_CPU_DESCRIPTOR_HANDLE cachedCbv = getOrCreateCachedCbvDescriptor(
          cbvGpuAddresses[slot],
          cbvSizes[slot]);

        if (cachedCbv.ptr) {
          d3d12Device->CopyDescriptorsSimple(1, cpuHandle, cachedCbv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalDescriptorCopies++;
        } else {
          D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
          cbvDesc.BufferLocation = cbvGpuAddresses[slot];
          cbvDesc.SizeInBytes = cbvSizes[slot];
          d3d12Device->CreateConstantBufferView(&cbvDesc, cpuHandle);
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalCbvCreates++;
        }
      }

      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
      gpuHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr +
                      startOffset * m_cbvSrvUavDescSize;

      // Perf: cap table cache to bound long-session growth and keep lookup O(1).
      // Clearing is semantics-preserving (miss just recomputes descriptors).
      if (cache.size() >= 1024) {
        cache.clear();
        cacheLookup.clear();
      }
      CbvTableCacheEntry entry = {};
      entry.descriptorCount = descriptorCount;
      entry.fingerprint = fingerprint;
      std::copy_n(cbvGpuAddresses.begin(), descriptorCount, entry.gpuAddresses.begin());
      std::copy_n(cbvSizes.begin(), descriptorCount, entry.sizes.begin());
      entry.gpuHandle = gpuHandle;
      cache.push_back(entry);
      cacheLookup[fingerprint] = cache.size() - 1;

      bindGpuHandle(gpuHandle);
    };

    if (m_isComputePipeline) {
      commitStage(static_cast<uint32_t>(ShaderStage::Compute), 0, true);
    } else {
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if (m_currentGraphicsStageMask & (1u << stage))
          commitStage(stage, stage, false);
    }
    m_cbvDirty = std::any_of(m_cbvStageDirty.begin(), m_cbvStageDirty.end(), [](bool dirty) { return dirty; });
#endif
  }

  void D3D12BackendContext::commitSrvBindings() {
#ifdef _WIN32
    auto hasActiveDirty = [&]() {
      if (m_isComputePipeline)
        return m_srvStageDirty[static_cast<uint32_t>(ShaderStage::Compute)];
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if ((m_currentGraphicsStageMask & (1u << stage)) && m_srvStageDirty[stage])
          return true;
      return false;
    };
    if (!hasActiveDirty()) return;

    auto* d3d12Device = m_device->getDevice();

    auto commitStage = [&](uint32_t stageIdx, uint32_t rootIndex, bool compute) {
      if (!m_srvStageDirty[stageIdx])
        return;
      m_srvStageDirty[stageIdx] = false;

      std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxShaderResources> srcHandles;
      std::array<uint64_t, kMaxShaderResources> srcVersions;
      uint32_t descriptorCount = 0;

      const uint32_t bindingEnd = m_srvBindingEnd[stageIdx];
      for (uint32_t slot = 0; slot < bindingEnd; slot++) {
        const auto& binding = m_srvBindings[stageIdx][slot];
        if (binding.handle.ptr || binding.nullHandle.ptr)
          descriptorCount = slot + 1;
      }
      descriptorCount = (std::min)(clampDescriptorCountToShader(stageIdx, descriptorCount, &ShaderResourceLimits::srvCount),
                                   uint32_t(kMaxShaderResources));
      if (!descriptorCount)
        return;

      // Unbound registers get a null view of the dimension the shader
      // declares for them.
      const ShaderResourceLimits* limits = activeResourceLimits(stageIdx);
      const uint8_t* dimensions = limits && limits->known ? limits->dimensions : nullptr;
      uint64_t fingerprint = descriptorTableFingerprintSeed(descriptorCount);
      for (uint32_t slot = 0; slot < descriptorCount; slot++) {
        const auto& binding = m_srvBindings[stageIdx][slot];
        srcHandles[slot] = binding.bound && binding.handle.ptr
          ? binding.handle
          : m_device->getNullSrv(dimensions ? dimensions[slot] : 0);
        srcVersions[slot] = binding.bound && binding.handle.ptr
          ? binding.descriptorVersion
          : 0;
        fingerprint = mixDescriptorTableFingerprint(fingerprint, static_cast<uint64_t>(srcHandles[slot].ptr));
        fingerprint = mixDescriptorTableFingerprint(fingerprint, srcVersions[slot]);
      }

      auto bindGpuHandle = [&](D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
        if (compute) {
          if (!computeRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          m_cmdList->SetComputeRootDescriptorTable(rootIndex, gpuHandle);
          noteComputeRootTable(rootIndex, gpuHandle);
        } else {
          if (!graphicsRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          m_cmdList->SetGraphicsRootDescriptorTable(rootIndex, gpuHandle);
          noteGraphicsRootTable(rootIndex, gpuHandle);
        }
      };

      auto& cache = m_srvTableCache[stageIdx];
      auto& sources = m_srvTableSources[stageIdx];
      auto& cacheLookup = m_srvTableCacheLookup[stageIdx];
      auto cachedIndex = cacheLookup.find(fingerprint);
      if (cachedIndex != cacheLookup.end() && cachedIndex->second < cache.size()) {
        const auto& entry = cache[cachedIndex->second];
        bool match = true;
        if (entry.descriptorCount != descriptorCount || entry.fingerprint != fingerprint) {
          match = false;
        } else {
          const TableSource* cached = sources.data() + entry.first;
          for (uint32_t slot = 0; slot < descriptorCount; slot++) {
            if (cached[slot].handle != srcHandles[slot].ptr
             || cached[slot].version != srcVersions[slot]) {
              match = false;
              break;
            }
          }
        }

        if (match) {
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalDescriptorCacheHits++;
          bindGpuHandle(entry.gpuHandle);
          return;
        }
      }

      D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = allocateShaderVisibleCbvSrvUav();
      uint32_t startOffset = m_cbvSrvUavHeapOffset - 1;
      for (uint32_t i = 1; i < descriptorCount; i++)
        allocateShaderVisibleCbvSrvUav();

      // One call gathers the scattered source descriptors into the table.
      static const std::array<UINT, kMaxShaderResources> kSingleDescriptorRanges = [] {
        std::array<UINT, kMaxShaderResources> ranges;
        ranges.fill(1u);
        return ranges;
      }();
      const UINT tableSize = descriptorCount;
      d3d12Device->CopyDescriptors(1, &cpuStart, &tableSize,
        descriptorCount, srcHandles.data(), kSingleDescriptorRanges.data(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      if (perfStatsEnabled() && !m_copyOnly)
        m_perfIntervalDescriptorCopies += descriptorCount;

      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
      gpuHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr +
                      startOffset * m_cbvSrvUavDescSize;

      // Perf: cap table cache to bound long-session growth and keep lookup O(1).
      // Clearing is semantics-preserving (miss just recomputes descriptors).
      if (cache.size() >= 1024) {
        cache.clear();
        sources.clear();
        cacheLookup.clear();
      }
      SrvTableCacheEntry entry;
      entry.descriptorCount = descriptorCount;
      entry.first = static_cast<uint32_t>(sources.size());
      entry.fingerprint = fingerprint;
      entry.gpuHandle = gpuHandle;
      for (uint32_t slot = 0; slot < descriptorCount; slot++)
        sources.push_back(TableSource{ srcHandles[slot].ptr, srcVersions[slot] });
      cache.push_back(entry);
      cacheLookup[fingerprint] = cache.size() - 1;

      bindGpuHandle(gpuHandle);
    };

    if (m_isComputePipeline) {
      commitStage(static_cast<uint32_t>(ShaderStage::Compute), 1, true);
    } else {
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if (m_currentGraphicsStageMask & (1u << stage))
          commitStage(stage, 5 + stage, false);
    }
    m_srvDirty = std::any_of(m_srvStageDirty.begin(), m_srvStageDirty.end(), [](bool dirty) { return dirty; });
#endif
  }

  void D3D12BackendContext::commitUavBindings() {
#ifdef _WIN32
    auto hasActiveDirty = [&]() {
      if (m_isComputePipeline)
        return m_uavStageDirty[static_cast<uint32_t>(ShaderStage::Compute)];
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if ((m_currentGraphicsStageMask & (1u << stage)) && m_uavStageDirty[stage])
          return true;
      return false;
    };
    if (!hasActiveDirty()) return;

    auto* d3d12Device = m_device->getDevice();
    static int uavCommitLogCount = 0;

    auto commitStage = [&](uint32_t stageIdx, uint32_t rootIndex, bool compute) {
      if (!m_uavStageDirty[stageIdx])
        return;
      m_uavStageDirty[stageIdx] = false;

      std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxUAVs> srcHandles;
      std::array<uint64_t, kMaxUAVs> srcVersions;
      uint32_t descriptorCount = 0;

      const uint32_t bindingEnd = m_uavBindingEnd[stageIdx];
      for (uint32_t slot = 0; slot < bindingEnd; slot++) {
        const auto& binding = m_uavBindings[stageIdx][slot];
        if (binding.handle.ptr || binding.nullHandle.ptr)
          descriptorCount = slot + 1;
      }
      descriptorCount = (std::min)(clampDescriptorCountToShader(stageIdx, descriptorCount, &ShaderResourceLimits::uavCount),
                                   uint32_t(kMaxUAVs));
      if (!descriptorCount)
        return;

      // As for shader resources: null views of the declared dimensions.
      const ShaderResourceLimits* limits = activeResourceLimits(stageIdx);
      const uint8_t* dimensions = limits && limits->known && limits->dimensions ? limits->dimensions + 128 : nullptr;
      uint64_t fingerprint = descriptorTableFingerprintSeed(descriptorCount);
      for (uint32_t slot = 0; slot < descriptorCount; slot++) {
        const auto& binding = m_uavBindings[stageIdx][slot];
        srcHandles[slot] = binding.bound && binding.handle.ptr
          ? binding.handle
          : m_device->getNullUav(dimensions ? dimensions[slot] : 11);
        srcVersions[slot] = binding.bound && binding.handle.ptr
          ? binding.descriptorVersion
          : 0;
        fingerprint = mixDescriptorTableFingerprint(fingerprint, static_cast<uint64_t>(srcHandles[slot].ptr));
        fingerprint = mixDescriptorTableFingerprint(fingerprint, srcVersions[slot]);
      }

      auto bindGpuHandle = [&](D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
        if (compute) {
          if (!computeRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          if (!safeSetComputeRootDescriptorTable(&m_cmdList, rootIndex, gpuHandle)) {
            logMessage(LogLevel::Error,
              "commitUavBindings: compute root table fault at stage=%u root=%u gpu=%p",
              stageIdx,
              rootIndex,
              reinterpret_cast<void*>(gpuHandle.ptr));
          } else {
            noteComputeRootTable(rootIndex, gpuHandle);
          }
        } else {
          if (!graphicsRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          if (!safeSetGraphicsRootDescriptorTable(&m_cmdList, rootIndex, gpuHandle)) {
            logMessage(LogLevel::Error,
              "commitUavBindings: graphics root table fault at stage=%u root=%u gpu=%p",
              stageIdx,
              rootIndex,
              reinterpret_cast<void*>(gpuHandle.ptr));
          } else {
            noteGraphicsRootTable(rootIndex, gpuHandle);
          }
        }
      };

      auto& cache = m_uavTableCache[stageIdx];
      auto& sources = m_uavTableSources[stageIdx];
      auto& cacheLookup = m_uavTableCacheLookup[stageIdx];
      auto cachedIndex = cacheLookup.find(fingerprint);
      if (cachedIndex != cacheLookup.end() && cachedIndex->second < cache.size()) {
        const auto& entry = cache[cachedIndex->second];
        bool match = true;
        if (entry.descriptorCount != descriptorCount || entry.fingerprint != fingerprint) {
          match = false;
        } else {
          const TableSource* cached = sources.data() + entry.first;
          for (uint32_t slot = 0; slot < descriptorCount; slot++) {
            if (cached[slot].handle != srcHandles[slot].ptr
             || cached[slot].version != srcVersions[slot]) {
              match = false;
              break;
            }
          }
        }

        if (match) {
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalDescriptorCacheHits++;
          bindGpuHandle(entry.gpuHandle);
          return;
        }
      }

      const bool logCommit = uavCommitLogCount < 8;
      if (logCommit) {
        logMessage(LogLevel::Info,
          "commitUavBindings: stage=%u root=%u compute=%u count=%u",
          stageIdx,
          rootIndex,
          compute ? 1u : 0u,
          descriptorCount);
      }
      uavCommitLogCount++;

      D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = allocateShaderVisibleCbvSrvUav();
      uint32_t startOffset = m_cbvSrvUavHeapOffset - 1;
      for (uint32_t i = 1; i < descriptorCount; i++)
        allocateShaderVisibleCbvSrvUav();

      for (uint32_t slot = 0; slot < descriptorCount; slot++) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        cpuHandle.ptr = cpuStart.ptr + slot * m_cbvSrvUavDescSize;

        if (logCommit && slot < 16) {
          logMessage(LogLevel::Info,
            "commitUavBindings: stage=%u slot=%u bound=%u handle=%p null=%p src=%p",
            stageIdx,
            slot,
            m_uavBindings[stageIdx][slot].bound ? 1u : 0u,
            reinterpret_cast<void*>(m_uavBindings[stageIdx][slot].handle.ptr),
            reinterpret_cast<void*>(m_uavBindings[stageIdx][slot].nullHandle.ptr),
            reinterpret_cast<void*>(srcHandles[slot].ptr));
        }

        if (!safeCopyCbvSrvUavDescriptor(d3d12Device, cpuHandle, srcHandles[slot])) {
          logMessage(LogLevel::Error,
            "commitUavBindings: descriptor copy fault at stage=%u slot=%u root=%u bound=%u handle=%p null=%p src=%p",
            stageIdx,
            slot,
            rootIndex,
            m_uavBindings[stageIdx][slot].bound ? 1u : 0u,
            reinterpret_cast<void*>(m_uavBindings[stageIdx][slot].handle.ptr),
            reinterpret_cast<void*>(m_uavBindings[stageIdx][slot].nullHandle.ptr),
            reinterpret_cast<void*>(srcHandles[slot].ptr));
          return;
        }
      }
      if (perfStatsEnabled() && !m_copyOnly)
        m_perfIntervalDescriptorCopies += descriptorCount;

      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
      gpuHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr +
                      startOffset * m_cbvSrvUavDescSize;

      if (logCommit) {
        logMessage(LogLevel::Info,
          "commitUavBindings: stage=%u setRoot gpu=%p heapOffset=%u",
          stageIdx,
          reinterpret_cast<void*>(gpuHandle.ptr),
          startOffset);
      }

      // Perf: cap table cache to bound long-session growth and keep lookup O(1).
      // Clearing is semantics-preserving (miss just recomputes descriptors).
      if (cache.size() >= 1024) {
        cache.clear();
        sources.clear();
        cacheLookup.clear();
      }
      UavTableCacheEntry entry;
      entry.descriptorCount = descriptorCount;
      entry.first = static_cast<uint32_t>(sources.size());
      entry.fingerprint = fingerprint;
      entry.gpuHandle = gpuHandle;
      for (uint32_t slot = 0; slot < descriptorCount; slot++)
        sources.push_back(TableSource{ srcHandles[slot].ptr, srcVersions[slot] });
      cache.push_back(entry);
      cacheLookup[fingerprint] = cache.size() - 1;

      bindGpuHandle(gpuHandle);
    };

    if (m_isComputePipeline) {
      commitStage(static_cast<uint32_t>(ShaderStage::Compute), 3, true);
    } else {
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if (m_currentGraphicsStageMask & (1u << stage))
          commitStage(stage, 15 + stage, false);
    }
    m_uavDirty = std::any_of(m_uavStageDirty.begin(), m_uavStageDirty.end(), [](bool dirty) { return dirty; });
#endif
  }

  void D3D12BackendContext::commitSamplerBindings() {
#ifdef _WIN32
    auto hasActiveDirty = [&]() {
      if (m_isComputePipeline)
        return m_samplerStageDirty[static_cast<uint32_t>(ShaderStage::Compute)];
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if ((m_currentGraphicsStageMask & (1u << stage)) && m_samplerStageDirty[stage])
          return true;
      return false;
    };
    if (!hasActiveDirty()) return;

    auto commitStage = [&](uint32_t stageIdx, uint32_t rootIndex, bool compute) {
      if (!m_samplerStageDirty[stageIdx])
        return;
      m_samplerStageDirty[stageIdx] = false;

      // Unbound samplers sample like D3D11's default sampler state.
      const D3D12_CPU_DESCRIPTOR_HANDLE fallback = m_device->getDefaultSampler();
      std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxSamplers> srcHandles;
      uint32_t descriptorCount = 0;

      for (uint32_t slot = 0; slot < kMaxSamplers; slot++) {
        const auto& binding = m_samplerBindings[stageIdx][slot];
        if (binding.handle.ptr)
          descriptorCount = slot + 1;
      }
      descriptorCount = (std::min)(clampDescriptorCountToShader(stageIdx, descriptorCount, &ShaderResourceLimits::samplerCount),
                                   uint32_t(kMaxSamplers));

      if (!descriptorCount || !fallback.ptr)
        return;

      std::array<uint64_t, kMaxSamplers> srcVersions;
      uint64_t fingerprint = descriptorTableFingerprintSeed(descriptorCount);
      for (uint32_t slot = 0; slot < descriptorCount; slot++) {
        const auto& binding = m_samplerBindings[stageIdx][slot];
        const bool bound = binding.bound && binding.handle.ptr;
        srcHandles[slot] = bound ? binding.handle : fallback;
        srcVersions[slot] = bound ? binding.descriptorVersion : 0;
        fingerprint = mixDescriptorTableFingerprint(fingerprint, static_cast<uint64_t>(srcHandles[slot].ptr));
        fingerprint = mixDescriptorTableFingerprint(fingerprint, srcVersions[slot]);
      }

      auto bindGpuHandle = [&](D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
        if (compute) {
          if (!computeRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          m_cmdList->SetComputeRootDescriptorTable(rootIndex, gpuHandle);
          noteComputeRootTable(rootIndex, gpuHandle);
        } else {
          if (!graphicsRootTableNeedsSet(rootIndex, gpuHandle))
            return;
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalRootTableSets++;
          m_cmdList->SetGraphicsRootDescriptorTable(rootIndex, gpuHandle);
          noteGraphicsRootTable(rootIndex, gpuHandle);
        }
      };

      const uint32_t offset = acquireSamplerTable(fingerprint, descriptorCount, srcHandles.data(), srcVersions.data());
      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
      gpuHandle.ptr = m_shaderVisibleSamplerHeap->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(offset) * m_samplerDescSize;
      bindGpuHandle(gpuHandle);
    };

    if (m_isComputePipeline) {
      commitStage(static_cast<uint32_t>(ShaderStage::Compute), 2, true);
    } else {
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        if (m_currentGraphicsStageMask & (1u << stage))
          commitStage(stage, 10 + stage, false);
    }
    m_samplerDirty = std::any_of(m_samplerStageDirty.begin(), m_samplerStageDirty.end(), [](bool dirty) { return dirty; });
#endif
  }

  void D3D12BackendContext::commitDescriptorBindings() {
#ifdef _WIN32
    const uint32_t activeStageMask = m_isComputePipeline
      ? (1u << static_cast<uint32_t>(ShaderStage::Compute))
      : m_currentGraphicsStageMask;

    // The common multi-draw path has neither a command-list reset nor changed
    // descriptors. A maintained stage mask makes this a pair of integer tests
    // instead of scanning four dirty arrays for every draw.
    if (!m_psoDirtyAfterFlush && !(m_descriptorDirtyStageMask & activeStageMask))
      return;

    const bool collectTiming = m_perfTimingEnabled && !m_copyOnly;
    if (collectTiming && m_perfCounterFrequency == 0) {
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      m_perfCounterFrequency = freq.QuadPart;
    }

    LARGE_INTEGER totalStart = {}, totalEnd = {}, stepStart = {}, stepEnd = {};
    if (collectTiming)
      QueryPerformanceCounter(&totalStart);

    // Re-bind PSO after flush if needed (deferred from resetCommandList)
    if (m_psoDirtyAfterFlush && m_currentPSO) {
      if (m_perfStatsEnabled && !m_copyOnly)
        m_perfIntervalPsoBinds++;
      m_cmdList->SetPipelineState(m_currentPSO);
      m_psoDirtyAfterFlush = false;
      // The reset bound every tracked vertex buffer again.
      if (!m_isComputePipeline)
        applyNullVertexSlot(m_nullVertexSlot);
    }

    if (collectTiming)
      QueryPerformanceCounter(&stepStart);
    commitCbvBindings();
    if (collectTiming) {
      QueryPerformanceCounter(&stepEnd);
      m_perfIntervalCommitCbvMs += qpcElapsedMs(stepStart, stepEnd, m_perfCounterFrequency);
      stepStart = stepEnd;
    }

    commitSrvBindings();
    if (collectTiming) {
      QueryPerformanceCounter(&stepEnd);
      m_perfIntervalCommitSrvMs += qpcElapsedMs(stepStart, stepEnd, m_perfCounterFrequency);
      stepStart = stepEnd;
    }

    commitUavBindings();
    if (collectTiming) {
      QueryPerformanceCounter(&stepEnd);
      m_perfIntervalCommitUavMs += qpcElapsedMs(stepStart, stepEnd, m_perfCounterFrequency);
      stepStart = stepEnd;
    }

    commitSamplerBindings();
    if (collectTiming) {
      QueryPerformanceCounter(&stepEnd);
      m_perfIntervalCommitSamplerMs += qpcElapsedMs(stepStart, stepEnd, m_perfCounterFrequency);
      QueryPerformanceCounter(&totalEnd);
      m_perfIntervalDescriptorCommitMs += qpcElapsedMs(totalStart, totalEnd, m_perfCounterFrequency);
    }

    uint32_t remainingDirtyStageMask = 0;
    for (uint32_t stage = 0; stage < kShaderStages; stage++) {
      if (m_cbvStageDirty[stage]
       || m_srvStageDirty[stage]
       || m_uavStageDirty[stage]
       || m_samplerStageDirty[stage]) {
        remainingDirtyStageMask |= 1u << stage;
      }
    }
    m_descriptorDirtyStageMask = remainingDirtyStageMask;
#endif
  }
#endif

  void D3D12BackendContext::bindShaderResource(ShaderStage stage, uint32_t slot, IBackendImageView* view) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxShaderResources) return;

    uint32_t stageIdx = static_cast<uint32_t>(stage);
    if (stageIdx >= kShaderStages) return;

    static int srvBindImageLogCount = 0;

    auto& binding = m_srvBindings[stageIdx][slot];
    if (slot >= m_srvBindingEnd[stageIdx])
      m_srvBindingEnd[stageIdx] = slot + 1;

    if (!view) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentSrvResources[stageIdx][slot] = nullptr;
      m_srvBindingSerial++;
      m_srvImageViews[stageIdx][slot] = nullptr;
      m_currentSrvFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindSRVImage %s[%u]=null", shaderStageName(stage), slot);
      m_srvDirty = true;
      m_srvStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12View = static_cast<D3D12BackendImageView*>(view);
    if (!d3d12View->getHandle().ptr) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentSrvResources[stageIdx][slot] = nullptr;
      m_srvBindingSerial++;
      m_srvImageViews[stageIdx][slot] = nullptr;
      m_currentSrvFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindSRVImage %s[%u]=nullHandle", shaderStageName(stage), slot);
      m_srvDirty = true;
      m_srvStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());
    // A state that includes the shader read states serves as it is, such as
    // a depth buffer bound read-only at the same time.
    noteSharedUse(d3d12Image);
    ensureImageViewState(d3d12View, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    holdBatchResourceOnce(d3d12Image->getResource(),
      binding.resourceHeld,
      binding.resourceHeldGeneration);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = d3d12View->getHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = d3d12View->getNullHandle();
    const uint64_t descriptorVersion = d3d12View->getDescriptorVersion();
    if (binding.bound
     && binding.handle.ptr == handle.ptr
     && binding.nullHandle.ptr == nullHandle.ptr
     && binding.descriptorVersion == descriptorVersion)
      return;

    const auto& viewDesc = d3d12View->getDesc();
    const auto& imageDesc = d3d12Image->getDesc();
    DXGI_FORMAT trackedFormat = viewDesc.format == DXGI_FORMAT_UNKNOWN ? imageDesc.format : viewDesc.format;

    binding.handle = handle;
    binding.nullHandle = nullHandle;
    binding.descriptorVersion = descriptorVersion;
    binding.bound = true;
    m_currentSrvResources[stageIdx][slot] = d3d12Image->getResource();
    m_srvBindingSerial++;
    m_srvImageViews[stageIdx][slot] = d3d12View;
    m_currentSrvFormats[stageIdx][slot] = trackedFormat;
    DXUP_RECENT_OP(
      "BindSRVImage %s[%u] res=%p view=%p viewFmt=%u imgFmt=%u size=%ux%u mip=%u+%u layer=%u+%u usage=0x%x",
      shaderStageName(stage),
      slot,
      d3d12Image->getResource(),
      d3d12View,
      static_cast<uint32_t>(trackedFormat),
      static_cast<uint32_t>(imageDesc.format),
      imageDesc.width,
      imageDesc.height,
      viewDesc.baseMipLevel,
      viewDesc.mipLevelCount,
      viewDesc.baseArrayLayer,
      viewDesc.arrayLayerCount,
      static_cast<uint32_t>(viewDesc.usage));
    if (srvBindImageLogCount < 96) {
      logMessage(LogLevel::Info,
        "bindSrvImage: stage=%s slot=%u res=%p view=%p viewFmt=%u imgFmt=%u size=%ux%u mip=%u+%u layer=%u+%u usage=0x%x handle=%p null=%p",
        shaderStageName(stage),
        slot,
        d3d12Image->getResource(),
        d3d12View,
        static_cast<uint32_t>(trackedFormat),
        static_cast<uint32_t>(imageDesc.format),
        imageDesc.width,
        imageDesc.height,
        viewDesc.baseMipLevel,
        viewDesc.mipLevelCount,
        viewDesc.baseArrayLayer,
        viewDesc.arrayLayerCount,
        static_cast<uint32_t>(viewDesc.usage),
        reinterpret_cast<void*>(handle.ptr),
        reinterpret_cast<void*>(nullHandle.ptr));
      srvBindImageLogCount++;
    }
    m_srvDirty = true;
    m_srvStageDirty[stageIdx] = true;
    m_descriptorDirtyStageMask |= 1u << stageIdx;
#endif
  }

  void D3D12BackendContext::bindShaderResource(ShaderStage stage, uint32_t slot, IBackendBufferView* view) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxShaderResources) return;

    uint32_t stageIdx = static_cast<uint32_t>(stage);
    if (stageIdx >= kShaderStages) return;

    static int srvBindBufferLogCount = 0;

    auto& binding = m_srvBindings[stageIdx][slot];
    if (slot >= m_srvBindingEnd[stageIdx])
      m_srvBindingEnd[stageIdx] = slot + 1;

    if (!view) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentSrvResources[stageIdx][slot] = nullptr;
      m_srvBindingSerial++;
      m_srvImageViews[stageIdx][slot] = nullptr;
      m_currentSrvFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindSRVBuffer %s[%u]=null", shaderStageName(stage), slot);
      m_srvDirty = true;
      m_srvStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12View = static_cast<D3D12BackendBufferView*>(view);
    if (!d3d12View->getSrvHandle().ptr) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentSrvResources[stageIdx][slot] = nullptr;
      m_srvBindingSerial++;
      m_srvImageViews[stageIdx][slot] = nullptr;
      m_currentSrvFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindSRVBuffer %s[%u]=nullHandle", shaderStageName(stage), slot);
      m_srvDirty = true;
      m_srvStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(d3d12View->getBuffer());
    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    holdBatchResourceOnce(d3d12Buffer->getResource(),
      binding.resourceHeld,
      binding.resourceHeldGeneration);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = d3d12View->getSrvHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = d3d12View->getNullSrvHandle();
    uint64_t descriptorVersion = d3d12View->getDescriptorVersion();
    if (binding.bound
     && binding.handle.ptr == handle.ptr
     && binding.nullHandle.ptr == nullHandle.ptr
     && binding.descriptorVersion == descriptorVersion)
      return;

    const auto& viewDesc = d3d12View->getDesc();

    binding.handle = handle;
    binding.nullHandle = nullHandle;
    binding.descriptorVersion = descriptorVersion;
    binding.bound = true;
    m_currentSrvResources[stageIdx][slot] = d3d12Buffer->getResource();
    m_srvBindingSerial++;
    m_srvImageViews[stageIdx][slot] = nullptr;
    m_currentSrvFormats[stageIdx][slot] = viewDesc.format;
    DXUP_RECENT_OP(
      "BindSRVBuffer %s[%u] res=%p view=%p fmt=%u raw=%u offset=%llu size=%llu",
      shaderStageName(stage),
      slot,
      d3d12Buffer->getResource(),
      d3d12View,
      static_cast<uint32_t>(viewDesc.format),
      viewDesc.raw ? 1u : 0u,
      static_cast<unsigned long long>(viewDesc.offset),
      static_cast<unsigned long long>(viewDesc.size));
    if (srvBindBufferLogCount < 96) {
      logMessage(LogLevel::Info,
        "bindSrvBuffer: stage=%s slot=%u res=%p view=%p fmt=%u raw=%u offset=%llu size=%llu handle=%p null=%p",
        shaderStageName(stage),
        slot,
        d3d12Buffer->getResource(),
        d3d12View,
        static_cast<uint32_t>(viewDesc.format),
        viewDesc.raw ? 1u : 0u,
        static_cast<unsigned long long>(viewDesc.offset),
        static_cast<unsigned long long>(viewDesc.size),
        reinterpret_cast<void*>(handle.ptr),
        reinterpret_cast<void*>(nullHandle.ptr));
      srvBindBufferLogCount++;
    }
    m_srvDirty = true;
    m_srvStageDirty[stageIdx] = true;
    m_descriptorDirtyStageMask |= 1u << stageIdx;
#endif
  }

  void D3D12BackendContext::bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendImageView* view) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxUAVs) return;

    uint32_t stageIdx = static_cast<uint32_t>(stage);
    if (stageIdx >= kShaderStages) return;

    static int uavBindImageLogCount = 0;

    auto& binding = m_uavBindings[stageIdx][slot];
    if (slot >= m_uavBindingEnd[stageIdx])
      m_uavBindingEnd[stageIdx] = slot + 1;

    if (!view) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentUavResources[stageIdx][slot] = nullptr;
      m_uavImageViews[stageIdx][slot] = nullptr;
      m_currentUavFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindUAVImage %s[%u]=null", shaderStageName(stage), slot);
      m_uavDirty = true;
      m_uavStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12View = static_cast<D3D12BackendImageView*>(view);
    if (!d3d12View->getUavHandle().ptr) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentUavResources[stageIdx][slot] = nullptr;
      m_uavImageViews[stageIdx][slot] = nullptr;
      m_currentUavFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindUAVImage %s[%u]=nullHandle", shaderStageName(stage), slot);
      m_uavDirty = true;
      m_uavStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());
    transitionResource(d3d12Image, d3d12View, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    holdBatchResourceOnce(d3d12Image->getResource(),
      binding.resourceHeld,
      binding.resourceHeldGeneration);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = d3d12View->getUavHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = d3d12View->getNullUavHandle();
    // A view created later in the place of a destroyed one has another
    // version: descriptor tables cached for the old view do not match it.
    const uint64_t descriptorVersion = d3d12View->getDescriptorVersion();
    const auto& viewDesc = d3d12View->getDesc();
    const auto& imageDesc = d3d12Image->getDesc();
    DXGI_FORMAT trackedFormat = viewDesc.format == DXGI_FORMAT_UNKNOWN ? imageDesc.format : viewDesc.format;

    if (uavBindImageLogCount < 32) {
      logMessage(LogLevel::Info,
        "bindUavImage: stage=%s slot=%u res=%p type=%u viewFmt=%u imgFmt=%u samples=%u array=%u mip=%u handle=%p null=%p",
        shaderStageName(stage),
        slot,
        d3d12Image->getResource(),
        static_cast<uint32_t>(viewDesc.type),
        static_cast<uint32_t>(trackedFormat),
        static_cast<uint32_t>(imageDesc.format),
        imageDesc.sampleCount,
        imageDesc.arraySize,
        imageDesc.mipLevels,
        reinterpret_cast<void*>(handle.ptr),
        reinterpret_cast<void*>(nullHandle.ptr));
      uavBindImageLogCount++;
    }

    if (binding.bound
     && binding.handle.ptr == handle.ptr
     && binding.nullHandle.ptr == nullHandle.ptr
     && binding.descriptorVersion == descriptorVersion)
      return;

    binding.handle = handle;
    binding.nullHandle = nullHandle;
    binding.descriptorVersion = descriptorVersion;
    binding.bound = true;
    m_currentUavResources[stageIdx][slot] = d3d12Image->getResource();
    m_uavImageViews[stageIdx][slot] = d3d12View;
    m_currentUavFormats[stageIdx][slot] = trackedFormat;
    DXUP_RECENT_OP(
      "BindUAVImage %s[%u] res=%p view=%p viewFmt=%u imgFmt=%u mip=%u+%u layer=%u+%u usage=0x%x",
      shaderStageName(stage),
      slot,
      d3d12Image->getResource(),
      d3d12View,
      static_cast<uint32_t>(trackedFormat),
      static_cast<uint32_t>(imageDesc.format),
      viewDesc.baseMipLevel,
      viewDesc.mipLevelCount,
      viewDesc.baseArrayLayer,
      viewDesc.arrayLayerCount,
      static_cast<uint32_t>(viewDesc.usage));
    m_uavDirty = true;
    m_uavStageDirty[stageIdx] = true;
    m_descriptorDirtyStageMask |= 1u << stageIdx;
#endif
  }

  void D3D12BackendContext::bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendBufferView* view) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxUAVs) return;

    uint32_t stageIdx = static_cast<uint32_t>(stage);
    if (stageIdx >= kShaderStages) return;

    static int uavBindBufferLogCount = 0;

    auto& binding = m_uavBindings[stageIdx][slot];
    if (slot >= m_uavBindingEnd[stageIdx])
      m_uavBindingEnd[stageIdx] = slot + 1;

    if (!view) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentUavResources[stageIdx][slot] = nullptr;
      m_uavImageViews[stageIdx][slot] = nullptr;
      m_currentUavFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindUAVBuffer %s[%u]=null", shaderStageName(stage), slot);
      m_uavDirty = true;
      m_uavStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12View = static_cast<D3D12BackendBufferView*>(view);
    if (!d3d12View->getUavHandle().ptr) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.nullHandle = {};
      binding.descriptorVersion = 0;
      binding.bound = false;
      m_currentUavResources[stageIdx][slot] = nullptr;
      m_uavImageViews[stageIdx][slot] = nullptr;
      m_currentUavFormats[stageIdx][slot] = DXGI_FORMAT_UNKNOWN;
      DXUP_RECENT_OP("BindUAVBuffer %s[%u]=nullHandle", shaderStageName(stage), slot);
      m_uavDirty = true;
      m_uavStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(d3d12View->getBuffer());
    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    holdBatchResourceOnce(d3d12Buffer->getResource(),
      binding.resourceHeld,
      binding.resourceHeldGeneration);

    // Also transition the counter buffer if present
    if (d3d12View->getCounterBuffer()) {
      auto* d3d12Counter = static_cast<D3D12BackendBuffer*>(d3d12View->getCounterBuffer());
      transitionResource(d3d12Counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      holdBatchResourceOnce(d3d12Counter->getResource(),
        binding.counterResourceHeld,
        binding.counterResourceHeldGeneration);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = d3d12View->getUavHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = d3d12View->getNullUavHandle();
    const auto& viewDesc = d3d12View->getDesc();

    if (uavBindBufferLogCount < 32) {
      logMessage(LogLevel::Info,
        "bindUavBuffer: stage=%s slot=%u res=%p fmt=%u raw=%u offset=%llu size=%llu handle=%p null=%p",
        shaderStageName(stage),
        slot,
        d3d12Buffer->getResource(),
        static_cast<uint32_t>(viewDesc.format),
        viewDesc.raw ? 1u : 0u,
        static_cast<unsigned long long>(viewDesc.offset),
        static_cast<unsigned long long>(viewDesc.size),
        reinterpret_cast<void*>(handle.ptr),
        reinterpret_cast<void*>(nullHandle.ptr));
      uavBindBufferLogCount++;
    }

    uint64_t descriptorVersion = d3d12View->getDescriptorVersion();
    if (binding.bound
     && binding.handle.ptr == handle.ptr
     && binding.nullHandle.ptr == nullHandle.ptr
     && binding.descriptorVersion == descriptorVersion)
      return;

    binding.handle = handle;
    binding.nullHandle = nullHandle;
    binding.descriptorVersion = descriptorVersion;
    binding.bound = true;
    m_currentUavResources[stageIdx][slot] = d3d12Buffer->getResource();
    m_uavImageViews[stageIdx][slot] = nullptr;
    m_currentUavFormats[stageIdx][slot] = viewDesc.format;
    DXUP_RECENT_OP(
      "BindUAVBuffer %s[%u] res=%p view=%p fmt=%u raw=%u offset=%llu size=%llu",
      shaderStageName(stage),
      slot,
      d3d12Buffer->getResource(),
      d3d12View,
      static_cast<uint32_t>(viewDesc.format),
      viewDesc.raw ? 1u : 0u,
      static_cast<unsigned long long>(viewDesc.offset),
      static_cast<unsigned long long>(viewDesc.size));
    m_uavDirty = true;
    m_uavStageDirty[stageIdx] = true;
    m_descriptorDirtyStageMask |= 1u << stageIdx;
#endif
  }

  void D3D12BackendContext::bindSampler(ShaderStage stage, uint32_t slot, IBackendSampler* sampler) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (slot >= kMaxSamplers) return;

    uint32_t stageIdx = static_cast<uint32_t>(stage);
    if (stageIdx >= kShaderStages) return;

    auto& binding = m_samplerBindings[stageIdx][slot];

    if (!sampler) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.bound = false;
      m_samplerDirty = true;
      m_samplerStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    auto* d3d12Sampler = static_cast<D3D12BackendSampler*>(sampler);
    if (!d3d12Sampler->getHandle().ptr) {
      if (!binding.bound && !binding.handle.ptr)
        return;

      binding.handle = {};
      binding.bound = false;
      m_samplerDirty = true;
      m_samplerStageDirty[stageIdx] = true;
      m_descriptorDirtyStageMask |= 1u << stageIdx;
      return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = d3d12Sampler->getHandle();
    const uint64_t version = d3d12Sampler->getDescriptorVersion();
    if (binding.bound && binding.handle.ptr == handle.ptr && binding.descriptorVersion == version)
      return;

    binding.handle = handle;
    binding.descriptorVersion = version;
    binding.bound = true;
    m_samplerDirty = true;
    m_samplerStageDirty[stageIdx] = true;
    m_descriptorDirtyStageMask |= 1u << stageIdx;
#endif
  }

  // ============================================================
  // Render Targets
  // ============================================================

  void D3D12BackendContext::bindRenderTargets(uint32_t count, IBackendImageView** rtvs, IBackendImageView* dsv) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    const bool logBindings = m_infoDiagnosticsEnabled;
    static int bindRtCount = 0;
    if (logBindings && bindRtCount < 5)
      logMessage(LogLevel::Info, "bindRenderTargets: count=%u dsv=%p", count, dsv);
    if (logBindings)
      bindRtCount++;
    uint32_t effectiveCount = count;
    while (effectiveCount && !rtvs[effectiveCount - 1])
      effectiveCount--;
    // Occlusion queries ended in the pass before are submitted now, at the
    // next pass: their results are known once the command list holding
    // them completed. Kena waits for its queries every frame; at the end of
    // the frame's command list that took the GPU's whole frame and drained
    // the GPU meanwhile (101 -> 124 fps, frames 7-9 ms instead of 3/16 ms).
    const void* passTargetKey = effectiveCount && rtvs[0] ? static_cast<const void*>(rtvs[0]) : dsv;
    if (passTargetKey != m_passTargetKey) {
      m_passTargetKey = passTargetKey;
      if (m_occlusionEndsSinceFlush && !m_copyOnly)
        flush();
    }
    if (m_gpuProfiler.enabled()) {
      GpuProfiler::Targets targets;
      IBackendImageView* first = effectiveCount && rtvs[0] ? rtvs[0] : dsv;
      if (first) {
        const auto& desc = first->getImage()->getDesc();
        targets.key = first;
        targets.width = desc.width;
        targets.height = desc.height;
        targets.samples = desc.sampleCount;
        targets.format = first != dsv ? desc.format : DXGI_FORMAT_UNKNOWN;
      }
      targets.count = effectiveCount;
      if (dsv)
        targets.depthFormat = dsv->getImage()->getDesc().format;
      m_gpuProfiler.setTargets(targets);
    }
    if (accessTracking()) {
      ID3D12Resource* written[9] = {};
      size_t writtenCount = 0;
      for (uint32_t i = 0; i < effectiveCount && i < 8; i++) {
        if (rtvs[i])
          written[writtenCount++] = static_cast<D3D12BackendImage*>(rtvs[i]->getImage())->getResource();
      }
      if (dsv)
        written[writtenCount++] = static_cast<D3D12BackendImage*>(dsv->getImage())->getResource();
      m_accessTracker.setTargetWrites(written, writtenCount);
    }

    // D3D11 reasserts OM state before repeated draws so resources that were
    // used by a copy or Present can transition back. When both the native
    // handles and uniform resource states are already correct, the command
    // list still has the tracked OM binding (also replayed after resets), so
    // avoid rebuilding and rebinding the same state.
    bool bindingsAlreadyCurrent = m_trackedOmBindingValid
      && effectiveCount == m_trackedRTVCount
      && effectiveCount == m_currentRenderTargetCount
      && ((dsv != nullptr) == m_trackedDSVBound);

    for (uint32_t i = 0; bindingsAlreadyCurrent && i < effectiveCount; i++) {
      if (!rtvs[i]) {
        bindingsAlreadyCurrent = false;
        break;
      }

      auto* d3d12View = static_cast<D3D12BackendImageView*>(rtvs[i]);
      auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());
      bindingsAlreadyCurrent = d3d12View->getRtvHandle().ptr == m_trackedRTVs[i].ptr
        && d3d12Image->getResource() == m_currentRenderTargetResources[i]
        && d3d12Image->hasUniformState()
        && d3d12Image->getCurrentState() == D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    if (bindingsAlreadyCurrent && dsv) {
      auto* d3d12View = static_cast<D3D12BackendImageView*>(dsv);
      auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());
      const auto& dsvViewDesc = d3d12View->getDesc();
      D3D12_RESOURCE_STATES dsvState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
      if (d3d12View->hasReadOnlyDepthStencil()) {
        dsvState = D3D12_RESOURCE_STATE_DEPTH_READ;
        if (static_cast<uint32_t>(dsvViewDesc.usage & ImageViewUsage::ShaderResource)) {
          dsvState = static_cast<D3D12_RESOURCE_STATES>(
            dsvState
            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
      }

      bindingsAlreadyCurrent = d3d12View->getDsvHandle().ptr == m_trackedDSV.ptr
        && d3d12Image->getResource() == m_currentDepthStencilResource
        && d3d12Image->hasUniformState()
        && d3d12Image->getCurrentState() == dsvState;
    } else if (bindingsAlreadyCurrent) {
      bindingsAlreadyCurrent = m_currentDepthStencilResource == nullptr;
    }

    if (bindingsAlreadyCurrent) {
      m_outputMergerRebindRequired.store(false, std::memory_order_relaxed);
      if (m_recentOpsEnabled) {
        DXUP_RECENT_OP(
          "OMSetRT unchanged count=%u eff=%u rtv0View=%p rt0Res=%p dsvView=%p",
          count,
          effectiveCount,
          effectiveCount && rtvs ? rtvs[0] : nullptr,
          m_currentRenderTargetResources[0],
          dsv);
      }
      return;
    }

    m_currentRenderTargetResources.fill(nullptr);
    m_currentRenderTargetCount = effectiveCount;
    m_currentDepthStencilResource = nullptr;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[kMaxRenderTargets] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
    bool hasDsv = false;
    bool hasRtvHole = false;

    for (uint32_t i = 0; i < effectiveCount; i++) {
      if (!rtvs[i]) {
        // A null descriptor: writes there go nowhere (D3D11's empty slot).
        if (!m_nullRtv.ptr) {
          m_nullRtv = m_device->getDescriptorHeapManager().allocateRtv();
          D3D12_RENDER_TARGET_VIEW_DESC nullDesc = {};
          nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          nullDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
          m_device->getDevice()->CreateRenderTargetView(nullptr, &nullDesc, m_nullRtv);
        }
        rtvHandles[i] = m_nullRtv;
        continue;
      }
      auto* d3d12View = static_cast<D3D12BackendImageView*>(rtvs[i]);
      auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());
      m_currentRenderTargetResources[i] = d3d12Image->getResource();
      transitionResource(d3d12Image, d3d12View, D3D12_RESOURCE_STATE_RENDER_TARGET);
      holdBatchResourceOnce(d3d12Image->getResource(),
        m_trackedRTVResourceHeld[i],
        m_trackedRTVResourceHeldGeneration[i]);
      rtvHandles[i] = d3d12View->getRtvHandle();
    }

    if (m_recentOpsEnabled) {
      DXUP_RECENT_OP(
        "OMSetRT count=%u eff=%u rtv0View=%p rt0Res=%p dsvView=%p",
        count,
        effectiveCount,
        effectiveCount && rtvs ? rtvs[0] : nullptr,
        m_currentRenderTargetResources[0],
        dsv);
    }

    (void)hasRtvHole;

    if (dsv) {
      auto* d3d12View = static_cast<D3D12BackendImageView*>(dsv);
      auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());
      const auto& dsvViewDesc = d3d12View->getDesc();
      // A plane the view writes needs the write state. A read-only plane
      // takes the shader read states too when shaders may read the image:
      // depth tests and shader reads of the same depth buffer (soft
      // particles, deferred lighting) then need no transitions.
      D3D12_RESOURCE_STATES readState = D3D12_RESOURCE_STATE_DEPTH_READ;
      if ((d3d12Image->getDesc().bindFlags & BindFlags::ShaderResource) != BindFlags::None)
        readState |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      const D3D12_RESOURCE_STATES depthState = d3d12View->isReadOnlyDepth()
        ? readState : D3D12_RESOURCE_STATE_DEPTH_WRITE;
      const D3D12_RESOURCE_STATES stencilState = d3d12View->isReadOnlyStencil()
        ? readState : D3D12_RESOURCE_STATE_DEPTH_WRITE;
      const bool splitPlanes = d3d12Image->getPlaneCount() > 1 && depthState != stencilState;
      const D3D12_RESOURCE_STATES dsvState = d3d12Image->getPlaneCount() > 1 ? stencilState : depthState;

      static int bindRtDsvLogCount = 0;
      if (logBindings && (bindRtDsvLogCount < 16 || (count == 0 && bindRtDsvLogCount < 512))) {
        D3D12_RESOURCE_DESC dsvResDesc = d3d12Image->getResource()->GetDesc();
        D3D12_RESOURCE_STATES currentDsvState = getImageViewCurrentState(d3d12Image, dsvViewDesc);
        logMessage(LogLevel::Info,
          "bindRenderTargets-dsv: view=%p img=%p res=%p handle=%p fmt=%u imgBind=0x%x resFlags=0x%x usage=0x%x flags=0x%x baseMip=%u mipCount=%u baseLayer=%u layerCount=%u roDepth=%u roStencil=%u prevState=0x%x nextState=0x%x",
          dsv,
          d3d12Image,
          d3d12Image->getResource(),
          reinterpret_cast<void*>(d3d12View->getDsvHandle().ptr),
          static_cast<uint32_t>(d3d12Image->getFormat()),
          static_cast<uint32_t>(d3d12Image->getDesc().bindFlags),
          static_cast<uint32_t>(dsvResDesc.Flags),
          static_cast<uint32_t>(dsvViewDesc.usage),
          static_cast<uint32_t>(dsvViewDesc.flags),
          dsvViewDesc.baseMipLevel,
          getImageViewMipCountForTransition(dsvViewDesc, d3d12Image->getDesc()),
          dsvViewDesc.baseArrayLayer,
          getImageViewArrayCountForTransition(dsvViewDesc, d3d12Image->getDesc()),
          d3d12View->isReadOnlyDepth() ? 1u : 0u,
          d3d12View->isReadOnlyStencil() ? 1u : 0u,
          static_cast<uint32_t>(currentDsvState),
          static_cast<uint32_t>(dsvState));
        bindRtDsvLogCount++;
      }

      m_currentDepthStencilResource = d3d12Image->getResource();
      if (splitPlanes)
        transitionDepthStencilPlanes(d3d12Image, d3d12View, depthState, stencilState);
      else
        transitionResource(d3d12Image, d3d12View, depthState);
      holdBatchResourceOnce(d3d12Image->getResource(),
        m_trackedDSVResourceHeld,
        m_trackedDSVResourceHeldGeneration);
      dsvHandle = d3d12View->getDsvHandle();
      hasDsv = dsvHandle.ptr != 0;
      if (!hasDsv) {
        logMessage(LogLevel::Error,
          "bindRenderTargets: DSV handle missing for view=%p img=%p usage=0x%x flags=0x%x",
          dsv,
          d3d12Image,
          static_cast<uint32_t>(d3d12View->getDesc().usage),
          static_cast<uint32_t>(d3d12View->getDesc().flags));
      }
    }

    static int bindRtPhaseLogCount = 0;
    // Depth-only passes (shadow maps) bind every frame: their lines stop too.
    const bool logDepthOnly = !effectiveCount && hasDsv && bindRtPhaseLogCount < 512;
    if (logBindings && (bindRtPhaseLogCount < 32 || logDepthOnly)) {
      logMessage(LogLevel::Info,
        "bindRenderTargets-beforeFlush: tid=%lu effectiveCount=%u rtv0=%p hasDsv=%u dsvHandle=%p",
        static_cast<unsigned long>(GetCurrentThreadId()),
        effectiveCount,
        effectiveCount ? reinterpret_cast<void*>(rtvHandles[0].ptr) : nullptr,
        hasDsv ? 1u : 0u,
        hasDsv ? reinterpret_cast<void*>(dsvHandle.ptr) : nullptr);
      bindRtPhaseLogCount++;
    }

    flushBarriers();
    if (logBindings && (bindRtPhaseLogCount < 32 || logDepthOnly)) {
      logMessage(LogLevel::Info,
        "bindRenderTargets-afterFlush: tid=%lu effectiveCount=%u hasDsv=%u dsvHandle=%p",
        static_cast<unsigned long>(GetCurrentThreadId()),
        effectiveCount,
        hasDsv ? 1u : 0u,
        hasDsv ? reinterpret_cast<void*>(dsvHandle.ptr) : nullptr);
    }
    if (!effectiveCount && !hasDsv) {
      bool omSetSucceeded = true;
      if (m_trackedRTVCount || m_trackedDSVBound)
        omSetSucceeded = safeOMSetRenderTargets(&m_cmdList, 0, nullptr, FALSE, nullptr);
      m_trackedRTVCount = 0;
      m_trackedDSVBound = false;
      m_trackedOmBindingValid = omSetSucceeded;
    } else if (!safeOMSetRenderTargets(
        &m_cmdList,
        effectiveCount,
        effectiveCount ? rtvHandles : nullptr,
        FALSE,
        hasDsv ? &dsvHandle : nullptr)) {
      m_trackedOmBindingValid = false;
      logD3D12DebugMessages(m_device->getDevice(), "OMSetRenderTargets");
    } else {
      // Track RTV/DSV handles for re-binding after command list reset
      m_trackedRTVCount = effectiveCount;
      for (uint32_t i = 0; i < effectiveCount; i++)
        m_trackedRTVs[i] = rtvHandles[i];
      m_trackedDSVBound = hasDsv;
      if (hasDsv) m_trackedDSV = dsvHandle;
      m_trackedOmBindingValid = true;

      if (logBindings && (bindRtPhaseLogCount < 32 || logDepthOnly)) {
        logMessage(LogLevel::Info,
          "bindRenderTargets-afterOM: tid=%lu effectiveCount=%u hasDsv=%u dsvHandle=%p",
          static_cast<unsigned long>(GetCurrentThreadId()),
          effectiveCount,
          hasDsv ? 1u : 0u,
          hasDsv ? reinterpret_cast<void*>(dsvHandle.ptr) : nullptr);
      }
    }
    m_outputMergerRebindRequired.store(!m_trackedOmBindingValid, std::memory_order_relaxed);
#endif
  }

  // ============================================================
  // State
  // ============================================================

  void D3D12BackendContext::clearState() {
#ifdef _WIN32
    ContextLock ctxLock(*this);

    if (m_predicate) {
      if (m_predicate->m_context == this && !m_predicate->m_active.load(std::memory_order_relaxed))
        m_predicate->m_context = nullptr;
      m_predicate = nullptr;
      if (!m_deviceRemoved && !m_closedForSubmission)
        applyPredication();
    }

    for (uint32_t slot = 0; slot < kMaxVertexBuffers; slot++) {
      if (m_trackedVBs[slot].bound) {
        D3D12_VERTEX_BUFFER_VIEW nullView = {};
        m_cmdList->IASetVertexBuffers(slot, 1, &nullView);
      }
    }

    if (m_trackedIBBound)
      m_cmdList->IASetIndexBuffer(nullptr);

    if (m_soBufferCount)
      m_cmdList->SOSetTargets(0, 0, nullptr);

    bool omSetSucceeded = true;
    if (m_trackedRTVCount || m_trackedDSVBound)
      omSetSucceeded = safeOMSetRenderTargets(&m_cmdList, 0, nullptr, FALSE, nullptr);

    resetBindingTracking();
    m_trackedOmBindingValid = omSetSucceeded;

    initNullDescriptorsAndRootTables();
#endif
  }

  void D3D12BackendContext::releaseBindings() {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_predicate && m_predicate->m_context == this && !m_predicate->m_active.load(std::memory_order_relaxed))
      m_predicate->m_context = nullptr;
    m_predicate = nullptr;
    // The command list may be closed or lack a root signature here, so only
    // the tracking is dropped. Nothing may reapply these descriptors later.
    resetBindingTracking();
    m_trackedOmBindingValid = false;
#endif
  }

  void D3D12BackendContext::resetBindingTracking() {
#ifdef _WIN32
    for (uint32_t slot = 0; slot < kMaxVertexBuffers; slot++)
      m_trackedVBs[slot] = {};

    m_trackedIB = {};
    m_trackedIBBound = false;

    m_soBufferCount = 0;
    for (uint32_t slot = 0; slot < 4; slot++) {
      m_soViews[slot] = {};
      m_soBuffers[slot] = nullptr;
      m_soResourceHeld[slot] = nullptr;
      m_soResourceHeldGeneration[slot] = 0;
    }

    m_currentRenderTargetResources.fill(nullptr);
    m_currentRenderTargetCount = 0;
    m_currentDepthStencilResource = nullptr;
    for (auto& handle : m_trackedRTVs)
      handle = {};
    for (auto& resource : m_trackedRTVResourceHeld)
      resource = nullptr;
    for (auto& generation : m_trackedRTVResourceHeldGeneration)
      generation = 0;
    m_trackedRTVCount = 0;
    m_trackedDSV = {};
    m_trackedDSVBound = false;
    m_outputMergerRebindRequired.store(false, std::memory_order_relaxed);
    m_trackedDSVResourceHeld = nullptr;
    m_trackedDSVResourceHeldGeneration = 0;

    m_cbvBindings = {};
    m_cbvBindingEnd = {};
    m_srvBindings = {};
    m_srvBindingEnd = {};
    m_uavBindings = {};
    m_uavBindingEnd = {};
    m_samplerBindings = {};
    m_currentSrvResources = {};
    m_currentSrvFormats = {};
    m_currentUavResources = {};
    m_srvImageViews = {};
    m_uavImageViews = {};
    m_currentUavFormats = {};

    m_currentPSO = nullptr;
    m_cbvDirty = false;
    m_srvDirty = false;
    m_uavDirty = false;
    m_samplerDirty = false;
    m_cbvStageDirty.fill(false);
    m_srvStageDirty.fill(false);
    m_uavStageDirty.fill(false);
    m_samplerStageDirty.fill(false);
    m_descriptorDirtyStageMask = 0;
    m_psoDirtyAfterFlush = true;
#endif
  }

  void D3D12BackendContext::setViewports(uint32_t count, const Viewport* viewports) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    D3D12_VIEWPORT d3d12Viewports[kMaxViewports] = {};
    for (uint32_t i = 0; i < count; i++) {
      d3d12Viewports[i].TopLeftX = viewports[i].x;
      d3d12Viewports[i].TopLeftY = viewports[i].y;
      d3d12Viewports[i].Width    = viewports[i].width;
      d3d12Viewports[i].Height   = viewports[i].height;
      d3d12Viewports[i].MinDepth = viewports[i].minDepth;
      d3d12Viewports[i].MaxDepth = viewports[i].maxDepth;
    }
    // Without viewports D3D11 rasterizes nothing; D3D12 takes no empty
    // list and would keep the previous viewports: an empty one instead.
    if (!count)
      count = 1;
    m_trackedViewportCount = count;
    std::memcpy(m_trackedViewports, d3d12Viewports, count * sizeof(D3D12_VIEWPORT));
    m_cmdList->RSSetViewports(count, d3d12Viewports);
#endif
  }

  void D3D12BackendContext::setScissorRects(uint32_t count, const Rect* rects) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    D3D12_RECT d3d12Rects[kMaxViewports];
    for (uint32_t i = 0; i < count; i++) {
      d3d12Rects[i].left   = rects[i].left;
      d3d12Rects[i].top    = rects[i].top;
      d3d12Rects[i].right  = rects[i].right;
      d3d12Rects[i].bottom = rects[i].bottom;
    }
    m_trackedScissorCount = count;
    std::memcpy(m_trackedScissors, d3d12Rects, count * sizeof(D3D12_RECT));
    m_cmdList->RSSetScissorRects(count, d3d12Rects);
#endif
  }

  void D3D12BackendContext::setBlendState(const BlendState& state) {
    ContextLock ctxLock(*this);
    (void)state; // Blend state is baked into the PSO in D3D12
  }

  void D3D12BackendContext::setRasterizerState(const RasterizerState& state) {
    ContextLock ctxLock(*this);
    (void)state; // Rasterizer state is baked into the PSO in D3D12
  }

  void D3D12BackendContext::setDepthStencilState(const DepthStencilState& state) {
    ContextLock ctxLock(*this);
    (void)state; // Depth stencil state is baked into the PSO in D3D12
  }

  void D3D12BackendContext::setStencilRef(uint32_t ref) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    m_trackedStencilRef = ref;
    m_cmdList->OMSetStencilRef(ref);
#endif
  }

  void D3D12BackendContext::setBlendFactor(const float factor[4]) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    std::memcpy(m_trackedBlendFactor, factor, sizeof(float) * 4);
    m_cmdList->OMSetBlendFactor(factor);
#endif
  }

  void D3D12BackendContext::setPrimitiveTopology(PrimitiveTopology topology) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    D3D_PRIMITIVE_TOPOLOGY d3dTopology;
    if (topology >= PrimitiveTopology::PatchList1
     && topology <= PrimitiveTopology::PatchList32) {
      uint32_t controlPointOffset = static_cast<uint32_t>(topology)
        - static_cast<uint32_t>(PrimitiveTopology::PatchList1);
      d3dTopology = static_cast<D3D_PRIMITIVE_TOPOLOGY>(
        static_cast<uint32_t>(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
        + controlPointOffset);
    } else {
      switch (topology) {
        case PrimitiveTopology::PointList:       d3dTopology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case PrimitiveTopology::LineList:        d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case PrimitiveTopology::LineStrip:       d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        case PrimitiveTopology::TriangleList:    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case PrimitiveTopology::TriangleStrip:   d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case PrimitiveTopology::LineListAdj:     d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; break;
        case PrimitiveTopology::LineStripAdj:    d3dTopology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ; break;
        case PrimitiveTopology::TriangleListAdj: d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ; break;
        case PrimitiveTopology::TriangleStripAdj:d3dTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ; break;
        default:                                 d3dTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED; break;
      }
    }
    m_trackedTopology = d3dTopology;
    m_cmdList->IASetPrimitiveTopology(d3dTopology);
#endif
  }

  // ============================================================
  // Pipeline
  // ============================================================

  void D3D12BackendContext::bindGraphicsPipeline(IBackendPipeline* pipeline) {
#ifdef _WIN32
    if (!pipeline) return;
    // A pipeline still compiling: wait for it here, on the context's
    // command thread, without the context lock.
    auto* gfxPipeline = static_cast<D3D12BackendGraphicsPipeline*>(pipeline);
    if (gfxPipeline->isPending())
      gfxPipeline = m_device->resolvePipeline(gfxPipeline);
    ContextLock ctxLock(*this);
    m_graphicsPipelineFailed = !gfxPipeline;
    if (!gfxPipeline)
      return;
    auto* newPSO = gfxPipeline->getPSO();
    auto* newRootSig = gfxPipeline->getRootSignature();

    if (!m_isComputePipeline
     && !m_psoDirtyAfterFlush
     && newPSO == m_currentPSO
     && newRootSig == m_currentRootSignature) {
      if (m_perfStatsEnabled && !m_copyOnly)
        m_perfIntervalPsoSkips++;
      return;
    }

    const auto newResourceLimits = gfxPipeline->getResourceLimits();
    const uint32_t newStageMask = gfxPipeline->getActiveStageMask();

    // Descriptor tables are sized to the active shader's reflected limits.
    // A PSO switch can therefore require a larger table even when the D3D11
    // bindings themselves did not change. Rebuild only the affected stages.
    for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++) {
      const bool activityChanged = ((m_currentGraphicsStageMask ^ newStageMask) & (1u << stage)) != 0;
      if (activityChanged
       || !sameShaderResourceLimits(m_currentGraphicsResourceLimits[stage], newResourceLimits[stage]))
        markDescriptorStageDirty(stage);
    }
    m_currentGraphicsResourceLimits = newResourceLimits;
    m_currentGraphicsStageMask = newStageMask;

    // Force dirty flags when switching between compute and graphics modes,
    // since the commit functions use different root param indices per mode
    // and a single dirty flag is shared across all stages.
    bool wasCompute = m_isComputePipeline;
    bool bindPSO = wasCompute || m_psoDirtyAfterFlush || newPSO != m_currentPSO;
    m_currentPSO = newPSO;
    m_isComputePipeline = false;

    const bool rebindVertexBuffers = m_psoDirtyAfterFlush;
    if (bindPSO) {
      if (m_perfStatsEnabled && !m_copyOnly)
        m_perfIntervalPsoBinds++;
      m_cmdList->SetPipelineState(m_currentPSO);
      m_psoDirtyAfterFlush = false;
    } else if (m_perfStatsEnabled && !m_copyOnly) {
      m_perfIntervalPsoSkips++;
    }
    if (gfxPipeline->getNullVertexSlot() != m_nullVertexSlot || rebindVertexBuffers)
      applyNullVertexSlot(gfxPipeline->getNullVertexSlot());

    // D3D12 keeps graphics and compute root signatures and their arguments
    // apart, so dispatches leave the graphics bindings alone. AMD drivers
    // needed the graphics root signature set again after compute to restore
    // the graphics root bindings.
    if (newRootSig != m_currentRootSignature || (wasCompute && m_resetRootOnModeSwitch)) {
      m_currentRootSignature = newRootSig;
      m_cmdList->SetGraphicsRootSignature(m_currentRootSignature);
      invalidateRootCache(false);
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++)
        markDescriptorStageDirty(stage);
    }
#endif
  }

  void D3D12BackendContext::bindComputePipeline(IBackendPipeline* pipeline) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!pipeline) return;

    auto* compPipeline = static_cast<D3D12BackendComputePipeline*>(pipeline);
    auto* newPSO = compPipeline->getPSO();
    auto* newRootSig = compPipeline->getRootSignature();

    if (m_isComputePipeline
     && !m_psoDirtyAfterFlush
     && newPSO == m_currentPSO
     && newRootSig == m_computeRootSignature) {
      if (m_perfStatsEnabled && !m_copyOnly)
        m_perfIntervalPsoSkips++;
      return;
    }

    m_computeShaderHash = compPipeline->getShaderHash();
    const auto newResourceLimits = compPipeline->getResourceLimits();
    if (!sameShaderResourceLimits(m_currentComputeResourceLimits, newResourceLimits))
      markDescriptorStageDirty(static_cast<uint32_t>(ShaderStage::Compute));
    m_currentComputeResourceLimits = newResourceLimits;

    // Force dirty flags when switching between graphics and compute modes
    bool wasGraphics = !m_isComputePipeline;
    bool bindPSO = wasGraphics || m_psoDirtyAfterFlush || newPSO != m_currentPSO;
    m_currentPSO = newPSO;
    m_isComputePipeline = true;

    if (bindPSO) {
      if (m_perfStatsEnabled && !m_copyOnly)
        m_perfIntervalPsoBinds++;
      m_cmdList->SetPipelineState(m_currentPSO);
      m_psoDirtyAfterFlush = false;
    } else if (m_perfStatsEnabled && !m_copyOnly) {
      m_perfIntervalPsoSkips++;
    }

    // As for graphics: the compute bindings survive draws (AMD: set again).
    if (newRootSig != m_computeRootSignature || (wasGraphics && m_resetRootOnModeSwitch)) {
      m_computeRootSignature = newRootSig;
      m_cmdList->SetComputeRootSignature(m_computeRootSignature);
      invalidateRootCache(true);
      markDescriptorStageDirty(static_cast<uint32_t>(ShaderStage::Compute));
    }
#endif
  }

  // ============================================================
  // Draw Commands
  // ============================================================

  void D3D12BackendContext::draw(uint32_t vertexCount, uint32_t instanceCount,
                                  uint32_t firstVertex, uint32_t firstInstance) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_graphicsPipelineFailed)
      return m_device->noteFailedPipelineDraw();
    if (accessTracking())
      trackCommand(false);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDraw(m_cmdList.raw());
    prepareShaderAccess(false);
    const bool infoDiagnostics = m_infoDiagnosticsEnabled;
    if (infoDiagnostics) {
      static uint32_t drawCount = 0;
      if (drawCount < 5)
        logMessage(LogLevel::Info, "Draw(%u, %u, %u, %u)", vertexCount, instanceCount, firstVertex, firstInstance);
      drawCount++;
    }
    if (m_recentOpsEnabled) {
      constexpr uint32_t psStage = static_cast<uint32_t>(ShaderStage::Pixel);
      DXUP_RECENT_OP(
        "Draw vc=%u ic=%u fv=%u fi=%u rtCount=%u rt0Res=%p dsvRes=%p psSrv0=%p/%u psSrv1=%p/%u psSrv2=%p/%u psSrv3=%p/%u psUav0=%p/%u",
        vertexCount,
        instanceCount,
        firstVertex,
        firstInstance,
        m_currentRenderTargetCount,
        m_currentRenderTargetResources[0],
        m_currentDepthStencilResource,
        m_currentSrvResources[psStage][0],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][0]),
        m_currentSrvResources[psStage][1],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][1]),
        m_currentSrvResources[psStage][2],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][2]),
        m_currentSrvResources[psStage][3],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][3]),
        m_currentUavResources[psStage][0],
        static_cast<uint32_t>(m_currentUavFormats[psStage][0]));
    }
    // Perf fast path: steady-state draws have no dirty descriptors and no
    // pending barriers, so skip heap/commit/barrier calls entirely.
    // Safe: heap offsets only grow on descriptor commits, barriers empty = no-op.
    {
      const uint32_t activeStageMask = m_isComputePipeline
        ? (1u << static_cast<uint32_t>(ShaderStage::Compute))
        : m_currentGraphicsStageMask;
      if (!m_psoDirtyAfterFlush && !(m_descriptorDirtyStageMask & activeStageMask) &&
          m_pendingBarriers.empty() && g_gpuOpLimit < 0) {
        m_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
        m_drawOpsSinceFlush++;
        submitIfGpuIdle();
        if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) {
          flush();
        }
        return;
      }
    }
    ensureDescriptorHeapSpace();
    commitDescriptorBindings();
    flushCommandBarriers();
    if (g_gpuOpLimit >= 0) {
      g_gpuOpCounter++;
      if (infoDiagnostics && g_gpuOpCounter <= 3)
        logMessage(LogLevel::Info, "GPU_OP #%d: Draw(%u, %u, %u, %u) isCompute=%d heapOff=%u",
          g_gpuOpCounter, vertexCount, instanceCount, firstVertex, firstInstance,
          m_isComputePipeline ? 1 : 0, m_cbvSrvUavHeapOffset);
      if (g_gpuOpCounter > g_gpuOpLimit) {
        DXUP_RECENT_OP("SKIPPED Draw #%d (limit=%d)", g_gpuOpCounter, g_gpuOpLimit);
        return;
      }
    }
    m_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    m_drawOpsSinceFlush++;
    submitIfGpuIdle();
    if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) {
      // Perf: no GetDeviceRemovedReason() poll here (driver round-trip on hot path).
      // flush() already handles device-removed detection and recovery.
      flush();
    }
#endif
  }

  void D3D12BackendContext::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                         uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_graphicsPipelineFailed)
      return m_device->noteFailedPipelineDraw();
    if (accessTracking())
      trackCommand(false);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDraw(m_cmdList.raw());
    prepareShaderAccess(false);
    const bool infoDiagnostics = m_infoDiagnosticsEnabled;
    if (infoDiagnostics) {
      static uint32_t drawIdxCount = 0;
      if (drawIdxCount < 5)
        logMessage(LogLevel::Info, "DrawIndexed(%u, %u, %u, %d, %u)", indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
      drawIdxCount++;
    }
    if (m_recentOpsEnabled) {
      constexpr uint32_t psStage = static_cast<uint32_t>(ShaderStage::Pixel);
      DXUP_RECENT_OP(
        "DrawIndexed ic=%u inst=%u first=%u vo=%d fi=%u rtCount=%u rt0Res=%p dsvRes=%p psSrv0=%p/%u psSrv1=%p/%u psSrv2=%p/%u psSrv3=%p/%u psUav0=%p/%u",
        indexCount,
        instanceCount,
        firstIndex,
        vertexOffset,
        firstInstance,
        m_currentRenderTargetCount,
        m_currentRenderTargetResources[0],
        m_currentDepthStencilResource,
        m_currentSrvResources[psStage][0],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][0]),
        m_currentSrvResources[psStage][1],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][1]),
        m_currentSrvResources[psStage][2],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][2]),
        m_currentSrvResources[psStage][3],
        static_cast<uint32_t>(m_currentSrvFormats[psStage][3]),
        m_currentUavResources[psStage][0],
        static_cast<uint32_t>(m_currentUavFormats[psStage][0]));
    }
    // Perf fast path: steady-state draws have no dirty descriptors and no
    // pending barriers, so skip heap/commit/barrier calls entirely.
    {
      const uint32_t activeStageMask = m_isComputePipeline
        ? (1u << static_cast<uint32_t>(ShaderStage::Compute))
        : m_currentGraphicsStageMask;
      if (!m_psoDirtyAfterFlush && !(m_descriptorDirtyStageMask & activeStageMask) &&
          m_pendingBarriers.empty() && g_gpuOpLimit < 0) {
        m_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        m_drawOpsSinceFlush++;
        submitIfGpuIdle();
        if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) {
          flush();
        }
        return;
      }
    }
    ensureDescriptorHeapSpace();
    commitDescriptorBindings();
    flushCommandBarriers();
    if (g_gpuOpLimit >= 0) {
      g_gpuOpCounter++;
      if (infoDiagnostics && g_gpuOpCounter <= 3)
        logMessage(LogLevel::Info, "GPU_OP #%d: DrawIndexed(%u, %u, %u, %d, %u) isCompute=%d heapOff=%u",
          g_gpuOpCounter, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance,
          m_isComputePipeline ? 1 : 0, m_cbvSrvUavHeapOffset);
      if (g_gpuOpCounter > g_gpuOpLimit) {
        DXUP_RECENT_OP("SKIPPED DrawIndexed #%d (limit=%d)", g_gpuOpCounter, g_gpuOpLimit);
        return;
      }
    }
    m_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    m_drawOpsSinceFlush++;
    submitIfGpuIdle();
    if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) {
      // Perf: no GetDeviceRemovedReason() poll here (driver round-trip on hot path).
      // flush() already handles device-removed detection and recovery.
      flush();
    }
#endif
  }

  void D3D12BackendContext::dispatch(uint32_t x, uint32_t y, uint32_t z) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (accessTracking())
      trackCommand(true);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDispatch(m_cmdList.raw(), m_computeShaderHash);
    prepareShaderAccess(true);
    if (m_recentOpsEnabled)
      DXUP_RECENT_OP("Dispatch(%u,%u,%u)", x, y, z);
    // Perf fast path: steady-state dispatches have no dirty descriptors and no
    // pending barriers, so skip heap/commit/barrier calls entirely.
    {
      const uint32_t activeStageMask = (1u << static_cast<uint32_t>(ShaderStage::Compute));
      if (!m_psoDirtyAfterFlush && !(m_descriptorDirtyStageMask & activeStageMask) &&
          m_pendingBarriers.empty() && g_gpuOpLimit < 0) {
        beginDispatchBreadcrumb(x, y, z);
        m_cmdList->Dispatch(x, y, z);
        endDispatchBreadcrumb();
        m_drawOpsSinceFlush++;
        submitIfGpuIdle();
        if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) {
          flush();
        }
        return;
      }
    }
    ensureDescriptorHeapSpace();
    commitDescriptorBindings();
    flushCommandBarriers();

    if (g_gpuOpLimit >= 0) {
      g_gpuOpCounter++;
      if (m_infoDiagnosticsEnabled && g_gpuOpCounter <= 3)
        logMessage(LogLevel::Info, "GPU_OP #%d: Dispatch(%u,%u,%u) isCompute=%d heapOff=%u",
          g_gpuOpCounter, x, y, z, m_isComputePipeline ? 1 : 0, m_cbvSrvUavHeapOffset);
      if (g_gpuOpCounter > g_gpuOpLimit) {
        DXUP_RECENT_OP("SKIPPED Dispatch #%d (limit=%d)", g_gpuOpCounter, g_gpuOpLimit);
        return;
      }
    }

    beginDispatchBreadcrumb(x, y, z);
    m_cmdList->Dispatch(x, y, z);
    endDispatchBreadcrumb();
    m_drawOpsSinceFlush++;
    submitIfGpuIdle();
    if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) flush();
#endif
  }

  void D3D12BackendContext::drawIndirect(IBackendBuffer* buffer, uint64_t offset) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_graphicsPipelineFailed)
      return m_device->noteFailedPipelineDraw();
    if (accessTracking())
      trackCommand(false);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDraw(m_cmdList.raw());
    prepareShaderAccess(false);
    if (!buffer) return;
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (m_recentOpsEnabled)
      DXUP_RECENT_OP("DrawIndirect buffer=%p offset=%llu", d3d12Buffer, static_cast<unsigned long long>(offset));

    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    holdBatchResource(d3d12Buffer->getResource());
    // As draws: nothing to commit or flush in the steady state.
    if (m_psoDirtyAfterFlush || (m_descriptorDirtyStageMask & m_currentGraphicsStageMask) || !m_pendingBarriers.empty()) {
      ensureDescriptorHeapSpace();
      commitDescriptorBindings();
      flushCommandBarriers();
    }
    m_cmdList->ExecuteIndirect(
      m_device->getDrawIndirectSignature(),
      1, d3d12Buffer->getResource(), d3d12Buffer->getOffset() + offset, nullptr, 0);
    m_drawOpsSinceFlush++;
    submitIfGpuIdle();
    if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) flush();
#endif
  }

  void D3D12BackendContext::drawIndexedIndirect(IBackendBuffer* buffer, uint64_t offset) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_graphicsPipelineFailed)
      return m_device->noteFailedPipelineDraw();
    if (accessTracking())
      trackCommand(false);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDraw(m_cmdList.raw());
    prepareShaderAccess(false);
    if (!buffer) return;
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (m_recentOpsEnabled)
      DXUP_RECENT_OP("DrawIndexedIndirect buffer=%p offset=%llu", d3d12Buffer, static_cast<unsigned long long>(offset));

    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    holdBatchResource(d3d12Buffer->getResource());
    // As draws: nothing to commit or flush in the steady state.
    if (m_psoDirtyAfterFlush || (m_descriptorDirtyStageMask & m_currentGraphicsStageMask) || !m_pendingBarriers.empty()) {
      ensureDescriptorHeapSpace();
      commitDescriptorBindings();
      flushCommandBarriers();
    }
    m_cmdList->ExecuteIndirect(
      m_device->getDrawIndexedIndirectSignature(),
      1, d3d12Buffer->getResource(), d3d12Buffer->getOffset() + offset, nullptr, 0);
    m_drawOpsSinceFlush++;
    submitIfGpuIdle();
    if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) flush();
#endif
  }

  void D3D12BackendContext::initDispatchBreadcrumbs() {
#ifdef _WIN32
    auto* d3d12Device = m_device->getDevice();
    D3D12_HEAP_PROPERTIES heapProps = d3d12Device->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kBreadcrumbSlots * 16;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if (FAILED(d3d12Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_breadcrumbBuffer)))
     || FAILED(m_breadcrumbBuffer->Map(0, nullptr, &mapped))
     || FAILED(m_cmdList.raw()->QueryInterface(IID_PPV_ARGS(&m_breadcrumbList)))) {
      m_breadcrumbBuffer = nullptr;
      m_breadcrumbList = nullptr;
      return;
    }
    std::memset(mapped, 0, size_t(desc.Width));
    m_breadcrumbs = static_cast<const uint32_t*>(mapped);
#endif
  }

  void D3D12BackendContext::writeDispatchBreadcrumb(bool begin) {
#ifdef _WIN32
    if (begin)
      m_dispatchSerial++;
    const uint32_t slot = m_dispatchSerial % kBreadcrumbSlots;
    const D3D12_GPU_VIRTUAL_ADDRESS address = m_breadcrumbBuffer->GetGPUVirtualAddress() + slot * 16;
    if (begin) {
      const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[3] = {
        { address, uint32_t(m_computeShaderHash) },
        { address + 4, uint32_t(m_computeShaderHash >> 32) },
        { address + 8, m_dispatchSerial },
      };
      const D3D12_WRITEBUFFERIMMEDIATE_MODE modes[3] = {
        D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
        D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
      };
      m_breadcrumbList->WriteBufferImmediate(3, params, modes);
    } else {
      const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param = { address + 12, m_dispatchSerial };
      const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
      m_breadcrumbList->WriteBufferImmediate(1, &param, &mode);
    }
#else
    (void)begin;
#endif
  }

  void D3D12BackendContext::noteDispatch(uint32_t x, uint32_t y, uint32_t z) {
#ifdef _WIN32
    DispatchRecord& record = m_dispatchRecords[m_dispatchSerial % m_dispatchRecords.size()];
    record = {};
    record.serial = m_dispatchSerial;
    record.groups[0] = x;
    record.groups[1] = y;
    record.groups[2] = z;
    record.fence = m_fenceValue + 1;
    constexpr uint32_t computeStage = static_cast<uint32_t>(ShaderStage::Compute);
    if ((record.uav = m_currentUavResources[computeStage][0]))
      record.uavDesc = record.uav->GetDesc();
    if (auto* view = m_srvImageViews[computeStage][0]) {
      if (auto* image = static_cast<D3D12BackendImage*>(view->getImage())) {
        if ((record.srv = image->getResource()))
          record.srvDesc = record.srv->GetDesc();
      }
    }
#else
    (void)x; (void)y; (void)z;
#endif
  }

  void D3D12BackendContext::logDispatchBreadcrumbs() {
#ifdef _WIN32
    if (!m_breadcrumbs)
      return;
    logMessage(LogLevel::Error, "dispatch breadcrumbs: fence completed %llu, signaled %llu",
      static_cast<unsigned long long>(m_fence ? m_fence->GetCompletedValue() : 0),
      static_cast<unsigned long long>(m_lastSignaledFenceValue));
    for (uint32_t slot = 0; slot < kBreadcrumbSlots; slot++) {
      const uint32_t* entry = m_breadcrumbs + slot * 4;
      if (!entry[2])
        continue;
      logMessage(LogLevel::Error, "dispatch breadcrumb: #%u cs %08x%08x %s", entry[2], entry[1], entry[0],
        entry[3] == entry[2] ? "finished" : "STARTED, NOT FINISHED");
      for (const DispatchRecord& record : m_dispatchRecords) {
        if (record.serial != entry[2])
          continue;
        logMessage(LogLevel::Error,
          "  groups %u,%u,%u fence %llu; u0 %p %llux%u fmt %u flags 0x%x; t0 %p %llux%u fmt %u dim %u",
          record.groups[0], record.groups[1], record.groups[2], static_cast<unsigned long long>(record.fence),
          static_cast<void*>(record.uav), static_cast<unsigned long long>(record.uavDesc.Width), record.uavDesc.Height,
          static_cast<uint32_t>(record.uavDesc.Format), static_cast<uint32_t>(record.uavDesc.Flags),
          static_cast<void*>(record.srv), static_cast<unsigned long long>(record.srvDesc.Width), record.srvDesc.Height,
          static_cast<uint32_t>(record.srvDesc.Format), static_cast<uint32_t>(record.srvDesc.Dimension));
      }
    }
#endif
  }

  void D3D12BackendContext::dispatchIndirect(IBackendBuffer* buffer, uint64_t offset) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (accessTracking())
      trackCommand(true);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDispatch(m_cmdList.raw(), m_computeShaderHash);
    prepareShaderAccess(true);
    if (!buffer) return;
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (m_recentOpsEnabled)
      DXUP_RECENT_OP("DispatchIndirect buffer=%p offset=%llu", d3d12Buffer, static_cast<unsigned long long>(offset));

    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    holdBatchResource(d3d12Buffer->getResource());
    // As dispatches: nothing to commit or flush in the steady state.
    constexpr uint32_t computeStageBit = 1u << static_cast<uint32_t>(ShaderStage::Compute);
    if (m_psoDirtyAfterFlush || (m_descriptorDirtyStageMask & computeStageBit) || !m_pendingBarriers.empty()) {
      ensureDescriptorHeapSpace();
      commitDescriptorBindings();
      flushCommandBarriers();
    }

    if (g_gpuOpLimit >= 0) {
      g_gpuOpCounter++;
      if (m_infoDiagnosticsEnabled && g_gpuOpCounter <= 3)
        logMessage(LogLevel::Info, "GPU_OP #%d: DispatchIndirect buffer=%p offset=%llu isCompute=%d heapOff=%u",
          g_gpuOpCounter, d3d12Buffer, static_cast<unsigned long long>(offset),
          m_isComputePipeline ? 1 : 0, m_cbvSrvUavHeapOffset);
      if (g_gpuOpCounter > g_gpuOpLimit) {
        DXUP_RECENT_OP("SKIPPED DispatchIndirect #%d (limit=%d)", g_gpuOpCounter, g_gpuOpLimit);
        return;
      }
    }

    // Pre-DispatchIndirect flush removed

    beginDispatchBreadcrumb();
    m_cmdList->ExecuteIndirect(
      m_device->getDispatchIndirectSignature(),
      1, d3d12Buffer->getResource(), d3d12Buffer->getOffset() + offset, nullptr, 0);
    endDispatchBreadcrumb();
    m_drawOpsSinceFlush++;
    submitIfGpuIdle();
    if (m_drawOpsSinceFlush >= kDrawOpsFlushThreshold) flush();
#endif
  }

  // ============================================================
  // Stream Output
  // ============================================================

#ifdef _WIN32
  void D3D12BackendContext::transitionStreamOutputCounter(D3D12BackendBuffer* buffer, D3D12_RESOURCE_STATES state) {
    ID3D12Resource* counter = buffer->streamOutputCounter();
    if (!counter || buffer->m_soCounterState == state)
      return;
    flushBarriers();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = counter;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = buffer->m_soCounterState;
    barrier.Transition.StateAfter  = state;
    m_cmdList->ResourceBarrier(1, &barrier);
    buffer->m_soCounterState = state;
  }
#endif

  void D3D12BackendContext::bindStreamOutputTargets(uint32_t count, IBackendBuffer** buffers, const uint32_t* offsets) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_deviceRemoved)
      return;

    m_soBufferCount = std::min<uint32_t>(count, 4);

    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER counterWrites[4] = {};
    uint32_t counterWriteCount = 0;
    for (uint32_t i = 0; i < 4; i++) {
      auto* d3d12Buffer = i < m_soBufferCount && buffers ? static_cast<D3D12BackendBuffer*>(buffers[i]) : nullptr;
      ID3D12Resource* counter = d3d12Buffer ? d3d12Buffer->getStreamOutputCounter(m_device->getDevice()) : nullptr;
      if (!counter) {
        m_soViews[i] = {};
        m_soBuffers[i] = nullptr;
        m_soResourceHeld[i] = nullptr;
        m_soResourceHeldGeneration[i] = 0;
        continue;
      }

      transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_STREAM_OUT);
      holdBatchResourceOnce(
        d3d12Buffer->getResource(),
        m_soResourceHeld[i],
        m_soResourceHeldGeneration[i]);
      holdBatchResource(counter);

      // D3D11 appends at the buffer's filled size for an offset of -1 and
      // starts at the given byte offset otherwise.
      const uint32_t offset = offsets ? offsets[i] : 0;
      if (offset != UINT32_MAX) {
        transitionStreamOutputCounter(d3d12Buffer, D3D12_RESOURCE_STATE_COPY_DEST);
        counterWrites[counterWriteCount].Dest  = counter->GetGPUVirtualAddress();
        counterWrites[counterWriteCount].Value = offset;
        counterWriteCount++;
      }

      m_soBuffers[i] = d3d12Buffer;
      m_soViews[i].BufferLocation = d3d12Buffer->getGpuAddress();
      m_soViews[i].SizeInBytes = static_cast<UINT64>(d3d12Buffer->getDesc().size);
      m_soViews[i].BufferFilledSizeLocation = counter->GetGPUVirtualAddress();
    }

    if (counterWriteCount) {
      flushBarriers();
      if (m_cmdList.hasList2())
        m_cmdList->WriteBufferImmediate(counterWriteCount, counterWrites, nullptr);
    }

    for (uint32_t i = 0; i < m_soBufferCount; i++) {
      if (m_soBuffers[i])
        transitionStreamOutputCounter(m_soBuffers[i], D3D12_RESOURCE_STATE_STREAM_OUT);
    }

    flushBarriers();
    m_cmdList->SOSetTargets(0, m_soBufferCount, m_soBufferCount > 0 ? m_soViews : nullptr);
#endif
  }

  uint64_t D3D12BackendContext::getStreamOutputFilledSize(IBackendBuffer* buffer) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalStreamOutputReadbacks++;
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (!d3d12Buffer || !d3d12Buffer->streamOutputCounter() || m_deviceRemoved)
      return 0;

    if (!m_soReadback) {
      D3D12_HEAP_PROPERTIES readbackProps = {};
      readbackProps.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC bufferDesc = {};
      bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
      bufferDesc.Width            = sizeof(uint64_t);
      bufferDesc.Height           = 1;
      bufferDesc.DepthOrArraySize = 1;
      bufferDesc.MipLevels        = 1;
      bufferDesc.SampleDesc.Count = 1;
      bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(m_device->getDevice()->CreateCommittedResource(&readbackProps, D3D12_HEAP_FLAG_NONE,
          &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_soReadback))))
        return 0;
    }

    transitionStreamOutputCounter(d3d12Buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
    holdBatchResource(d3d12Buffer->streamOutputCounter());
    const bool paused = pausePredication();
    m_cmdList->CopyBufferRegion(m_soReadback.Get(), 0, d3d12Buffer->streamOutputCounter(), 0, sizeof(uint64_t));
    resumePredication(paused);

    // A buffer that is still a stream output target keeps being written.
    for (uint32_t i = 0; i < m_soBufferCount; i++) {
      if (m_soBuffers[i] == d3d12Buffer)
        transitionStreamOutputCounter(d3d12Buffer, D3D12_RESOURCE_STATE_STREAM_OUT);
    }

    flush();
    synchronize();

    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, sizeof(uint64_t) };
    if (FAILED(m_soReadback->Map(0, &readRange, &mapped)) || !mapped)
      return 0;
    uint64_t value = 0;
    std::memcpy(&value, mapped, sizeof(value));
    D3D12_RANGE writeRange = { 0, 0 };
    m_soReadback->Unmap(0, &writeRange);
    return value;
#else
    (void)buffer;
    return 0;
#endif
  }

  void D3D12BackendContext::drawAuto(IBackendBuffer* vertexBuffer, uint32_t stride) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (accessTracking())
      trackCommand(false);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDraw(m_cmdList.raw());
    prepareShaderAccess(false);
    if (!vertexBuffer || !stride)
      return;
    // D3D12 has no DrawAuto: read back how much stream output wrote.
    const uint64_t filledSize = getStreamOutputFilledSize(vertexBuffer);
    const uint32_t vertexCount = static_cast<uint32_t>(filledSize / stride);
    if (vertexCount)
      draw(vertexCount, 1, 0, 0);
#else
    (void)vertexBuffer;
    (void)stride;
#endif
  }

  // ============================================================
  // Copy Operations
  // ============================================================

  void D3D12BackendContext::copyBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                                       IBackendBuffer* src, uint64_t srcOffset, uint64_t size) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onCopy();
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopyBufferCalls++;
    auto* d3d12Dst = static_cast<D3D12BackendBuffer*>(dst);
    auto* d3d12Src = static_cast<D3D12BackendBuffer*>(src);
    markGpuUse(stagingGpuUse(dst));
    markGpuRead(stagingGpuUse(src));
    prepareBufferGpuWrite(d3d12Dst);

    // Slices of one default block cannot be source and destination at
    // once: copy through a buffer of its own.
    if (d3d12Dst != d3d12Src && d3d12Dst->getResource() == d3d12Src->getResource()) {
      BufferDesc bounceDesc = {};
      bounceDesc.size = size;
      bounceDesc.usage = ResourceUsage::Default;
      bounceDesc.bindFlags = BindFlags::UnorderedAccess;
      if (auto bounce = m_device->createBuffer(bounceDesc)) {
        copyBuffer(bounce.get(), 0, src, srcOffset, size);
        copyBuffer(dst, dstOffset, bounce.get(), 0, size);
        retainTemporaryBuffer(std::move(bounce));
      }
      return;
    }

    const auto restoreSource = restoreAfterCopy(d3d12Src != d3d12Dst ? d3d12Src : nullptr, false);
    transitionResource(d3d12Dst, D3D12_RESOURCE_STATE_COPY_DEST);
    transitionResource(d3d12Src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    holdBatchResource(d3d12Dst->getResource());
    holdBatchResource(d3d12Src->getResource());
    flushBarriers();

    m_cmdList->CopyBufferRegion(d3d12Dst->getResource(), d3d12Dst->getOffset() + dstOffset,
                                 d3d12Src->getResource(), d3d12Src->getOffset() + srcOffset, size);
    restoreBoundBufferState(d3d12Dst);
#endif
  }

  void D3D12BackendContext::copyWholeResource(IBackendImage* dst, IBackendImage* src) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopyImageCalls++;
    auto* d3d12Dst = static_cast<D3D12BackendImage*>(dst);
    auto* d3d12Src = static_cast<D3D12BackendImage*>(src);

    // Use whole-image transitions (not subresource) and CopyResource (not CopyTextureRegion)
    transitionResource(d3d12Dst, D3D12_RESOURCE_STATE_COPY_DEST);
    transitionResource(d3d12Src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    holdBatchResource(d3d12Dst->getResource());
    holdBatchResource(d3d12Src->getResource());
    flushBarriers();

    m_cmdList->CopyResource(d3d12Dst->getResource(), d3d12Src->getResource());

    static int copyOps = 0;
    m_copyOpsSinceFlush++;
#endif
  }

  void D3D12BackendContext::copyImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                                       IBackendImage* src, const ImageSubresource& srcSubres) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onCopy();
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopyImageCalls++;
    auto* d3d12Dst = static_cast<D3D12BackendImage*>(dst);
    auto* d3d12Src = static_cast<D3D12BackendImage*>(src);

    if (d3d12Dst->isStaging() || d3d12Src->isStaging()) {
      copyImageRegion(dst, dstSubres, 0, 0, 0, src, srcSubres, nullptr);
      return;
    }

    // For full-image copies (default subresource, single mip, single array slice),
    // use CopyResource with whole-image transitions. This avoids subresource-level
    // barriers that can cause E_INVALIDARG on swap chain buffers.
    if (dstSubres.mipLevel == 0 && dstSubres.arrayLayer == 0 && dstSubres.arrayLayerCount == 1 &&
        srcSubres.mipLevel == 0 && srcSubres.arrayLayer == 0 && srcSubres.arrayLayerCount == 1 &&
        d3d12Dst->getMipLevelCount() == 1 && d3d12Src->getMipLevelCount() == 1 &&
        d3d12Dst->getArraySubresourceCount() == 1 && d3d12Src->getArraySubresourceCount() == 1) {
      transitionResource(d3d12Dst, D3D12_RESOURCE_STATE_COPY_DEST);
      transitionResource(d3d12Src, D3D12_RESOURCE_STATE_COPY_SOURCE);
      // Both live until the GPU has copied (the application may let go now).
      holdBatchResource(d3d12Dst->getResource());
      holdBatchResource(d3d12Src->getResource());
      flushBarriers();
      m_cmdList->CopyResource(d3d12Dst->getResource(), d3d12Src->getResource());
      m_copyOpsSinceFlush++;
      return;
    }

    copyImageRegion(dst, dstSubres, 0, 0, 0, src, srcSubres, nullptr);
#endif
  }

  void D3D12BackendContext::blitPixels(IBackendImage* dst, uint32_t dstX, uint32_t dstY,
                                        uint32_t width, uint32_t height,
                                        const uint32_t* rgbaPixels) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopyBufferToImageCalls++;
    if (m_deviceRemoved || !width || !height || !rgbaPixels)
      return;

    auto* d3d12Dst = static_cast<D3D12BackendImage*>(dst);
    if (!d3d12Dst || !d3d12Dst->getResource())
      return;

    // Row pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256)
    const uint32_t rowPitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                            & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const uint64_t totalSize = uint64_t(rowPitch) * height;

    // From the upload ring, which the GPU's progress frees (a buffer of its
    // own was written again while the last copy could still read it).
    ID3D12Resource* upload = nullptr;
    uint64_t uploadOffset = 0;
    void* uploadPtr = nullptr;
    if (!allocateUploadScratch(totalSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &upload, &uploadOffset, &uploadPtr))
      return;
    auto* mapped = static_cast<uint8_t*>(uploadPtr);
    for (uint32_t y = 0; y < height; y++)
      memcpy(mapped + y * rowPitch, rgbaPixels + y * width, width * 4);

    // Source: upload buffer with placed footprint
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = uploadOffset;
    srcLoc.PlacedFootprint.Footprint.Format = d3d12Dst->getFormat();
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    // Destination: subresource 0 of the target image, in the copy state.
    transitionResource(d3d12Dst, D3D12_RESOURCE_STATE_COPY_DEST);
    holdBatchResource(d3d12Dst->getResource());
    flushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = d3d12Dst->getResource();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    // Cut to the image (an overlay wider than a small window).
    const uint32_t dstWidth = d3d12Dst->getDesc().width, dstHeight = d3d12Dst->getDesc().height;
    if (dstX >= dstWidth || dstY >= dstHeight)
      return;
    D3D12_BOX srcBox = {0, 0, 0, (std::min)(width, dstWidth - dstX), (std::min)(height, dstHeight - dstY), 1};
    m_cmdList->CopyTextureRegion(&dstLoc, dstX, dstY, 0, &srcLoc, &srcBox);
#endif
  }

  void D3D12BackendContext::copyImageRegion(IBackendImage* dst, const ImageSubresource& dstSubres,
                                            uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                                            IBackendImage* src, const ImageSubresource& srcSubres,
                                            const ImageCopyRegion* srcRegion) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopyImageRegionCalls++;
    if (m_deviceRemoved)
      return;
    auto* d3d12Dst = static_cast<D3D12BackendImage*>(dst);
    auto* d3d12Src = static_cast<D3D12BackendImage*>(src);
    markGpuUse(stagingGpuUse(dst));
    markGpuRead(stagingGpuUse(src));

    if (d3d12Dst->isStaging() || d3d12Src->isStaging()) {
      copyImageRegionStaging(d3d12Dst, dstSubres, dstX, dstY, dstZ, d3d12Src, srcSubres, srcRegion);
      return;
    }

    transitionResource(d3d12Dst, dstSubres, D3D12_RESOURCE_STATE_COPY_DEST);
    transitionResource(d3d12Src, srcSubres, D3D12_RESOURCE_STATE_COPY_SOURCE);
    holdBatchResource(d3d12Dst->getResource());
    holdBatchResource(d3d12Src->getResource());
    flushBarriers();

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = d3d12Dst->getResource();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = calcImageSubresourceIndex(d3d12Dst, dstSubres);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = d3d12Src->getResource();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = calcImageSubresourceIndex(d3d12Src, srcSubres);

    D3D12_BOX srcBox = {};
    const D3D12_BOX* srcBoxPtr = nullptr;
    if (srcRegion) {
      srcBox.left = srcRegion->x;
      srcBox.top = srcRegion->y;
      srcBox.front = srcRegion->z;
      srcBox.right = srcRegion->x + (std::max)(srcRegion->width, 1u);
      srcBox.bottom = srcRegion->y + (std::max)(srcRegion->height, 1u);
      srcBox.back = srcRegion->z + (std::max)(srcRegion->depth, 1u);
      alignCopyBox(d3d12Src->getFormat(), srcBox);
      srcBoxPtr = &srcBox;
    }

    safeCopyTextureRegionSimple(&m_cmdList, &dstLoc, dstX, dstY, dstZ, &srcLoc, srcBoxPtr, m_device);
    // The second plane too (D3D11 copies both): depth/stencil copies are of
    // whole subresources; the chroma of video images is subsampled, its
    // region and offset scaled (NV11: a quarter of the width; else halves).
    const uint32_t planes = (std::min)(d3d12Dst->getPlaneCount(), d3d12Src->getPlaneCount());
    const DXGI_FORMAT dstFormat = d3d12Dst->getDesc().format;
    const bool video = isPlanarVideoFormat(dstFormat);
    const uint32_t sx = dstFormat == DXGI_FORMAT_NV11 ? 4u : 2u, sy = dstFormat == DXGI_FORMAT_NV11 ? 1u : 2u;
    for (uint32_t plane = 1; plane < planes; plane++) {
      D3D12_TEXTURE_COPY_LOCATION dstPlane = dstLoc;
      D3D12_TEXTURE_COPY_LOCATION srcPlane = srcLoc;
      dstPlane.SubresourceIndex = d3d12Dst->calcSubresource(dstSubres.mipLevel, dstSubres.arrayLayer, plane);
      srcPlane.SubresourceIndex = d3d12Src->calcSubresource(srcSubres.mipLevel, srcSubres.arrayLayer, plane);
      if (!video) {
        m_cmdList->CopyTextureRegion(&dstPlane, 0, 0, 0, &srcPlane, nullptr);
        continue;
      }
      D3D12_BOX chromaBox = {};
      if (srcBoxPtr)
        chromaBox = { srcBox.left / sx, srcBox.top / sy, 0, (srcBox.right + sx - 1) / sx, (srcBox.bottom + sy - 1) / sy, 1 };
      m_cmdList->CopyTextureRegion(&dstPlane, dstX / sx, dstY / sy, 0, &srcPlane, srcBoxPtr ? &chromaBox : nullptr);
    }
#endif
  }

  void D3D12BackendContext::prepareSharedSubmit() {
#ifdef _WIN32
    // Other devices find shared images in COMMON. Views of them that stay
    // bound return to their states before the next draw.
    for (size_t i = 0; i < m_sharedImagesInBatch.size(); i++)
      transitionResource(m_sharedImagesInBatch[i], D3D12_RESOURCE_STATE_COMMON);

    // Wait for the other devices' last submissions that used the resources.
    std::vector<std::pair<ComPtr<ID3D12Fence>, uint64_t>> waits;
    auto collect = [&](D3D12SharedSync* sync) {
      std::lock_guard<std::mutex> lock(sync->mutex);
      if (!sync->fence || sync->fence.Get() == m_fence.Get())
        return;
      for (auto& wait : waits) {
        if (wait.first.Get() == sync->fence.Get()) {
          wait.second = (std::max)(wait.second, sync->value);
          return;
        }
      }
      waits.emplace_back(sync->fence, sync->value);
    };
    for (auto* image : m_sharedImagesInBatch)
      collect(image->getSharedSync());
    for (auto* buffer : m_sharedBuffersInBatch)
      collect(buffer->getSharedSync());
    for (const auto& wait : waits) {
      if (wait.first->GetCompletedValue() >= wait.second)
        continue;
      ComPtr<ID3D12Fence> fence = wait.first;
      const uint64_t value = wait.second;
      m_device->submitQueue().enqueue([fence, value](ID3D12CommandQueue* queue) { queue->Wait(fence.Get(), value); });
    }
#endif
  }

  void D3D12BackendContext::publishSharedSubmit() {
#ifdef _WIN32
    auto publish = [&](D3D12SharedSync* sync) {
      std::lock_guard<std::mutex> lock(sync->mutex);
      sync->fence = m_fence;
      sync->value = m_fenceValue;
    };
    for (auto* image : m_sharedImagesInBatch)
      publish(image->getSharedSync());
    // Buffers decay to COMMON at the end of the submission.
    for (auto* buffer : m_sharedBuffersInBatch) {
      publish(buffer->getSharedSync());
      buffer->setCurrentState(D3D12_RESOURCE_STATE_COMMON);
    }
    m_sharedImagesInBatch.clear();
    m_sharedBuffersInBatch.clear();
#endif
  }

  void D3D12BackendContext::prepareShaderAccess(bool compute) {
#ifdef _WIN32
    // A submission for descriptor space comes first: it forgets the
    // unordered access written so far, which then must not include this
    // draw's (its next reader would get no barrier).
    ensureDescriptorHeapSpace();
    if (compute ? m_computeImageStatesDirty : m_graphicsImageStatesDirty)
      restoreBoundImageStates(compute);
    orderUavAccess(compute);
#else
    (void)compute;
#endif
  }

  void D3D12BackendContext::restoreBoundImageStates(bool compute) {
#ifdef _WIN32
    constexpr uint32_t computeStage = static_cast<uint32_t>(ShaderStage::Compute);
    const uint32_t firstStage = compute ? computeStage : 0u;
    const uint32_t lastStage = compute ? computeStage + 1 : computeStage;
    for (uint32_t stage = firstStage; stage < lastStage; stage++) {
      for (uint32_t slot = 0; slot < m_srvBindingEnd[stage]; slot++) {
        if (auto* view = m_srvImageViews[stage][slot])
          ensureImageViewState(view, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      for (uint32_t slot = 0; slot < m_uavBindingEnd[stage]; slot++) {
        if (auto* view = m_uavImageViews[stage][slot])
          ensureImageViewState(view, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
    }
    // The transitions made here are the ones this pipeline needs; the other
    // pipeline checks its views again.
    if (compute)
      m_computeImageStatesDirty = false;
    else
      m_graphicsImageStatesDirty = false;
#else
    (void)compute;
#endif
  }

  void D3D12BackendContext::transitionDepthStencilPlanes(D3D12BackendImage* image, const D3D12BackendImageView* view,
                                                         D3D12_RESOURCE_STATES depthState, D3D12_RESOURCE_STATES stencilState) {
#ifdef _WIN32
    if (memstats::enabled() && ((depthState | stencilState) & kImageWriteStates))
      memstats::noteWritten(image->getResource());
    noteSharedUse(image);
    const auto& imageDesc = image->getDesc();
    const auto& viewDesc = view->getDesc();
    const uint32_t mipCount = getImageViewMipCountForTransition(viewDesc, imageDesc);
    const uint32_t arrayCount = getImageViewArrayCountForTransition(viewDesc, imageDesc);
    for (uint32_t plane = 0; plane < 2; plane++) {
      const D3D12_RESOURCE_STATES state = plane ? stencilState : depthState;
      for (uint32_t arrayLayer = 0; arrayLayer < arrayCount; arrayLayer++) {
        const uint32_t layer = imageDesc.type == ImageType::Tex3D ? 0u : viewDesc.baseArrayLayer + arrayLayer;
        for (uint32_t mip = 0; mip < mipCount; mip++)
          transitionSubresource(image, image->calcSubresource(viewDesc.baseMipLevel + mip, layer, plane), state);
      }
    }
#else
    (void)image;
    (void)view;
    (void)depthState;
    (void)stencilState;
#endif
  }

  void D3D12BackendContext::ensureImageViewState(D3D12BackendImageView* view, D3D12_RESOURCE_STATES needed) {
#ifdef _WIN32
    auto* image = static_cast<D3D12BackendImage*>(view->getImage());
    if (!image || image->isStaging())
      return;
    // A state that includes the needed one also serves, such as a depth
    // buffer bound read-only for depth tests and for shaders at once.
    if (image->hasUniformState()) {
      if ((image->getCurrentState() & needed) != needed)
        transitionResource(image, view, needed);
      return;
    }

    const auto& imageDesc = image->getDesc();
    const auto& viewDesc = view->getDesc();
    const uint32_t mipCount = getImageViewMipCountForTransition(viewDesc, imageDesc);
    const uint32_t arrayCount = getImageViewArrayCountForTransition(viewDesc, imageDesc);
    const uint32_t planeStart = getImageViewPlaneStartForTransition(viewDesc, imageDesc);
    const uint32_t planeEnd = (std::min)(planeStart + getImageViewPlaneCountForTransition(image, viewDesc, imageDesc),
      image->getPlaneCount());
    for (uint32_t plane = planeStart; plane < planeEnd; plane++) {
      for (uint32_t arrayLayer = 0; arrayLayer < arrayCount; arrayLayer++) {
        const uint32_t layer = imageDesc.type == ImageType::Tex3D ? 0u : viewDesc.baseArrayLayer + arrayLayer;
        for (uint32_t mip = 0; mip < mipCount; mip++) {
          const uint32_t subresource = image->calcSubresource(viewDesc.baseMipLevel + mip, layer, plane);
          if ((image->getSubresourceState(subresource) & needed) != needed)
            transitionSubresource(image, subresource, needed);
        }
      }
    }
#else
    (void)view;
    (void)needed;
#endif
  }

  void D3D12BackendContext::orderUavAccess(bool compute) {
#ifdef _WIN32
    constexpr uint32_t computeStage = static_cast<uint32_t>(ShaderStage::Compute);
    const uint32_t firstStage = compute ? computeStage : 0u;
    const uint32_t lastStage = compute ? computeStage + 1 : computeStage;
    // A shader cannot write a UAV it does not declare: UAVs left bound need
    // no barrier for draws and dispatches that do not use them. Graphics
    // UAVs are shared by the stages (bound with the pixel stage's).
    uint32_t declared = UINT32_MAX;
    if (compute) {
      if (m_currentComputeResourceLimits.known)
        declared = m_currentComputeResourceLimits.uavCount;
    } else {
      declared = 0;
      for (uint32_t stage = 0; stage < kGraphicsShaderStages; stage++) {
        if (!(m_currentGraphicsStageMask & (1u << stage)))
          continue;
        const ShaderResourceLimits& limits = m_currentGraphicsResourceLimits[stage];
        declared = limits.known ? (std::max)(declared, limits.uavCount) : UINT32_MAX;
        if (declared == UINT32_MAX)
          break;
      }
    }
    for (uint32_t stage = firstStage; stage < lastStage; stage++) {
      const uint32_t end = (std::min)(m_uavBindingEnd[stage], declared);
      for (uint32_t slot = 0; slot < end; slot++) {
        ID3D12Resource* resource = m_currentUavResources[stage][slot];
        if (!resource)
          continue;
        uint16_t access = 0;
        if (compute) {
          access = m_currentComputeResourceLimits.uavAccess
            ? m_currentComputeResourceLimits.uavAccess[slot] : uint16_t(dxbc::kUavReadWrite);
        } else {
          for (uint32_t shaderStage = 0; shaderStage < kGraphicsShaderStages; shaderStage++) {
            if (!(m_currentGraphicsStageMask & (1u << shaderStage)))
              continue;
            const uint16_t* stageAccess = m_currentGraphicsResourceLimits[shaderStage].uavAccess;
            access |= stageAccess ? stageAccess[slot] : uint16_t(dxbc::kUavReadWrite);
          }
        }
        if (access)
          orderUavAccess(resource, access, m_currentPSO);
      }
    }
#else
    (void)compute;
#endif
  }

  void D3D12BackendContext::trackCommand(bool compute) {
    const uint64_t serial = (m_srvBindingSerial << 8) ^ (compute ? 0x80u : m_currentGraphicsStageMask);
    if (!m_accessTracker.readsCurrent(serial)) {
      constexpr uint32_t computeStage = static_cast<uint32_t>(ShaderStage::Compute);
      for (uint32_t stage = compute ? computeStage : 0u; stage < (compute ? computeStage + 1 : computeStage); stage++) {
        if (!compute && !(m_currentGraphicsStageMask & (1u << stage)))
          continue;
        for (uint32_t slot = 0; slot < m_srvBindingEnd[stage] && slot < kMaxShaderResources; slot++)
          m_accessTracker.noteRead(m_currentSrvResources[stage][slot]);
      }
      m_accessTracker.setReadSerial(serial);
    }
    m_accessTracker.commit(!compute);
  }

  uint64_t D3D12BackendContext::barrierBatchSignature() const {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](uint64_t value) {
      hash = fnv64Multiply(hash ^ value);
    };
    for (const auto& b : m_pendingBarriers) {
      if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        mix(reinterpret_cast<uintptr_t>(b.Transition.pResource));
        mix(b.Transition.Subresource);
        mix(b.Transition.StateAfter);
      } else {
        mix(reinterpret_cast<uintptr_t>(b.UAV.pResource) ^ 0x5555u);
      }
    }
    return hash;
  }

  void D3D12BackendContext::handleBarrierBatch() {
    const bool tracking = accessTracking();
    const auto* barriers = m_pendingBarriers.data();
    const size_t count = m_pendingBarriers.size();
    int kind = BarrierAccessTracker::Written;
    if (tracking) {
      kind = m_accessTracker.classify(barriers, count);
      if (m_gpuProfiler.capturing())
        m_gpuProfiler.countBatch(kind);
    }
    const uint64_t signature = (m_hoistLearning || !m_hoistPlan.empty()) ? barrierBatchSignature() : 0;
    // Only transitions of textures DXUP tracks between the states of draws
    // and dispatches can move (not those of copies, resolves and swap chain
    // buffers, which go through the common state).
    constexpr D3D12_RESOURCE_STATES kMovable = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE
      | D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
      | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool movable = kind == BarrierAccessTracker::Neither && m_hoistAnchor;
    const bool profiling = m_gpuProfiler.capturing();
    for (size_t i = 0; i < count && movable; i++) {
      const auto& t = barriers[i].Transition;
      movable = barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
        && t.StateBefore && !(t.StateBefore & ~kMovable) && t.StateAfter && !(t.StateAfter & ~kMovable)
        && (!m_hoistLearning || m_learnImages.count(t.pResource));
    }
    if (kind == BarrierAccessTracker::Neither && m_gpuProfiler.capturing()) {
      const char* reason = movable ? (m_hoistLearning ? "learning frame" : nullptr) : !m_hoistAnchor ? "no batch before in the frame" : nullptr;
      for (size_t i = 0; i < count && !reason; i++) {
        const auto& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
          reason = "UAV barrier";
        else if (b.Transition.pResource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
          reason = "buffer";
        else if (!b.Transition.StateBefore || (b.Transition.StateBefore & ~kMovable) || !b.Transition.StateAfter
              || (b.Transition.StateAfter & ~kMovable))
          reason = "copy/common/other state";
      }
      if (!reason) {
        for (size_t i = 0; i < count && !reason; i++) {
          if (std::find(m_hoistSkipped.begin(), m_hoistSkipped.end(), barriers[i].Transition.pResource) != m_hoistSkipped.end())
            reason = "planned, skipped (bound or other state)";
        }
      }
      m_gpuProfiler.countUnmoved(reason ? reason : m_hoistAnchorNote ? m_hoistAnchorNote : "image not learned");
    }
    if (m_hoistLearning && movable) {
      auto& entries = m_hoistLearned[m_hoistAnchor];
      for (size_t i = 0; i < count; i++) {
        const auto& t = barriers[i].Transition;
        entries.push_back({ m_learnImages[t.pResource], t.Subresource, t.StateBefore, t.StateAfter });
      }
    }
    // Learning frames see the batches as the application makes them.
    if (!m_hoistLearning && m_hoistHere && !m_hoistPlan.empty()) {
      auto it = m_hoistPlan.find(signature);
      bool anyApplied = false;
      if (it != m_hoistPlan.end()) {
        // Untouched: what this batch transitions and what the draw or
        // dispatch after it has bound keep the states it needs.
        std::vector<ID3D12Resource*>& bound = m_hoistBound;
        bound.clear();
        if (profiling)
          m_hoistSkipped.clear();
        for (const auto& b : m_pendingBarriers) {
          if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            bound.push_back(b.Transition.pResource);
        }
        for (uint32_t i = 0; i < m_currentRenderTargetCount; i++)
          bound.push_back(m_currentRenderTargetResources[i]);
        bound.push_back(m_currentDepthStencilResource);
        for (uint32_t stage = 0; stage < kShaderStages; stage++) {
          for (uint32_t slot = 0; slot < m_srvBindingEnd[stage] && slot < kMaxShaderResources; slot++)
            bound.push_back(m_currentSrvResources[stage][slot]);
          for (uint32_t slot = 0; slot < m_uavBindingEnd[stage] && slot < kMaxUAVs; slot++)
            bound.push_back(m_currentUavResources[stage][slot]);
        }
        std::sort(bound.begin(), bound.end());
        for (const HoistEntry& entry : it->second) {
          auto image = entry.image.lock();
          if (!image || std::binary_search(bound.begin(), bound.end(), image->getResource())) {
            if (image && profiling)
              m_hoistSkipped.push_back(image->getResource());
            continue;
          }
          bool applied = false;
          if (entry.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            if ((applied = image->hasUniformState() && image->getCurrentState() == entry.before))
              transitionResource(image.get(), entry.after);
          } else if (entry.subresource < image->getSubresourceCount()
                  && image->getSubresourceState(entry.subresource) == entry.before) {
            transitionSubresource(image.get(), entry.subresource, entry.after);
            applied = true;
          }
          if (!applied && profiling)
            m_hoistSkipped.push_back(image->getResource());
          anyApplied |= applied;
        }
      }
      (void)anyApplied;
    }
    if (tracking) {
      if (movable)
        m_accessTracker.addWrites(m_pendingBarriers.data(), m_pendingBarriers.size());
      else
        m_accessTracker.anchor(m_pendingBarriers.data(), m_pendingBarriers.size());
    }
    if (!movable) {
      m_hoistAnchor = signature ? signature : 1;
      if (profiling) {
        m_hoistAnchorNote = !m_hoistHere ? "batch before not for an app draw/dispatch"
          : m_hoistPlan.count(signature) ? "not in the plan of the batch before" : "no plan for the batch before";
        if (m_hoistLearning || !m_hoistHere || m_hoistPlan.count(signature) == 0)
          m_hoistSkipped.clear();
      }
    }
  }

  void D3D12BackendContext::hoistFrameEnd() {
    if (!m_hoistEnabled || m_copyOnly)
      return;
    // Frames 32 and 33 of every 256 learn.
    const uint64_t frame = m_hoistFrames++ & 255;
    m_hoistAnchor = 0;
    if (!m_hoistLearning) {
      if (frame == 31) {
        m_hoistLearning = true;
        m_hoistLearned.clear();
        m_hoistFirstLearned.clear();
        m_accessTracker.clear();
      }
      return;
    }
    auto same = [](const HoistEntry& a, const HoistEntry& b) {
      return !a.image.owner_before(b.image) && !b.image.owner_before(a.image)
        && a.subresource == b.subresource && a.before == b.before && a.after == b.after;
    };
    m_learnImages.clear();
    m_accessTracker.clear();
    if (frame == 32) {
      // What the second frame sees again becomes the plan: a frame that
      // differs teaches nothing that hurts the ones after.
      m_hoistFirstLearned = std::move(m_hoistLearned);
      m_hoistLearned.clear();
      return;
    }
    std::unordered_map<uint64_t, std::vector<HoistEntry>> plan;
    for (auto& [anchor, entries] : m_hoistLearned) {
      auto first = m_hoistFirstLearned.find(anchor);
      if (first == m_hoistFirstLearned.end())
        continue;
      std::vector<HoistEntry> kept;
      for (auto& entry : entries) {
        const bool again = std::any_of(first->second.begin(), first->second.end(),
          [&](const HoistEntry& other) { return same(other, entry); });
        if (again && std::none_of(kept.begin(), kept.end(), [&](const HoistEntry& other) { return same(other, entry); }))
          kept.push_back(std::move(entry));
      }
      if (!kept.empty())
        plan.emplace(anchor, std::move(kept));
    }
    m_hoistPlan = std::move(plan);
    m_hoistLearned.clear();
    m_hoistFirstLearned.clear();
    m_hoistLearning = false;
  }

  void D3D12BackendContext::orderUavAccess(ID3D12Resource* resource, uint16_t access, ID3D12PipelineState* writer) {
#ifdef _WIN32
    if (accessTracking()) {
      if (access & ~dxbc::kUavRead)
        m_accessTracker.noteWrite(resource);
      else
        m_accessTracker.noteRead(resource);
    }
    auto it = std::find(m_uavWritten.begin(), m_uavWritten.end(), resource);
    if (it != m_uavWritten.end()) {
      const size_t index = it - m_uavWritten.begin();
      uint16_t& earlier = m_uavWrittenAccess[index];
      const uint16_t both = earlier | access;
      const bool readsOnly = !(both & ~dxbc::kUavRead);
      const bool oneAtomicKind = !(both & dxbc::kUavReadWrite) && !(both & (both - 1));
      // Relaxed: the same pipeline again overlaps with itself; another one
      // waits for all of them.
      const bool overlap = m_relaxedUavBarriers && writer && m_uavWrittenBy[index] == writer;
      if (readsOnly || oneAtomicKind || overlap) {
        earlier = both;
        return;
      }
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      barrier.UAV.pResource = resource;
      m_pendingBarriers.push_back(barrier);
      earlier = access;
      m_uavWrittenBy[index] = writer;
      return;
    }
    if (m_uavWritten.size() >= 64) {
      // Too many to track one by one: wait for all unordered access.
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      m_pendingBarriers.push_back(barrier);
      m_uavWritten.clear();
      m_uavWrittenAccess.clear();
      m_uavWrittenBy.clear();
    }
    m_uavWritten.push_back(resource);
    m_uavWrittenAccess.push_back(access);
    m_uavWrittenBy.push_back(writer);
#else
    (void)resource;
    (void)access;
    (void)writer;
#endif
  }

  void D3D12BackendContext::restoreBoundBufferState(D3D12BackendBuffer* buffer) {
#ifdef _WIN32
    if (!buffer || buffer->hasFixedState())
      return;
    // Other slices of the block may be bound: it returns to every state
    // its slices are read in.
    if (buffer->hasSharedState()) {
      if (!m_copyOnly)
        transitionResource(buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
          | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_COPY_SOURCE);
      return;
    }
    auto* resource = buffer->getResource();
    D3D12_RESOURCE_STATES boundState = static_cast<D3D12_RESOURCE_STATES>(0);
    const D3D12_GPU_VIRTUAL_ADDRESS bufferStart = buffer->getGpuAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS bufferEnd = bufferStart + buffer->getDesc().size;
    auto inBuffer = [&](D3D12_GPU_VIRTUAL_ADDRESS address) {
      return address >= bufferStart && address < bufferEnd;
    };

    for (uint32_t slot = 0; slot < kMaxVertexBuffers; slot++) {
      if (m_trackedVBs[slot].bound && inBuffer(m_trackedVBs[slot].view.BufferLocation))
        boundState |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if (m_trackedIBBound && inBuffer(m_trackedIB.BufferLocation))
      boundState |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    for (uint32_t stage = 0; stage < kShaderStages; stage++) {
      for (uint32_t slot = 0; slot < m_cbvBindingEnd[stage]; slot++) {
        const auto& binding = m_cbvBindings[stage][slot];
        if (binding.bound && inBuffer(binding.gpuAddress))
          boundState |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
      }
      for (uint32_t slot = 0; slot < m_srvBindingEnd[stage]; slot++) {
        if (m_currentSrvResources[stage][slot] == resource)
          boundState |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      }
    }
    // A buffer bound for unordered access is written by shaders: that state
    // excludes the read states.
    bool unordered = false;
    for (uint32_t stage = 0; stage < kShaderStages; stage++) {
      for (uint32_t slot = 0; slot < m_uavBindingEnd[stage]; slot++) {
        if (m_currentUavResources[stage][slot] == resource)
          unordered = true;
      }
    }
    if (unordered)
      transitionResource(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    else if (boundState != 0)
      transitionResource(buffer, boundState);
#else
    (void)buffer;
#endif
  }

  void D3D12BackendContext::copyFootprintRows(
      ID3D12Resource* dst, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& dstFootprint,
      uint32_t dstRowsPerSlice, uint64_t dstX, uint32_t dstRow, uint32_t dstZ,
      ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& srcFootprint,
      uint32_t srcRowsPerSlice, uint64_t srcX, uint32_t srcRow, uint32_t srcZ,
      uint64_t rowBytes, uint32_t rows, uint32_t slices) {
#ifdef _WIN32
    if (!dst || !src || !rowBytes || !rows)
      return;
    const uint64_t dstPitch = dstFootprint.Footprint.RowPitch;
    const uint64_t srcPitch = srcFootprint.Footprint.RowPitch;
    // Rows that fill their pitch in both layouts are one span.
    const bool contiguous = dstPitch == srcPitch && rowBytes == dstPitch && dstX == 0 && srcX == 0;
    for (uint32_t slice = 0; slice < slices; slice++) {
      const uint64_t dstBase = dstFootprint.Offset + uint64_t(dstZ + slice) * dstPitch * dstRowsPerSlice + dstX;
      const uint64_t srcBase = srcFootprint.Offset + uint64_t(srcZ + slice) * srcPitch * srcRowsPerSlice + srcX;
      if (contiguous) {
        m_cmdList->CopyBufferRegion(dst, dstBase + uint64_t(dstRow) * dstPitch,
          src, srcBase + uint64_t(srcRow) * srcPitch, rowBytes * rows);
        continue;
      }
      for (uint32_t row = 0; row < rows; row++) {
        m_cmdList->CopyBufferRegion(dst, dstBase + uint64_t(dstRow + row) * dstPitch,
          src, srcBase + uint64_t(srcRow + row) * srcPitch, rowBytes);
      }
    }
#endif
  }

  void D3D12BackendContext::copyImageRegionStaging(
      D3D12BackendImage* dst, const ImageSubresource& dstSubres,
      uint32_t dstX, uint32_t dstY, uint32_t dstZ,
      D3D12BackendImage* src, const ImageSubresource& srcSubres,
      const ImageCopyRegion* srcRegion) {
#ifdef _WIN32
    transitionResource(dst, dstSubres, D3D12_RESOURCE_STATE_COPY_DEST);
    transitionResource(src, srcSubres, D3D12_RESOURCE_STATE_COPY_SOURCE);
    holdBatchResource(dst->getResource());
    holdBatchResource(src->getResource());
    flushBarriers();

    const UINT dstIndex = calcImageSubresourceIndex(dst, dstSubres);
    const UINT srcIndex = calcImageSubresourceIndex(src, srcSubres);
    const ImageCopyRegion region = srcRegion ? *srcRegion : getFullSubresourceRegion(src, srcSubres);

    // Planar video formats: a whole-subresource copy takes every plane,
    // as the D3D11 subresource does.
    D3D12BackendImage* staging = dst->isStaging() ? dst : src;
    const uint32_t planeStride = staging->getMipLevelCount() * staging->getArraySubresourceCount();
    const uint32_t planes = srcRegion || !planeStride
      ? 1u
      : (std::max)(uint32_t(staging->getStagingLayout().footprints.size() / planeStride), 1u);

    // Each image steps to its planes by its own subresource count (a level
    // of a mipmapped texture against a one-level staging texture).
    const uint32_t dstPlaneStride = dst->getMipLevelCount() * dst->getArraySubresourceCount();
    const uint32_t srcPlaneStride = src->getMipLevelCount() * src->getArraySubresourceCount();
    for (uint32_t plane = 0; plane < planes; plane++) {
      const UINT dstSub = dstIndex + plane * dstPlaneStride;
      const UINT srcSub = srcIndex + plane * srcPlaneStride;

      if (dst->isStaging() && src->isStaging()) {
        const auto& dstLayout = dst->getStagingLayout();
        const auto& srcLayout = src->getStagingLayout();
        if (dstSub >= dstLayout.footprints.size() || srcSub >= srcLayout.footprints.size())
          return;
        const auto& srcFootprint = srcLayout.footprints[srcSub];
        uint32_t blockWidth = 1, blockHeight = 1;
        getCopyBlockExtent(srcFootprint.Footprint.Format, blockWidth, blockHeight);
        const uint32_t blocksWide = (srcFootprint.Footprint.Width + blockWidth - 1) / blockWidth;
        const uint64_t blockBytes = blocksWide ? srcLayout.rowSizes[srcSub] / blocksWide : 0;
        const uint32_t width = plane ? srcFootprint.Footprint.Width : region.width;
        const uint32_t height = plane ? srcFootprint.Footprint.Height : region.height;
        const uint32_t rows = plane ? srcLayout.numRows[srcSub] : (height + blockHeight - 1) / blockHeight;
        copyFootprintRows(
          dst->getResource(), dstLayout.footprints[dstSub], dstLayout.numRows[dstSub],
          uint64_t(dstX / blockWidth) * blockBytes, dstY / blockHeight, dstZ,
          src->getResource(), srcFootprint, srcLayout.numRows[srcSub],
          uint64_t(region.x / blockWidth) * blockBytes, region.y / blockHeight, region.z,
          uint64_t((width + blockWidth - 1) / blockWidth) * blockBytes, rows,
          (std::max)(plane ? srcFootprint.Footprint.Depth : region.depth, 1u));
        continue;
      }

      auto location = [](D3D12BackendImage* image, UINT subresource) {
        D3D12_TEXTURE_COPY_LOCATION loc = {};
        loc.pResource = image->getResource();
        if (image->isStaging()) {
          loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          loc.PlacedFootprint = image->getStagingLayout().footprints[subresource];
        } else {
          loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          loc.SubresourceIndex = subresource;
        }
        return loc;
      };
      if (staging->isStaging() && (dst->isStaging() ? dstSub : srcSub) >= staging->getStagingLayout().footprints.size())
        return;
      const D3D12_TEXTURE_COPY_LOCATION dstLoc = location(dst, dstSub);
      const D3D12_TEXTURE_COPY_LOCATION srcLoc = location(src, srcSub);
      if (plane) {
        m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        continue;
      }
      D3D12_BOX box = {};
      box.left   = region.x;
      box.top    = region.y;
      box.front  = region.z;
      box.right  = region.x + (std::max)(region.width, 1u);
      box.bottom = region.y + (std::max)(region.height, 1u);
      box.back   = region.z + (std::max)(region.depth, 1u);
      alignCopyBox(src->getFormat(), box);
      m_cmdList->CopyTextureRegion(&dstLoc, dstX, dstY, dstZ, &srcLoc, &box);
    }
#endif
  }

  bool D3D12BackendContext::waitForGpuUse(BackendGpuUse& use, bool doNotWait) {
    HitchScope hitch("backend waitForGpuUse");
#ifdef _WIN32
    ContextLock ctxLock(*this);
    const uint64_t value = use.fenceValue.load(std::memory_order_acquire);
    if (!value || !m_fence || m_deviceRemoved || m_fence->GetCompletedValue() >= value)
      return true;
    // Still in the command list being recorded.
    if (value > m_lastSignaledFenceValue)
      flush();
    if (m_deviceRemoved || m_fence->GetCompletedValue() >= value)
      return true;
    if (doNotWait)
      return false;
    if (SUCCEEDED(m_fence->SetEventOnCompletion(value, m_fenceEvent)))
      WaitForSingleObject(m_fenceEvent, INFINITE);
    return true;
#else
    (void)use;
    (void)doNotWait;
    return true;
#endif
  }

  bool D3D12BackendContext::renameStagingImage(IBackendImage* image) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(image);
    if (!d3d12Image || !d3d12Image->isStaging() || !m_fence || m_deviceRemoved)
      return false;
    const uint64_t completed = m_fence->GetCompletedValue();
    const uint64_t used = image->gpuUse.fenceValue.load(std::memory_order_acquire);
    if (used <= completed || image->gpuUse.writeFenceValue.load(std::memory_order_acquire) > completed)
      return false;
    return d3d12Image->renameStaging(used, completed);
#else
    (void)image;
    return false;
#endif
  }

  void D3D12BackendContext::copyBufferToImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                                               IBackendBuffer* src, uint64_t srcOffset,
                                               const ImageCopyRegion* region) { 
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onCopy();
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopyBufferToImageCalls++;
    if (m_deviceRemoved) return;
    auto* d3d12Dst = static_cast<D3D12BackendImage*>(dst);
    auto* d3d12Src = static_cast<D3D12BackendBuffer*>(src);
    markGpuUse(stagingGpuUse(dst));
    markGpuRead(stagingGpuUse(src));
    const auto& dstImageDesc = d3d12Dst->getDesc();
    ImageCopyRegion copyRegion = region
      ? *region
      : getFullSubresourceRegion(d3d12Dst, dstSubres);
    static uint32_t copyBufferToImageCallId = 0;
    uint32_t callId = ++copyBufferToImageCallId;

    if (callId <= 5 || (callId % 1000) == 0) {
      logMessage(LogLevel::Info,
        "copyBufferToImage: id=%u dst=%p sub=%u fmt=%u %ux%u src=%p region=%u,%u %ux%u",
        callId, d3d12Dst,
        d3d12Dst->calcSubresource(dstSubres.mipLevel, dstSubres.arrayLayer),
        static_cast<uint32_t>(dstImageDesc.format),
        dstImageDesc.width, dstImageDesc.height, d3d12Src,
        copyRegion.x, copyRegion.y, copyRegion.width, copyRegion.height);
    }

    // Validate copy region bounds
    if (region) {
      uint32_t mipWidth  = getMipExtent(dstImageDesc.width, dstSubres.mipLevel);
      uint32_t mipHeight = getMipExtent(dstImageDesc.height, dstSubres.mipLevel);
      if (copyRegion.x + copyRegion.width > mipWidth ||
          copyRegion.y + copyRegion.height > mipHeight) {
        if (copyRegion.x >= mipWidth || copyRegion.y >= mipHeight)
          return;
        if (copyRegion.x + copyRegion.width > mipWidth)
          copyRegion.width = mipWidth - copyRegion.x;
        if (copyRegion.y + copyRegion.height > mipHeight)
          copyRegion.height = mipHeight - copyRegion.y;
      }
    }

    // Validate resource pointers
    auto* dstRes = d3d12Dst->getResource();
    auto* srcRes = d3d12Src->getResource();
    if (!dstRes || !srcRes) {
      logMessage(LogLevel::Error, "copyBufferToImage %u: null resource ptr dst=%p src=%p",
        callId, dstRes, srcRes);
      return;
    }

    const auto restoreSource = restoreAfterCopy(d3d12Src, false);
    transitionResource(d3d12Dst, dstSubres, D3D12_RESOURCE_STATE_COPY_DEST);
    transitionResource(d3d12Src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    flushBarriers();

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = dstRes;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = calcImageSubresourceIndex(d3d12Dst, dstSubres);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = srcRes;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    D3D12_RESOURCE_DESC footprintDesc = region
      ? getCopyableRegionDesc(d3d12Dst, copyRegion)
      : d3d12Dst->getTextureDesc();
    UINT footprintSubresource = region ? 0u : calcImageSubresourceIndex(d3d12Dst, dstSubres);
    UINT subresourceIdx = calcImageSubresourceIndex(d3d12Dst, dstSubres);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    m_device->getDevice()->GetCopyableFootprints(
      &footprintDesc, footprintSubresource, 1, d3d12Src->getOffset() + srcOffset,
      &footprint, &numRows, &rowSizeBytes, nullptr);

    D3D12_BOX srcBox = {};
    const D3D12_BOX* srcBoxPtr = nullptr;
    if (region) {
      srcBox.left   = 0;
      srcBox.top    = 0;
      srcBox.front  = 0;
      srcBox.right  = (std::max)(copyRegion.width, 1u);
      srcBox.bottom = (std::max)(copyRegion.height, 1u);
      srcBox.back   = (std::max)(copyRegion.depth, 1u);
      alignCopyBox(dstImageDesc.format, srcBox);
      srcBoxPtr = &srcBox;
    }
    srcLoc.PlacedFootprint = footprint;

    DXUP_RECENT_OP(
      "CopyBufferToImage id=%u dstRes=%p sub=%u fmt=%u mip=%u array=%u src=%p offset=%llu rowPitch=%u region=%u,%u,%u %ux%ux%u",
      callId, dstRes, subresourceIdx,
      static_cast<uint32_t>(dstImageDesc.format),
      dstSubres.mipLevel, dstSubres.arrayLayer, d3d12Src,
      static_cast<unsigned long long>(footprint.Offset), footprint.Footprint.RowPitch,
      copyRegion.x, copyRegion.y, copyRegion.z,
      copyRegion.width, copyRegion.height, copyRegion.depth);

    if (d3d12Dst->isStaging()) {
      // Buffer to buffer: the rows of the region.
      const auto& layout = d3d12Dst->getStagingLayout();
      if (subresourceIdx >= layout.footprints.size())
        return;
      uint32_t blockWidth = 1, blockHeight = 1;
      getCopyBlockExtent(dstImageDesc.format, blockWidth, blockHeight);
      const auto& dstFootprint = layout.footprints[subresourceIdx];
      const uint32_t blocksWide = (dstFootprint.Footprint.Width + blockWidth - 1) / blockWidth;
      const uint64_t blockBytes = blocksWide ? layout.rowSizes[subresourceIdx] / blocksWide : 0;
      copyFootprintRows(dstRes, dstFootprint, layout.numRows[subresourceIdx],
        uint64_t(copyRegion.x / blockWidth) * blockBytes, copyRegion.y / blockHeight, copyRegion.z,
        srcRes, footprint, numRows, 0, 0, 0,
        rowSizeBytes, numRows, (std::max)(footprint.Footprint.Depth, 1u));
      return;
    }

    bool copyOk = safeCopyTextureRegion(
      &m_cmdList, &dstLoc, copyRegion.x, copyRegion.y, copyRegion.z,
      &srcLoc, srcBoxPtr, d3d12Src, &copyRegion, subresourceIdx, numRows, rowSizeBytes);

    if (!copyOk) {
      logMessage(LogLevel::Error, "copyBufferToImage FAILED: id=%u sub=%u fmt=%u %ux%u",
        callId, subresourceIdx, static_cast<uint32_t>(dstImageDesc.format),
        footprint.Footprint.Width, footprint.Footprint.Height);
    }

    // Planar video images: the chroma rows follow the luma rows at the same
    // pitch (getImageUploadLayout).
    if (copyOk && !region && isPlanarVideoFormat(dstImageDesc.format) && d3d12Dst->getPlaneCount() == 2) {
      const UINT chromaIdx = subresourceIdx + d3d12Dst->getMipLevelCount() * d3d12Dst->getArraySubresourceCount();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT chroma = {};
      UINT chromaRows = 0;
      const D3D12_RESOURCE_DESC dstDesc = d3d12Dst->getTextureDesc();
      m_device->getDevice()->GetCopyableFootprints(&dstDesc, chromaIdx, 1,
        footprint.Offset + uint64_t(footprint.Footprint.RowPitch) * numRows, &chroma, &chromaRows, nullptr, nullptr);
      if (chromaRows && chroma.Footprint.RowPitch == footprint.Footprint.RowPitch
       && !(chroma.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)
       && chroma.Offset + uint64_t(chroma.Footprint.RowPitch) * chromaRows <= d3d12Src->getOffset() + d3d12Src->getDesc().size) {
        D3D12_TEXTURE_COPY_LOCATION chromaDst = dstLoc;
        chromaDst.SubresourceIndex = chromaIdx;
        D3D12_TEXTURE_COPY_LOCATION chromaSrc = srcLoc;
        chromaSrc.PlacedFootprint = chroma;
        m_cmdList->CopyTextureRegion(&chromaDst, 0, 0, 0, &chromaSrc, nullptr);
      }
    }

    m_copyOpsSinceFlush++;
#endif
  }

  void D3D12BackendContext::copyImageToBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                                               IBackendImage* src, const ImageSubresource& srcSubres) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onCopy();
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopyImageToBufferCalls++;
    auto* d3d12Dst = static_cast<D3D12BackendBuffer*>(dst);
    auto* d3d12Src = static_cast<D3D12BackendImage*>(src);
    markGpuUse(stagingGpuUse(dst));
    markGpuRead(stagingGpuUse(src));
    prepareBufferGpuWrite(d3d12Dst);

    const auto restoreDestination = restoreAfterCopy(d3d12Dst, true);
    transitionResource(d3d12Dst, D3D12_RESOURCE_STATE_COPY_DEST);
    transitionResource(d3d12Src, srcSubres, D3D12_RESOURCE_STATE_COPY_SOURCE);
    flushBarriers();

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = d3d12Dst->getResource();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    // Use GetCopyableFootprints to get the correct layout
    D3D12_RESOURCE_DESC srcDesc = d3d12Src->getTextureDesc();
    UINT subresourceIdx = d3d12Src->calcSubresource(srcSubres.mipLevel, srcSubres.arrayLayer);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    // Suballocated buffers start at their offset in the resource.
    m_device->getDevice()->GetCopyableFootprints(
      &srcDesc, subresourceIdx, 1, d3d12Dst->getOffset() + dstOffset,
      &footprint, &numRows, &rowSizeBytes, nullptr);
    dstLoc.PlacedFootprint = footprint;

    if (d3d12Src->isStaging()) {
      const auto& layout = d3d12Src->getStagingLayout();
      if (subresourceIdx >= layout.footprints.size())
        return;
      copyFootprintRows(d3d12Dst->getResource(), footprint, numRows, 0, 0, 0,
        d3d12Src->getResource(), layout.footprints[subresourceIdx], layout.numRows[subresourceIdx], 0, 0, 0,
        rowSizeBytes, numRows, (std::max)(footprint.Footprint.Depth, 1u));
      return;
    }

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = d3d12Src->getResource();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = d3d12Src->calcSubresource(srcSubres.mipLevel, srcSubres.arrayLayer);

    m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
#endif
  }

  // ============================================================
  // Resource Mapping
  // ============================================================

  void* D3D12BackendContext::mapBuffer(IBackendBuffer* buffer, MapMode mode) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_deviceRemoved) return nullptr;
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    // Behind a CS thread this runs on the application thread too, beside
    // the recording one: no counters of the recording state then.
    if (perfStatsEnabled() && !m_copyOnly && !m_externallySynchronized) {
      if (mode == MapMode::Read || mode == MapMode::ReadWrite)
        m_perfIntervalMapReadCalls++;
      if (mode == MapMode::Write || mode == MapMode::WriteDiscard ||
          mode == MapMode::WriteNoOverwrite || mode == MapMode::ReadWrite)
        m_perfIntervalMapWriteCalls++;
    }

    auto* resource = d3d12Buffer->getResource();
    if (!resource) {
      logMessage(LogLevel::Error, "mapBuffer: null D3D12 resource buf=%p mode=%u size=%llu",
        buffer, static_cast<uint32_t>(mode), buffer->getDesc().size);
      return nullptr;
    }

    // Staged WriteDiscard: write to a fresh upload buffer, copy on unmap.
    // This prevents CPU writes to buffers the GPU may still be reading
    // (required for safe ring allocator overlap).
    if (mode == MapMode::WriteDiscard && !m_copyOnly &&
        d3d12Buffer->getDesc().usage != ResourceUsage::Default &&
        d3d12Buffer->getDesc().usage != ResourceUsage::Immutable) {
      if (perfStatsEnabled())
        m_perfIntervalStagedMaps++;
      uint64_t bufSize = buffer->getDesc().size;
      BufferDesc stagingDesc = {};
      stagingDesc.size     = bufSize;
      stagingDesc.usage    = ResourceUsage::Dynamic;
      stagingDesc.cpuAccess = CpuAccess::Write;
      auto staging = m_device->createBuffer(stagingDesc);
      if (!staging) return nullptr;

      auto* stagingBuffer = static_cast<D3D12BackendBuffer*>(staging.get());
      auto* stagingRes = stagingBuffer->getResource();
      void* ptr = stagingBuffer->getPersistentMappedData();
      bool requiresUnmap = false;
      if (!ptr) {
        D3D12_RANGE readRange = {0, 0};
        HRESULT hr = stagingRes->Map(0, &readRange, &ptr);
        if (FAILED(hr)) return nullptr;
        requiresUnmap = true;
      }

      m_stagedMap.target    = buffer;
      m_stagedMap.staging   = std::move(staging);
      m_stagedMap.mappedPtr = ptr;
      m_stagedMap.size      = bufSize;
      m_stagedMap.requiresUnmap = requiresUnmap;
      return ptr;
    }

    if (mode == MapMode::Write || mode == MapMode::WriteDiscard || mode == MapMode::WriteNoOverwrite) {
      // The newest slice: this may run on the application thread ahead of
      // the recording one.
      if (void* persistentData = d3d12Buffer->getAppMappedData())
        return persistentData;
    }

    D3D12_RANGE readRange = {};
    if (mode == MapMode::Write || mode == MapMode::WriteDiscard || mode == MapMode::WriteNoOverwrite) {
      readRange.Begin = 0;
      readRange.End   = 0;  // No CPU read needed
    } else {
      readRange.Begin = 0;
      readRange.End   = static_cast<SIZE_T>(buffer->getDesc().size);
    }

    void* mappedData = nullptr;
    HRESULT hr = resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "mapBuffer: Map failed hr=0x%08x buf=%p mode=%u size=%llu",
        hr, buffer, static_cast<uint32_t>(mode), buffer->getDesc().size);
      return nullptr;
    }

    return mappedData;
#else
    return nullptr;
#endif
  }

  void D3D12BackendContext::copyStructureCount(IBackendBuffer* dst, uint64_t dstOffset, IBackendBufferView* view) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!dst || !view) return;
    IBackendBuffer* counterBuf = view->getCounterBuffer();
    if (!counterBuf) {
      logMessage(LogLevel::Warn, "copyStructureCount: view has no counter buffer");
      return;
    }

    copyBuffer(dst, dstOffset, counterBuf, 0, sizeof(uint32_t));
#endif
  }

  void D3D12BackendContext::setUnorderedAccessCounter(IBackendBufferView* view, uint32_t value) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!view) return;
    IBackendBuffer* counterBuf = view->getCounterBuffer();
    if (!counterBuf) return;

    auto* d3d12Counter = static_cast<D3D12BackendBuffer*>(counterBuf);

    // Write the initial counter value via upload buffer + copy
    updateBuffer(counterBuf, 0, sizeof(uint32_t), &value);

    // Transition counter buffer to UNORDERED_ACCESS for upcoming dispatch
    transitionResource(d3d12Counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    holdBatchResource(d3d12Counter->getResource());
#endif
  }

  void D3D12BackendContext::unmapBuffer(IBackendBuffer* buffer) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);

    // If this was a staged WriteDiscard, copy from staging to target
    if (m_stagedMap.target == buffer && m_stagedMap.staging) {
      auto* stagingRes = static_cast<D3D12BackendBuffer*>(m_stagedMap.staging.get())->getResource();
      if (m_stagedMap.requiresUnmap) {
        D3D12_RANGE writtenRange = { 0, static_cast<SIZE_T>(m_stagedMap.size) };
        stagingRes->Unmap(0, &writtenRange);
      }

      // Copy staging → target via GPU
      copyBuffer(buffer, 0, m_stagedMap.staging.get(), 0, m_stagedMap.size);
      retainTemporaryBuffer(std::move(m_stagedMap.staging));

      m_stagedMap.target    = nullptr;
      m_stagedMap.mappedPtr = nullptr;
      m_stagedMap.size      = 0;
      m_stagedMap.requiresUnmap = false;
      return;
    }

    if (d3d12Buffer->getPersistentMappedData())
      return;

    const auto& desc = buffer->getDesc();
    const uint32_t cpuAccess = static_cast<uint32_t>(desc.cpuAccess);
    const bool isReadback = desc.usage == ResourceUsage::Staging
      && (cpuAccess & static_cast<uint32_t>(CpuAccess::Read)) != 0;
    D3D12_RANGE writtenRange = {
      0,
      isReadback ? 0 : static_cast<SIZE_T>(desc.size)
    };
    d3d12Buffer->getResource()->Unmap(0, &writtenRange);
#endif
  }

  bool D3D12BackendContext::uploadImageSubresource(IBackendImage* dst, const ImageSubresource& dstSubres,
                                                   const void* data, uint32_t rowPitch, uint32_t depthPitch) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* image = static_cast<D3D12BackendImage*>(dst);
    if (m_deviceRemoved || !image || !data || image->isStaging() || !image->getResource())
      return false;
    // Planar video data: the planes' rows follow each other.
    const UINT planes = isPlanarVideoFormat(image->getDesc().format) ? image->getPlaneCount() : 1u;
    if (image->getPlaneCount() > planes || planes > 2)
      return false;

    const D3D12_RESOURCE_DESC desc = image->getTextureDesc();
    UINT subresources[2] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT numRows[2] = {};
    UINT64 rowSizes[2] = {}, totalSizes[2] = {};
    for (UINT plane = 0; plane < planes; plane++) {
      subresources[plane] = image->calcSubresource(dstSubres.mipLevel, dstSubres.arrayLayer, plane);
      m_device->getDevice()->GetCopyableFootprints(&desc, subresources[plane], 1, 0, &footprints[plane],
        &numRows[plane], &rowSizes[plane], &totalSizes[plane]);
      if (!totalSizes[plane] || totalSizes[plane] == UINT64_MAX || rowSizes[plane] > rowPitch)
        return false;
    }

    ID3D12Resource* uploads[2] = {};
    uint64_t uploadOffsets[2] = {};
    const auto* srcBytes = static_cast<const uint8_t*>(data);
    for (UINT plane = 0; plane < planes; plane++) {
      void* uploadPtr = nullptr;
      if (!allocateUploadScratch(totalSizes[plane], D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &uploads[plane],
                                 &uploadOffsets[plane], &uploadPtr))
        return false;
      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[plane];
      const uint32_t depth = footprint.Footprint.Depth;
      const uint64_t sourceSlicePitch = depthPitch ? depthPitch : uint64_t(rowPitch) * numRows[plane];
      auto* dstBytes = static_cast<uint8_t*>(uploadPtr);
      for (uint32_t z = 0; z < depth; z++) {
        for (UINT row = 0; row < numRows[plane]; row++) {
          std::memcpy(dstBytes + (uint64_t(z) * numRows[plane] + row) * footprint.Footprint.RowPitch,
                      srcBytes + z * sourceSlicePitch + uint64_t(row) * rowPitch, size_t(rowSizes[plane]));
        }
      }
      srcBytes += uint64_t(rowPitch) * numRows[plane];
    }

    // New images get all their data: one barrier for the whole image (and
    // for all planes).
    if (image->hasUniformState() || planes > 1)
      transitionResource(image, D3D12_RESOURCE_STATE_COPY_DEST);
    else
      transitionResource(image, dstSubres, D3D12_RESOURCE_STATE_COPY_DEST);
    holdBatchResource(image->getResource());
    flushBarriers();
    for (UINT plane = 0; plane < planes; plane++) {
      D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
      dstLoc.pResource = image->getResource();
      dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dstLoc.SubresourceIndex = subresources[plane];
      D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
      srcLoc.pResource = uploads[plane];
      srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      srcLoc.PlacedFootprint = footprints[plane];
      srcLoc.PlacedFootprint.Offset = uploadOffsets[plane];
      m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
      m_copyOpsSinceFlush++;
    }
    return true;
#else
    (void)dst; (void)dstSubres; (void)data; (void)rowPitch; (void)depthPitch;
    return false;
#endif
  }

  void* D3D12BackendContext::discardBuffer(IBackendBuffer* buffer) {
#ifdef _WIN32
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (!d3d12Buffer || !d3d12Buffer->isSuballocated() || d3d12Buffer->pinnedContents || m_copyOnly)
      return nullptr;
    const BufferDesc& desc = d3d12Buffer->getDesc();
    D3D12UploadSuballocator::Slice slice;
    if (!d3d12Buffer->getSuballocator()->allocate(m_device->getDevice(), desc.size, slice))
      return nullptr;
    D3D12UploadSuballocator::prepareSlice(slice, desc.dataSize ? desc.dataSize : desc.size,
      (desc.bindFlags & BindFlags::ConstantBuffer) != BindFlags::None);
    d3d12Buffer->setAppMappedData(slice.mapped);
    setBufferSlice(d3d12Buffer, slice);
    return slice.mapped;
#else
    (void)buffer;
    return nullptr;
#endif
  }

  void D3D12BackendContext::setBufferSlice(D3D12BackendBuffer* buffer, const D3D12UploadSuballocator::Slice& slice,
                                           D3D12UploadSuballocator* owner) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    m_pendingSlices.push_back(buffer->replaceSlice(slice, owner));
    if ((buffer->getDesc().bindFlags & BindFlags::ConstantBuffer) != BindFlags::None)
      rebindConstantBuffer(buffer);
#else
    (void)buffer;
    (void)slice;
#endif
  }

  void D3D12BackendContext::moveToDefaultSlice(D3D12BackendBuffer* buffer) {
#ifdef _WIN32
    const BufferDesc& desc = buffer->getDesc();
    auto& allocator = m_device->getDefaultSuballocator();
    D3D12UploadSuballocator::Slice slice;
    if (!allocator.allocate(m_device->getDevice(), desc.size, slice)) {
      logMessage(LogLevel::Error, "moveToDefaultSlice: no default slice for %llu bytes",
        static_cast<unsigned long long>(desc.size));
      return;
    }
    ID3D12Resource* source = buffer->getResource();
    const uint64_t sourceOffset = buffer->getOffset();
    m_pendingSlices.push_back(buffer->replaceSlice(slice, &allocator));
    transitionResource(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
    flushBarriers();
    const bool paused = pausePredication();
    m_cmdList->CopyBufferRegion(buffer->getResource(), buffer->getOffset(), source, sourceOffset, desc.size);
    resumePredication(paused);
    restoreBoundBufferState(buffer);
    rebindConstantBuffer(buffer);
#else
    (void)buffer;
#endif
  }

  void D3D12BackendContext::rebindConstantBuffer(D3D12BackendBuffer* buffer) {
#ifdef _WIN32
    for (uint32_t stage = 0; stage < kShaderStages; stage++) {
      for (uint32_t slot = 0; slot < m_cbvBindingEnd[stage]; slot++) {
        const CbvSlot& binding = m_cbvBindings[stage][slot];
        if (binding.buffer != buffer)
          continue;
        if (binding.firstConstant || binding.numConstants)
          bindConstantBufferRange(static_cast<ShaderStage>(stage), slot, buffer,
            binding.firstConstant, binding.numConstants);
        else
          bindConstantBuffer(static_cast<ShaderStage>(stage), slot, buffer);
      }
    }
#else
    (void)buffer;
#endif
  }

  void D3D12BackendContext::retireAfterGpu(std::vector<std::shared_ptr<void>>& objects) {
    ContextLock ctxLock(*this);
    if (m_pendingRetired.empty()) {
      m_pendingRetired.swap(objects);
    } else {
      for (auto& object : objects)
        m_pendingRetired.push_back(std::move(object));
    }
    objects.clear();
  }

  void D3D12BackendContext::retainTemporaryBuffer(Rc<IBackendBuffer> buffer) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (buffer) {
      m_pendingUploadBytes += buffer->getDesc().size;
      m_pendingUploads.push_back(std::move(buffer));

      // WRITE_DISCARD renaming must not be allowed to accumulate an entire
      // multi-gigabyte command list when an application records many updates
      // without presenting. Keep the normal small-update path batched, but
      // submit large upload batches before they exhaust system/GPU memory.
      if (!m_copyOnly
       && !m_closedForSubmission
       && m_pendingUploadBytes >= kPendingUploadFlushThreshold) {
        flush();
      }
    }
#else
    (void)buffer;
#endif
  }

#ifdef _WIN32
  void D3D12BackendContext::releaseCompletedUploads() {
#ifdef _WIN32
    const bool collectTiming = perfTimingEnabled() && !m_copyOnly;
    LARGE_INTEGER timingStart = {}, timingEnd = {}, perfFreq = {};
    if (collectTiming) {
      QueryPerformanceFrequency(&perfFreq);
      QueryPerformanceCounter(&timingStart);
    }

    const bool hasInFlightObjects = !m_inFlightUploads.empty()
      || !m_inFlightResourceBatches.empty()
      || !m_inFlightRetired.empty()
      || !m_inFlightSlices.empty();
    const uint64_t completed = hasInFlightObjects ? m_fence->GetCompletedValue() : 0;

    if (!m_inFlightSlices.empty()) {
      size_t done = 0;
      while (done < m_inFlightSlices.size() && m_inFlightSlices[done].fenceValue <= completed) {
        m_device->freeSlices(m_inFlightSlices[done].slices);
        done++;
      }
      if (done)
        m_inFlightSlices.erase(m_inFlightSlices.begin(), m_inFlightSlices.begin() + done);
    }

    if (!m_inFlightRetired.empty()) {
      size_t done = 0;
      while (done < m_inFlightRetired.size() && m_inFlightRetired[done].fenceValue <= completed)
        done++;
      if (done)
        m_inFlightRetired.erase(m_inFlightRetired.begin(), m_inFlightRetired.begin() + done);
    }

    if (!m_inFlightUploads.empty()) {
      m_inFlightUploads.erase(
        std::remove_if(m_inFlightUploads.begin(), m_inFlightUploads.end(),
          [this, completed](FencedUpload& upload) {
            if (completed < upload.fenceValue)
              return false;
            if (perfStatsEnabled() && !m_copyOnly)
              m_perfIntervalCompletedUploads++;
            if (m_device->recycleTemporaryBuffer(std::move(upload.buffer)) && perfStatsEnabled() && !m_copyOnly)
              m_perfIntervalRecycledUploads++;
            return true;
          }),
        m_inFlightUploads.end());
    }

    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalUploadReleaseMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
      QueryPerformanceCounter(&timingStart);
    }
    releaseCompletedResourceBatches(completed);
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalResourceBatchReleaseMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }
#endif
  }

  void D3D12BackendContext::holdBatchResource(ID3D12Resource* res) {
#ifdef _WIN32
    // With a CS thread every backend object is retired only after the GPU
    // finished with it (retireAfterGpu).
    if (!res || m_externallySynchronized)
      return;

    if (m_batchResourceRefTableOccupancy < (kBatchResourceRefTableSize * 3u) / 4u) {
      uintptr_t hash = reinterpret_cast<uintptr_t>(res) >> 4;
      hash ^= hash >> 17;
      hash *= 0xed5ad4bbU;
      hash ^= hash >> 11;

      uint32_t index = static_cast<uint32_t>(hash) & (kBatchResourceRefTableSize - 1u);
      for (uint32_t probe = 0; probe < kBatchResourceRefTableSize; probe++) {
        auto& slot = m_batchResourceRefTable[index];
        if (slot == res)
          return;
        if (!slot) {
          slot = res;
          m_batchResourceRefOccupiedSlots.push_back(index);
          m_batchResourceRefTableOccupancy++;
          break;
        }
        index = (index + 1u) & (kBatchResourceRefTableSize - 1u);
      }
    }

    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalBatchResourceRefs++;
    res->AddRef();
    m_batchResourceRefs.push_back(res);
#endif
  }

  void D3D12BackendContext::clearBatchResourceRefTable() {
#ifdef _WIN32
    if (m_batchResourceRefTableOccupancy) {
      for (uint32_t index : m_batchResourceRefOccupiedSlots)
        m_batchResourceRefTable[index] = nullptr;
      m_batchResourceRefOccupiedSlots.clear();
      m_batchResourceRefTableOccupancy = 0;
    }
#endif
  }

  void D3D12BackendContext::holdBatchResourceOnce(ID3D12Resource* res,
                                                   ID3D12Resource*& heldResource,
                                                   uint64_t& heldGeneration) {
#ifdef _WIN32
    if (!res)
      return;

    if (heldGeneration == m_resourceBatchGeneration && heldResource == res)
      return;

    holdBatchResource(res);
    heldResource = res;
    heldGeneration = m_resourceBatchGeneration;
#else
    (void)res;
    (void)heldResource;
    (void)heldGeneration;
#endif
  }

  void D3D12BackendContext::snapshotBoundResources() {
#ifdef _WIN32
    // resetCommandList() snapshots persistent bindings immediately after it
    // advances the batch generation. Bind calls retain any resources added
    // later in that same generation, so the pre-flush safety snapshot can
    // avoid rescanning every binding table when it is already current.
    if (m_boundResourceSnapshotGeneration == m_resourceBatchGeneration)
      return;

    // Snapshot all currently bound resources to keep them alive until the
    // GPU finishes this command list.  This catches resources that were
    // bound before the current batch started recording (persistent bindings).
    for (auto& vertexBuffer : m_trackedVBs) {
      if (vertexBuffer.bound && vertexBuffer.resourceHeld)
        holdBatchResourceOnce(
          vertexBuffer.resourceHeld,
          vertexBuffer.resourceHeld,
          vertexBuffer.resourceHeldGeneration);
    }
    if (m_trackedIBBound && m_trackedIBResourceHeld)
      holdBatchResourceOnce(
        m_trackedIBResourceHeld,
        m_trackedIBResourceHeld,
        m_trackedIBResourceHeldGeneration);
    for (uint32_t slot = 0; slot < m_soBufferCount; slot++) {
      if (m_soResourceHeld[slot])
        holdBatchResourceOnce(
          m_soResourceHeld[slot],
          m_soResourceHeld[slot],
          m_soResourceHeldGeneration[slot]);
    }

    for (uint32_t stage = 0; stage < kShaderStages; stage++) {
      for (uint32_t slot = 0; slot < m_cbvBindingEnd[stage]; slot++) {
        auto& binding = m_cbvBindings[stage][slot];
        if (binding.bound && binding.resourceHeld)
          holdBatchResourceOnce(binding.resourceHeld, binding.resourceHeld, binding.resourceHeldGeneration);
      }

      for (uint32_t slot = 0; slot < m_srvBindingEnd[stage]; slot++) {
        auto& binding = m_srvBindings[stage][slot];
        if (binding.bound && binding.resourceHeld)
          holdBatchResourceOnce(binding.resourceHeld, binding.resourceHeld, binding.resourceHeldGeneration);
      }
      for (uint32_t slot = 0; slot < m_uavBindingEnd[stage]; slot++) {
        auto& binding = m_uavBindings[stage][slot];
        if (binding.bound && binding.resourceHeld)
          holdBatchResourceOnce(binding.resourceHeld, binding.resourceHeld, binding.resourceHeldGeneration);
        if (binding.bound && binding.counterResourceHeld)
          holdBatchResourceOnce(binding.counterResourceHeld, binding.counterResourceHeld, binding.counterResourceHeldGeneration);
      }
    }
    for (uint32_t i = 0; i < m_currentRenderTargetCount; i++) {
      if (m_currentRenderTargetResources[i])
        holdBatchResourceOnce(
          m_currentRenderTargetResources[i],
          m_trackedRTVResourceHeld[i],
          m_trackedRTVResourceHeldGeneration[i]);
    }
    if (m_currentDepthStencilResource)
      holdBatchResourceOnce(
        m_currentDepthStencilResource,
        m_trackedDSVResourceHeld,
        m_trackedDSVResourceHeldGeneration);
    m_boundResourceSnapshotGeneration = m_resourceBatchGeneration;
#endif
  }

  void D3D12BackendContext::releaseCompletedResourceBatches(uint64_t completedFenceValue) {
#ifdef _WIN32
    if (m_inFlightResourceBatches.empty()) return;
    auto it = std::remove_if(m_inFlightResourceBatches.begin(), m_inFlightResourceBatches.end(),
      [this, completedFenceValue](FencedResourceBatch& batch) {
        if (completedFenceValue >= batch.fenceValue) {
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalReleasedResourceRefs += static_cast<uint64_t>(batch.resources.size());
          for (auto* res : batch.resources) res->Release();
          return true;
        }
        return false;
      });
    m_inFlightResourceBatches.erase(it, m_inFlightResourceBatches.end());
#endif
  }

  void D3D12BackendContext::releaseAllResourceBatches() {
#ifdef _WIN32
    for (auto& batch : m_inFlightResourceBatches)
      for (auto* res : batch.resources) res->Release();
    m_inFlightResourceBatches.clear();
    for (auto* res : m_batchResourceRefs) res->Release();
    m_batchResourceRefs.clear();
    clearBatchResourceRefTable();
#endif
  }

  bool D3D12BackendContext::ensureUploadScratchCapacity(uint64_t capacity) {
#ifdef _WIN32
    auto& scratchBuffer = m_uploadScratchBuffers[m_currentAllocatorIndex];
    auto& scratchMapped = m_uploadScratchMapped[m_currentAllocatorIndex];
    auto& scratchCapacity = m_uploadScratchCapacities[m_currentAllocatorIndex];
    auto& scratchOffset = m_uploadScratchOffsets[m_currentAllocatorIndex];

    if (scratchBuffer && scratchCapacity >= capacity)
      return true;

    uint64_t newCapacity = kDefaultUploadScratchCapacity;
    while (newCapacity < capacity)
      newCapacity *= 2;

    if (scratchBuffer && scratchMapped) {
      D3D12_RANGE writtenRange = { 0, static_cast<SIZE_T>(scratchOffset) };
      scratchBuffer->Unmap(0, &writtenRange);
    }
    scratchBuffer.Reset();
    scratchMapped = nullptr;
    scratchCapacity = 0;
    scratchOffset = 0;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = newCapacity;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_device->getDevice()->CreateCommittedResource(
      &heapProps,
      D3D12_HEAP_FLAG_NONE,
      &bufferDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ,
      nullptr,
      IID_PPV_ARGS(&scratchBuffer));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "ensureUploadScratchCapacity: CreateCommittedResource failed hr=0x%08x size=%llu",
        hr, static_cast<unsigned long long>(newCapacity));
      return false;
    }

    D3D12_RANGE readRange = { 0, 0 };
    hr = scratchBuffer->Map(0, &readRange, &scratchMapped);
    if (FAILED(hr) || !scratchMapped) {
      logMessage(LogLevel::Error, "ensureUploadScratchCapacity: Map failed hr=0x%08x size=%llu",
        hr, static_cast<unsigned long long>(newCapacity));
      scratchBuffer.Reset();
      scratchMapped = nullptr;
      return false;
    }

    scratchCapacity = newCapacity;
    return true;
#else
    (void)capacity;
    return false;
#endif
  }

  bool D3D12BackendContext::allocateUploadScratch(uint64_t size, uint64_t alignment,
                                                   ID3D12Resource** resource, uint64_t* offset, void** ptr) {
#ifdef _WIN32
    if (!resource || !offset || !ptr || size == 0)
      return false;

    if (alignment == 0)
      alignment = 1;

    auto currentCapacity = [&]() -> uint64_t {
      return m_uploadScratchCapacities[m_currentAllocatorIndex];
    };

    if (m_uploadScratchBuffers[m_currentAllocatorIndex] && size > currentCapacity())
      flush();

    if (!m_uploadScratchBuffers[m_currentAllocatorIndex] || size > currentCapacity()) {
      if (!ensureUploadScratchCapacity(size))
        return false;
    }

    uint64_t alignedOffset = ((m_uploadScratchOffsets[m_currentAllocatorIndex] + alignment - 1) / alignment) * alignment;
    if (alignedOffset + size > currentCapacity()) {
      flush();
      if (!m_uploadScratchBuffers[m_currentAllocatorIndex] || size > currentCapacity()) {
        if (!ensureUploadScratchCapacity(size))
          return false;
      }
      alignedOffset = ((m_uploadScratchOffsets[m_currentAllocatorIndex] + alignment - 1) / alignment) * alignment;
      if (size > currentCapacity() && !ensureUploadScratchCapacity(size))
        return false;
    }

    auto& scratchBuffer = m_uploadScratchBuffers[m_currentAllocatorIndex];
    auto* scratchMapped = m_uploadScratchMapped[m_currentAllocatorIndex];
    if (!scratchBuffer || !scratchMapped || alignedOffset + size > currentCapacity())
      return false;

    *resource = scratchBuffer.Get();
    *offset = alignedOffset;
    *ptr = static_cast<uint8_t*>(scratchMapped) + alignedOffset;
    m_uploadScratchOffsets[m_currentAllocatorIndex] = alignedOffset + size;
    return true;
#else
    (void)size;
    (void)alignment;
    (void)resource;
    (void)offset;
    (void)ptr;
    return false;
#endif
  }

  void D3D12BackendContext::markDescriptorStageDirty(uint32_t stage) {
#ifdef _WIN32
    if (stage >= kShaderStages)
      return;

    m_cbvDirty = true;
    m_srvDirty = true;
    m_uavDirty = true;
    m_samplerDirty = true;
    m_cbvStageDirty[stage] = true;
    m_srvStageDirty[stage] = true;
    m_uavStageDirty[stage] = true;
    m_samplerStageDirty[stage] = true;
    m_descriptorDirtyStageMask |= 1u << stage;
#else
    (void)stage;
#endif
  }

  void D3D12BackendContext::markAllDescriptorStagesDirty() {
#ifdef _WIN32
    m_cbvDirty = true;
    m_srvDirty = true;
    m_uavDirty = true;
    m_samplerDirty = true;
    m_cbvStageDirty.fill(true);
    m_srvStageDirty.fill(true);
    m_uavStageDirty.fill(true);
    m_samplerStageDirty.fill(true);
    m_descriptorDirtyStageMask = (1u << kShaderStages) - 1u;
#endif
  }

  void D3D12BackendContext::perfRecordFlush(uint32_t drawOps, uint32_t copyOps, uint32_t cbvSrvUavOff, uint32_t samplerOff,
                                            double execMs, double totalMs) {
#ifdef _WIN32
    if (!perfStatsEnabled() || m_copyOnly)
      return;

    m_perfIntervalFlushes++;
    if (drawOps || copyOps)
      m_perfIntervalWorkFlushes++;
    m_perfIntervalDrawOps += drawOps;
    m_perfIntervalCopyOps += copyOps;
    m_perfIntervalFlushExecMs += execMs;
    m_perfIntervalFlushTotalMs += totalMs;
    if (cbvSrvUavOff > m_perfIntervalMaxCbvSrvUavOff)
      m_perfIntervalMaxCbvSrvUavOff = cbvSrvUavOff;
    if (samplerOff > m_perfIntervalMaxSamplerOff)
      m_perfIntervalMaxSamplerOff = samplerOff;
#else
    (void)drawOps;
    (void)copyOps;
    (void)cbvSrvUavOff;
    (void)samplerOff;
    (void)execMs;
    (void)totalMs;
#endif
  }

  void D3D12BackendContext::perfRecordResetWait(double waitMs) {
#ifdef _WIN32
    if (!perfStatsEnabled() || m_copyOnly)
      return;

    m_perfIntervalResetWaitMs += waitMs;
#else
    (void)waitMs;
#endif
  }

  void D3D12BackendContext::perfRecordPresent(double presentWaitMs) {
#ifdef _WIN32
    if (!perfStatsEnabled() || m_copyOnly)
      return;

    LARGE_INTEGER now = {};
    if (m_perfCounterFrequency == 0) {
      LARGE_INTEGER freq = {};
      QueryPerformanceFrequency(&freq);
      m_perfCounterFrequency = freq.QuadPart;
    }
    QueryPerformanceCounter(&now);
    if (m_perfIntervalStartTicks == 0)
      m_perfIntervalStartTicks = now.QuadPart;

    m_perfIntervalPresents++;
    m_perfIntervalPresentWaitMs += presentWaitMs;

    constexpr uint64_t kPerfWindowPresents = 30;
    if (m_perfIntervalPresents < kPerfWindowPresents)
      return;

    double elapsedSec = m_perfCounterFrequency > 0
      ? static_cast<double>(now.QuadPart - m_perfIntervalStartTicks) / static_cast<double>(m_perfCounterFrequency)
      : 0.0;
    double fps = elapsedSec > 0.0
      ? static_cast<double>(m_perfIntervalPresents) / elapsedSec
      : 0.0;
    double presents = static_cast<double>(m_perfIntervalPresents);

    logMessage(LogLevel::Warn,
      "PERF presents=%llu sec=%.2f fps=%.2f avgFlush=%.2f avgWorkFlush=%.2f avgDrawOps=%.1f "
      "avgCopyOps=%.1f flushCpu=%.2fms execCpu=%.2fms resetWait=%.2fms presentWait=%.2fms "
      "timing(desc/cbv/srv/uav/samp/snap/bar/close/reset)=%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2fms "
      "flushParts(dev/release/upload/postDev/signal/move)=%.2f/%.2f/%.2f/%.2f/%.2f/%.2fms "
      "releaseSplit(uploads/refs)=%.2f/%.2fms uploads(done/recycled)=%llu/%llu relRefs=%llu "
      "updates=%llu updateMB=%.2f upd(cb/ia/srvuav/other)=%llu/%llu/%llu/%llu srvOnly/uav=%llu/%llu updPath(direct/imm/copy)=%llu/%llu/%llu mapsR=%llu mapsW=%llu staged=%llu copies(buf/img/reg/b2i/i2b)=%llu/%llu/%llu/%llu/%llu "
      "soRead=%llu descFlush=%llu refs=%llu pso(bind/skip)=%llu/%llu desc(root/copy/hit/cbv)=%llu/%llu/%llu/%llu barriers(call/trans)=%llu/%llu barrierKind(buf/img/toCopy/fromCopy/toSrv/toRT/toUav/toDepth)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu maxHeap=%u/%u inFlightUploads=%llu inFlightBatches=%llu",
      static_cast<unsigned long long>(m_perfIntervalPresents),
      elapsedSec,
      fps,
      static_cast<double>(m_perfIntervalFlushes) / presents,
      static_cast<double>(m_perfIntervalWorkFlushes) / presents,
      static_cast<double>(m_perfIntervalDrawOps) / presents,
      static_cast<double>(m_perfIntervalCopyOps) / presents,
      m_perfIntervalFlushTotalMs / presents,
      m_perfIntervalFlushExecMs / presents,
      m_perfIntervalResetWaitMs / presents,
      m_perfIntervalPresentWaitMs / presents,
      m_perfIntervalDescriptorCommitMs / presents,
      m_perfIntervalCommitCbvMs / presents,
      m_perfIntervalCommitSrvMs / presents,
      m_perfIntervalCommitUavMs / presents,
      m_perfIntervalCommitSamplerMs / presents,
      m_perfIntervalSnapshotMs / presents,
      m_perfIntervalFlushBarriersMs / presents,
      m_perfIntervalCloseExecuteMs / presents,
      m_perfIntervalResetCommandListMs / presents,
      m_perfIntervalDeviceCheckMs / presents,
      m_perfIntervalReleaseCompletedMs / presents,
      m_perfIntervalUploadBookkeepingMs / presents,
      m_perfIntervalPostDeviceCheckMs / presents,
      m_perfIntervalSignalMs / presents,
      m_perfIntervalBatchMoveMs / presents,
      m_perfIntervalUploadReleaseMs / presents,
      m_perfIntervalResourceBatchReleaseMs / presents,
      static_cast<unsigned long long>(m_perfIntervalCompletedUploads),
      static_cast<unsigned long long>(m_perfIntervalRecycledUploads),
      static_cast<unsigned long long>(m_perfIntervalReleasedResourceRefs),
      static_cast<unsigned long long>(m_perfIntervalUpdateBufferCalls),
      static_cast<double>(m_perfIntervalUpdateBytes) / (1024.0 * 1024.0),
      static_cast<unsigned long long>(m_perfIntervalUpdateConstantBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalUpdateIaBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalUpdateSrvUavBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalUpdateOtherBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalUpdateSrvOnlyBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalUpdateUavCapableBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalDirectBufferUpdateCalls),
      static_cast<unsigned long long>(m_perfIntervalImmediateBufferUpdateCalls),
      static_cast<unsigned long long>(m_perfIntervalCopiedBufferUpdateCalls),
      static_cast<unsigned long long>(m_perfIntervalMapReadCalls),
      static_cast<unsigned long long>(m_perfIntervalMapWriteCalls),
      static_cast<unsigned long long>(m_perfIntervalStagedMaps),
      static_cast<unsigned long long>(m_perfIntervalCopyBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalCopyImageCalls),
      static_cast<unsigned long long>(m_perfIntervalCopyImageRegionCalls),
      static_cast<unsigned long long>(m_perfIntervalCopyBufferToImageCalls),
      static_cast<unsigned long long>(m_perfIntervalCopyImageToBufferCalls),
      static_cast<unsigned long long>(m_perfIntervalStreamOutputReadbacks),
      static_cast<unsigned long long>(m_perfIntervalDescriptorFlushes),
      static_cast<unsigned long long>(m_perfIntervalBatchResourceRefs),
      static_cast<unsigned long long>(m_perfIntervalPsoBinds),
      static_cast<unsigned long long>(m_perfIntervalPsoSkips),
      static_cast<unsigned long long>(m_perfIntervalRootTableSets),
      static_cast<unsigned long long>(m_perfIntervalDescriptorCopies),
      static_cast<unsigned long long>(m_perfIntervalDescriptorCacheHits),
      static_cast<unsigned long long>(m_perfIntervalCbvCreates),
      static_cast<unsigned long long>(m_perfIntervalBarrierCalls),
      static_cast<unsigned long long>(m_perfIntervalBarrierTransitions),
      static_cast<unsigned long long>(m_perfIntervalBarrierBufferTransitions),
      static_cast<unsigned long long>(m_perfIntervalBarrierImageTransitions),
      static_cast<unsigned long long>(m_perfIntervalBarrierToCopyDest),
      static_cast<unsigned long long>(m_perfIntervalBarrierFromCopyDest),
      static_cast<unsigned long long>(m_perfIntervalBarrierToSrv),
      static_cast<unsigned long long>(m_perfIntervalBarrierToRenderTarget),
      static_cast<unsigned long long>(m_perfIntervalBarrierToUav),
      static_cast<unsigned long long>(m_perfIntervalBarrierToDepth),
      m_perfIntervalMaxCbvSrvUavOff,
      m_perfIntervalMaxSamplerOff,
      static_cast<unsigned long long>(m_inFlightUploads.size()),
      static_cast<unsigned long long>(m_inFlightResourceBatches.size()));

    m_perfIntervalPresents = 0;
    m_perfIntervalFlushes = 0;
    m_perfIntervalWorkFlushes = 0;
    m_perfIntervalDrawOps = 0;
    m_perfIntervalCopyOps = 0;
    m_perfIntervalUpdateBufferCalls = 0;
    m_perfIntervalUpdateBytes = 0;
    m_perfIntervalUpdateConstantBufferCalls = 0;
    m_perfIntervalUpdateIaBufferCalls = 0;
    m_perfIntervalUpdateSrvUavBufferCalls = 0;
    m_perfIntervalUpdateSrvOnlyBufferCalls = 0;
    m_perfIntervalUpdateUavCapableBufferCalls = 0;
    m_perfIntervalUpdateOtherBufferCalls = 0;
    m_perfIntervalDirectBufferUpdateCalls = 0;
    m_perfIntervalImmediateBufferUpdateCalls = 0;
    m_perfIntervalCopiedBufferUpdateCalls = 0;
    m_perfIntervalMapReadCalls = 0;
    m_perfIntervalMapWriteCalls = 0;
    m_perfIntervalStagedMaps = 0;
    m_perfIntervalCopyBufferCalls = 0;
    m_perfIntervalCopyImageCalls = 0;
    m_perfIntervalCopyImageRegionCalls = 0;
    m_perfIntervalCopyBufferToImageCalls = 0;
    m_perfIntervalCopyImageToBufferCalls = 0;
    m_perfIntervalStreamOutputReadbacks = 0;
    m_perfIntervalDescriptorFlushes = 0;
    m_perfIntervalBatchResourceRefs = 0;
    m_perfIntervalPsoBinds = 0;
    m_perfIntervalPsoSkips = 0;
    m_perfIntervalRootTableSets = 0;
    m_perfIntervalDescriptorCopies = 0;
    m_perfIntervalDescriptorCacheHits = 0;
    m_perfIntervalCbvCreates = 0;
    m_perfIntervalBarrierCalls = 0;
    m_perfIntervalBarrierTransitions = 0;
    m_perfIntervalBarrierBufferTransitions = 0;
    m_perfIntervalBarrierImageTransitions = 0;
    m_perfIntervalBarrierToCopyDest = 0;
    m_perfIntervalBarrierFromCopyDest = 0;
    m_perfIntervalBarrierToSrv = 0;
    m_perfIntervalBarrierToRenderTarget = 0;
    m_perfIntervalBarrierToUav = 0;
    m_perfIntervalBarrierToDepth = 0;
    m_perfIntervalMaxCbvSrvUavOff = 0;
    m_perfIntervalMaxSamplerOff = 0;
    m_perfIntervalFlushExecMs = 0.0;
    m_perfIntervalFlushTotalMs = 0.0;
    m_perfIntervalResetWaitMs = 0.0;
    m_perfIntervalPresentWaitMs = 0.0;
    m_perfIntervalDescriptorCommitMs = 0.0;
    m_perfIntervalCommitCbvMs = 0.0;
    m_perfIntervalCommitSrvMs = 0.0;
    m_perfIntervalCommitUavMs = 0.0;
    m_perfIntervalCommitSamplerMs = 0.0;
    m_perfIntervalSnapshotMs = 0.0;
    m_perfIntervalFlushBarriersMs = 0.0;
    m_perfIntervalCloseExecuteMs = 0.0;
    m_perfIntervalResetCommandListMs = 0.0;
    m_perfIntervalDeviceCheckMs = 0.0;
    m_perfIntervalReleaseCompletedMs = 0.0;
    m_perfIntervalUploadBookkeepingMs = 0.0;
    m_perfIntervalPostDeviceCheckMs = 0.0;
    m_perfIntervalSignalMs = 0.0;
    m_perfIntervalBatchMoveMs = 0.0;
    m_perfIntervalUploadReleaseMs = 0.0;
    m_perfIntervalResourceBatchReleaseMs = 0.0;
    m_perfIntervalReleasedResourceRefs = 0;
    m_perfIntervalCompletedUploads = 0;
    m_perfIntervalRecycledUploads = 0;
    m_perfIntervalStartTicks = now.QuadPart;
#else
    (void)presentWaitMs;
#endif
  }
#endif

  void D3D12BackendContext::updateBuffer(IBackendBuffer* buffer, uint64_t offset, uint64_t size, const void* data) {
#ifdef _WIN32
    BufferUpdateRegion region = {};
    region.offset = offset;
    region.size = size;
    region.data = data;
    updateBufferRegions(buffer, &region, 1);
#else
    (void)buffer;
    (void)offset;
    (void)size;
    (void)data;
#endif
  }

  void D3D12BackendContext::updateBufferRegions(IBackendBuffer* buffer, const BufferUpdateRegion* regions, uint32_t regionCount) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (!d3d12Buffer || !regions || !regionCount)
      return;
    markGpuUse(stagingGpuUse(buffer));

    struct UploadedRegion {
      uint64_t dstOffset = 0;
      uint64_t srcOffset = 0;
      uint64_t size = 0;
    };

    // Reused across calls; this runs for every UpdateSubresource.
    static thread_local std::vector<UploadedRegion> uploadedRegions;
    uploadedRegions.clear();

    constexpr uint64_t kUploadAlignment = 256;
    auto alignUp = [](uint64_t value, uint64_t alignment) {
      return ((value + alignment - 1) / alignment) * alignment;
    };

    uint64_t totalUploadSize = 0;
    uint64_t validRegionCount = 0;
    for (uint32_t i = 0; i < regionCount; i++) {
      if (!regions[i].data || !regions[i].size)
        continue;
      validRegionCount++;
      totalUploadSize = alignUp(totalUploadSize, kUploadAlignment);
      totalUploadSize += regions[i].size;

      if (perfStatsEnabled() && !m_copyOnly) {
        m_perfIntervalUpdateBufferCalls++;
        m_perfIntervalUpdateBytes += regions[i].size;
      }
    }

    if (!totalUploadSize)
      return;

    if (perfStatsEnabled() && !m_copyOnly) {
      const auto bindFlags = d3d12Buffer->getDesc().bindFlags;
      const uint32_t bindMask = static_cast<uint32_t>(bindFlags);
      const uint32_t iaMask = static_cast<uint32_t>(BindFlags::VertexBuffer)
                            | static_cast<uint32_t>(BindFlags::IndexBuffer);
      const uint32_t srvUavMask = static_cast<uint32_t>(BindFlags::ShaderResource)
                                | static_cast<uint32_t>(BindFlags::UnorderedAccess);
      for (uint32_t i = 0; i < regionCount; i++) {
        if (!regions[i].data || !regions[i].size)
          continue;

        if (bindMask & static_cast<uint32_t>(BindFlags::ConstantBuffer)) {
          m_perfIntervalUpdateConstantBufferCalls++;
        } else if (bindMask & iaMask) {
          m_perfIntervalUpdateIaBufferCalls++;
        } else if (bindMask & srvUavMask) {
          m_perfIntervalUpdateSrvUavBufferCalls++;
          if (bindMask & static_cast<uint32_t>(BindFlags::UnorderedAccess))
            m_perfIntervalUpdateUavCapableBufferCalls++;
          else
            m_perfIntervalUpdateSrvOnlyBufferCalls++;
        } else {
          m_perfIntervalUpdateOtherBufferCalls++;
        }
      }
    }

    const auto& dstDesc = d3d12Buffer->getDesc();
    // Staging buffers take the ordered GPU copy: copies recorded before may
    // still read them.
    const bool cpuWritableBuffer = dstDesc.usage == ResourceUsage::Dynamic;
    if (!cpuWritableBuffer)
      prepareBufferGpuWrite(d3d12Buffer);
    if (cpuWritableBuffer) {
      void* mappedData = d3d12Buffer->getPersistentMappedData();
      const bool persistentMap = mappedData != nullptr;
      if (!persistentMap) {
        D3D12_RANGE readRange = { 0, 0 };
        HRESULT hr = d3d12Buffer->getResource()->Map(0, &readRange, &mappedData);
        if (FAILED(hr) || !mappedData) {
          logMessage(LogLevel::Error,
            "updateBufferRegions: direct Map failed hr=0x%08x buf=%p size=%llu",
            hr, buffer, dstDesc.size);
          return;
        }
      }

      uint64_t writtenBegin = dstDesc.size;
      uint64_t writtenEnd = 0;
      for (uint32_t i = 0; i < regionCount; i++) {
        if (!regions[i].data || !regions[i].size)
          continue;
        if (regions[i].offset >= dstDesc.size)
          continue;

        uint64_t copySize = regions[i].size;
        if (regions[i].offset + copySize > dstDesc.size)
          copySize = dstDesc.size - regions[i].offset;
        std::memcpy(static_cast<uint8_t*>(mappedData) + regions[i].offset, regions[i].data, static_cast<size_t>(copySize));
        if (regions[i].offset < writtenBegin)
          writtenBegin = regions[i].offset;
        if (regions[i].offset + copySize > writtenEnd)
          writtenEnd = regions[i].offset + copySize;
      }

      if (!persistentMap) {
        D3D12_RANGE writtenRange = {};
        if (writtenEnd > writtenBegin) {
          writtenRange.Begin = static_cast<SIZE_T>(writtenBegin);
          writtenRange.End = static_cast<SIZE_T>(writtenEnd);
        }
        d3d12Buffer->getResource()->Unmap(0, &writtenRange);
      }
      if (perfStatsEnabled() && !m_copyOnly)
        m_perfIntervalDirectBufferUpdateCalls += validRegionCount;
      return;
    }


    if (immediateSmallUpdatesEnabled()) {
      bool canUseImmediate = true;
      uint64_t dwordCount = 0;
      constexpr uint64_t kMaxImmediateRegionSize = 256;
      constexpr uint64_t kMaxImmediateDwords = 4096;

      for (uint32_t i = 0; i < regionCount; i++) {
        if (!regions[i].data || !regions[i].size)
          continue;

        if ((regions[i].offset & 3ull) != 0
         || (regions[i].size & 3ull) != 0
         || regions[i].size > kMaxImmediateRegionSize) {
          canUseImmediate = false;
          break;
        }

        dwordCount += regions[i].size / 4;
        if (dwordCount > kMaxImmediateDwords) {
          canUseImmediate = false;
          break;
        }
      }

      if (canUseImmediate && dwordCount) {
        if (m_cmdList.hasList2()) {
          std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> params;
          params.reserve(static_cast<size_t>(dwordCount));

          for (uint32_t i = 0; i < regionCount; i++) {
            if (!regions[i].data || !regions[i].size)
              continue;

            const uint8_t* src = static_cast<const uint8_t*>(regions[i].data);
            uint64_t dst = d3d12Buffer->getGpuAddress() + regions[i].offset;
            for (uint64_t byteOffset = 0; byteOffset < regions[i].size; byteOffset += 4) {
              uint32_t value = 0;
              std::memcpy(&value, src + byteOffset, sizeof(value));

              D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param = {};
              param.Dest = dst + byteOffset;
              param.Value = value;
              params.push_back(param);
            }
          }

          transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_COPY_DEST);
          holdBatchResource(d3d12Buffer->getResource());
          flushBarriers();
          m_cmdList->WriteBufferImmediate(static_cast<UINT>(params.size()), params.data(), nullptr);
          restoreBoundBufferState(d3d12Buffer);
          if (perfStatsEnabled() && !m_copyOnly)
            m_perfIntervalImmediateBufferUpdateCalls += validRegionCount;
          return;
        }
      }
    }

    // 32-bit processes: a large update goes in pieces of the ring's slot
    // size. A slot grown for it would keep the address space: the driver
    // does not reliably give freed upload memory back to the process.
    if (kSmallAddressSpace && totalUploadSize > kDefaultUploadScratchCapacity) {
      for (uint32_t i = 0; i < regionCount; i++) {
        if (!regions[i].data || !regions[i].size)
          continue;
        for (uint64_t done = 0; done < regions[i].size;) {
          const uint64_t piece = (std::min)(regions[i].size - done, kDefaultUploadScratchCapacity);
          ID3D12Resource* upload = nullptr;
          uint64_t offset = 0;
          void* ptr = nullptr;
          // May submit the command list when the slot is full: the copies
          // before stay in order.
          if (!allocateUploadScratch(piece, kUploadAlignment, &upload, &offset, &ptr)) {
            // Pieces before may have moved the buffer to COPY_DEST.
            restoreBoundBufferState(d3d12Buffer);
            return;
          }
          std::memcpy(ptr, static_cast<const uint8_t*>(regions[i].data) + done, static_cast<size_t>(piece));
          transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_COPY_DEST);
          holdBatchResource(d3d12Buffer->getResource());
          flushBarriers();
          m_cmdList->CopyBufferRegion(d3d12Buffer->getResource(), d3d12Buffer->getOffset() + regions[i].offset + done,
                                      upload, offset, piece);
          done += piece;
        }
      }
      restoreBoundBufferState(d3d12Buffer);
      return;
    }

    ID3D12Resource* uploadResource = nullptr;
    uint64_t uploadOffset = 0;
    void* uploadPtr = nullptr;
    allocateUploadScratch(totalUploadSize, kUploadAlignment, &uploadResource, &uploadOffset, &uploadPtr);
    if (!uploadResource || !uploadPtr)
      return;

    uint64_t relativeOffset = 0;
    for (uint32_t i = 0; i < regionCount; i++) {
      if (!regions[i].data || !regions[i].size)
        continue;

      relativeOffset = alignUp(relativeOffset, kUploadAlignment);
      std::memcpy(static_cast<uint8_t*>(uploadPtr) + relativeOffset, regions[i].data, static_cast<size_t>(regions[i].size));

      UploadedRegion uploaded = {};
      uploaded.dstOffset = regions[i].offset;
      uploaded.srcOffset = uploadOffset + relativeOffset;
      uploaded.size = regions[i].size;
      uploadedRegions.push_back(uploaded);
      relativeOffset += regions[i].size;
    }

    if (uploadedRegions.empty())
      return;
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalCopiedBufferUpdateCalls += static_cast<uint64_t>(uploadedRegions.size());

    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_COPY_DEST);
    holdBatchResource(d3d12Buffer->getResource());
    flushBarriers();

    for (const auto& uploaded : uploadedRegions) {
      m_cmdList->CopyBufferRegion(
        d3d12Buffer->getResource(),
        d3d12Buffer->getOffset() + uploaded.dstOffset,
        uploadResource,
        uploaded.srcOffset,
        uploaded.size);
    }

    restoreBoundBufferState(d3d12Buffer);
#else
    (void)buffer;
    (void)regions;
    (void)regionCount;
#endif
  }

  // ============================================================
  // Queries
  // ============================================================

  void D3D12BackendContext::beginQuery(IBackendQuery* query) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!query || m_deviceRemoved || m_copyOnly)
      return;

    auto* q = static_cast<D3D12BackendQuery*>(query);
    switch (q->getType()) {
      case QueryType::Timestamp:
      case QueryType::Event:
        // Only End applies to these.
        return;

      case QueryType::TimestampDisjoint:
        q->m_issued.store(false, std::memory_order_relaxed);
        q->m_active.store(true, std::memory_order_release);
        return;

      default:
        break;
    }

    // Beginning again discards the running segment.
    if (q->m_active.load(std::memory_order_relaxed) && q->m_context == this) {
      q->endSegment(&m_cmdList, m_pendingQueryResolves);
      m_activeQueries.erase(std::remove(m_activeQueries.begin(), m_activeQueries.end(), q), m_activeQueries.end());
    }

    if (m_predicate == q) {
      q->m_predicateValid = false;
      applyPredication();
    }

    q->m_segments = 0;
    q->m_issued.store(false, std::memory_order_relaxed);
    q->m_active.store(true, std::memory_order_release);
    if (!q->beginSegment(&m_cmdList)) {
      q->m_active.store(false, std::memory_order_release);
      return;
    }
    q->m_context = this;
    m_activeQueries.push_back(q);
#else
    (void)query;
#endif
  }

  void D3D12BackendContext::trackSwapChainBuffer(IUnknown* resource) {
#ifdef _WIN32
    ComPtr<ID3D12Resource> buffer;
    if (memstats::enabled() && resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))
      memstats::track(m_device->getDevice(), buffer.Get(), memstats::SwapChain);
#else
    (void)resource;
#endif
  }

  void D3D12BackendContext::endQuery(IBackendQuery* query) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!query || m_deviceRemoved || m_copyOnly)
      return;

    auto* q = static_cast<D3D12BackendQuery*>(query);
    switch (q->getType()) {
      case QueryType::Timestamp:
        q->m_segments = 0;
        if (!q->writeTimestamp(&m_cmdList, m_pendingQueryResolves))
          return;
        break;

      case QueryType::Event:
        break;

      case QueryType::TimestampDisjoint:
        if (!q->m_active.load(std::memory_order_relaxed))
          return;
        break;

      default: {
        // End without a matching Begin on this context is ignored.
        if (!q->m_active.load(std::memory_order_relaxed) || q->m_context != this)
          return;

        q->endSegment(&m_cmdList, m_pendingQueryResolves);
        m_activeQueries.erase(std::remove(m_activeQueries.begin(), m_activeQueries.end(), q), m_activeQueries.end());

        // Occlusion predicates also resolve into a buffer SetPredication
        // can read. A predicate split across command lists would need its
        // segments combined on the GPU; it renders unconditionally instead.
        if (q->getType() == QueryType::Occlusion && q->getDesc().binary) {
          q->m_predicateValid = q->m_segments == 1;
          if (q->m_predicateValid) {
            if (m_predicate == q && m_predicationApplied) {
              m_cmdList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
              m_predicationApplied = false;
            }
            if (ID3D12Resource* buffer = q->predicateBuffer()) {
              transitionPredicateBuffer(q, D3D12_RESOURCE_STATE_COPY_DEST);
              const D3D12QuerySlot& slot = q->m_slots[0];
              m_cmdList->ResolveQueryData(slot.heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, slot.index, 1, buffer, 0);
            } else {
              q->m_predicateValid = false;
            }
          }
          if (m_predicate == q)
            applyPredication();
        }

        if (m_predicate != q)
          q->m_context = nullptr;
        break;
      }
    }

    // The End lands in the command list that the next submission signals.
    if (q->getType() == QueryType::Occlusion)
      m_occlusionEndsSinceFlush++;
    q->m_issuedSegments = q->m_segments;
    q->m_issueSerial.fetch_add(1, std::memory_order_relaxed);
    if (q->m_fence.Get() != m_fence.Get())
      q->m_fence = m_fence;
    if (q->m_submittedFence != m_submittedFence)
      q->m_submittedFence = m_submittedFence;
    q->m_fenceValue.store(m_fenceValue + 1, std::memory_order_relaxed);
    q->m_issued.store(true, std::memory_order_release);
    q->m_active.store(false, std::memory_order_release);
#else
    (void)query;
#endif
  }

  void D3D12BackendContext::setPredication(IBackendQuery* predicate, bool value) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_copyOnly)
      return;

    auto* q = static_cast<D3D12BackendQuery*>(predicate);
    if (m_predicate && m_predicate != q && m_predicate->m_context == this
     && !m_predicate->m_active.load(std::memory_order_relaxed))
      m_predicate->m_context = nullptr;

    m_predicate = q;
    m_predicateValue = value;
    if (q)
      q->m_context = this;
    if (!m_deviceRemoved)
      applyPredication();
#else
    (void)predicate;
    (void)value;
#endif
  }

  void D3D12BackendContext::executeCommandList(const Rc<IBackendCommandList>& list) {
    replayCsCommandList(this, list);
  }

  void D3D12BackendContext::signalFence(IUnknown* fence, uint64_t value) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    ComPtr<ID3D12Fence> nativeFence;
    if (m_copyOnly || !fence || FAILED(fence->QueryInterface(IID_PPV_ARGS(&nativeFence))))
      return;
    flush();
    m_device->submitQueue().enqueue([nativeFence, value](ID3D12CommandQueue* queue) { queue->Signal(nativeFence.Get(), value); });
#else
    (void)fence; (void)value;
#endif
  }

  void D3D12BackendContext::waitFence(IUnknown* fence, uint64_t value) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    ComPtr<ID3D12Fence> nativeFence;
    if (m_copyOnly || !fence || FAILED(fence->QueryInterface(IID_PPV_ARGS(&nativeFence))))
      return;
    // Only the commands recorded after the wait have to wait.
    flush();
    m_device->submitQueue().enqueue([nativeFence, value](ID3D12CommandQueue* queue) { queue->Wait(nativeFence.Get(), value); });
#else
    (void)fence; (void)value;
#endif
  }

  void D3D12BackendContext::notifyOnCompletion(void* event) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!event)
      return;
    flush();
    if (m_fence)
      m_fence->SetEventOnCompletion(m_fenceValue, static_cast<HANDLE>(event));
#else
    (void)event;
#endif
  }

  // ============================================================
  // Tiled resources
  // ============================================================

#ifdef _WIN32
  static_assert(sizeof(TileCoordinate) == sizeof(D3D12_TILED_RESOURCE_COORDINATE), "tile coordinate layout");
  static_assert(sizeof(TileRegionSize) == sizeof(D3D12_TILE_REGION_SIZE), "tile region layout");

  namespace {
    ID3D12Resource* nativeTiledResource(TiledResourceRef resource) {
      if (resource.buffer)
        return static_cast<D3D12BackendBuffer*>(resource.buffer)->getResource();
      if (resource.image)
        return static_cast<D3D12BackendImage*>(resource.image)->getResource();
      return nullptr;
    }

    uint32_t regionTileCount(const TileRegionSize& size) {
      return size.useBox ? size.width * size.height * size.depth : size.numTiles;
    }
  }

  ID3D12Resource* D3D12BackendContext::transitionTiledResource(TiledResourceRef resource, D3D12_RESOURCE_STATES state) {
    if (resource.buffer) {
      auto* buffer = static_cast<D3D12BackendBuffer*>(resource.buffer);
      transitionResource(buffer, state);
      holdBatchResource(buffer->getResource());
      return buffer->getResource();
    }
    if (resource.image) {
      auto* image = static_cast<D3D12BackendImage*>(resource.image);
      transitionResource(image, state);
      holdBatchResource(image->getResource());
      return image->getResource();
    }
    return nullptr;
  }
#endif

  void D3D12BackendContext::updateTileMappings(const TileMappingUpdate& update) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    ID3D12Resource* resource = nativeTiledResource(update.resource);
    if (!resource || m_copyOnly || m_deviceRemoved)
      return;
    auto* pool = static_cast<D3D12BackendBuffer*>(update.tilePool);

    // Commands recorded so far use the previous mappings: after them in
    // queue order (the submit thread's jobs first).
    flush();
    m_device->submitQueue().drain();
    ID3D12CommandQueue* queue = m_device->getCommandQueue();

    const UINT regionCount = static_cast<UINT>(update.coordinates.size());
    const auto* coordinates = reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(update.coordinates.data());
    const auto* sizes = update.sizes.empty() ? nullptr : reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(update.sizes.data());
    const UINT rangeCount = static_cast<UINT>((std::max)({ update.rangeFlags.size(), update.poolOffsets.size(), update.rangeTileCounts.size() }));
    const auto* rangeFlags = update.rangeFlags.empty() ? nullptr : reinterpret_cast<const D3D12_TILE_RANGE_FLAGS*>(update.rangeFlags.data());
    const UINT* poolOffsets = update.poolOffsets.empty() ? nullptr : update.poolOffsets.data();
    const UINT* rangeTileCounts = update.rangeTileCounts.empty() ? nullptr : update.rangeTileCounts.data();
    const auto flags = static_cast<D3D12_TILE_MAPPING_FLAGS>(update.flags);

    // A pool that never grew is one heap: the arguments carry over as they are.
    if (!pool || pool->m_tileHeaps.size() <= 1) {
      queue->UpdateTileMappings(resource, regionCount, coordinates, sizes,
        pool && !pool->m_tileHeaps.empty() ? pool->m_tileHeaps[0].heap.Get() : nullptr,
        rangeCount ? rangeCount : 1, rangeFlags, poolOffsets, rangeTileCounts, flags);
      return;
    }

    // A grown pool spans several heaps: list every tile of the regions, pair
    // it with its pool tile and map tile by tile, one call per heap.
    UINT totalTiles = 0;
    D3D12_PACKED_MIP_INFO packedMips = {};
    D3D12_TILE_SHAPE tileShape = {};
    UINT subresourceCount = resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
      ? 1u : resource->GetDesc().MipLevels * (resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : resource->GetDesc().DepthOrArraySize);
    std::vector<D3D12_SUBRESOURCE_TILING> tilings(subresourceCount);
    m_device->getDevice()->GetResourceTiling(resource, &totalTiles, &packedMips, &tileShape,
      &subresourceCount, 0, tilings.data());

    std::vector<D3D12_TILED_RESOURCE_COORDINATE> tiles;
    for (UINT region = 0; region < regionCount; region++) {
      D3D12_TILE_REGION_SIZE size = sizes ? sizes[region] : D3D12_TILE_REGION_SIZE{ 1, FALSE, 0, 0, 0 };
      D3D12_TILED_RESOURCE_COORDINATE c = coordinates[region];
      if (size.UseBox) {
        for (UINT z = 0; z < size.Depth; z++)
          for (UINT y = 0; y < size.Height; y++)
            for (UINT x = 0; x < size.Width; x++)
              tiles.push_back({ c.X + x, c.Y + y, c.Z + z, c.Subresource });
        continue;
      }
      for (UINT i = 0; i < size.NumTiles; i++) {
        tiles.push_back(c);
        const bool packed = c.Subresource >= tilings.size() || tilings[c.Subresource].WidthInTiles == 0;
        if (packed) {
          c.X++;
          continue;
        }
        const auto& tiling = tilings[c.Subresource];
        if (++c.X >= tiling.WidthInTiles) {
          c.X = 0;
          if (++c.Y >= tiling.HeightInTiles) {
            c.Y = 0;
            if (++c.Z >= tiling.DepthInTiles) {
              c.Z = 0;
              c.Subresource++;
            }
          }
        }
      }
    }

    std::vector<D3D12_TILED_RESOURCE_COORDINATE> nullTiles;
    std::vector<std::vector<D3D12_TILED_RESOURCE_COORDINATE>> heapTiles(pool->m_tileHeaps.size());
    std::vector<std::vector<UINT>> heapOffsets(pool->m_tileHeaps.size());
    size_t cursor = 0;
    for (UINT range = 0; range < (rangeCount ? rangeCount : 1) && cursor < tiles.size(); range++) {
      const UINT count = rangeTileCounts ? rangeTileCounts[range] : static_cast<UINT>(tiles.size() - cursor);
      const UINT rangeFlag = rangeFlags ? static_cast<UINT>(rangeFlags[range]) : 0u;
      const UINT offset = poolOffsets ? poolOffsets[range] : 0u;
      for (UINT t = 0; t < count && cursor < tiles.size(); t++, cursor++) {
        if (rangeFlag & D3D12_TILE_RANGE_FLAG_SKIP)
          continue;
        if (rangeFlag & D3D12_TILE_RANGE_FLAG_NULL) {
          nullTiles.push_back(tiles[cursor]);
          continue;
        }
        const UINT poolTile = (rangeFlag & D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) ? offset : offset + t;
        for (size_t h = 0; h < pool->m_tileHeaps.size(); h++) {
          const auto& heap = pool->m_tileHeaps[h];
          if (poolTile >= heap.firstTile && poolTile < heap.firstTile + heap.tileCount) {
            heapTiles[h].push_back(tiles[cursor]);
            heapOffsets[h].push_back(poolTile - heap.firstTile);
            break;
          }
        }
      }
    }

    if (!nullTiles.empty()) {
      const D3D12_TILE_RANGE_FLAGS nullFlag = D3D12_TILE_RANGE_FLAG_NULL;
      const UINT nullCount = static_cast<UINT>(nullTiles.size());
      queue->UpdateTileMappings(resource, nullCount, nullTiles.data(), nullptr, nullptr,
        1, &nullFlag, nullptr, &nullCount, flags);
    }
    for (size_t h = 0; h < heapTiles.size(); h++) {
      if (heapTiles[h].empty())
        continue;
      const UINT count = static_cast<UINT>(heapTiles[h].size());
      std::vector<UINT> ones(count, 1u);
      queue->UpdateTileMappings(resource, count, heapTiles[h].data(), nullptr, pool->m_tileHeaps[h].heap.Get(),
        count, nullptr, heapOffsets[h].data(), ones.data(), flags);
    }
#else
    (void)update;
#endif
  }

  void D3D12BackendContext::copyTileMappings(TiledResourceRef dst, const TileCoordinate& dstCoordinate,
                                             TiledResourceRef src, const TileCoordinate& srcCoordinate,
                                             const TileRegionSize& size, uint32_t flags) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    ID3D12Resource* dstResource = nativeTiledResource(dst);
    ID3D12Resource* srcResource = nativeTiledResource(src);
    if (!dstResource || !srcResource || m_copyOnly || m_deviceRemoved)
      return;
    flush();
    m_device->submitQueue().drain();
    m_device->getCommandQueue()->CopyTileMappings(dstResource,
      reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(&dstCoordinate), srcResource,
      reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(&srcCoordinate),
      reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(&size), static_cast<D3D12_TILE_MAPPING_FLAGS>(flags));
#else
    (void)dst; (void)dstCoordinate; (void)src; (void)srcCoordinate; (void)size; (void)flags;
#endif
  }

  void D3D12BackendContext::copyTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                                      IBackendBuffer* buffer, uint64_t bufferOffset, uint32_t flags) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(buffer);
    if (!d3d12Buffer || !d3d12Buffer->getResource() || m_copyOnly || m_deviceRemoved)
      return;
    markGpuUse(stagingGpuUse(buffer));
    const bool toBuffer = (flags & D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER) != 0;
    ID3D12Resource* tiled = transitionTiledResource(resource,
      toBuffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
    if (!tiled)
      return;
    transitionResource(d3d12Buffer, toBuffer ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE);
    holdBatchResource(d3d12Buffer->getResource());
    flushBarriers();
    m_cmdList->CopyTiles(tiled, reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(&coordinate),
      reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(&size), d3d12Buffer->getResource(), d3d12Buffer->getOffset() + bufferOffset,
      static_cast<D3D12_TILE_COPY_FLAGS>(flags));
#else
    (void)resource; (void)coordinate; (void)size; (void)buffer; (void)bufferOffset; (void)flags;
#endif
  }

  void D3D12BackendContext::updateTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                                        const void* data, uint32_t flags) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    const uint64_t bytes = uint64_t(regionTileCount(size)) * kTileSizeInBytes;
    if (!data || !bytes || m_copyOnly || m_deviceRemoved)
      return;

    BufferDesc uploadDesc = {};
    uploadDesc.size = bytes;
    uploadDesc.usage = ResourceUsage::Dynamic;
    uploadDesc.cpuAccess = CpuAccess::Write;
    auto upload = m_device->createBuffer(uploadDesc);
    if (!upload)
      return;
    void* mapped = mapBuffer(upload.get(), MapMode::Write);
    if (!mapped)
      return;
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    unmapBuffer(upload.get());

    // D3D11_TILE_COPY_NO_OVERWRITE is the only flag UpdateTiles takes.
    copyTiles(resource, coordinate, size, upload.get(), 0,
      D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE | (flags & D3D12_TILE_COPY_FLAG_NO_HAZARD));
    retainTemporaryBuffer(std::move(upload));
#else
    (void)resource; (void)coordinate; (void)size; (void)data; (void)flags;
#endif
  }

  bool D3D12BackendContext::resizeTilePool(IBackendBuffer* tilePool, uint64_t newSize) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* pool = static_cast<D3D12BackendBuffer*>(tilePool);
    if (!pool)
      return false;
    D3D12BackendBuffer::TileHeap heap;
    HRESULT hr = pool->createTileHeap(m_device->getDevice(), newSize, heap);
    if (FAILED(hr))
      return false;
    if (hr == S_OK)
      pool->appendTileHeap(std::move(heap));
    return true;
#else
    (void)tilePool; (void)newSize;
    return false;
#endif
  }

  void D3D12BackendContext::tiledResourceBarrier() {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_copyOnly || m_deviceRemoved)
      return;
    flushBarriers();
    // An aliasing barrier without resources orders every tile access.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    m_cmdList->ResourceBarrier(1, &barrier);
#endif
  }

  void D3D12BackendContext::setImageMinLod(IBackendImage* image, float minLod) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(image);
    if (!d3d12Image || m_copyOnly)
      return;

    std::lock_guard<std::mutex> lock(d3d12Image->m_srvViewMutex);
    for (D3D12BackendImageView* view : d3d12Image->m_srvViews) {
      view->setMinLodClamp(minLod);

      // Bound views keep their handles but have new contents: make the
      // next commit copy them again.
      const D3D12_CPU_DESCRIPTOR_HANDLE handle = view->getHandle();
      for (uint32_t stage = 0; stage < kShaderStages; stage++) {
        for (uint32_t slot = 0; slot < m_srvBindingEnd[stage]; slot++) {
          auto& binding = m_srvBindings[stage][slot];
          if (binding.bound && binding.handle.ptr == handle.ptr) {
            binding.descriptorVersion = view->getDescriptorVersion();
            m_srvDirty = true;
            m_srvStageDirty[stage] = true;
            m_descriptorDirtyStageMask |= 1u << stage;
          }
        }
      }
    }
#else
    (void)image;
    (void)minLod;
#endif
  }

  void D3D12BackendContext::nextCommandList() {
#ifdef _WIN32
    // The pool, made closed (no allocator) the first time (what fails then
    // is not tried again: one list).
    if (!m_commandListPoolMade) {
      m_commandListPoolMade = true;
      ComPtr<ID3D12Device4> device4;
      if (SUCCEEDED(m_device->getDevice()->QueryInterface(IID_PPV_ARGS(&device4)))) {
        while (m_commandListCount < kCommandListPoolSize
            && SUCCEEDED(device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                 IID_PPV_ARGS(&m_commandLists[m_commandListCount]))))
          m_commandListTickets[m_commandListCount++] = 0;
      }
    }
    SubmitQueue& submit = m_device->submitQueue();
    if (m_commandListCount < 2) {
      // One list: it resets once the thread executed it.
      submit.waitFor(m_commandListTickets[m_currentCommandList]);
      return;
    }
    m_currentCommandList = (m_currentCommandList + 1) % m_commandListCount;
    submit.waitFor(m_commandListTickets[m_currentCommandList]);
    m_cmdList.setList(m_commandLists[m_currentCommandList]);
#endif
  }

  void D3D12BackendContext::afterSubmitted(std::function<void()> fn) {
#ifdef _WIN32
    m_device->submitQueue().enqueue([fn = std::move(fn)](ID3D12CommandQueue*) { fn(); });
#else
    fn();
#endif
  }

  void D3D12BackendContext::waitForQueue() {
#ifdef _WIN32
    m_device->submitQueue().drain();
#endif
  }

  void D3D12BackendContext::setExternallySynchronized(bool enabled) {
    m_externallySynchronized = enabled;
#ifdef _WIN32
    // d3d12.submitThread: the immediate context's lists and the initial
    // data reach the queue on a thread of their own (SubmitQueue).
    if (enabled && !m_copyOnly && UserOverrides::get().submitThread && !m_gpuProfiler.enabled() && !m_breadcrumbList
     && !m_device->submitQueue().threaded()) {
      m_device->submitQueue().start();
      if (m_device->submitQueue().threaded())
        logMessage(LogLevel::Info, "D3D12BackendContext: submit thread on (d3d12.submitThread)");
    }
    // The CS worker is then the only thread that records. The GPU profiler
    // and DRED breadcrumbs record into the list themselves.
    if (enabled && !m_copyOnly && UserOverrides::get().recordThread && !m_gpuProfiler.enabled() && !m_breadcrumbList) {
      m_cmdList.startThread();
      if (m_cmdList.deferred())
        logMessage(LogLevel::Info, "D3D12BackendContext: record thread on (d3d12.recordThread)");
    }
#endif
  }

  void D3D12BackendContext::retargetBufferViews(IBackendBuffer* buffer, const std::vector<IBackendBufferView*>& views) {
#ifdef _WIN32
    // Unordered access views are rewritten, which clears read.
    if (buffer && static_cast<uint32_t>(buffer->getDesc().bindFlags & BindFlags::UnorderedAccess))
      m_cmdList.drain();
#endif
    IBackendContext::retargetBufferViews(buffer, views);
  }

  void D3D12BackendContext::retargetImage(IBackendImage* image, IUnknown* resource) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(image);
    ComPtr<ID3D12Resource> d3d12Resource;
    if (!d3d12Image || m_copyOnly
     || (resource && FAILED(resource->QueryInterface(IID_PPV_ARGS(d3d12Resource.GetAddressOf())))))
      return;
    if (d3d12Resource.Get() == d3d12Image->getResource())
      return;
    flushBarriers();
    ID3D12Resource* previous = d3d12Image->getResource();
    // The views' render target descriptors are rewritten.
    m_cmdList.drain();
    std::lock_guard<std::mutex> lock(d3d12Image->m_srvViewMutex);
    d3d12Image->retarget(std::move(d3d12Resource), D3D12_RESOURCE_STATE_PRESENT);
    ID3D12Resource* current = d3d12Image->getResource();
    for (D3D12BackendImageView* view : d3d12Image->m_srvViews) {
      view->rewriteForImage();
      // Bound shader resource views have new contents: the next commit
      // copies them again, the resource reads in its new state.
      const D3D12_CPU_DESCRIPTOR_HANDLE handle = view->getHandle();
      for (uint32_t stage = 0; handle.ptr && stage < kShaderStages; stage++) {
        for (uint32_t slot = 0; slot < m_srvBindingEnd[stage]; slot++) {
          auto& binding = m_srvBindings[stage][slot];
          if (!binding.bound || binding.handle.ptr != handle.ptr)
            continue;
          binding.descriptorVersion = view->getDescriptorVersion();
          m_currentSrvResources[stage][slot] = current;
          if (current) {
            ensureImageViewState(view, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
              | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            holdBatchResource(current);
          }
          m_srvDirty = true;
          m_srvStageDirty[stage] = true;
          m_descriptorDirtyStageMask |= 1u << stage;
        }
      }
    }
    // Render targets bound with the previous resource are bound again
    // before the next draw.
    for (uint32_t i = 0; previous && i < kMaxRenderTargets; i++) {
      if (m_currentRenderTargetResources[i] == previous) {
        m_trackedOmBindingValid = false;
        m_outputMergerRebindRequired.store(true, std::memory_order_relaxed);
      }
    }
#else
    (void)image;
    (void)resource;
#endif
  }

  bool D3D12BackendContext::pausePredication() {
#ifdef _WIN32
    if (!m_predicationApplied)
      return false;
    m_cmdList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    m_predicationApplied = false;
    return true;
#else
    return false;
#endif
  }

  void D3D12BackendContext::resumePredication(bool paused) {
#ifdef _WIN32
    if (paused && !m_deviceRemoved)
      applyPredication();
#else
    (void)paused;
#endif
  }

  void D3D12BackendContext::suspendPredication(bool suspended) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_copyOnly || m_predicationSuspended == suspended)
      return;
    m_predicationSuspended = suspended;
    if (!m_deviceRemoved)
      applyPredication();
#else
    (void)suspended;
#endif
  }

  void D3D12BackendContext::forgetQuery(D3D12BackendQuery* query) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto active = std::find(m_activeQueries.begin(), m_activeQueries.end(), query);
    if (active != m_activeQueries.end()) {
      // Destroyed while begun: its open segment ends before the slots go
      // back to the pool (a slot begun twice fails the command list). Its
      // results are nobody's.
      if (!m_deviceRemoved && !m_closedForSubmission && m_cmdList) {
        std::vector<D3D12QueryResolve> unused;
        query->endSegment(&m_cmdList, unused);
      }
      m_activeQueries.erase(active);
    }
    if (m_predicate == query) {
      m_predicate = nullptr;
      if (!m_deviceRemoved && !m_closedForSubmission)
        applyPredication();
    }
#else
    (void)query;
#endif
  }

#ifdef _WIN32
  void D3D12BackendContext::suspendActiveQueries() {
    for (auto* query : m_activeQueries)
      query->endSegment(&m_cmdList, m_pendingQueryResolves);
    resolvePendingQueries();
  }

  void D3D12BackendContext::resolvePendingQueries() {
    if (m_pendingQueryResolves.empty())
      return;
    auto& resolves = m_pendingQueryResolves;
    std::sort(resolves.begin(), resolves.end(), [](const D3D12QueryResolve& a, const D3D12QueryResolve& b) {
      if (a.heap != b.heap) return a.heap < b.heap;
      if (a.type != b.type) return a.type < b.type;
      return a.index < b.index;
    });
    // One resolve per run of consecutive slots of a heap; their readback
    // offsets are consecutive as well.
    size_t start = 0;
    while (start < resolves.size()) {
      size_t end = start + 1;
      uint32_t nextIndex = resolves[start].index + 1;
      while (end < resolves.size()
          && resolves[end].heap == resolves[start].heap
          && resolves[end].type == resolves[start].type
          && resolves[end].index <= nextIndex) {
        if (resolves[end].index == nextIndex)
          nextIndex++;
        end++;
      }
      const auto& first = resolves[start];
      m_cmdList->ResolveQueryData(first.heap, first.type, first.index, nextIndex - first.index,
        first.readback, first.readbackOffset);
      start = end;
    }
    resolves.clear();
  }

  void D3D12BackendContext::resumeActiveQueries() {
    for (auto* query : m_activeQueries)
      query->beginSegment(&m_cmdList);
  }

  void D3D12BackendContext::transitionPredicateBuffer(D3D12BackendQuery* query, D3D12_RESOURCE_STATES state) {
    if (query->m_predicateState == state)
      return;
    flushBarriers();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = query->m_predicateBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = query->m_predicateState;
    barrier.Transition.StateAfter  = state;
    m_cmdList->ResourceBarrier(1, &barrier);
    query->m_predicateState = state;
  }

  void D3D12BackendContext::applyPredication() {
    D3D12BackendQuery* predicate = m_predicationSuspended ? nullptr : m_predicate;
    if (predicate && predicate->m_predicateValid && predicate->m_predicateBuffer) {
      transitionPredicateBuffer(predicate, D3D12_RESOURCE_STATE_PREDICATION);
      // D3D11 skips rendering while the predicate's result equals the
      // value; an occlusion predicate is true when any sample passed.
      m_cmdList->SetPredication(predicate->m_predicateBuffer.Get(), 0,
        m_predicateValue ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO);
      m_predicationApplied = true;
    } else if (m_predicationApplied) {
      m_cmdList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
      m_predicationApplied = false;
    }
  }
#endif

  // ============================================================
  // Synchronization
  // ============================================================

  void D3D12BackendContext::flush() {
    HitchScope hitch("backend flush");
#ifdef _WIN32
    // Hitch trace: where a slow submission spends its time.
    struct FlushSteps {
      bool    trace = hitchTraceEnabled();
      int64_t marks[10] = {};
      int64_t closed = 0;
      void mark(int i) { if (trace) marks[i] = hitchTimestamp(); }
      ~FlushSteps() {
        if (!trace || !marks[9])
          return;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double toMs = 1000.0 / double(frequency.QuadPart);
        if (double(marks[9] - marks[0]) * toMs < 4.0)
          return;
        auto ms = [&](int i) { return double(marks[i + 1] - marks[i]) * toMs; };
        const double close = closed ? double(closed - marks[5]) * toMs : 0.0;
        logMessage(LogLevel::Warn,
          "hitch: flush steps: initial data %.1f, lock %.1f, release %.1f, bookkeeping %.1f, record %.1f, "
          "close %.1f, execute %.1f, after %.1f, signal %.1f, reset %.1f ms",
          ms(0), ms(1), ms(2), ms(3), ms(4), close, ms(5) - close, ms(6), ms(7), ms(8));
      }
    } steps;
    steps.mark(0);
    void* flushCaller[2] = {};
    if (m_gpuProfiler.capturing())
      CaptureStackBackTrace(1, 2, flushCaller, nullptr);
    // Which code flushes, for the perf statistics.
    if (perfStatsEnabled() && !m_copyOnly) {
      static std::mutex s_callerMutex;
      static std::unordered_map<void*, uint64_t> s_callers;
      static uint64_t s_flushes = 0;
      void* frames[3] = {};
      CaptureStackBackTrace(1, 3, frames, nullptr);
      std::lock_guard<std::mutex> lock(s_callerMutex);
      s_callers[frames[0]]++;
      s_callers[frames[1]] += 0;  // keep the second frame visible in the dump below
      if (++s_flushes % 4000 == 0) {
        std::vector<std::pair<uint64_t, void*>> sorted;
        for (const auto& entry : s_callers)
          if (entry.second)
            sorted.push_back({ entry.second, entry.first });
        std::sort(sorted.rbegin(), sorted.rend());
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&perfStatsEnabled), &module);
        for (size_t i = 0; i < sorted.size() && i < 12; i++)
          logMessage(LogLevel::Warn, "flush caller +0x%llx: %llu",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(sorted[i].second) - reinterpret_cast<uintptr_t>(module)),
            static_cast<unsigned long long>(sorted[i].first));
        s_callers.clear();
      }
    }
    // New resources get their data first.
    if (!m_copyOnly)
      m_device->submitInitialData();
    steps.mark(1);
    ContextLock ctxLock(*this);
    steps.mark(2);
    m_uavWritten.clear();
    m_uavWrittenAccess.clear();
    m_uavWrittenBy.clear();
    if (m_deviceRemoved) return;
    static int flushCount = 0;
    flushCount++;
    static int flushesThisFrame = 0;
    flushesThisFrame++;

    // Keep timing probes out of the normal render path unless diagnostics are
    // explicitly enabled. QPC is useful for profiling but not for every
    // production frame/flush.
    const bool collectFlushTiming = !m_copyOnly
      && (perfStatsEnabled() || perfTimingEnabled() || infoDiagnosticsEnabled());
    LARGE_INTEGER flushStart = {}, flushAfterExec = {}, flushEnd = {}, perfFreq = {};
    if (collectFlushTiming) {
      QueryPerformanceFrequency(&perfFreq);
      QueryPerformanceCounter(&flushStart);
    }
    const bool collectTiming = perfTimingEnabled() && !m_copyOnly;
    LARGE_INTEGER timingStart = {}, timingEnd = {};

    if (g_gpuOpLimit >= 0)
      g_gpuOpCounter = 0;  // Diagnostic counter is inactive in production.
    uint32_t savedDrawOps = m_drawOpsSinceFlush;
    uint32_t savedCopyOps = m_copyOpsSinceFlush;
    uint32_t savedCbvOff  = m_cbvSrvUavHeapOffset >= m_cbvSrvUavHeapBaseOffset
      ? m_cbvSrvUavHeapOffset - m_cbvSrvUavHeapBaseOffset
      : m_cbvSrvUavHeapOffset;
    uint32_t savedSamplerOff = m_samplerDescriptorsInBatch;
    m_samplerDescriptorsInBatch = 0;
    m_copyOpsSinceFlush = 0;
    m_drawOpsSinceFlush = 0;
    m_occlusionEndsSinceFlush = 0;
    m_stagingWritePending = false;
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    releaseCompletedUploads();
    steps.mark(3);
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalReleaseCompletedMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    // Tag pending uploads with the fence value that will be signaled after this flush
    // (m_fenceValue will be incremented and signaled AFTER execute)
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    if (!m_pendingUploads.empty()) {
      uint64_t nextFence = m_fenceValue + 1;  // The value that will be signaled
      for (auto& buf : m_pendingUploads) {
        m_inFlightUploads.push_back(FencedUpload{nextFence, std::move(buf)});
      }
      m_pendingUploads.clear();
      m_pendingUploadBytes = 0;
    }
    if (!m_pendingRetired.empty())
      m_inFlightRetired.push_back(FencedRetired{ m_fenceValue + 1, std::move(m_pendingRetired) });
    m_pendingRetired.clear();
    if (!m_pendingSlices.empty())
      m_inFlightSlices.push_back(FencedSlices{ m_fenceValue + 1, std::move(m_pendingSlices) });
    m_pendingSlices.clear();

    // Safety valve: if too many in-flight uploads accumulate, wait for GPU
    // then release completed uploads again. Do NOT clear all uploads here
    // because pending uploads were just moved to in-flight and are still
    // referenced by the command list about to be executed.
    if (m_inFlightUploads.size() > 8192) {
      if (m_fence && m_lastSignaledFenceValue > 0 && m_fence->GetCompletedValue() < m_lastSignaledFenceValue) {
        m_fence->SetEventOnCompletion(m_lastSignaledFenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, 2000);
      }
      releaseCompletedUploads();
    }
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalUploadBookkeepingMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    // Active queries end their segment in this command list and continue
    // in the next one.
    steps.mark(4);
    suspendActiveQueries();

    // Snapshot all currently bound resources so they stay alive until the
    // GPU finishes this command list (deferred resource destruction).
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    snapshotBoundResources();
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalSnapshotMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    if (!m_sharedImagesInBatch.empty() || !m_sharedBuffersInBatch.empty())
      prepareSharedSubmit();

    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    flushBarriers();
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalFlushBarriersMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    if (m_gpuProfiler.enabled())
      m_gpuProfiler.beforeClose(m_cmdList.raw());
    // Close and execute command list (SEH-protected)
    steps.mark(5);
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    SubmitQueue& submit = m_device->submitQueue();
    const bool threaded = submit.threaded();
    bool closeOk = threaded
      ? safeClose(&m_cmdList, flushCount - 1, steps.trace ? &steps.closed : nullptr)
      : safeCloseAndExecute(&m_cmdList, m_device->getCommandQueue(), flushCount - 1, collectFlushTiming,
          steps.trace ? &steps.closed : nullptr);
    steps.mark(6);
    if (m_gpuProfiler.enabled()) {
      HMODULE module = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&perfStatsEnabled), &module);
      const uint64_t base = reinterpret_cast<uintptr_t>(module);
      m_gpuProfiler.onExecuted(flushCaller[0] ? reinterpret_cast<uintptr_t>(flushCaller[0]) - base : 0,
        flushCaller[1] ? reinterpret_cast<uintptr_t>(flushCaller[1]) - base : 0);
    }
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalCloseExecuteMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }
    if (!closeOk) {
      // ROLLBACK: revert all tracked state transitions since the last successful flush,
      // because the failed command list's transitions were never executed on the GPU.
      for (auto it = m_stateRollback.rbegin(); it != m_stateRollback.rend(); ++it) {
        if (it->subresource == UINT_MAX) {
          it->image->setCurrentState(it->oldState);
        } else {
          it->image->setSubresourceState(it->subresource, it->oldState);
        }
      }
      m_stateRollback.clear();
      for (auto it = m_bufferStateRollback.rbegin(); it != m_bufferStateRollback.rend(); ++it) {
        it->buffer->setCurrentState(it->oldState);
      }
      m_bufferStateRollback.clear();
      m_commonDecaySubresources.clear();
      m_sharedImagesInBatch.clear();
      m_sharedBuffersInBatch.clear();

      static int closeFailCount = 0;
      closeFailCount++;
      if (closeFailCount <= 3) {
        logMessage(LogLevel::Error, "flush() #%d: Close/Execute CRASHED (fail #%d) — dumping recentOps", flushCount - 1, closeFailCount);
        const uint32_t recentOpCapacity = static_cast<uint32_t>(m_recentOps.size());
        for (uint32_t i = 0; i < m_recentOpCount; i++) {
          uint32_t idx = (m_recentOpWriteIndex + recentOpCapacity - m_recentOpCount + i) % recentOpCapacity;
          logMessage(LogLevel::Error, "TDR recentOp[%u]: %s", i, m_recentOps[idx].data());
        }
        // Dump all barriers emitted during this command list recording
        logMessage(LogLevel::Error, "flush() #%d: dumping %u emitted barriers", flushCount - 1, static_cast<uint32_t>(m_emittedBarriers.size()));
        for (uint32_t i = 0; i < m_emittedBarriers.size(); i++) {
          const auto& b = m_emittedBarriers[i];
          if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            logMessage(LogLevel::Error, "  emittedBarrier[%u]: TRANSITION res=%p sub=%u before=0x%x after=0x%x flags=0x%x",
              i, b.Transition.pResource, b.Transition.Subresource,
              static_cast<uint32_t>(b.Transition.StateBefore),
              static_cast<uint32_t>(b.Transition.StateAfter),
              static_cast<uint32_t>(b.Flags));
          } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            logMessage(LogLevel::Error, "  emittedBarrier[%u]: UAV res=%p", i, b.UAV.pResource);
          } else {
            logMessage(LogLevel::Error, "  emittedBarrier[%u]: type=%u", i, static_cast<uint32_t>(b.Type));
          }
        }
      } else if (closeFailCount == 4) {
        logMessage(LogLevel::Error, "flush(): suppressing further Close/Execute crash dumps (had %d failures)", closeFailCount);
      }
      logDeviceRemovedReason(m_device, "CloseAndExecute");

      // Log D3D12 debug layer messages (requires dxup_debug.txt)
      if (closeFailCount <= 3) {
        logD3D12DebugMessages(m_device->getDevice(), "CloseFailure");
      }
      // Check device removal
      HRESULT removedHr = m_device->getDevice()->GetDeviceRemovedReason();
      if (FAILED(removedHr)) {
        m_deviceRemoved = true;
        logMessage(LogLevel::Error, "flush() #%d: DEVICE REMOVED (hr=0x%08x)", flushCount - 1, removedHr);
        m_commonDecaySubresources.clear();
        return;
      }

      // Device is OK but command list is in error state after failed Close().
      // Cannot call Reset() on a non-closed command list — must recreate it.
      logMessage(LogLevel::Warn, "flush() #%d: recovering command list (fail #%d)", flushCount - 1, closeFailCount);

      // Wait for any previous GPU work before resetting allocator
      if (m_fence && m_lastSignaledFenceValue > 0 && m_fence->GetCompletedValue() < m_lastSignaledFenceValue) {
        m_fence->SetEventOnCompletion(m_lastSignaledFenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, 5000);
      }

      // Reset allocator (safe — previous GPU work is done, failed batch was never executed)
      m_currentAllocator->Reset();

      // Release broken command list and create a fresh one (the submit
      // thread is done with it: it never got it).
      m_cmdList.setList(nullptr);
      m_commandLists[m_currentCommandList] = nullptr;
      ComPtr<ID3D12GraphicsCommandList> freshList;
      HRESULT createHr = m_device->getDevice()->CreateCommandList(0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_currentAllocator.Get(), nullptr,
        IID_PPV_ARGS(&freshList));
      if (FAILED(createHr)) {
        logMessage(LogLevel::Error, "flush() recovery: CreateCommandList failed (hr=0x%08x) — setting deviceRemoved", createHr);
        m_deviceRemoved = true;
        return;
      }
      m_commandLists[m_currentCommandList] = freshList;
      // DRED's markers go into the new list.
      if (m_breadcrumbList && FAILED(freshList.As(&m_breadcrumbList)))
        m_breadcrumbList = nullptr;
      m_cmdList.setList(std::move(freshList));

      // Re-bind state on the fresh command list
      if (m_shaderVisibleCbvSrvUavHeap && m_shaderVisibleSamplerHeap) {
        ID3D12DescriptorHeap* heaps[] = {
          m_shaderVisibleCbvSrvUavHeap.Get(),
          m_shaderVisibleSamplerHeap.Get()
        };
        m_cmdList->SetDescriptorHeaps(2, heaps);
      }
      if (m_currentRootSignature)
        m_cmdList->SetGraphicsRootSignature(m_currentRootSignature);
      if (m_computeRootSignature)
        m_cmdList->SetComputeRootSignature(m_computeRootSignature);

      // Error recovery did a full GPU sync above, so the current descriptor
      // partition can be reused safely.
      resetDescriptorHeapPartition();
      m_partitionsInBatch = 1u << m_descriptorPartition;
      initNullDescriptorsAndRootTables();

      markAllDescriptorStagesDirty();
      m_psoDirtyAfterFlush = true;  // PSO must be re-bound on new command list
      m_pendingUploads.clear();
      m_pendingUploadBytes = 0;
      // Release batch resource refs — command list was never executed
      for (auto* res : m_batchResourceRefs) res->Release();
      m_batchResourceRefs.clear();
      clearBatchResourceRefTable();
      if (++m_resourceBatchGeneration == 0)
        m_resourceBatchGeneration = 1;
      m_emittedBarriers.clear();
      m_recentOpCount = 0;
      m_recentOpWriteIndex = 0;
      m_commonDecaySubresources.clear();

      // Re-bind tracked dynamic state on the fresh command list
      // (mirrors what resetCommandList does after a successful flush)
      for (uint32_t i = 0; i < kMaxVertexBuffers; i++) {
        if (m_trackedVBs[i].bound)
          m_cmdList->IASetVertexBuffers(i, 1, &m_trackedVBs[i].view);
      }
      if (m_trackedIBBound)
        m_cmdList->IASetIndexBuffer(&m_trackedIB);
      if (m_trackedViewportCount)
        m_cmdList->RSSetViewports(m_trackedViewportCount, m_trackedViewports);
      if (m_trackedScissorCount)
        m_cmdList->RSSetScissorRects(m_trackedScissorCount, m_trackedScissors);
      m_cmdList->OMSetStencilRef(m_trackedStencilRef);
      m_cmdList->OMSetBlendFactor(m_trackedBlendFactor);
      if (m_trackedTopology != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
        m_cmdList->IASetPrimitiveTopology(m_trackedTopology);
      if (m_trackedRTVCount || m_trackedDSVBound) {
        m_cmdList->OMSetRenderTargets(
          m_trackedRTVCount,
          m_trackedRTVCount ? m_trackedRTVs : nullptr,
          FALSE,
          m_trackedDSVBound ? &m_trackedDSV : nullptr);
      }
      m_trackedOmBindingValid = true;

      if (m_soBufferCount)
        m_cmdList->SOSetTargets(0, m_soBufferCount, m_soViews);

      // The fresh command list now references persistent native resources.
      // Acquire this batch's references before D3D11 can unbind and release
      // the corresponding wrappers after recovery returns.
      snapshotBoundResources();

      // The lost batch's fence value still comes: the uploads tagged with
      // it are released and the waits for its work (Map, queries) end.
      m_fenceValue++;
      {
        ID3D12Fence* fence = m_fence.Get();
        const uint64_t value = m_fenceValue;
        SubmitQueue* owner = &m_device->submitQueue();
        owner->enqueue([fence, value, owner](ID3D12CommandQueue* queue) {
          owner->checkResult(queue->Signal(fence, value), "Signal");
        });
        m_lastSignaledFenceValue = m_fenceValue;
        m_submittedFence->store(m_fenceValue, std::memory_order_release);
      }

      logMessage(LogLevel::Warn, "flush() #%d: command list recovered successfully", flushCount - 1);
      return;  // Skip normal fence/reset path
    }

    // Close/Execute succeeded. Implicitly promoted read-only resources and
    // simultaneous-access textures decay to COMMON at ExecuteCommandLists
    // boundaries, so mirror that in our tracked state before recording more work.
    decayPromotedImageStates();

    // Clear state rollback log and check for device removal.
    m_stateRollback.clear();
    m_bufferStateRollback.clear();
    m_emittedBarriers.clear();
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    {
      // The submit thread executes later: what earlier submissions found.
      HRESULT removedHr = threaded ? submit.lostReason() : m_device->getDevice()->GetDeviceRemovedReason();
      if (FAILED(removedHr)) {
        m_deviceRemoved = true;
        logMessage(LogLevel::Error, "flush() #%d: DEVICE REMOVED after Execute (hr=0x%08x)",
          flushCount - 1, removedHr);

        // Log GPU memory at point of failure
        {
          ComPtr<IDXGIAdapter3> adapter3;
          auto* adapter = m_device->getAdapter();
          if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);
            logMessage(LogLevel::Error,
              "GPU memory at DEVICE REMOVED: budget=%lluMB usage=%lluMB avail=%lluMB inFlightUploads=%zu pendingUploads=%zu",
              localInfo.Budget / (1024*1024), localInfo.CurrentUsage / (1024*1024),
              (localInfo.Budget > localInfo.CurrentUsage) ? (localInfo.Budget - localInfo.CurrentUsage) / (1024*1024) : 0,
              m_inFlightUploads.size(), m_pendingUploads.size());
          }
        }

        const uint32_t recentOpCapacity = static_cast<uint32_t>(m_recentOps.size());
        for (uint32_t i = 0; i < m_recentOpCount; i++) {
          uint32_t idx = (m_recentOpWriteIndex + recentOpCapacity - m_recentOpCount + i) % recentOpCapacity;
          logMessage(LogLevel::Error, "TDR recentOp[%u]: %s", i, m_recentOps[idx].data());
        }
        logDeviceRemovedReason(m_device, "ExecuteCommandLists");
        logDispatchBreadcrumbs();
        m_commonDecaySubresources.clear();
        return;  // Don't continue with fence/reset after device removal
      }
    }
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalPostDeviceCheckMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    steps.mark(7);
    // With the debug layer its findings reach the log while the game runs.
    if (!m_copyOnly && m_device->isDebugLayerEnabled() && (m_fenceValue & 31) == 0)
      m_device->drainDebugMessages();
    m_fenceValue++;
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    HRESULT signalHr = S_OK;
    if (threaded) {
      ID3D12CommandList* list = m_cmdList.raw();
      ID3D12Fence* fence = m_fence.Get();
      const uint64_t value = m_fenceValue;
      SubmitQueue* owner = &submit;
      m_commandListTickets[m_currentCommandList] = submit.enqueue([list, fence, value, owner](ID3D12CommandQueue* queue) {
        safeExecuteAndSignal(queue, list, fence, value, owner);
      });
    } else {
      signalHr = m_device->getCommandQueue()->Signal(m_fence.Get(), m_fenceValue);
    }
    if (FAILED(signalHr)) {
      logMessage(LogLevel::Error, "Command queue Signal() failed (hr=0x%08x)", signalHr);
      logDeviceRemovedReason(m_device, "Signal");
      m_deviceRemoved = true;
      return;
    }
    m_lastSignaledFenceValue = m_fenceValue;
    m_submittedFence->store(m_fenceValue, std::memory_order_release);
    if (!m_sharedImagesInBatch.empty() || !m_sharedBuffersInBatch.empty())
      publishSharedSubmit();
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalSignalMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    // Move batch resource refs to in-flight, tagged with the fence just signaled
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    if (!m_batchResourceRefs.empty()) {
      m_inFlightResourceBatches.push_back({m_fenceValue, std::move(m_batchResourceRefs)});
      m_batchResourceRefs = {};
      clearBatchResourceRefTable();
    }
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalBatchMoveMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    if (collectFlushTiming)
      QueryPerformanceCounter(&flushAfterExec);

    // Reset for next batch
    steps.mark(8);
    if (collectTiming)
      QueryPerformanceCounter(&timingStart);
    resetCommandList();
    steps.mark(9);
    if (collectTiming) {
      QueryPerformanceCounter(&timingEnd);
      m_perfIntervalResetCommandListMs += qpcElapsedMs(timingStart, timingEnd, perfFreq.QuadPart);
    }

    if (collectFlushTiming)
      QueryPerformanceCounter(&flushEnd);

    // Log flush timing — only for flushes with actual work, sample periodically
    if (collectFlushTiming) {
      double execMs = (double)(flushAfterExec.QuadPart - flushStart.QuadPart) * 1000.0 / perfFreq.QuadPart;
      double waitMs = (double)(flushEnd.QuadPart - flushAfterExec.QuadPart) * 1000.0 / perfFreq.QuadPart;
      double totalMs = (double)(flushEnd.QuadPart - flushStart.QuadPart) * 1000.0 / perfFreq.QuadPart;
      perfRecordFlush(savedDrawOps, savedCopyOps, savedCbvOff, savedSamplerOff, execMs, totalMs);
      static int flushTimingLog = 0;
      if (savedDrawOps > 0 && flushTimingLog < 120) {
        logMessage(LogLevel::Info, "FLUSH#%d exec=%.1fms wait=%.1fms total=%.1fms cbvOff=%u draws=%u flushThisFrame=%d",
          flushCount, execMs, waitMs, totalMs, savedCbvOff, savedDrawOps, flushesThisFrame);
        flushTimingLog++;
      }
    }
#endif
  }

  void D3D12BackendContext::synchronize() {
    HitchScope hitch("backend synchronize");
#ifdef _WIN32
    ContextLock ctxLock(*this);
    flush();
    if (m_deviceRemoved)
      return;

    // Wait for GPU
    if (m_fence->GetCompletedValue() < m_fenceValue) {
      HRESULT waitHr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
      if (FAILED(waitHr)) {
        logMessage(LogLevel::Error, "Fence SetEventOnCompletion failed in synchronize (hr=0x%08x)", waitHr);
        logDeviceRemovedReason(m_device, "FenceSetEventOnCompletion/synchronize");
      }
      DWORD waitResult = WaitForSingleObject(m_fenceEvent, 10000);
      if (waitResult == WAIT_TIMEOUT) {
        logMessage(LogLevel::Error, "synchronize: GPU fence wait TIMED OUT (10s) — setting deviceRemoved");
        m_deviceRemoved = true;
        return;
      }
      if (waitResult != WAIT_OBJECT_0) {
        logMessage(LogLevel::Error, "WaitForSingleObject failed in synchronize (result=0x%08x)", static_cast<uint32_t>(waitResult));
      }
    }

    logDeviceRemovedReason(m_device, "synchronize");
    releaseCompletedUploads();
#endif
  }

  void D3D12BackendContext::closeForSubmission() {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_closedForSubmission) return;

    suspendActiveQueries();
    flushBarriers();

    // Move pending uploads to in-flight (tagged with next fence)
    if (!m_pendingUploads.empty()) {
      uint64_t nextFence = m_fenceValue + 1;
      for (auto& buf : m_pendingUploads) {
        m_inFlightUploads.push_back(FencedUpload{nextFence, std::move(buf)});
      }
      m_pendingUploads.clear();
      m_pendingUploadBytes = 0;
    }

    if (m_gpuProfiler.enabled())
      m_gpuProfiler.beforeClose(m_cmdList.raw());
    m_cmdList->Close();
    m_closedForSubmission = true;
#endif
  }

  void D3D12BackendContext::submitRecordedCommands() {
#ifdef _WIN32
    if (!m_copyOnly)
      m_device->submitInitialData();
    ContextLock ctxLock(*this);
    if (!m_closedForSubmission) {
      closeForSubmission();
    }

    // Submit the closed command list, signal and wait for completion
    m_fenceValue++;
    {
      ID3D12CommandList* list = m_cmdList.raw();
      ID3D12Fence* fence = m_fence.Get();
      const uint64_t value = m_fenceValue;
      SubmitQueue* owner = &m_device->submitQueue();
      owner->enqueue([list, fence, value, owner](ID3D12CommandQueue* queue) {
        safeExecuteAndSignal(queue, list, fence, value, owner);
      });
    }
    m_submittedFence->store(m_fenceValue, std::memory_order_release);
    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    decayPromotedImageStates();

    // Release in-flight resources (full sync done, safe to clear all)
    m_inFlightUploads.clear();
    m_inFlightRetired.clear();
    for (auto& fenced : m_inFlightSlices)
      m_device->freeSlices(fenced.slices);
    m_inFlightSlices.clear();
    releaseAllResourceBatches();
    // submitRecordedCommands() can reset a context without going through the
    // normal allocator-ring reset path. Advance the retention generation so a
    // reused context cannot mistake released resource references for current
    // ones, and force its next safety snapshot to rebuild the batch ownership.
    if (++m_resourceBatchGeneration == 0)
      m_resourceBatchGeneration = 1;
    m_boundResourceSnapshotGeneration = 0;
    m_currentPSO = nullptr;  // PSO might have been freed

    // Reset for potential reuse
    m_cmdList.ResetAllocator(m_currentAllocator.Get());
    m_cmdList->Reset(m_currentAllocator.Get(), nullptr);
    m_closedForSubmission = false;
    m_predicationApplied = false;
    resumeActiveQueries();
    applyPredication();
    m_copyOpsSinceFlush = 0;
    m_drawOpsSinceFlush = 0;
    m_occlusionEndsSinceFlush = 0;
    m_uploadScratchOffsets[m_currentAllocatorIndex] = 0;
#endif
  }

  // 32-bit processes: a warning in the log when the address space runs
  // short (2 GB without large-address awareness): allocations fail then,
  // and textures or buffers stay broken. Again for every 128 MB less.
  void D3D12BackendContext::warnLowAddressSpace() {
#ifdef _WIN32
    MEMORYSTATUSEX status = { sizeof(status) };
    if (!GlobalMemoryStatusEx(&status))
      return;
    const uint64_t freeMb = status.ullAvailVirtual >> 20;
    // Process-wide (every device's worker checks): the one that lowers the
    // mark warns.
    static std::atomic<uint64_t> s_warnedBelow{ 384 };
    uint64_t mark = s_warnedBelow.load(std::memory_order_relaxed);
    if (freeMb >= mark || !s_warnedBelow.compare_exchange_strong(mark, freeMb / 128 * 128, std::memory_order_relaxed))
      return;
    size_t largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (char* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)); p = static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize) {
      if (mbi.State == MEM_FREE)
        largest = (std::max)(largest, size_t(mbi.RegionSize));
      if (static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize < p)
        break;
    }
    // 2 GB in all: the executable is not large address aware.
    const bool twoGb = status.ullTotalVirtual < (3ull << 30);
    logMessage(LogLevel::Warn, "glitch: address space low: %llu MB of %llu MB free, largest free block %zu MB (32-bit process%s)",
      static_cast<unsigned long long>(freeMb), static_cast<unsigned long long>(status.ullTotalVirtual >> 20), largest >> 20,
      twoGb ? "; DXUP Manager: Memory, Address space, 4 GB instead of 2 GB" : "");
#endif
  }

  void D3D12BackendContext::presentImage(IBackendImage* backBuffer) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.enabled() && !m_closedForSubmission) {
      m_gpuProfiler.onPresent(m_cmdList.raw(), m_fenceValue + 1);
      if (m_gpuProfiler.wantsResults())
        m_gpuProfiler.writeResults(m_fence->GetCompletedValue());
    }
    static uint64_t presentCount = 0;
    const uint64_t presentIndex = presentCount++;
    const bool logPresent = infoDiagnosticsEnabled()
      && (presentIndex < 3 || (presentIndex % 300) == 0);
    m_device->trimIdleMemory();
    if (kSmallAddressSpace && presentIndex % 300 == 0)
      warnLowAddressSpace();
    if (memstats::enabled()) {
      m_descriptorsFrameMax = (std::max)(m_descriptorsFrameMax, m_descriptorsTaken - m_descriptorsAtPresent);
      m_descriptorsAtPresent = m_descriptorsTaken;
    }
    if (memstats::enabled() && presentIndex % 600 == 0) {
      const auto pipelines = m_device->pipelineCounts();
      logMessage(LogLevel::Warn, "  pipelines: %zu graphics, %zu compute", pipelines.first, pipelines.second);
      logMessage(LogLevel::Warn, "  shader-visible descriptors: %llu a frame on average, at most %llu (heap %u)",
        static_cast<unsigned long long>(m_descriptorsTaken / (presentIndex + 1)),
        static_cast<unsigned long long>(m_descriptorsFrameMax), m_cbvSrvUavHeapSize);
      m_descriptorsFrameMax = 0;
      ComPtr<IDXGIAdapter3> adapter3;
      DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
      auto* adapter = m_device->getAdapter();
      if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
        adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);
      logMessage(LogLevel::Warn, "memory at present %llu: process %llu MB; %s",
        static_cast<unsigned long long>(presentIndex), static_cast<unsigned long long>(localInfo.CurrentUsage >> 20),
        memstats::report().c_str());
      logMessage(LogLevel::Warn, "  upload slices: %s", m_device->getUploadSuballocator().describe().c_str());
      logMessage(LogLevel::Warn, "  default slices: %s", m_device->getDefaultSuballocator().describe().c_str());
      if (presentIndex == 3000)
        logMessage(LogLevel::Warn, "  large resources:%s", memstats::listLive().c_str());
    }

    if (backBuffer) {
      auto* d3d12Image = static_cast<D3D12BackendImage*>(backBuffer);

      // DXGI video-memory queries cross the driver boundary. Sample them only
      // when verbose diagnostics are explicitly enabled.
      if (logPresent) {
        ComPtr<IDXGIAdapter3> adapter3;
        auto* adapter = m_device->getAdapter();
        if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
          DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {}, nonLocalInfo = {};
          adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);
          adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo);
          logMessage(LogLevel::Info,
            "presentImage #%llu: GPU memory LOCAL: budget=%lluMB usage=%lluMB avail=%lluMB | NON-LOCAL: budget=%lluMB usage=%lluMB",
            static_cast<unsigned long long>(presentIndex),
            localInfo.Budget / (1024*1024), localInfo.CurrentUsage / (1024*1024),
            (localInfo.Budget > localInfo.CurrentUsage) ? (localInfo.Budget - localInfo.CurrentUsage) / (1024*1024) : 0,
            nonLocalInfo.Budget / (1024*1024), nonLocalInfo.CurrentUsage / (1024*1024));
        }
      }

      if (logPresent) {
        logMessage(LogLevel::Info, "presentImage #%llu: backBuffer=%p res=%p state=0x%x drawOps=%u cbvSrvUavOff=%u samplerOff=%u",
          static_cast<unsigned long long>(presentIndex), backBuffer, d3d12Image->getResource(),
          static_cast<uint32_t>(d3d12Image->getCurrentState()),
          m_drawOpsSinceFlush, m_cbvSrvUavHeapOffset, m_samplerDescriptorsInBatch);
      }

      DXUP_RECENT_OP(
        "presentImage backBuffer=%p res=%p state=%u",
        backBuffer,
        d3d12Image->getResource(),
        static_cast<uint32_t>(d3d12Image->getCurrentState()));
      holdBatchResource(d3d12Image->getResource());
      transitionResource(d3d12Image, D3D12_RESOURCE_STATE_PRESENT);
      // A buffer drawn into directly is unbound, as flip model Presents do
      // in Direct3D 11: the next command list must not bind it again (one
      // list writes one buffer of a swap chain), the next draw binds the
      // next buffer.
      if (d3d12Image->isRetargetable()) {
        bool bound = false;
        for (uint32_t i = 0; i < m_currentRenderTargetCount; i++)
          bound |= m_currentRenderTargetResources[i] == d3d12Image->getResource();
        if (bound) {
          m_currentRenderTargetResources.fill(nullptr);
          m_currentRenderTargetCount = 0;
          m_currentDepthStencilResource = nullptr;
          for (auto& handle : m_trackedRTVs)
            handle = {};
          m_trackedRTVCount = 0;
          m_trackedDSV = {};
          m_trackedDSVBound = false;
          m_trackedOmBindingValid = false;
          m_outputMergerRebindRequired.store(true, std::memory_order_relaxed);
        }
      }
    } else {
      if (logPresent)
        logMessage(LogLevel::Info, "presentImage #%llu: backBuffer=null",
          static_cast<unsigned long long>(presentIndex));
    }
    flush();

    // Full GPU sync at frame boundary to ensure all in-flight command lists
    // are done before the app can release resources referenced by them.
    double presentWaitMs = 0.0;
    bool asyncReset = m_commandAllocatorCount > 1 && !m_copyOnly;
    if (!asyncReset && m_fence && m_fenceValue > 0 && m_fence->GetCompletedValue() < m_fenceValue) {
      bool measureWait = perfStatsEnabled() && !m_copyOnly;
      LARGE_INTEGER waitStart = {}, waitEnd = {}, waitFreq = {};
      if (measureWait) {
        QueryPerformanceFrequency(&waitFreq);
        QueryPerformanceCounter(&waitStart);
      }
      m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 10000);
      if (measureWait) {
        QueryPerformanceCounter(&waitEnd);
        presentWaitMs = waitFreq.QuadPart > 0
          ? static_cast<double>(waitEnd.QuadPart - waitStart.QuadPart) * 1000.0 / static_cast<double>(waitFreq.QuadPart)
          : 0.0;
      }
    }
    releaseCompletedUploads();
    perfRecordPresent(presentWaitMs);

    // flush() signaled the fence after this frame's work, if it had any.
    m_presentFenceValues[m_presentCount % kPresentFenceHistory] = m_fenceValue;
    m_presentCount++;
    hoistFrameEnd();

    if (logPresent)
      logMessage(LogLevel::Info, "presentImage #%llu: flush() returned OK",
        static_cast<unsigned long long>(presentIndex));
#endif
  }

  uint64_t D3D12BackendContext::getLastPresentFenceValue() const {
#ifdef _WIN32
    return m_presentCount ? m_presentFenceValues[(m_presentCount - 1) % kPresentFenceHistory] : 0;
#else
    return 0;
#endif
  }

  void D3D12BackendContext::waitForFenceValue(uint64_t value) {
#ifdef _WIN32
    if (!value || !m_fence || m_fence->GetCompletedValue() >= value)
      return;
    if (!m_frameLatencyEvent)
      m_frameLatencyEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!m_frameLatencyEvent || FAILED(m_fence->SetEventOnCompletion(value, m_frameLatencyEvent)))
      return;
    if (WaitForSingleObject(m_frameLatencyEvent, 10000) == WAIT_TIMEOUT)
      logMessage(LogLevel::Error, "waitForFenceValue: GPU fence wait timed out (fence=%llu)",
        static_cast<unsigned long long>(value));
#else
    (void)value;
#endif
  }

  void D3D12BackendContext::waitForFrameLatency(uint32_t maxLatency) {
#ifdef _WIN32
    uint64_t targetFence = 0;
    {
      ContextLock ctxLock(*this);
      if (!maxLatency || m_copyOnly || !m_fence || m_deviceRemoved)
        return;
      if (maxLatency > kPresentFenceHistory)
        maxLatency = kPresentFenceHistory;

      // Frames newer than the last maxLatency presents may stay in flight;
      // the one presented maxLatency frames before the latest must be done.
      if (m_presentCount <= maxLatency)
        return;
      const uint64_t frame = m_presentCount - 1 - maxLatency;
      targetFence = m_presentFenceValues[frame % kPresentFenceHistory];
      if (!targetFence || m_fence->GetCompletedValue() >= targetFence)
        return;

      if (!m_frameLatencyEvent)
        m_frameLatencyEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
      if (!m_frameLatencyEvent)
        return;
      if (FAILED(m_fence->SetEventOnCompletion(targetFence, m_frameLatencyEvent)))
        return;
    }

    // Wait without the context lock, like a native Present that blocks on
    // queued frames, so other users of this context are not held up.
    if (WaitForSingleObject(m_frameLatencyEvent, 10000) == WAIT_TIMEOUT)
      logMessage(LogLevel::Error, "waitForFrameLatency: GPU fence wait timed out (fence=%llu)",
        static_cast<unsigned long long>(targetFence));
#else
    (void)maxLatency;
#endif
  }

  // ============================================================
  // Clear
  // ============================================================

  void D3D12BackendContext::clearRenderTarget(IBackendImageView* rtv, const float color[4]) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onClear();
    auto* d3d12View = static_cast<D3D12BackendImageView*>(rtv);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());

    DXUP_RECENT_OP(
      "ClearRTV res=%p fmt=%u color=%.3f,%.3f,%.3f,%.3f",
      d3d12Image->getResource(),
      static_cast<uint32_t>(d3d12Image->getFormat()),
      color[0],
      color[1],
      color[2],
      color[3]);
    transitionResource(d3d12Image, d3d12View, D3D12_RESOURCE_STATE_RENDER_TARGET);
    holdBatchResource(d3d12Image->getResource());
    flushBarriers();

    m_cmdList->ClearRenderTargetView(d3d12View->getRtvHandle(), color,
      static_cast<UINT>(m_clearRects.size()), m_clearRects.empty() ? nullptr : m_clearRects.data());
#endif
  }

  void D3D12BackendContext::clearView(IBackendImageView* view, bool renderTarget, const float color[4],
                                      uint32_t rectCount, const Rect* rects) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onClear();
    if (!view)
      return;
    m_clearRects.clear();
    for (uint32_t i = 0; i < rectCount && rects; i++)
      m_clearRects.push_back({ rects[i].left, rects[i].top, rects[i].right, rects[i].bottom });
    // An empty rect list after dropping nothing still means no rects.
    if (rectCount && m_clearRects.empty())
      return;
    if (renderTarget)
      clearRenderTarget(view, color);
    else
      clearUnorderedAccessViewFloat(view, color);
    m_clearRects.clear();
#else
    (void)view; (void)renderTarget; (void)color; (void)rectCount; (void)rects;
#endif
  }

  void D3D12BackendContext::clearDepthStencil(IBackendImageView* dsv, float depth, uint8_t stencil, uint32_t flags) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onClear();
    auto* d3d12View = static_cast<D3D12BackendImageView*>(dsv);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());

    DXUP_RECENT_OP("ClearDSV dsv=%p img=%p fmt=%u flags=0x%x", dsv, d3d12Image, static_cast<uint32_t>(d3d12Image->getFormat()), flags);
    transitionResource(d3d12Image, d3d12View, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    holdBatchResource(d3d12Image->getResource());
    flushBarriers();

    D3D12_CLEAR_FLAGS d3d12Flags = {};
    if (flags & static_cast<uint32_t>(ClearFlags::Depth))
      d3d12Flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (flags & static_cast<uint32_t>(ClearFlags::Stencil))
      d3d12Flags |= D3D12_CLEAR_FLAG_STENCIL;

    m_cmdList->ClearDepthStencilView(d3d12View->getDsvHandle(), d3d12Flags, depth, stencil, 0, nullptr);
#endif
  }

  void D3D12BackendContext::clearUnorderedAccessViewUint(IBackendImageView* view, const uint32_t values[4]) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!view) return;
    // Its descriptors before any state is set (see ensureDescriptorHeapSpace).
    ensureDescriptorHeapSpace();
    auto* d3d12View = static_cast<D3D12BackendImageView*>(view);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());

    if (!d3d12View->getUavHandle().ptr) {
      logMessage(LogLevel::Warn, "clearUnorderedAccessViewUint: image view has no UAV handle");
      return;
    }

    DXUP_RECENT_OP("ClearUAVuint(image) res=%p vals=%u,%u,%u,%u", d3d12Image->getResource(), values[0], values[1], values[2], values[3]);

    transitionResource(d3d12Image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Clears write through the view like a draw or dispatch would.
    orderUavAccess(d3d12Image->getResource(), dxbc::kUavWrite);
    holdBatchResource(d3d12Image->getResource());
    flushBarriers();

    // Copy CPU UAV descriptor into shader-visible heap to get a GPU handle
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDst = allocateShaderVisibleCbvSrvUav();
    uint32_t heapOffset = m_cbvSrvUavHeapOffset - 1;
    m_device->getDevice()->CopyDescriptorsSimple(1, cpuDst, d3d12View->getUavHandle(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    gpuHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr + heapOffset * m_cbvSrvUavDescSize;

    m_cmdList->ClearUnorderedAccessViewUint(gpuHandle, d3d12View->getUavHandle(), d3d12Image->getResource(), values, 0, nullptr);
#endif
  }

  void D3D12BackendContext::clearUnorderedAccessViewUint(IBackendBufferView* view, const uint32_t values[4]) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!view) return;
    // Its descriptors before any state is set (see ensureDescriptorHeapSpace).
    ensureDescriptorHeapSpace();
    auto* d3d12View = static_cast<D3D12BackendBufferView*>(view);
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(d3d12View->getBuffer());

    if (!d3d12View->getUavHandle().ptr) {
      logMessage(LogLevel::Warn, "clearUnorderedAccessViewUint: buffer view has no UAV handle");
      return;
    }

    DXUP_RECENT_OP("ClearUAVuint(buffer) res=%p vals=%u,%u,%u,%u", d3d12Buffer->getResource(), values[0], values[1], values[2], values[3]);

    // Structured buffers are cleared through a raw view of the same bytes,
    // which writes values[0] to every 32-bit word as D3D11 does.
    const auto& viewDesc = d3d12View->getDesc();
    const bool structured = viewDesc.format == DXGI_FORMAT_UNKNOWN && !viewDesc.raw;
    const D3D12_CPU_DESCRIPTOR_HANDLE clearHandle = structured ? d3d12View->getRawClearUavHandle() : d3d12View->getUavHandle();
    if (!clearHandle.ptr)
      return;
    const uint32_t rawValues[4] = { values[0], values[0], values[0], values[0] };

    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Clears write through the view like a draw or dispatch would.
    orderUavAccess(d3d12Buffer->getResource(), dxbc::kUavWrite);
    holdBatchResource(d3d12Buffer->getResource());
    flushBarriers();

    D3D12_CPU_DESCRIPTOR_HANDLE cpuDst = allocateShaderVisibleCbvSrvUav();
    uint32_t heapOffset = m_cbvSrvUavHeapOffset - 1;
    m_device->getDevice()->CopyDescriptorsSimple(1, cpuDst, clearHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    gpuHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr + heapOffset * m_cbvSrvUavDescSize;

    m_cmdList->ClearUnorderedAccessViewUint(gpuHandle, clearHandle, d3d12Buffer->getResource(),
      structured ? rawValues : values, 0, nullptr);
#endif
  }

  void D3D12BackendContext::clearUnorderedAccessViewFloat(IBackendImageView* view, const float values[4]) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!view) return;
    // Its descriptors before any state is set (see ensureDescriptorHeapSpace).
    ensureDescriptorHeapSpace();
    auto* d3d12View = static_cast<D3D12BackendImageView*>(view);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());

    if (!d3d12View->getUavHandle().ptr) {
      logMessage(LogLevel::Warn, "clearUnorderedAccessViewFloat: image view has no UAV handle");
      return;
    }

    DXUP_RECENT_OP("ClearUAVfloat(image) res=%p vals=%.3f,%.3f,%.3f,%.3f", d3d12Image->getResource(), values[0], values[1], values[2], values[3]);

    transitionResource(d3d12Image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Clears write through the view like a draw or dispatch would.
    orderUavAccess(d3d12Image->getResource(), dxbc::kUavWrite);
    holdBatchResource(d3d12Image->getResource());
    flushBarriers();

    D3D12_CPU_DESCRIPTOR_HANDLE cpuDst = allocateShaderVisibleCbvSrvUav();
    uint32_t heapOffset = m_cbvSrvUavHeapOffset - 1;
    m_device->getDevice()->CopyDescriptorsSimple(1, cpuDst, d3d12View->getUavHandle(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    gpuHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr + heapOffset * m_cbvSrvUavDescSize;

    m_cmdList->ClearUnorderedAccessViewFloat(gpuHandle, d3d12View->getUavHandle(), d3d12Image->getResource(), values,
      static_cast<UINT>(m_clearRects.size()), m_clearRects.empty() ? nullptr : m_clearRects.data());
#endif
  }

  void D3D12BackendContext::clearUnorderedAccessViewFloat(IBackendBufferView* view, const float values[4]) {
    clearBufferView(view, values, 0, nullptr);
  }

  void D3D12BackendContext::clearBufferView(IBackendBufferView* view, const float values[4], uint32_t rectCount,
                                            const Rect* rects) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!view) return;
    // Its descriptors before any state is set (see ensureDescriptorHeapSpace).
    ensureDescriptorHeapSpace();
    // Element ranges; empty ones clear nothing.
    std::vector<D3D12_RECT> clearRects;
    for (uint32_t i = 0; i < rectCount; i++)
      if (rects[i].right > rects[i].left)
        clearRects.push_back({ rects[i].left, 0, rects[i].right, 1 });
    if (rectCount && clearRects.empty())
      return;
    auto* d3d12View = static_cast<D3D12BackendBufferView*>(view);
    auto* d3d12Buffer = static_cast<D3D12BackendBuffer*>(d3d12View->getBuffer());

    if (!d3d12View->getUavHandle().ptr) {
      logMessage(LogLevel::Warn, "clearUnorderedAccessViewFloat: buffer view has no UAV handle");
      return;
    }

    DXUP_RECENT_OP("ClearUAVfloat(buffer) res=%p vals=%.3f,%.3f,%.3f,%.3f", d3d12Buffer->getResource(), values[0], values[1], values[2], values[3]);

    transitionResource(d3d12Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Clears write through the view like a draw or dispatch would.
    orderUavAccess(d3d12Buffer->getResource(), dxbc::kUavWrite);
    holdBatchResource(d3d12Buffer->getResource());
    flushBarriers();

    D3D12_CPU_DESCRIPTOR_HANDLE cpuDst = allocateShaderVisibleCbvSrvUav();
    uint32_t heapOffset = m_cbvSrvUavHeapOffset - 1;
    m_device->getDevice()->CopyDescriptorsSimple(1, cpuDst, d3d12View->getUavHandle(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    gpuHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr + heapOffset * m_cbvSrvUavDescSize;

    m_cmdList->ClearUnorderedAccessViewFloat(gpuHandle, d3d12View->getUavHandle(), d3d12Buffer->getResource(), values,
      UINT(clearRects.size()), clearRects.empty() ? nullptr : clearRects.data());
#endif
  }

  // ============================================================
  // Misc
  // ============================================================

#ifdef _WIN32
  void D3D12BackendContext::initMipGenResources() {
#ifdef _WIN32
    if (m_mipGenInitialized) return;
    m_mipGenInitialized = true;

    auto* device = m_device->getDevice();

    // Root signature: 1 SRV descriptor table (t0), the normalized depth of
    // the rendered 3D slice (b0) and a static linear sampler (s0)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace      = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[1].Constants.ShaderRegister = 0;
    rootParams[1].Constants.Num32BitValues = 1;
    rootParams[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter           = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.ShaderRegister   = 0;
    staticSampler.RegisterSpace    = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    staticSampler.MaxLOD           = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters     = 2;
    rootSigDesc.pParameters       = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers   = &staticSampler;
    rootSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
      serialized.ReleaseAndGetAddressOf(), errorBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "MipGen root sig serialize failed: 0x%08x %s",
        hr, errorBlob ? (const char*)errorBlob->GetBufferPointer() : "");
      return;
    }

    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
      IID_PPV_ARGS(&m_mipGenRootSignature));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "MipGen root sig creation failed: 0x%08x", hr);
      return;
    }

    // Compile fullscreen triangle VS
    static const char kMipGenVsSrc[] =
      "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
      "  uv = float2((id << 1) & 2, id & 2);\n"
      "  pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
      "  uv.y = 1.0 - uv.y;\n"
      "}\n";

    hr = D3DCompile(kMipGenVsSrc, sizeof(kMipGenVsSrc) - 1, "mipgen_vs", nullptr, nullptr,
      "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
      m_mipGenVsBytecode.ReleaseAndGetAddressOf(), errorBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "MipGen VS compile failed: 0x%08x %s",
        hr, errorBlob ? (const char*)errorBlob->GetBufferPointer() : "");
      return;
    }

    // Linear downsampling: pixel centres of the smaller mip fall between
    // two (2D: 2x2, 3D: 2x2x2) texels of the larger one.
    static const char kMipGenPsSrc[] =
      "#if DIM == 0\n"
      "Texture1DArray<float4> srcTex : register(t0);\n"
      "#elif DIM == 1\n"
      "Texture2DArray<float4> srcTex : register(t0);\n"
      "#else\n"
      "Texture3D<float4> srcTex : register(t0);\n"
      "#endif\n"
      "SamplerState samp : register(s0);\n"
      "cbuffer slice : register(b0) { float sliceDepth; };\n"
      "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
      "#if DIM == 0\n"
      "  return srcTex.SampleLevel(samp, float2(uv.x, 0.0), 0.0);\n"
      "#elif DIM == 1\n"
      "  return srcTex.SampleLevel(samp, float3(uv, 0.0), 0.0);\n"
      "#else\n"
      "  return srcTex.SampleLevel(samp, float3(uv, sliceDepth), 0.0);\n"
      "#endif\n"
      "}\n";

    static const char* const kDimensions[] = { "0", "1", "2" };
    for (uint32_t dim = 0; dim < 3; dim++) {
      const D3D_SHADER_MACRO defines[] = { { "DIM", kDimensions[dim] }, { nullptr, nullptr } };
      hr = D3DCompile(kMipGenPsSrc, sizeof(kMipGenPsSrc) - 1, "mipgen_ps", defines, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        m_mipGenPsBytecode[dim].ReleaseAndGetAddressOf(), errorBlob.ReleaseAndGetAddressOf());
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "MipGen PS compile failed: 0x%08x %s",
          hr, errorBlob ? (const char*)errorBlob->GetBufferPointer() : "");
        m_mipGenRootSignature.Reset();
        return;
      }
    }

    logMessage(LogLevel::Info, "MipGen resources initialized successfully");
#endif
  }

  ID3D12PipelineState* D3D12BackendContext::getOrCreateMipGenPso(DXGI_FORMAT format, uint32_t dimension) {
#ifdef _WIN32
    const uint32_t key = static_cast<uint32_t>(format) | (dimension << 16);
    auto it = m_mipGenPsoCache.find(key);
    if (it != m_mipGenPsoCache.end())
      return it->second.Get();

    if (!m_mipGenVsBytecode || !m_mipGenPsBytecode[dimension] || !m_mipGenRootSignature)
      return nullptr;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_mipGenRootSignature.Get();
    psoDesc.VS = { m_mipGenVsBytecode->GetBufferPointer(), m_mipGenVsBytecode->GetBufferSize() };
    psoDesc.PS = { m_mipGenPsBytecode[dimension]->GetBufferPointer(), m_mipGenPsBytecode[dimension]->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = format;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_device->getDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "MipGen PSO creation failed for fmt=%u: 0x%08x",
        static_cast<uint32_t>(format), hr);
      return nullptr;
    }

    logMessage(LogLevel::Info, "MipGen PSO created for fmt=%u pso=%p", static_cast<uint32_t>(format), pso.Get());

    ID3D12PipelineState* raw = pso.Get();
    m_mipGenPsoCache[key] = std::move(pso);
    return raw;
#else
    return nullptr;
#endif
  }
#endif

  void D3D12BackendContext::generateMips(IBackendImageView* srv) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    if (!srv) return;
    // Its descriptors before any state is set (see ensureDescriptorHeapSpace).
    ensureDescriptorHeapSpace();

    auto* d3d12View = static_cast<D3D12BackendImageView*>(srv);
    auto* d3d12Image = static_cast<D3D12BackendImage*>(d3d12View->getImage());
    const auto& imageDesc = d3d12Image->getDesc();
    const auto& viewDesc = d3d12View->getDesc();
    holdBatchResource(d3d12Image->getResource());

    // The mips of the view: its most detailed one is the source of the rest.
    const uint32_t baseMip = viewDesc.baseMipLevel;
    if (baseMip >= imageDesc.mipLevels)
      return;
    const uint32_t dimension = imageDesc.type == ImageType::Tex1D ? 0u
      : imageDesc.type == ImageType::Tex3D ? 2u : 1u;
    uint32_t mipCount = viewDesc.mipLevelCount ? viewDesc.mipLevelCount : imageDesc.mipLevels - baseMip;
    mipCount = (std::min)(mipCount, imageDesc.mipLevels - baseMip);
    if (mipCount <= 1) return;

    initMipGenResources();
    if (!m_mipGenRootSignature) return;

    DXGI_FORMAT rtvFormat = viewDesc.format != DXGI_FORMAT_UNKNOWN ? viewDesc.format : imageDesc.format;
    ID3D12PipelineState* pso = getOrCreateMipGenPso(rtvFormat, dimension);
    if (!pso) return;

    auto* device = m_device->getDevice();
    if (!m_mipGenRtv.ptr)
      m_mipGenRtv = m_device->getDescriptorHeapManager().allocateRtv();

    const uint32_t layers = dimension == 2 ? 1u : (imageDesc.arraySize ? imageDesc.arraySize : 1u);
    const uint32_t baseArrayLayer = (std::min)(viewDesc.baseArrayLayer, layers - 1);
    uint32_t arrayCount = viewDesc.arrayLayerCount ? viewDesc.arrayLayerCount : layers - baseArrayLayer;
    arrayCount = (std::min)(arrayCount, layers - baseArrayLayer);

    m_cmdList->SetPipelineState(pso);
    m_cmdList->SetGraphicsRootSignature(m_mipGenRootSignature.Get());
    invalidateRootTableCache();
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* heaps[] = {
      m_shaderVisibleCbvSrvUavHeap.Get(),
      m_shaderVisibleSamplerHeap.Get()
    };
    m_cmdList->SetDescriptorHeaps(2, heaps);

    for (uint32_t arraySlice = 0; arraySlice < arrayCount; arraySlice++) {
      const uint32_t layer = baseArrayLayer + arraySlice;
      for (uint32_t mip = 1; mip < mipCount; mip++) {
        const uint32_t srcMip = baseMip + mip - 1;
        const uint32_t dstMip = baseMip + mip;
        const uint32_t dstWidth  = (std::max)(imageDesc.width >> dstMip, 1u);
        const uint32_t dstHeight = dimension == 0 ? 1u : (std::max)(imageDesc.height >> dstMip, 1u);
        const uint32_t dstDepth  = dimension == 2 ? (std::max)(imageDesc.depth >> dstMip, 1u) : 1u;

        ImageSubresource srcSub = { srcMip, layer, 1 };
        ImageSubresource dstSub = { dstMip, layer, 1 };
        transitionResource(d3d12Image, srcSub, kShaderReadState);
        transitionResource(d3d12Image, dstSub, D3D12_RESOURCE_STATE_RENDER_TARGET);
        flushBarriers();

        // Array views select the layer for plain, array and cube textures.
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = rtvFormat;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (dimension == 0) {
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
          srvDesc.Texture1DArray.MostDetailedMip = srcMip;
          srvDesc.Texture1DArray.MipLevels = 1;
          srvDesc.Texture1DArray.FirstArraySlice = layer;
          srvDesc.Texture1DArray.ArraySize = 1;
        } else if (dimension == 1) {
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          srvDesc.Texture2DArray.MostDetailedMip = srcMip;
          srvDesc.Texture2DArray.MipLevels = 1;
          srvDesc.Texture2DArray.FirstArraySlice = layer;
          srvDesc.Texture2DArray.ArraySize = 1;
        } else {
          srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
          srvDesc.Texture3D.MostDetailedMip = srcMip;
          srvDesc.Texture3D.MipLevels = 1;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpuSrvHandle = allocateShaderVisibleCbvSrvUav();
        const uint32_t srvHeapIndex = m_cbvSrvUavHeapOffset - 1;
        device->CreateShaderResourceView(d3d12Image->getResource(), &srvDesc, cpuSrvHandle);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuSrvHandle;
        gpuSrvHandle.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr
          + srvHeapIndex * m_cbvSrvUavDescSize;

        D3D12_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(dstWidth);
        viewport.Height = static_cast<float>(dstHeight);
        viewport.MaxDepth = 1.0f;
        m_cmdList->RSSetViewports(1, &viewport);
        D3D12_RECT scissor = { 0, 0, static_cast<LONG>(dstWidth), static_cast<LONG>(dstHeight) };
        m_cmdList->RSSetScissorRects(1, &scissor);
        m_cmdList->SetGraphicsRootDescriptorTable(0, gpuSrvHandle);

        // 3D mips are rendered one depth slice at a time.
        for (uint32_t z = 0; z < dstDepth; z++) {
          D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
          rtvDesc.Format = rtvFormat;
          if (dimension == 0) {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
            rtvDesc.Texture1DArray.MipSlice = dstMip;
            rtvDesc.Texture1DArray.FirstArraySlice = layer;
            rtvDesc.Texture1DArray.ArraySize = 1;
          } else if (dimension == 1) {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.MipSlice = dstMip;
            rtvDesc.Texture2DArray.FirstArraySlice = layer;
            rtvDesc.Texture2DArray.ArraySize = 1;
          } else {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            rtvDesc.Texture3D.MipSlice = dstMip;
            rtvDesc.Texture3D.FirstWSlice = z;
            rtvDesc.Texture3D.WSize = 1;
          }
          // Render target views are read when bound: one descriptor serves all.
          m_cmdList.drain();
          device->CreateRenderTargetView(d3d12Image->getResource(), &rtvDesc, m_mipGenRtv);
          m_cmdList->OMSetRenderTargets(1, &m_mipGenRtv, FALSE, nullptr);

          const float sliceDepth = (static_cast<float>(z) + 0.5f) / static_cast<float>(dstDepth);
          uint32_t sliceBits;
          std::memcpy(&sliceBits, &sliceDepth, sizeof(sliceBits));
          m_cmdList->SetGraphicsRoot32BitConstant(1, sliceBits, 0);
          m_cmdList->DrawInstanced(3, 1, 0, 0);
        }
      }
    }

    // Give the application its state back.
    restoreAfterInternalPass();
#endif
  }

  ID3D12PipelineState* D3D12BackendContext::getOrCreateMsaaPassPso(MsaaPass pass, DXGI_FORMAT format, uint32_t samples,
                                                                    uint32_t srcSamples) {
#ifdef _WIN32
    const bool finite = pass == MsaaPass::ResolveFinite;
    uint32_t sampleIndex = 0;
    while (finite && sampleIndex < 4 && (2u << sampleIndex) <= srcSamples)
      sampleIndex++;
    const uint64_t key = (uint64_t(sampleIndex) << 48) | (uint64_t(pass) << 40) | (uint64_t(samples) << 32)
                       | uint64_t(format);
    auto it = m_msaaPassPsoCache.find(key);
    if (it != m_msaaPassPsoCache.end())
      return it->second.Get();

    const uint32_t index = static_cast<uint32_t>(pass);
    ComPtr<ID3DBlob>& blob = finite ? m_msaaFinitePs[sampleIndex] : m_msaaPassPs[index];
    if (!blob) {
      static const char* const kSources[] = {
        "Texture2D<float4> src : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { return src.Load(int3(pos.xy, 0)); }\n",
        "Texture2D<float> src : register(t0);\n"
        "float main(float4 pos : SV_Position) : SV_Depth { return src.Load(int3(pos.xy, 0)); }\n",
        "Texture2DMS<float> src : register(t0);\n"
        "float main(float4 pos : SV_Position) : SV_Depth { return src.Load(int2(pos.xy), 0); }\n",
        "Texture2DMS<float4> src : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { return src.Load(int2(pos.xy), 0); }\n",
        "Texture2DMS<float4> src : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "  float4 sum = 0.0f, count = 0.0f;\n"
        "  [unroll] for (uint i = 0; i < SAMPLES; i++) {\n"
        "    const float4 value = src.Load(int2(pos.xy), i);\n"
        "    const bool4 finite = isfinite(value);\n"
        "    sum += finite ? value : 0.0f;\n"
        "    count += finite ? 1.0f : 0.0f;\n"
        "  }\n"
        "  return count > 0.0f ? sum / max(count, 1.0f) : src.Load(int2(pos.xy), 0);\n"
        "}\n",
      };
      static const char* const kSampleCounts[] = { "1", "2", "4", "8", "16" };
      const D3D_SHADER_MACRO macros[] = { { "SAMPLES", kSampleCounts[sampleIndex] }, { nullptr, nullptr } };
      ComPtr<ID3DBlob> errors;
      const HRESULT hr = D3DCompile(kSources[index], strlen(kSources[index]), "msaa_pass_ps", macros, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.ReleaseAndGetAddressOf(),
        errors.ReleaseAndGetAddressOf());
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "forced MSAA: pass shader compile failed: 0x%08x %s", hr,
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return nullptr;
      }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_mipGenRootSignature.Get();
    psoDesc.VS = { m_mipGenVsBytecode->GetBufferPointer(), m_mipGenVsBytecode->GetBufferSize() };
    psoDesc.PS = { blob->GetBufferPointer(), blob->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = samples;
    if (pass == MsaaPass::UpsampleColor || pass == MsaaPass::ResolveColor || pass == MsaaPass::ResolveFinite) {
      psoDesc.NumRenderTargets = 1;
      psoDesc.RTVFormats[0] = format;
    } else {
      // Depth written as it is: always passes, stencil untouched.
      psoDesc.DSVFormat = format;
      psoDesc.DepthStencilState.DepthEnable = TRUE;
      psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = m_device->getDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "forced MSAA: pass PSO failed: pass=%u fmt=%u samples=%u hr=0x%08x",
        index, static_cast<uint32_t>(format), samples, hr);
      return nullptr;
    }
    ID3D12PipelineState* result = pso.Get();
    m_msaaPassPsoCache[key] = std::move(pso);
    return result;
#else
    (void)pass; (void)format; (void)samples; (void)srcSamples;
    return nullptr;
#endif
  }

  void D3D12BackendContext::runMsaaPass(MsaaPass pass, D3D12BackendImage* dst, D3D12BackendImage* src,
                                        DXGI_FORMAT format, DXGI_FORMAT readFormat) {
#ifdef _WIN32
    if (!dst || !src || m_deviceRemoved)
      return;
    initMipGenResources();
    if (!m_mipGenRootSignature)
      return;
    // Its descriptors before any state is set (see ensureDescriptorHeapSpace).
    ensureDescriptorHeapSpace();
    const auto& dstDesc = dst->getDesc();
    const auto& srcDesc = src->getDesc();
    ID3D12PipelineState* pso = getOrCreateMsaaPassPso(pass, format, dstDesc.sampleCount ? dstDesc.sampleCount : 1u,
                                                      srcDesc.sampleCount ? srcDesc.sampleCount : 1u);
    if (!pso)
      return;
    if (m_gpuProfiler.capturing()) {
      static const char* const names[] = { "upsample color", "upsample depth", "resolve depth", "resolve color",
                                           "resolve finite" };
      m_gpuProfiler.onInternal(m_cmdList.raw(), names[(std::min)(static_cast<uint32_t>(pass), 4u)],
        (pass == MsaaPass::UpsampleColor || pass == MsaaPass::UpsampleDepth ? dst : src)->getResource(), format);
    }
    holdBatchResource(dst->getResource());
    holdBatchResource(src->getResource());
    const bool depth = pass == MsaaPass::UpsampleDepth || pass == MsaaPass::ResolveDepth;
    transitionResource(src, kShaderReadState);
    transitionResource(dst, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
    flushBarriers();

    auto* device = m_device->getDevice();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = readFormat;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (srcDesc.sampleCount > 1) {
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Texture2D.MipLevels = 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv = allocateShaderVisibleCbvSrvUav();
    const uint32_t srvIndex = m_cbvSrvUavHeapOffset - 1;
    device->CreateShaderResourceView(src->getResource(), &srvDesc, cpuSrv);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv;
    gpuSrv.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr
      + srvIndex * m_cbvSrvUavDescSize;

    if (depth) {
      if (!m_msaaPassDsv.ptr)
        m_msaaPassDsv = m_device->getDescriptorHeapManager().allocateDsv();
      D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
      dsvDesc.Format = format;
      dsvDesc.ViewDimension = dstDesc.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
      m_cmdList.drain();
      device->CreateDepthStencilView(dst->getResource(), &dsvDesc, m_msaaPassDsv);
      m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_msaaPassDsv);
    } else {
      if (!m_mipGenRtv.ptr)
        m_mipGenRtv = m_device->getDescriptorHeapManager().allocateRtv();
      D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
      rtvDesc.Format = format;
      rtvDesc.ViewDimension = dstDesc.sampleCount > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
      m_cmdList.drain();
      device->CreateRenderTargetView(dst->getResource(), &rtvDesc, m_mipGenRtv);
      m_cmdList->OMSetRenderTargets(1, &m_mipGenRtv, FALSE, nullptr);
    }

    m_cmdList->SetPipelineState(pso);
    m_cmdList->SetGraphicsRootSignature(m_mipGenRootSignature.Get());
    invalidateRootTableCache();
    ID3D12DescriptorHeap* heaps[] = { m_shaderVisibleCbvSrvUavHeap.Get(), m_shaderVisibleSamplerHeap.Get() };
    m_cmdList->SetDescriptorHeaps(2, heaps);
    m_cmdList->SetGraphicsRootDescriptorTable(0, gpuSrv);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(dstDesc.width);
    viewport.Height = static_cast<float>(dstDesc.height);
    viewport.MaxDepth = 1.0f;
    m_cmdList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(dstDesc.width), static_cast<LONG>(dstDesc.height) };
    m_cmdList->RSSetScissorRects(1, &scissor);
    m_cmdList->DrawInstanced(3, 1, 0, 0);

    // Give the application its state back.
    restoreAfterInternalPass();
    m_copyOpsSinceFlush++;
#else
    (void)pass; (void)dst; (void)src; (void)format; (void)readFormat;
#endif
  }

  ID3D12PipelineState* D3D12BackendContext::getOrCreateStencilPassPso(DXGI_FORMAT format, uint32_t samples,
                                                                      bool msSource, uint32_t bit) {
#ifdef _WIN32
    const uint64_t key = (uint64_t(bit) << 48) | (uint64_t(msSource) << 40) | (uint64_t(samples) << 32)
                       | uint64_t(format);
    auto it = m_stencilPassPsoCache.find(key);
    if (it != m_stencilPassPsoCache.end())
      return it->second.Get();

    ComPtr<ID3DBlob>& blob = m_stencilPassPs[msSource ? 1 : 0];
    if (!blob) {
      static const char* const kSources[] = {
        "Texture2D<uint2> src : register(t0);\n"
        "cbuffer bits : register(b0) { uint bit; };\n"
        "void main(float4 pos : SV_Position) { if (!(src.Load(int3(pos.xy, 0)).y & bit)) discard; }\n",
        "Texture2DMS<uint2> src : register(t0);\n"
        "cbuffer bits : register(b0) { uint bit; };\n"
        "void main(float4 pos : SV_Position) { if (!(src.Load(int2(pos.xy), 0).y & bit)) discard; }\n",
      };
      const char* source = kSources[msSource ? 1 : 0];
      ComPtr<ID3DBlob> errors;
      const HRESULT hr = D3DCompile(source, strlen(source), "stencil_pass_ps", nullptr, nullptr, "main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "forced MSAA: stencil pass shader compile failed: 0x%08x %s", hr,
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return nullptr;
      }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_mipGenRootSignature.Get();
    psoDesc.VS = { m_mipGenVsBytecode->GetBufferPointer(), m_mipGenVsBytecode->GetBufferSize() };
    psoDesc.PS = { blob->GetBufferPointer(), blob->GetBufferSize() };
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = samples;
    psoDesc.DSVFormat = format;
    // Depth untouched; the bit set (reference 0xff) where the shader keeps
    // the pixel.
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = TRUE;
    psoDesc.DepthStencilState.StencilReadMask = 0xff;
    psoDesc.DepthStencilState.StencilWriteMask = static_cast<UINT8>(1u << bit);
    const D3D12_DEPTH_STENCILOP_DESC face = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE,
                                              D3D12_COMPARISON_FUNC_ALWAYS };
    psoDesc.DepthStencilState.FrontFace = face;
    psoDesc.DepthStencilState.BackFace = face;
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = m_device->getDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "forced MSAA: stencil pass PSO failed: fmt=%u samples=%u hr=0x%08x",
        static_cast<uint32_t>(format), samples, hr);
      return nullptr;
    }
    ID3D12PipelineState* result = pso.Get();
    m_stencilPassPsoCache[key] = std::move(pso);
    return result;
#else
    (void)format; (void)samples; (void)msSource; (void)bit;
    return nullptr;
#endif
  }

  bool D3D12BackendContext::resolveStencilPlane(D3D12BackendImage* dst, D3D12BackendImage* src, DXGI_FORMAT readFormat) {
#ifdef _WIN32
    if (m_stencilResolveFailed || src->getDesc().sampleCount <= 1 || dst->getDesc().sampleCount > 1
     || (readFormat != DXGI_FORMAT_X32_TYPELESS_G8X24_UINT && readFormat != DXGI_FORMAT_X24_TYPELESS_G8_UINT))
      return false;
    auto* device = m_device->getDevice();
    const D3D12_RESOURCE_DESC dstDesc = dst->getResource()->GetDesc();
    // Plane 1, mip 0, slice 0.
    const UINT subresource = dstDesc.MipLevels * dstDesc.DepthOrArraySize;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    device->GetCopyableFootprints(&dstDesc, subresource, 1, 0, &layout, &rows, &rowSize, &total);
    const DXGI_FORMAT copyFormat = layout.Footprint.Format;
    if ((copyFormat != DXGI_FORMAT_R8_TYPELESS && copyFormat != DXGI_FORMAT_R8_UINT) || !total || total == UINT64(-1)
     || layout.Footprint.RowPitch % 4)
      return false;

    if (!m_stencilResolveRootSignature) {
      D3D12_DESCRIPTOR_RANGE range = {};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 1;
      D3D12_ROOT_PARAMETER params[3] = {};
      params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[0].DescriptorTable.NumDescriptorRanges = 1;
      params[0].DescriptorTable.pDescriptorRanges = &range;
      params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[2].Constants.Num32BitValues = 4;
      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = 3;
      desc.pParameters = params;
      ComPtr<ID3DBlob> serialized, errors;
      HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(),
        errors.GetAddressOf());
      if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
          IID_PPV_ARGS(&m_stencilResolveRootSignature));
      // Four pixels' stencil bytes per thread.
      static const char kSource[] =
        "Texture2DMS<uint2> src : register(t0);\n"
        "RWByteAddressBuffer dst : register(u0);\n"
        "cbuffer size : register(b0) { uint width; uint height; uint pitch; uint unused; };\n"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
        "  const uint x = id.x * 4;\n"
        "  if (x >= width || id.y >= height) return;\n"
        "  uint bytes = 0;\n"
        "  [unroll] for (uint i = 0; i < 4; i++) {\n"
        "    if (x + i < width)\n"
        "      bytes |= (src.Load(int2(x + i, id.y), 0).y & 0xff) << (8 * i);\n"
        "  }\n"
        "  dst.Store(id.y * pitch + x, bytes);\n"
        "}\n";
      ComPtr<ID3DBlob> bytecode;
      if (SUCCEEDED(hr))
        hr = D3DCompile(kSource, sizeof(kSource) - 1, "stencil_resolve_cs", nullptr, nullptr, "main", "cs_5_0",
          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
      if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_stencilResolveRootSignature.Get();
        pso.CS = { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_stencilResolvePso));
      }
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "forced MSAA: stencil resolve pass failed: 0x%08x %s", static_cast<unsigned>(hr),
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        m_stencilResolveFailed = true;
        return false;
      }
    }
    if (m_stencilResolveBufferSize < total) {
      if (m_stencilResolveBuffer)
        holdBatchResource(m_stencilResolveBuffer.Get());
      m_stencilResolveBuffer.Reset();
      m_stencilResolveBufferSize = 0;
      D3D12_HEAP_PROPERTIES heap = {};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC buffer = {};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = total;
      buffer.Height = 1;
      buffer.DepthOrArraySize = 1;
      buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_stencilResolveBuffer))))
        return false;
      m_stencilResolveBufferSize = total;
      // Between resolves the buffer waits as a copy source.
      D3D12_RESOURCE_BARRIER start = {};
      start.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      start.Transition.pResource = m_stencilResolveBuffer.Get();
      start.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      start.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      start.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      m_cmdList->ResourceBarrier(1, &start);
    }

    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onInternal(m_cmdList.raw(), "resolve stencil", src->getResource(), dstDesc.Format);
    ensureDescriptorHeapSpace();
    holdBatchResource(dst->getResource());
    holdBatchResource(src->getResource());
    holdBatchResource(m_stencilResolveBuffer.Get());
    transitionResource(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    flushBarriers();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_stencilResolveBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_cmdList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = readFormat;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv = allocateShaderVisibleCbvSrvUav();
    const uint32_t srvIndex = m_cbvSrvUavHeapOffset - 1;
    device->CreateShaderResourceView(src->getResource(), &srvDesc, cpuSrv);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv;
    gpuSrv.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr
      + srvIndex * m_cbvSrvUavDescSize;
    const uint32_t width = layout.Footprint.Width, height = layout.Footprint.Height;
    const uint32_t constants[4] = { width, height, layout.Footprint.RowPitch, 0 };
    ID3D12DescriptorHeap* heaps[] = { m_shaderVisibleCbvSrvUavHeap.Get(), m_shaderVisibleSamplerHeap.Get() };
    m_cmdList->SetDescriptorHeaps(2, heaps);
    m_cmdList->SetComputeRootSignature(m_stencilResolveRootSignature.Get());
    m_cmdList->SetPipelineState(m_stencilResolvePso.Get());
    m_cmdList->SetComputeRootDescriptorTable(0, gpuSrv);
    m_cmdList->SetComputeRootUnorderedAccessView(1, m_stencilResolveBuffer->GetGPUVirtualAddress());
    m_cmdList->SetComputeRoot32BitConstants(2, 4, constants, 0);
    m_cmdList->Dispatch(((width + 3) / 4 + 7) / 8, (height + 7) / 8, 1);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_cmdList->ResourceBarrier(1, &barrier);
    transitionResource(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    flushBarriers();
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = dst->getResource();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = m_stencilResolveBuffer.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = layout;
    m_cmdList->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    restoreAfterForeignCommands();
    m_copyOpsSinceFlush++;
    return true;
#else
    (void)dst; (void)src; (void)readFormat;
    return false;
#endif
  }

  void D3D12BackendContext::copyStencil(IBackendImage* dstImage, IBackendImage* srcImage, DXGI_FORMAT format,
                                        DXGI_FORMAT readFormat) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* dst = static_cast<D3D12BackendImage*>(dstImage);
    auto* src = static_cast<D3D12BackendImage*>(srcImage);
    if (!dst || !src || m_deviceRemoved)
      return;
    // Its descriptors before any state is set (see ensureDescriptorHeapSpace).
    ensureDescriptorHeapSpace();
    if (resolveStencilPlane(dst, src, readFormat))
      return;
    initMipGenResources();
    if (!m_mipGenRootSignature)
      return;
    const auto& dstDesc = dst->getDesc();
    const auto& srcDesc = src->getDesc();
    const uint32_t samples = dstDesc.sampleCount ? dstDesc.sampleCount : 1u;
    const bool msSource = srcDesc.sampleCount > 1;
    ID3D12PipelineState* psos[8];
    for (uint32_t bit = 0; bit < 8; bit++) {
      psos[bit] = getOrCreateStencilPassPso(format, samples, msSource, bit);
      if (!psos[bit])
        return;
    }
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onInternal(m_cmdList.raw(), msSource ? "resolve stencil" : "upsample stencil",
        (msSource ? src : dst)->getResource(), format);
    holdBatchResource(dst->getResource());
    holdBatchResource(src->getResource());
    transitionResource(src, kShaderReadState);
    transitionResource(dst, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    flushBarriers();

    auto* device = m_device->getDevice();
    // The stencil plane: by the view format in a multisampled view, by
    // PlaneSlice in a single-sampled one.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = readFormat;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (msSource) {
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Texture2D.MipLevels = 1;
      srvDesc.Texture2D.PlaneSlice = 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv = allocateShaderVisibleCbvSrvUav();
    const uint32_t srvIndex = m_cbvSrvUavHeapOffset - 1;
    device->CreateShaderResourceView(src->getResource(), &srvDesc, cpuSrv);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv;
    gpuSrv.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr
      + srvIndex * m_cbvSrvUavDescSize;

    if (!m_msaaPassDsv.ptr)
      m_msaaPassDsv = m_device->getDescriptorHeapManager().allocateDsv();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = format;
    dsvDesc.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    m_cmdList.drain();
    device->CreateDepthStencilView(dst->getResource(), &dsvDesc, m_msaaPassDsv);
    m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_msaaPassDsv);
    m_cmdList->ClearDepthStencilView(m_msaaPassDsv, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);

    m_cmdList->SetGraphicsRootSignature(m_mipGenRootSignature.Get());
    invalidateRootTableCache();
    ID3D12DescriptorHeap* heaps[] = { m_shaderVisibleCbvSrvUavHeap.Get(), m_shaderVisibleSamplerHeap.Get() };
    m_cmdList->SetDescriptorHeaps(2, heaps);
    m_cmdList->SetGraphicsRootDescriptorTable(0, gpuSrv);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(dstDesc.width);
    viewport.Height = static_cast<float>(dstDesc.height);
    viewport.MaxDepth = 1.0f;
    m_cmdList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(dstDesc.width), static_cast<LONG>(dstDesc.height) };
    m_cmdList->RSSetScissorRects(1, &scissor);
    m_cmdList->OMSetStencilRef(0xff);
    for (uint32_t bit = 0; bit < 8; bit++) {
      m_cmdList->SetPipelineState(psos[bit]);
      m_cmdList->SetGraphicsRoot32BitConstant(1, 1u << bit, 0);
      m_cmdList->DrawInstanced(3, 1, 0, 0);
    }

    // Give the application its state back.
    m_cmdList->OMSetStencilRef(m_trackedStencilRef);
    restoreAfterInternalPass();
    m_copyOpsSinceFlush++;
#else
    (void)dstImage; (void)srcImage; (void)format; (void)readFormat;
#endif
  }

  void D3D12BackendContext::upsampleImage(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format,
                                          DXGI_FORMAT readFormat) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    const bool depth = readFormat != format;
    runMsaaPass(depth ? MsaaPass::UpsampleDepth : MsaaPass::UpsampleColor,
      static_cast<D3D12BackendImage*>(dst), static_cast<D3D12BackendImage*>(src), format, readFormat);
#else
    (void)dst; (void)src; (void)format; (void)readFormat;
#endif
  }

  void D3D12BackendContext::resolveSampleZero(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format,
                                              DXGI_FORMAT readFormat) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    const bool depth = readFormat != format;
    runMsaaPass(depth ? MsaaPass::ResolveDepth : MsaaPass::ResolveColor, static_cast<D3D12BackendImage*>(dst),
      static_cast<D3D12BackendImage*>(src), format, readFormat);
#else
    (void)dst; (void)src; (void)format; (void)readFormat;
#endif
  }

  void D3D12BackendContext::resolveFinite(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    runMsaaPass(MsaaPass::ResolveFinite, static_cast<D3D12BackendImage*>(dst), static_cast<D3D12BackendImage*>(src),
      format, format);
#else
    (void)dst; (void)src; (void)format;
#endif
  }

  void D3D12BackendContext::resolveSubresource(IBackendImage* dst, uint32_t dstSubres,
                                                IBackendImage* src, uint32_t srcSubres, DXGI_FORMAT format) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* d3d12Dst = static_cast<D3D12BackendImage*>(dst);
    auto* d3d12Src = static_cast<D3D12BackendImage*>(src);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onInternal(m_cmdList.raw(), "resolve", d3d12Src->getResource(), format);

    DXUP_RECENT_OP("ResolveSubresource dst=%p src=%p fmt=%u dstSub=%u srcSub=%u dstSamples=%u srcSamples=%u",
      d3d12Dst,
      d3d12Src,
      static_cast<uint32_t>(format),
      dstSubres,
      srcSubres,
      d3d12Dst->getDesc().sampleCount,
      d3d12Src->getDesc().sampleCount);
    transitionSubresource(d3d12Dst, dstSubres, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    transitionSubresource(d3d12Src, srcSubres, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    holdBatchResource(d3d12Dst->getResource());
    holdBatchResource(d3d12Src->getResource());
    flushBarriers();

    m_cmdList->ResolveSubresource(d3d12Dst->getResource(), dstSubres,
                                   d3d12Src->getResource(), srcSubres, format);
#endif
  }

  // ============================================================
  // Internal Helpers
  // ============================================================

#ifdef _WIN32
  void D3D12BackendContext::recordRecentOp(const char* fmt, ...) {
    if (!m_recentOpsEnabled)
      return;

    auto& slot = m_recentOps[m_recentOpWriteIndex];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(slot.data(), slot.size(), fmt, args);
    va_end(args);

    m_recentOpWriteIndex = (m_recentOpWriteIndex + 1) % m_recentOps.size();
    if (m_recentOpCount < m_recentOps.size())
      m_recentOpCount++;
  }

  void D3D12BackendContext::transitionResource(D3D12BackendBuffer* buffer, D3D12_RESOURCE_STATES newState) {
    // Upload heap resources must stay in GENERIC_READ; buffers in custom
    // heaps are tracked like default ones.
    if (buffer->hasFixedState())
      return;
    noteSharedUse(buffer);
    // The initial data context writes slices of default blocks on another
    // thread than the one that tracks their state: buffers start in COMMON
    // at every submission and get to COPY_DEST by promotion.
    if (m_copyOnly && buffer->hasSharedState())
      return;

    D3D12_RESOURCE_STATES currentState = buffer->getCurrentState();
    D3D12_RESOURCE_STATES targetState = mergeBufferReadStates(currentState, newState);
    if (currentState == targetState)
      return;

    static int mergedBufferStateLogCount = 0;
    if (targetState != newState && mergedBufferStateLogCount < 64) {
      logMessage(LogLevel::Info,
        "bufferStateMerge: buffer=%p prevState=0x%x requestedState=0x%x mergedState=0x%x",
        buffer,
        static_cast<uint32_t>(currentState),
        static_cast<uint32_t>(newState),
        static_cast<uint32_t>(targetState));
      mergedBufferStateLogCount++;
    }

    // Record rollback so we can revert if Close() fails
    m_bufferStateRollback.push_back({buffer, currentState});

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = buffer->getResource();
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter  = targetState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    holdBatchResource(buffer->getResource());
    m_pendingBarriers.push_back(barrier);
    buffer->setCurrentState(targetState);
  }

  void D3D12BackendContext::markImageForCommonDecay(
      D3D12BackendImage*      image,
      uint32_t                subresource,
      D3D12_RESOURCE_STATES   promotedState) {
    if (!image || !commonPromotionEnabled())
      return;

    if (subresource >= image->getSubresourceCount())
      return;

    if (!image->allowsSimultaneousAccess() && !isImageReadOnlyPromotableState(promotedState))
      return;

    auto existing = std::find_if(
      m_commonDecaySubresources.begin(),
      m_commonDecaySubresources.end(),
      [image, subresource] (const CommonDecayEntry& entry) {
        return entry.image == image && entry.subresource == subresource;
      });
    if (existing == m_commonDecaySubresources.end())
      m_commonDecaySubresources.push_back({image, subresource});
  }

  void D3D12BackendContext::decayPromotedImageStates() {
    if (m_commonDecaySubresources.empty())
      return;

    for (const auto& entry : m_commonDecaySubresources) {
      if (entry.image) {
        markOutputMergerRebindRequired(entry.image->getResource());
        entry.image->setSubresourceState(entry.subresource, D3D12_RESOURCE_STATE_COMMON);
      }
    }
    m_commonDecaySubresources.clear();
  }

  void D3D12BackendContext::markOutputMergerRebindRequired(ID3D12Resource* resource) {
    if (!resource)
      return;

    if (resource == m_currentDepthStencilResource) {
      m_outputMergerRebindRequired.store(true, std::memory_order_relaxed);
      return;
    }

    for (uint32_t i = 0; i < m_currentRenderTargetCount; i++) {
      if (resource == m_currentRenderTargetResources[i]) {
        m_outputMergerRebindRequired.store(true, std::memory_order_relaxed);
        return;
      }
    }
  }

  bool D3D12BackendContext::transitionWholeImage(D3D12BackendImage* image, D3D12_RESOURCE_STATES newState) {
    noteSharedUse(image);
    noteLearnedImage(image);
    if (!image->hasUniformState())
      return false;
    const D3D12_RESOURCE_STATES currentState = image->getCurrentState();
    if (currentState == newState)
      return true;
    if (currentState == D3D12_RESOURCE_STATE_COMMON && isImageCommonPromotableState(image, newState))
      return false;

    markOutputMergerRebindRequired(image->getResource());
    holdBatchResource(image->getResource());
    m_stateRollback.push_back({image, UINT_MAX, currentState});

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = image->getResource();
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter  = newState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_pendingBarriers.push_back(barrier);
    image->setCurrentState(newState);
    markBoundImageStatesDirty();
    return true;
  }

  namespace {
    // A typed format of a typeless one for a view that clears to zero
    // (zero has the same bits in every type of a format family).
    DXGI_FORMAT zeroClearFormat(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_UINT;
        case DXGI_FORMAT_R32G32B32_TYPELESS:    return DXGI_FORMAT_R32G32B32_UINT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UINT;
        case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_UINT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UINT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UINT;
        case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_UINT;
        case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_UINT;
        case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UINT;
        case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UINT;
        case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UINT;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:                                return format;
      }
    }
  }

  void D3D12BackendContext::commitLazyImage(D3D12BackendImage* image) {
    if (!m_device->commitLazyImage(image))
      return;
    // Newly mapped tiles hold undefined contents and, on GPUs that compress
    // render targets, undefined compression data: D3D12 wants a reserved
    // render target cleared, discarded or copied over before anything else
    // uses it. Zero matches the zeroed memory of a new D3D11 resource.
    if (m_copyOnly)
      return;
    transitionResource(image, D3D12_RESOURCE_STATE_RENDER_TARGET);
    flushBarriers();
    ID3D12Resource* resource = image->getResource();
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT format = zeroClearFormat(desc.Format);
    if (!m_lazyClearRtv.ptr)
      m_lazyClearRtv = m_device->getDescriptorHeapManager().allocateRtv();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    // The descriptor is read when the clear is recorded: one serves all.
    m_cmdList.drain();
    m_device->getDevice()->CreateRenderTargetView(resource, &rtvDesc, m_lazyClearRtv);
    const float zero[4] = {};
    const bool paused = pausePredication();
    m_cmdList->ClearRenderTargetView(m_lazyClearRtv, zero, 0, nullptr);
    resumePredication(paused);
  }

  void D3D12BackendContext::transitionResource(D3D12BackendImage* image, D3D12_RESOURCE_STATES newState) {
    if (image->isStaging()) {
      transitionResource(image->getStagingBuffer(), newState);
      return;
    }
    if (image->isLazyUncommitted() && (newState & kImageWriteStates))
      commitLazyImage(image);
    if (memstats::enabled() && (newState & kImageWriteStates))
      memstats::noteWritten(image->getResource());
    noteSharedUse(image);
    noteLearnedImage(image);
    if (image->hasUniformState() && image->getCurrentState() == newState)
      return;
    if (transitionWholeImage(image, newState))
      return;

    markOutputMergerRebindRequired(image->getResource());
    holdBatchResource(image->getResource());

    // Subresources in different states (or implicitly promoted from
    // COMMON) transition one by one.
    for (uint32_t subresource = 0; subresource < image->getSubresourceCount(); subresource++) {
      D3D12_RESOURCE_STATES currentState = image->getSubresourceState(subresource);
      if (currentState == newState)
        continue;

      m_stateRollback.push_back({image, subresource, currentState});

      if (currentState == D3D12_RESOURCE_STATE_COMMON
       && isImageCommonPromotableState(image, newState)) {
        image->setSubresourceState(subresource, newState);
        markImageForCommonDecay(image, subresource, newState);
        continue;
      }

      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource   = image->getResource();
      barrier.Transition.StateBefore = currentState;
      barrier.Transition.StateAfter  = newState;
      barrier.Transition.Subresource = subresource;

      m_pendingBarriers.push_back(barrier);
    }

    image->setCurrentState(newState);
    markBoundImageStatesDirty();
  }

  void D3D12BackendContext::transitionSubresource(D3D12BackendImage* image, uint32_t subresource, D3D12_RESOURCE_STATES newState) {
    if (image->isStaging()) {
      transitionResource(image->getStagingBuffer(), newState);
      return;
    }
    if (image->isLazyUncommitted() && (newState & kImageWriteStates))
      commitLazyImage(image);
    if (memstats::enabled() && (newState & kImageWriteStates))
      memstats::noteWritten(image->getResource());
    noteSharedUse(image);
    noteLearnedImage(image);
    if (subresource >= image->getSubresourceCount()) {
      transitionResource(image, newState);
      return;
    }

    D3D12_RESOURCE_STATES currentState = image->getSubresourceState(subresource);
    if (currentState == newState)
      return;

    markOutputMergerRebindRequired(image->getResource());
    holdBatchResource(image->getResource());

    // Record rollback
    m_stateRollback.push_back({image, subresource, currentState});

    if (currentState == D3D12_RESOURCE_STATE_COMMON
     && isImageCommonPromotableState(image, newState)) {
      image->setSubresourceState(subresource, newState);
      markImageForCommonDecay(image, subresource, newState);
      return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = image->getResource();
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter  = newState;
    barrier.Transition.Subresource = subresource;

    m_pendingBarriers.push_back(barrier);
    image->setSubresourceState(subresource, newState);
    markBoundImageStatesDirty();
  }

  void D3D12BackendContext::transitionResource(
      D3D12BackendImage*         image,
      const ImageSubresource&    subresource,
      D3D12_RESOURCE_STATES      newState) {
    if (image->isStaging()) {
      transitionResource(image->getStagingBuffer(), newState);
      return;
    }
    if (image->isLazyUncommitted() && (newState & kImageWriteStates))
      commitLazyImage(image);
    if (memstats::enabled() && (newState & kImageWriteStates))
      memstats::noteWritten(image->getResource());
    const auto& imageDesc = image->getDesc();

    if (subresource.mipLevel >= image->getMipLevelCount())
      return;

    uint32_t baseArrayLayer = imageDesc.type == ImageType::Tex3D ? 0u : subresource.arrayLayer;
    if (baseArrayLayer >= image->getArraySubresourceCount())
      return;

    uint32_t arrayLayerCount = imageDesc.type == ImageType::Tex3D
      ? 1u
      : (subresource.arrayLayerCount ? subresource.arrayLayerCount : 1u);
    uint32_t remainingArrayLayers = image->getArraySubresourceCount() - baseArrayLayer;
    if (arrayLayerCount > remainingArrayLayers)
      arrayLayerCount = remainingArrayLayers;

    for (uint32_t plane = 0; plane < image->getPlaneCount(); plane++) {
      for (uint32_t arrayLayer = 0; arrayLayer < arrayLayerCount; arrayLayer++) {
        transitionSubresource(
          image,
          image->calcSubresource(subresource.mipLevel, baseArrayLayer + arrayLayer, plane),
          newState);
      }
    }
  }

  void D3D12BackendContext::transitionResource(
      D3D12BackendImage*             image,
      const D3D12BackendImageView*   view,
      D3D12_RESOURCE_STATES          newState) {
    if (!view) {
      transitionResource(image, newState);
      return;
    }
    if (image->isLazyUncommitted() && (newState & kImageWriteStates))
      commitLazyImage(image);
    if (memstats::enabled() && (newState & kImageWriteStates))
      memstats::noteWritten(image->getResource());
    noteSharedUse(image);

    // Every subresource is already in the requested state.
    if (image->hasUniformState() && image->getCurrentState() == newState)
      return;

    const auto& imageDesc = image->getDesc();
    const auto& viewDesc = view->getDesc();
    uint32_t mipCount = getImageViewMipCountForTransition(viewDesc, imageDesc);
    uint32_t arrayCount = getImageViewArrayCountForTransition(viewDesc, imageDesc);
    uint32_t planeStart = getImageViewPlaneStartForTransition(viewDesc, imageDesc);
    uint32_t planeCount = getImageViewPlaneCountForTransition(image, viewDesc, imageDesc);

    if (!mipCount || !arrayCount || planeStart >= image->getPlaneCount())
      return;

    uint32_t planeEnd = planeStart + planeCount;
    if (planeEnd > image->getPlaneCount())
      planeEnd = image->getPlaneCount();

    // A view of the whole image: one barrier when the state is uniform.
    const bool wholeImage = mipCount >= image->getMipLevelCount()
      && (imageDesc.type == ImageType::Tex3D || arrayCount >= image->getArraySubresourceCount())
      && planeStart == 0 && planeEnd == image->getPlaneCount();
    if (wholeImage && transitionWholeImage(image, newState))
      return;

    for (uint32_t plane = planeStart; plane < planeEnd; plane++) {
      for (uint32_t arrayLayer = 0; arrayLayer < arrayCount; arrayLayer++) {
        uint32_t subresourceArrayLayer = imageDesc.type == ImageType::Tex3D
          ? 0u
          : viewDesc.baseArrayLayer + arrayLayer;

        for (uint32_t mip = 0; mip < mipCount; mip++) {
          transitionSubresource(
            image,
            image->calcSubresource(viewDesc.baseMipLevel + mip, subresourceArrayLayer, plane),
            newState);
        }
      }
    }
  }

  void D3D12BackendContext::foldPendingBarriers() {
    auto& barriers = m_pendingBarriers;
    for (size_t i = 1; i < barriers.size(); i++) {
      auto& barrier = barriers[i];
      if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
        continue;
      ID3D12Resource* resource = barrier.Transition.pResource;
      // The latest barrier before it that concerns the same resource.
      for (size_t j = i; j-- > 0;) {
        auto& earlier = barriers[j];
        if (earlier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
          if (!earlier.UAV.pResource || earlier.UAV.pResource == resource)
            break;
          continue;
        }
        if (earlier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
          break;
        // Folded away already (or a no-op from the start).
        if (earlier.Transition.pResource != resource
         || earlier.Transition.StateBefore == earlier.Transition.StateAfter)
          continue;
        if (earlier.Transition.Subresource != barrier.Transition.Subresource) {
          // Whole-resource and single-subresource transitions stay apart.
          if (earlier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
           || barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
            break;
          continue;
        }
        if (earlier.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE
         && earlier.Transition.StateAfter == barrier.Transition.StateBefore) {
          earlier.Transition.StateAfter = barrier.Transition.StateAfter;
          barrier.Transition.StateBefore = barrier.Transition.StateAfter;
        }
        break;
      }
    }
    barriers.erase(std::remove_if(barriers.begin(), barriers.end(), [](const D3D12_RESOURCE_BARRIER& barrier) {
      return barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
          && barrier.Transition.StateBefore == barrier.Transition.StateAfter;
    }), barriers.end());
  }

  void D3D12BackendContext::flushBarriers() {
    if (m_pendingBarriers.empty())
      return;

    const bool collectPerfStats = m_perfStatsEnabled && !m_copyOnly;
    const bool collectBarrierDetails = collectPerfStats && m_barrierStatsEnabled;
    auto recordBarrierBatchStats = [&](const std::vector<D3D12_RESOURCE_BARRIER>& barriers) {
      if (m_gpuProfiler.capturing())
        m_gpuProfiler.onBarriers(barriers.data(), barriers.size());
      if (!collectPerfStats)
        return;

      m_perfIntervalBarrierCalls++;
      m_perfIntervalBarrierTransitions += static_cast<uint64_t>(barriers.size());

      if (!collectBarrierDetails)
        return;

      for (const auto& barrier : barriers) {
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
          continue;

        D3D12_RESOURCE_DESC desc = barrier.Transition.pResource->GetDesc();
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
          m_perfIntervalBarrierBufferTransitions++;
        else
          m_perfIntervalBarrierImageTransitions++;

        const D3D12_RESOURCE_STATES before = barrier.Transition.StateBefore;
        const D3D12_RESOURCE_STATES after = barrier.Transition.StateAfter;
        if (after & D3D12_RESOURCE_STATE_COPY_DEST)
          m_perfIntervalBarrierToCopyDest++;
        if (before & D3D12_RESOURCE_STATE_COPY_DEST)
          m_perfIntervalBarrierFromCopyDest++;
        if (after & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
          m_perfIntervalBarrierToSrv++;
        if (after & D3D12_RESOURCE_STATE_RENDER_TARGET)
          m_perfIntervalBarrierToRenderTarget++;
        if (after & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
          m_perfIntervalBarrierToUav++;
        if (after & (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_DEPTH_WRITE))
          m_perfIntervalBarrierToDepth++;
      }
    };

    // Transitions of one subresource that follow each other fold into one:
    // A->B then B->C becomes A->C, and A->A goes away. Every GPU command
    // flushes the pending barriers first, so nothing in between needed B.
    // Typical: a buffer block updated twice before the next draw.
    if (m_pendingBarriers.size() > 1)
      foldPendingBarriers();
    if (m_pendingBarriers.empty())
      return;
    if (!m_copyOnly && (accessTracking() || !m_hoistPlan.empty()))
      handleBarrierBatch();
    if (m_pendingBarriers.empty())
      return;

    // Most barrier flushes contain only unique transitions and can be submitted
    // directly from the retained pending vector. Avoid allocating and copying
    // two temporary vectors on that common draw/copy hot path.
    bool canSubmitDirectly = true;
    for (size_t i = 0; i < m_pendingBarriers.size() && canSubmitDirectly; i++) {
      const auto& barrier = m_pendingBarriers[i];
      if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        continue;
      if (barrier.Transition.StateBefore == barrier.Transition.StateAfter) {
        canSubmitDirectly = false;
        break;
      }

      for (size_t j = 0; j < i; j++) {
        const auto& previous = m_pendingBarriers[j];
        if (previous.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
         || previous.Transition.pResource != barrier.Transition.pResource)
          continue;
        if (previous.Transition.Subresource == barrier.Transition.Subresource
         || previous.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
         || barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
          canSubmitDirectly = false;
          break;
        }
      }
    }

    if (canSubmitDirectly) {
      DXUP_RECENT_OP("ResourceBarrier count=%u", static_cast<uint32_t>(m_pendingBarriers.size()));
      // Kept for the crash dump, which comes with the recent ops.
      if (m_recentOpsEnabled)
        m_emittedBarriers.insert(
          m_emittedBarriers.end(), m_pendingBarriers.begin(), m_pendingBarriers.end());
      recordBarrierBatchStats(m_pendingBarriers);
      safeResourceBarrier(
        &m_cmdList,
        static_cast<UINT>(m_pendingBarriers.size()),
        m_pendingBarriers.data());
      m_pendingBarriers.clear();
      return;
    }

    // D3D12 forbids multiple transitions for the same resource+subresource in a
    // single ResourceBarrier call. Split the barrier list into groups where each
    // resource+subresource appears at most once, and issue each group separately.
    // This handles cases like RT→SRV then SRV→RT for the same resource between
    // two draws in the same batch.
    {
      // Members, so splitting does not allocate.
      std::vector<D3D12_RESOURCE_BARRIER>& currentBatch = m_barrierBatch;
      currentBatch.clear();

      // Key: (resource ptr, subresource). Track which resources are in the current batch.
      std::vector<std::pair<ID3D12Resource*, UINT>>& batchResources = m_barrierBatchResources;
      batchResources.clear();

      for (auto& barrier : m_pendingBarriers) {
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
          // Skip no-op barriers
          if (barrier.Transition.StateBefore == barrier.Transition.StateAfter)
            continue;

          // Check if this resource+subresource is already in the current batch
          bool duplicate = false;
          auto key = std::make_pair(barrier.Transition.pResource, barrier.Transition.Subresource);
          for (const auto& existing : batchResources) {
            if (existing.first == key.first &&
                (existing.second == key.second ||
                 existing.second == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
                 key.second == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
              duplicate = true;
              break;
            }
          }

          if (duplicate) {
            // Flush current batch first
          if (!currentBatch.empty()) {
            DXUP_RECENT_OP("ResourceBarrier count=%u", static_cast<uint32_t>(currentBatch.size()));
            if (m_recentOpsEnabled)
              m_emittedBarriers.insert(m_emittedBarriers.end(), currentBatch.begin(), currentBatch.end());
            recordBarrierBatchStats(currentBatch);
            safeResourceBarrier(&m_cmdList,
              static_cast<UINT>(currentBatch.size()),
              currentBatch.data());
              currentBatch.clear();
              batchResources.clear();
            }
          }

          batchResources.push_back(key);
        }
        currentBatch.push_back(barrier);
      }

      // Flush remaining
      if (!currentBatch.empty()) {
        DXUP_RECENT_OP("ResourceBarrier count=%u", static_cast<uint32_t>(currentBatch.size()));
        if (m_recentOpsEnabled)
          m_emittedBarriers.insert(m_emittedBarriers.end(), currentBatch.begin(), currentBatch.end());
        recordBarrierBatchStats(currentBatch);
        safeResourceBarrier(&m_cmdList,
          static_cast<UINT>(currentBatch.size()),
          currentBatch.data());
      }
    }

    m_pendingBarriers.clear();
  }

  void D3D12BackendContext::resetCommandList() {
    HitchScope hitch("backend resetCommandList");
    bool asyncReset = m_commandAllocatorCount > 1;
    uint64_t waitFenceValue = m_fenceValue;
    bool waitedForGpu = false;

    if (asyncReset) {
      m_commandAllocatorFenceValues[m_currentAllocatorIndex] = m_fenceValue;
      uint32_t nextAllocatorIndex = (m_currentAllocatorIndex + 1) % m_commandAllocatorCount;
      waitFenceValue = m_commandAllocatorFenceValues[nextAllocatorIndex];
      m_currentAllocatorIndex = nextAllocatorIndex;
      m_currentAllocator = m_commandAllocators[m_currentAllocatorIndex];
    }

    if (waitFenceValue > 0 && m_fence->GetCompletedValue() < waitFenceValue) {
      waitedForGpu = true;
      bool measureWait = perfStatsEnabled() && !m_copyOnly;
      LARGE_INTEGER waitStart = {}, waitEnd = {}, waitFreq = {};
      if (measureWait) {
        QueryPerformanceFrequency(&waitFreq);
        QueryPerformanceCounter(&waitStart);
      }
      HRESULT waitHr = m_fence->SetEventOnCompletion(waitFenceValue, m_fenceEvent);
      if (FAILED(waitHr)) {
        logMessage(LogLevel::Error, "Fence SetEventOnCompletion failed in resetCommandList (hr=0x%08x)", waitHr);
        logDeviceRemovedReason(m_device, "FenceSetEventOnCompletion/resetCommandList");
      }
      DWORD waitResult = WaitForSingleObject(m_fenceEvent, 30000);
      if (measureWait) {
        QueryPerformanceCounter(&waitEnd);
        double waitMs = waitFreq.QuadPart > 0
          ? static_cast<double>(waitEnd.QuadPart - waitStart.QuadPart) * 1000.0 / static_cast<double>(waitFreq.QuadPart)
          : 0.0;
        perfRecordResetWait(waitMs);
      }
      if (waitResult == WAIT_TIMEOUT) {
        logMessage(LogLevel::Error, "resetCommandList: GPU fence wait TIMED OUT (30s) — setting deviceRemoved");
        m_deviceRemoved = true;
        return;
      }
      if (waitResult != WAIT_OBJECT_0) {
        logMessage(LogLevel::Error, "WaitForSingleObject failed in resetCommandList (result=0x%08x)", static_cast<uint32_t>(waitResult));
      }
    }

    // The successful post-execute check and queue Signal already covered the
    // no-wait path. Query the device again only after an actual blocking wait.
    if (waitedForGpu) {
      HRESULT removedHr = m_device->getDevice()->GetDeviceRemovedReason();
      if (FAILED(removedHr)) {
        m_deviceRemoved = true;
        logMessage(LogLevel::Error, "resetCommandList: DEVICE REMOVED after wait (hr=0x%08x)", removedHr);
        logDeviceRemovedReason(m_device, "resetCommandList/wait");
        return;
      }
    }

    // Release resources whose fences have completed. In the default path the
    // wait above completes all work; in async mode it only covers the allocator
    // slot being reused.
    releaseCompletedUploads();
    m_pendingUploads.clear();
    m_pendingUploadBytes = 0;

    if (m_device->submitQueue().threaded())
      nextCommandList();
    m_cmdList.ResetAllocator(m_currentAllocator.Get());
    HRESULT cmdListResetHr = m_cmdList->Reset(m_currentAllocator.Get(), nullptr);
    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onListBegin(m_cmdList.raw());
    if (FAILED(cmdListResetHr)) {
      logMessage(LogLevel::Error, "Command list Reset() failed (hr=0x%08x)", cmdListResetHr);
      logDeviceRemovedReason(m_device, "CommandList::Reset");
    }
    if (++m_resourceBatchGeneration == 0)
      m_resourceBatchGeneration = 1;

    // Reset recentOps so next batch's ops are isolated for TDR diagnosis
    m_recentOpCount = 0;
    m_recentOpWriteIndex = 0;

    // Re-set descriptor heaps (only if we have them - copy-only contexts don't)
    if (m_shaderVisibleCbvSrvUavHeap && m_shaderVisibleSamplerHeap) {
      ID3D12DescriptorHeap* heaps[] = {
        m_shaderVisibleCbvSrvUavHeap.Get(),
        m_shaderVisibleSamplerHeap.Get()
      };
      m_cmdList->SetDescriptorHeaps(2, heaps);
    }

    // Re-set root signatures
    if (m_currentRootSignature) {
      m_cmdList->SetGraphicsRootSignature(m_currentRootSignature);
    }
    if (m_computeRootSignature) {
      m_cmdList->SetComputeRootSignature(m_computeRootSignature);
    }

    // Descriptors of the submitted list stay where they are: the next list
    // goes on in the same partition, which records the submission's fence.
    // Without the ring, the wait above drained the GPU and the heap starts
    // over.
    if (asyncReset) {
      for (uint32_t i = 0; i < kDescriptorPartitionCount; i++) {
        if (m_partitionsInBatch & (1u << i))
          m_descriptorPartitionFences[i] = m_fenceValue;
      }
      m_partitionsInBatch = 1u << m_descriptorPartition;
      constexpr uint32_t kResetReserve = 16;
      if (m_cbvSrvUavHeapOffset + kResetReserve >= cbvSrvUavPartitionEnd()) {
        const uint32_t next = (m_descriptorPartition + 1) % descriptorPartitionCount();
        const uint64_t fenceValue = m_descriptorPartitionFences[next];
        if (fenceValue && m_fence->GetCompletedValue() < fenceValue) {
          m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
          WaitForSingleObject(m_fenceEvent, 10000);
        }
        m_descriptorPartition = next;
        m_partitionsInBatch = 1u << next;
        resetDescriptorHeapPartition();
      }
    } else {
      resetDescriptorHeapPartition();
    }
    m_uploadScratchOffsets[m_currentAllocatorIndex] = 0;
    // The GPU is done with this allocator's upload slot: one a large upload
    // grew goes (made again at its default size when needed).
    if (kSmallAddressSpace && m_uploadScratchCapacities[m_currentAllocatorIndex] > kDefaultUploadScratchCapacity) {
      if (m_uploadScratchMapped[m_currentAllocatorIndex])
        m_uploadScratchBuffers[m_currentAllocatorIndex]->Unmap(0, nullptr);
      m_uploadScratchBuffers[m_currentAllocatorIndex].Reset();
      m_uploadScratchMapped[m_currentAllocatorIndex] = nullptr;
      m_uploadScratchCapacities[m_currentAllocatorIndex] = 0;
    }
    initNullDescriptorsAndRootTables();

    markAllDescriptorStagesDirty();
    m_psoDirtyAfterFlush = true;  // Defer PSO re-bind to next draw/dispatch

    // A reset clears predication and ends nothing else; continue the
    // queries that were active when the previous list was submitted.
    m_predicationApplied = false;
    resumeActiveQueries();
    applyPredication();

    // Re-bind tracked dynamic state that was cleared by command list reset
    // PSO is deferred — SetPipelineState will be called before next draw/dispatch

    for (uint32_t i = 0; i < kMaxVertexBuffers; i++) {
      if (m_trackedVBs[i].bound)
        m_cmdList->IASetVertexBuffers(i, 1, &m_trackedVBs[i].view);
    }
    if (m_trackedIBBound)
      m_cmdList->IASetIndexBuffer(&m_trackedIB);

    if (m_trackedViewportCount)
      m_cmdList->RSSetViewports(m_trackedViewportCount, m_trackedViewports);
    if (m_trackedScissorCount)
      m_cmdList->RSSetScissorRects(m_trackedScissorCount, m_trackedScissors);

    m_cmdList->OMSetStencilRef(m_trackedStencilRef);
    m_cmdList->OMSetBlendFactor(m_trackedBlendFactor);

    if (m_trackedTopology != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
      m_cmdList->IASetPrimitiveTopology(m_trackedTopology);

    // Re-bind tracked render targets
    if (m_trackedRTVCount || m_trackedDSVBound) {
      m_cmdList->OMSetRenderTargets(
        m_trackedRTVCount,
        m_trackedRTVCount ? m_trackedRTVs : nullptr,
        FALSE,
        m_trackedDSVBound ? &m_trackedDSV : nullptr);
    }
    m_trackedOmBindingValid = true;

    if (m_soBufferCount)
      m_cmdList->SOSetTargets(0, m_soBufferCount, m_soViews);

    // Replayed bindings make the fresh command list reference the same native
    // resources even when D3D11 does not repeat its Set* calls. Retain one
    // reference per resource for this allocator generation now, before the app
    // can ClearState or release its D3D11 wrappers ahead of the next flush.
    snapshotBoundResources();

// Queries are no-op — no need to re-begin after reset
  }

  void D3D12BackendContext::initNullDescriptorsAndRootTables() {
    // Pre-set all root parameters with null descriptor tables.
    // D3D12 requires ALL root params to be set before any draw/dispatch.
    // Without this, commit functions that find no bindings skip setting
    // the root param, leaving it unset and causing GPU hangs (esp. AMD).
    invalidateRootTableCache();
    if (m_shaderVisibleCbvSrvUavHeap && m_shaderVisibleSamplerHeap) {
      auto* d3d12Device = m_device->getDevice();

      D3D12_CPU_DESCRIPTOR_HANDLE nullCbvCpu = allocateShaderVisibleCbvSrvUav();
      uint32_t nullCbvOff = m_cbvSrvUavHeapOffset - 1;
      D3D12_CONSTANT_BUFFER_VIEW_DESC nullCbvDesc = {};
      d3d12Device->CreateConstantBufferView(&nullCbvDesc, nullCbvCpu);

      D3D12_CPU_DESCRIPTOR_HANDLE nullSrvCpu = allocateShaderVisibleCbvSrvUav();
      uint32_t nullSrvOff = m_cbvSrvUavHeapOffset - 1;
      D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
      nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      nullSrvDesc.Texture2D.MipLevels = 1;
      d3d12Device->CreateShaderResourceView(nullptr, &nullSrvDesc, nullSrvCpu);

      D3D12_CPU_DESCRIPTOR_HANDLE nullUavCpu = allocateShaderVisibleCbvSrvUav();
      uint32_t nullUavOff = m_cbvSrvUavHeapOffset - 1;
      D3D12_UNORDERED_ACCESS_VIEW_DESC nullUavDesc = {};
      nullUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      nullUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
      nullUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      d3d12Device->CreateUnorderedAccessView(nullptr, nullptr, &nullUavDesc, nullUavCpu);

      // The null sampler stays at the start of the sampler heap.
      const uint32_t nullSampOff = 0;

      D3D12_GPU_DESCRIPTOR_HANDLE cbvGpu, srvGpu, uavGpu, sampGpu;
      cbvGpu.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr + nullCbvOff * m_cbvSrvUavDescSize;
      srvGpu.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr + nullSrvOff * m_cbvSrvUavDescSize;
      uavGpu.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr + nullUavOff * m_cbvSrvUavDescSize;
      sampGpu.ptr = m_shaderVisibleSamplerHeap->GetGPUDescriptorHandleForHeapStart().ptr + nullSampOff * m_samplerDescSize;

      if (m_currentRootSignature) {
        for (int i = 0; i < 5; i++) {
          m_cmdList->SetGraphicsRootDescriptorTable(i,      cbvGpu);
          noteGraphicsRootTable(i, cbvGpu);
          m_cmdList->SetGraphicsRootDescriptorTable(5 + i,  srvGpu);
          noteGraphicsRootTable(5 + i, srvGpu);
          m_cmdList->SetGraphicsRootDescriptorTable(10 + i, sampGpu);
          noteGraphicsRootTable(10 + i, sampGpu);
          m_cmdList->SetGraphicsRootDescriptorTable(15 + i, uavGpu);
          noteGraphicsRootTable(15 + i, uavGpu);
        }
      }

      if (m_computeRootSignature) {
        m_cmdList->SetComputeRootDescriptorTable(0, cbvGpu);
        noteComputeRootTable(0, cbvGpu);
        m_cmdList->SetComputeRootDescriptorTable(1, srvGpu);
        noteComputeRootTable(1, srvGpu);
        m_cmdList->SetComputeRootDescriptorTable(2, sampGpu);
        noteComputeRootTable(2, sampGpu);
        m_cmdList->SetComputeRootDescriptorTable(3, uavGpu);
        noteComputeRootTable(3, uavGpu);
      }
    }
  }

  uint32_t D3D12BackendContext::descriptorPartitionCount() const {
    return (m_commandAllocatorCount > 1 && !m_copyOnly) ? kDescriptorPartitionCount : 1u;
  }

  uint32_t D3D12BackendContext::cbvSrvUavPartitionSize() const {
    return m_cbvSrvUavHeapSize / descriptorPartitionCount();
  }

  uint32_t D3D12BackendContext::cbvSrvUavPartitionEnd() const {
    const uint32_t partitionCount = descriptorPartitionCount();
    if (partitionCount == 1u || m_descriptorPartition + 1u >= partitionCount)
      return m_cbvSrvUavHeapSize;
    return m_cbvSrvUavHeapBaseOffset + cbvSrvUavPartitionSize();
  }

  void D3D12BackendContext::switchDescriptorPartition() {
#ifdef _WIN32
    const bool trace = hitchTraceEnabled();
    const int64_t start = trace ? hitchTimestamp() : 0;
    const uint32_t next = (m_descriptorPartition + 1) % descriptorPartitionCount();
    // The open command list still uses the partition: submit it first.
    const bool submitted = (m_partitionsInBatch & (1u << next)) != 0;
    if (submitted)
      flush();
    const int64_t afterFlush = trace ? hitchTimestamp() : 0;
    const uint64_t fenceValue = m_descriptorPartitionFences[next];
    const uint64_t completed = m_fence->GetCompletedValue();
    while (!m_retiredDescriptorHeaps.empty() && m_retiredDescriptorHeaps.front().second <= completed)
      m_retiredDescriptorHeaps.erase(m_retiredDescriptorHeaps.begin());
    // The heap is too small for this game: a bigger one instead of waiting
    // (tests make it grow with DXUP_TEST_GROW_DESCRIPTORS).
    const bool testGrow = GetEnvironmentVariableA("DXUP_TEST_GROW_DESCRIPTORS", nullptr, 0) > 0;
    if (((fenceValue && completed < fenceValue) || testGrow) && m_cbvSrvUavHeapSize < kShaderVisibleCbvSrvUavHeapSize) {
      growDescriptorHeap();
      return;
    }
    if (fenceValue && completed < fenceValue) {
      m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 10000);
    }
    const int64_t afterWait = trace ? hitchTimestamp() : 0;
    m_descriptorPartition = next;
    m_partitionsInBatch |= 1u << next;
    resetDescriptorHeapPartition();
    initNullDescriptorsAndRootTables();
    markAllDescriptorStagesDirty();
    if (trace) {
      LARGE_INTEGER frequency;
      QueryPerformanceFrequency(&frequency);
      const double toMs = 1000.0 / double(frequency.QuadPart);
      const double totalMs = double(hitchTimestamp() - start) * toMs;
      if (totalMs >= 2.0)
        logMessage(LogLevel::Warn,
          "hitch: descriptor partition %u: %.1f ms (submit %d %.1f ms, wait for %llu at %llu/%llu %.1f ms, reset %.1f ms)",
          next, totalMs, submitted ? 1 : 0, double(afterFlush - start) * toMs,
          static_cast<unsigned long long>(fenceValue), static_cast<unsigned long long>(completed),
          static_cast<unsigned long long>(m_fenceValue), double(afterWait - afterFlush) * toMs,
          double(hitchTimestamp() - afterWait) * toMs);
    }
#endif
  }

  bool D3D12BackendContext::createShaderVisibleCbvSrvUavHeap(uint32_t size) {
#ifdef _WIN32
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = size;
    desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(m_device->getDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf()))))
      return false;
    m_shaderVisibleCbvSrvUavHeap = std::move(heap);
    m_cbvSrvUavHeapSize = size;
    memstats::attach(m_shaderVisibleCbvSrvUavHeap.Get(), memstats::Internal, false,
      uint64_t(size) * m_cbvSrvUavDescSize);
    return true;
#else
    (void)size;
    return false;
#endif
  }

  void D3D12BackendContext::growDescriptorHeap() {
#ifdef _WIN32
    // The submitted lists keep the old heap until the GPU is done with them.
    flush();
    ComPtr<ID3D12DescriptorHeap> old = m_shaderVisibleCbvSrvUavHeap;
    if (!createShaderVisibleCbvSrvUavHeap(kShaderVisibleCbvSrvUavHeapSize))
      return;
    m_retiredDescriptorHeaps.emplace_back(std::move(old), m_fenceValue);
    logMessage(LogLevel::Info, "descriptor heap: grown to %u descriptors", m_cbvSrvUavHeapSize);
    m_descriptorPartitionFences.fill(0);
    m_descriptorPartition = 0;
    m_partitionsInBatch = 1;
    resetDescriptorHeapPartition();
    ID3D12DescriptorHeap* heaps[] = { m_shaderVisibleCbvSrvUavHeap.Get(), m_shaderVisibleSamplerHeap.Get() };
    m_cmdList->SetDescriptorHeaps(2, heaps);
    initNullDescriptorsAndRootTables();
    markAllDescriptorStagesDirty();
#endif
  }

  void D3D12BackendContext::resetDescriptorHeapPartition() {
    const uint32_t partitionCount = descriptorPartitionCount();
    if (partitionCount > 1u)
      m_cbvSrvUavHeapBaseOffset = cbvSrvUavPartitionSize() * m_descriptorPartition;
    else
      m_cbvSrvUavHeapBaseOffset = 0;

    m_cbvSrvUavHeapOffset = m_cbvSrvUavHeapBaseOffset;

    for (auto& cache : m_cbvTableCache)
      cache.clear();
    for (auto& cache : m_srvTableCache)
      cache.clear();
    for (auto& cache : m_uavTableCache)
      cache.clear();
    for (auto& sources : m_srvTableSources)
      sources.clear();
    for (auto& sources : m_uavTableSources)
      sources.clear();
    for (auto& lookup : m_cbvTableCacheLookup)
      lookup.clear();
    for (auto& lookup : m_srvTableCacheLookup)
      lookup.clear();
    for (auto& lookup : m_uavTableCacheLookup)
      lookup.clear();
  }

  D3D12_CPU_DESCRIPTOR_HANDLE D3D12BackendContext::allocateShaderVisibleCbvSrvUav() {
    if (m_cbvSrvUavHeapOffset >= cbvSrvUavPartitionEnd())
      logMessage(LogLevel::Error, "Shader-visible CBV/SRV/UAV heap exhausted at %u descriptors", m_cbvSrvUavHeapOffset);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_shaderVisibleCbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_cbvSrvUavHeapOffset * m_cbvSrvUavDescSize;
    m_cbvSrvUavHeapOffset++;
    m_descriptorsTaken++;
    return handle;
  }

  void D3D12BackendContext::initSamplerTables() {
#ifdef _WIN32
    D3D12_SAMPLER_DESC nullDesc = {};
    nullDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    nullDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    nullDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    nullDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    nullDesc.MaxLOD = D3D12_FLOAT32_MAX;
    m_device->getDevice()->CreateSampler(&nullDesc, m_shaderVisibleSamplerHeap->GetCPUDescriptorHandleForHeapStart());
    m_freeSamplerRanges.clear();
    m_freeSamplerRanges[1] = kShaderVisibleSamplerHeapSize - 1;
    m_freeSamplerCount = kShaderVisibleSamplerHeapSize - 1;
    m_samplerTables.reserve(256);
    m_samplerTableIds.reserve(256);
#endif
  }

  uint32_t D3D12BackendContext::acquireSamplerTable(uint64_t fingerprint, uint32_t count,
                                                    const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                                                    const uint64_t* versions) {
#ifdef _WIN32
    // The submission the open command list becomes.
    const uint64_t batch = m_fenceValue + 1;
    auto use = [&](SamplerTable& table) {
      if (table.lastUse != batch) {
        table.lastUse = batch;
        m_samplerDescriptorsInBatch += table.count;
      }
      return table.offset;
    };

    auto found = m_samplerTableIds.find(fingerprint);
    if (found != m_samplerTableIds.end()) {
      SamplerTable& table = m_samplerTables[found->second];
      bool match = table.count == count;
      for (uint32_t slot = 0; match && slot < count; slot++)
        match = table.handles[slot].ptr == handles[slot].ptr && table.versions[slot] == versions[slot];
      if (match) {
        if (perfStatsEnabled() && !m_copyOnly)
          m_perfIntervalDescriptorCacheHits++;
        return use(table);
      }
    }

    uint32_t offset = 0;
    if (!allocateSamplerRange(count, offset) && !(evictSamplerTables(count) && allocateSamplerRange(count, offset))) {
      // Only the open command list's tables are left and they fill the
      // heap; ensureDescriptorHeapSpace submits long before that.
      logMessage(LogLevel::Error, "sampler heap full: %u samplers in the open command list", m_samplerDescriptorsInBatch);
      return 0;
    }

    auto* device = m_device->getDevice();
    const D3D12_CPU_DESCRIPTOR_HANDLE start = m_shaderVisibleSamplerHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t slot = 0; slot < count; slot++) {
      D3D12_CPU_DESCRIPTOR_HANDLE dst;
      dst.ptr = start.ptr + uint64_t(offset + slot) * m_samplerDescSize;
      device->CopyDescriptorsSimple(1, dst, handles[slot], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }
    if (perfStatsEnabled() && !m_copyOnly)
      m_perfIntervalDescriptorCopies += count;

    uint32_t id;
    if (!m_freeSamplerTableIds.empty()) {
      id = m_freeSamplerTableIds.back();
      m_freeSamplerTableIds.pop_back();
    } else {
      id = static_cast<uint32_t>(m_samplerTables.size());
      m_samplerTables.emplace_back();
    }
    SamplerTable& table = m_samplerTables[id];
    table.offset = offset;
    table.count = count;
    table.lastUse = 0;
    table.fingerprint = fingerprint;
    std::copy_n(handles, count, table.handles.begin());
    std::copy_n(versions, count, table.versions.begin());
    // A table with the same fingerprint but other contents is no longer
    // found; its place returns once it is evicted.
    m_samplerTableIds[fingerprint] = id;
    return use(table);
#else
    (void)fingerprint; (void)count; (void)handles; (void)versions;
    return 0;
#endif
  }

  bool D3D12BackendContext::allocateSamplerRange(uint32_t count, uint32_t& offset) {
    for (auto it = m_freeSamplerRanges.begin(); it != m_freeSamplerRanges.end(); ++it) {
      if (it->second < count)
        continue;
      offset = it->first;
      const uint32_t rest = it->second - count;
      m_freeSamplerRanges.erase(it);
      if (rest)
        m_freeSamplerRanges[offset + count] = rest;
      m_freeSamplerCount -= count;
      return true;
    }
    return false;
  }

  void D3D12BackendContext::freeSamplerTable(uint32_t id) {
    SamplerTable& table = m_samplerTables[id];
    auto indexed = m_samplerTableIds.find(table.fingerprint);
    if (indexed != m_samplerTableIds.end() && indexed->second == id)
      m_samplerTableIds.erase(indexed);
    uint32_t start = table.offset, length = table.count;
    auto next = m_freeSamplerRanges.lower_bound(start);
    if (next != m_freeSamplerRanges.end() && next->first == start + length) {
      length += next->second;
      next = m_freeSamplerRanges.erase(next);
    }
    if (next != m_freeSamplerRanges.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == start) {
        start = previous->first;
        length += previous->second;
        m_freeSamplerRanges.erase(previous);
      }
    }
    m_freeSamplerRanges[start] = length;
    m_freeSamplerCount += table.count;
    table.count = 0;
    m_freeSamplerTableIds.push_back(id);
  }

  // Frees the least recently used tables the GPU is done with until a range
  // of count samplers is free, waiting for submissions when it has to.
  bool D3D12BackendContext::evictSamplerTables(uint32_t count) {
#ifdef _WIN32
    HitchScope hitch("backend evict sampler tables");
    const uint64_t batch = m_fenceValue + 1;
    std::vector<std::pair<uint64_t, uint32_t>> candidates;
    for (uint32_t id = 0; id < m_samplerTables.size(); id++) {
      const SamplerTable& table = m_samplerTables[id];
      if (table.count && table.lastUse < batch)
        candidates.emplace_back(table.lastUse, id);
    }
    std::sort(candidates.begin(), candidates.end());
    // Free a quarter of the heap at once, so that new tables do not evict
    // one at a time.
    const uint32_t target = (std::max)(count, kShaderVisibleSamplerHeapSize / 4);
    uint32_t largest = 0;
    for (const auto& range : m_freeSamplerRanges)
      largest = (std::max)(largest, range.second);
    for (const auto& candidate : candidates) {
      if (largest >= count && m_freeSamplerCount >= target)
        break;
      if (m_fence->GetCompletedValue() < candidate.first) {
        if (largest >= count)
          break;
        // Every table the GPU is done with is gone: wait for the oldest.
        m_fence->SetEventOnCompletion(candidate.first, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, 10000);
      }
      freeSamplerTable(candidate.second);
      largest = 0;
      for (const auto& range : m_freeSamplerRanges)
        largest = (std::max)(largest, range.second);
    }
    return largest >= count;
#else
    (void)count;
    return false;
#endif
  }

  void D3D12BackendContext::ensureDescriptorHeapSpace() {
    // Worst case per draw: CBV 70 + SRV 640 + UAV 320 = 1030, Sampler 80
    // Flush mid-frame if heaps are too full to avoid out-of-bounds descriptors.
    constexpr uint32_t kCbvSrvUavReserve = 1100;
    // Called for most draws: the usual case without a hitch scope (its
    // check and destructor were 2 % of the worker in draw-heavy work).
    if (m_samplerDescriptorsInBatch <= kShaderVisibleSamplerHeapSize / 2
     && m_cbvSrvUavHeapOffset + kCbvSrvUavReserve < cbvSrvUavPartitionEnd())
      return;
    HitchScope hitch("backend ensureDescriptorHeapSpace");
    // Sampler tables of the open command list stay until it completes:
    // submit it before they could fill the heap.
    if (m_samplerDescriptorsInBatch > kShaderVisibleSamplerHeapSize / 2)
      flush();
    if (m_cbvSrvUavHeapOffset + kCbvSrvUavReserve >= cbvSrvUavPartitionEnd()) {
      if (m_perfStatsEnabled && !m_copyOnly)
        m_perfIntervalDescriptorFlushes++;
      // Moving on to the next partition is routine, many times a second in
      // busy games: no warning, whose synchronous log write alone can stall
      // the recording thread for milliseconds.
      if (m_commandAllocatorCount > 1 && !m_copyOnly) {
        logMessage(LogLevel::Info, "ensureDescriptorHeapSpace: next partition (cbvSrvUavOff=%u/%u)",
          m_cbvSrvUavHeapOffset, cbvSrvUavPartitionEnd());
        switchDescriptorPartition();
        return;
      }
      logMessage(LogLevel::Warn,
        "ensureDescriptorHeapSpace: mid-frame flush (cbvSrvUavOff=%u/%u)",
        m_cbvSrvUavHeapOffset,
        cbvSrvUavPartitionEnd());

      flush();

      // Full GPU sync before resetting descriptor heaps
      if (m_fenceValue > 0 && m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, 10000);
      }

      // Now safe to reset — all GPU work using these descriptors is complete
      resetDescriptorHeapPartition();
      for (uint32_t i = 0; i < kCommandAllocatorRingSize; i++)
        m_uploadScratchOffsets[i] = 0;
      releaseCompletedUploads();
      initNullDescriptorsAndRootTables();
    }
  }
#endif

}
