#include "d3d11_backend_device.h"
#ifdef DXUP_D3D12_BACKEND
#include "../d3d12/d3d12_backend_context.h"
#endif
#include "../util/util_config.h"
#include "../util/util_types.h"
#include <cstring>
#include <stdexcept>
#include <vector>

namespace dxup {

  namespace {

    // An initial data upload: the backend holds a lock from begin to end,
    // which ends here whatever the upload throws.
    class InitialDataScope {
    public:
      explicit InitialDataScope(IBackendDevice* backend)
      : m_backend(backend), m_ctx(backend->beginInitialData()) { }
      InitialDataScope(const InitialDataScope&) = delete;
      InitialDataScope& operator=(const InitialDataScope&) = delete;
      ~InitialDataScope() {
        try {
          end();
        } catch (const std::exception& e) {
          logMessage(LogLevel::Error, "initial data upload: ending it failed (%s)", e.what());
        }
      }

      void end() {
        if (Rc<IBackendContext> context = std::move(m_ctx))
          m_backend->endInitialData(context.get());
      }

      explicit operator bool() const { return m_ctx != nullptr; }
      IBackendContext* operator->() const { return m_ctx.get(); }

    private:
      IBackendDevice*     m_backend;
      Rc<IBackendContext> m_ctx;
    };

    // Staging textures take their initial data straight into their memory.
    void writeStagingInitialData(IBackendImage* image, const D3D11_SUBRESOURCE_DATA* initialData) {
      const ImageDesc& desc = image->getDesc();
      const uint32_t mips = desc.mipLevels ? desc.mipLevels : 1u;
      const uint32_t layers = desc.type == ImageType::Tex3D ? 1u : (desc.arraySize ? desc.arraySize : 1u);
      // Planar video formats: luma rows, then chroma rows at half height.
      uint32_t planarBytes = 0;
      switch (desc.format) {
        case DXGI_FORMAT_NV12: case DXGI_FORMAT_420_OPAQUE: planarBytes = 1; break;
        case DXGI_FORMAT_P010: case DXGI_FORMAT_P016: planarBytes = 2; break;
        default: break;
      }
      for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t mip = 0; mip < mips; mip++) {
          const uint32_t subresource = mip + layer * mips;
          const D3D11_SUBRESOURCE_DATA& source = initialData[subresource];
          if (!source.pSysMem)
            continue;
          ImageSubresourceLayout layout;
          uint8_t* target = image->mapStaging(subresource, MapMode::Write, layout);
          if (!target)
            continue;
          const uint32_t width = (std::max)(desc.width >> mip, 1u);
          const uint32_t height = (std::max)(desc.height >> mip, 1u);
          const uint32_t depth = desc.type == ImageType::Tex3D ? (std::max)(desc.depth >> mip, 1u) : 1u;
          const uint32_t rowBytes = planarBytes ? width * planarBytes : computeRowPitch(width, desc.format);
          const uint32_t rows = planarBytes ? height + (height + 1) / 2 : computeNumRows(height, desc.format);
          const size_t copyBytes = (std::min)(size_t(rowBytes), size_t(layout.rowPitch));
          for (uint32_t z = 0; z < depth; z++) {
            for (uint32_t row = 0; row < rows; row++) {
              std::memcpy(target + size_t(z) * layout.depthPitch + size_t(row) * layout.rowPitch,
                static_cast<const uint8_t*>(source.pSysMem) + size_t(z) * source.SysMemSlicePitch + size_t(row) * source.SysMemPitch,
                copyBytes);
            }
          }
          image->unmapStaging(subresource);
        }
      }
    }

  }

  D3D11BackendDevice::D3D11BackendDevice(BackendType type, const LUID* adapterLuid)
  : m_backendType(type) {
    if (type == BackendType::D3D12) {
#ifdef DXUP_D3D12_BACKEND
      m_backend = std::make_shared<d3d12::D3D12BackendDevice>(adapterLuid);
#endif
    }
  }

  D3D11BackendDevice::~D3D11BackendDevice() {
  }

#ifdef _WIN32
  Rc<IBackendBuffer> D3D11BackendDevice::createBuffer(
    const D3D11_BUFFER_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initialData) {
    if (!m_backend) return nullptr;

    BufferDesc backendDesc = convertBufferDesc(desc);
    auto buffer = m_backend->createBuffer(backendDesc);

    // A constant buffer reads 0 past its size; D3D12 reads whole 256-byte
    // units of its memory, which may hold what an earlier buffer left there.
    // GPU-only constant buffers get those units written once: the data and
    // zeros after it. Dynamic ones are cleared when their memory is handed out.
    if (buffer && (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && desc->Usage != D3D11_USAGE_DYNAMIC
     && desc->ByteWidth % 256) {
      std::vector<uint8_t> padded((desc->ByteWidth + 255) & ~255u, 0);
      if (initialData && initialData->pSysMem)
        std::memcpy(padded.data(), initialData->pSysMem, desc->ByteWidth);
      if (InitialDataScope ctx(m_backend.get()); ctx)
        ctx->updateBuffer(buffer.get(), 0, padded.size(), padded.data());
      return buffer;
    }

    if (buffer && initialData && initialData->pSysMem) {
      logMessage(LogLevel::Info, "D3D11BackendDevice::createBuffer - uploading initial data for buffer Size=%u, BindFlags=0x%x",
        desc->ByteWidth, desc->BindFlags);
      InitialDataScope ctx(m_backend.get());
      if (ctx) {
        ctx->updateBuffer(buffer.get(), 0, desc->ByteWidth, initialData->pSysMem);
        ctx.end();
        // Synchronous GPU check after upload
#ifdef DXUP_D3D12_BACKEND
        if (m_backendType == BackendType::D3D12) {
          auto* d3d12Device = static_cast<d3d12::D3D12BackendDevice*>(m_backend.get());
          if (d3d12Device->isGpuSyncValidationEnabled())
            d3d12Device->flushAndCheckGpu("buffer-upload");
        }
#endif
      }
    }

    return buffer;
  }

  Rc<IBackendImage> D3D11BackendDevice::createTexture1D(
    const D3D11_TEXTURE1D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initialData) {
    if (!m_backend) return nullptr;

    ImageDesc backendDesc = convertTexture1DDesc(desc);
    auto image = m_backend->createImage(backendDesc);

    if (image && initialData && image->isCpuAccessible()) {
      writeStagingInitialData(image.get(), initialData);
      return image;
    }

    if (image && initialData) {
      InitialDataScope ctx(m_backend.get());
      if (ctx) {
        for (uint32_t arraySlice = 0; arraySlice < desc->ArraySize; arraySlice++) {
          for (uint32_t mip = 0; mip < backendDesc.mipLevels; mip++) {
            uint32_t subIdx = mip + arraySlice * backendDesc.mipLevels;
            const auto& sub = initialData[subIdx];
            if (!sub.pSysMem) continue;

            uint32_t mipW = (desc->Width >> mip) > 0 ? (desc->Width >> mip) : 1;
            uint32_t rowPitch = computeRowPitch(mipW, desc->Format);
            {
              // 1D initial data is a single row: D3D11 ignores SysMemPitch.
              ImageSubresource subres = {};
              subres.mipLevel   = mip;
              subres.arrayLayer = arraySlice;
              if (ctx->uploadImageSubresource(image.get(), subres, sub.pSysMem, rowPitch, 0))
                continue;
            }
            UploadLayout uploadLayout = getImageUploadLayout(
              m_backendType,
              m_backend.get(),
              image.get(),
              subIdx,
              rowPitch,
              1,
              1);

            BufferDesc uploadDesc = {};
            uploadDesc.size      = uploadLayout.totalSize;
            uploadDesc.usage     = ResourceUsage::Dynamic;
            uploadDesc.cpuAccess = CpuAccess::Write;
            auto uploadBuf = m_backend->createBuffer(uploadDesc);
            if (uploadBuf) {
              void* ptr = ctx->mapBuffer(uploadBuf.get(), MapMode::Write);
              if (ptr) {
                // 1D initial data is a single row: D3D11 ignores SysMemPitch.
                std::memcpy(ptr, sub.pSysMem, uploadLayout.copyRowBytes);
                ctx->unmapBuffer(uploadBuf.get());
                ImageSubresource subres = {};
                subres.mipLevel   = mip;
                subres.arrayLayer = arraySlice;
                ctx->copyBufferToImage(image.get(), subres, uploadBuf.get(), 0);
              }
              ctx->retainTemporaryBuffer(std::move(uploadBuf));
            }
          }
        }
        ctx.end();
      }
    }

    return image;
  }

  Rc<IBackendImage> D3D11BackendDevice::createTexture2D(
    const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initialData) {
    if (!m_backend) return nullptr;

    ImageDesc backendDesc = convertTexture2DDesc(desc);
    auto image = m_backend->createImage(backendDesc);

    if (image && initialData && image->isCpuAccessible()) {
      writeStagingInitialData(image.get(), initialData);
      return image;
    }

    if (image && initialData) {
      InitialDataScope ctx(m_backend.get());
      if (ctx) {
        for (uint32_t arraySlice = 0; arraySlice < desc->ArraySize; arraySlice++) {
          for (uint32_t mip = 0; mip < backendDesc.mipLevels; mip++) {
            uint32_t subIdx = mip + arraySlice * backendDesc.mipLevels;
            const auto& sub = initialData[subIdx];
            if (!sub.pSysMem) continue;

            uint32_t mipW     = (desc->Width  >> mip) > 0 ? (desc->Width  >> mip) : 1;
            uint32_t mipH     = (desc->Height >> mip) > 0 ? (desc->Height >> mip) : 1;
            uint32_t rowPitch = computeRowPitch(mipW, desc->Format);
            uint32_t numRows  = computeNumRows(mipH, desc->Format);
            if (sub.SysMemPitch >= rowPitch) {
              ImageSubresource subres = {};
              subres.mipLevel   = mip;
              subres.arrayLayer = arraySlice;
              if (ctx->uploadImageSubresource(image.get(), subres, sub.pSysMem, sub.SysMemPitch, sub.SysMemSlicePitch))
                continue;
            }
            UploadLayout uploadLayout = getImageUploadLayout(
              m_backendType,
              m_backend.get(),
              image.get(),
              subIdx,
              rowPitch,
              numRows,
              1);

            BufferDesc uploadDesc = {};
            uploadDesc.size      = uploadLayout.totalSize;
            uploadDesc.usage     = ResourceUsage::Dynamic;
            uploadDesc.cpuAccess = CpuAccess::Write;
            auto uploadBuf = m_backend->createBuffer(uploadDesc);
            if (uploadBuf) {
              void* ptr = ctx->mapBuffer(uploadBuf.get(), MapMode::Write);
              if (ptr) {
                auto* dst = static_cast<uint8_t*>(ptr);
                auto* src = static_cast<const uint8_t*>(sub.pSysMem);
                for (uint32_t row = 0; row < uploadLayout.numRows; row++) {
                  copyUploadRow(dst + row * uploadLayout.rowPitch, src + row * sub.SysMemPitch,
                    uploadLayout.copyRowBytes, sub.SysMemPitch, desc->Format);
                }
                ctx->unmapBuffer(uploadBuf.get());
                ImageSubresource subres = {};
                subres.mipLevel   = mip;
                subres.arrayLayer = arraySlice;
                ctx->copyBufferToImage(image.get(), subres, uploadBuf.get(), 0);
              }
              ctx->retainTemporaryBuffer(std::move(uploadBuf));
            }
          }
        }
        ctx.end();
        // Synchronous GPU check after texture upload
#ifdef DXUP_D3D12_BACKEND
        if (m_backendType == BackendType::D3D12) {
          auto* d3d12Device = static_cast<d3d12::D3D12BackendDevice*>(m_backend.get());
          if (d3d12Device->isGpuSyncValidationEnabled())
            d3d12Device->flushAndCheckGpu("tex2d-upload");
        }
#endif
      }
    }

    return image;
  }

  Rc<IBackendImage> D3D11BackendDevice::createTexture3D(
    const D3D11_TEXTURE3D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initialData) {
    if (!m_backend) return nullptr;

    ImageDesc backendDesc = convertTexture3DDesc(desc);
    auto image = m_backend->createImage(backendDesc);

    if (image && initialData && image->isCpuAccessible()) {
      writeStagingInitialData(image.get(), initialData);
      return image;
    }

    if (image && initialData) {
      InitialDataScope ctx(m_backend.get());
      if (ctx) {
        for (uint32_t mip = 0; mip < backendDesc.mipLevels; mip++) {
          const auto& sub = initialData[mip];
          if (!sub.pSysMem) continue;

          uint32_t mipW     = (desc->Width  >> mip) > 0 ? (desc->Width  >> mip) : 1;
          uint32_t mipH     = (desc->Height >> mip) > 0 ? (desc->Height >> mip) : 1;
          uint32_t mipD     = (desc->Depth  >> mip) > 0 ? (desc->Depth  >> mip) : 1;
          uint32_t rowPitch = computeRowPitch(mipW, desc->Format);
          uint32_t numRows  = computeNumRows(mipH, desc->Format);
          if (sub.SysMemPitch >= rowPitch && (mipD == 1 || sub.SysMemSlicePitch >= uint64_t(sub.SysMemPitch) * numRows)) {
            ImageSubresource subres = {};
            subres.mipLevel = mip;
            if (ctx->uploadImageSubresource(image.get(), subres, sub.pSysMem, sub.SysMemPitch, sub.SysMemSlicePitch))
              continue;
          }
          UploadLayout uploadLayout = getImageUploadLayout(
            m_backendType,
            m_backend.get(),
            image.get(),
            mip,
            rowPitch,
            numRows,
            mipD);
          uint64_t slicePitch = static_cast<uint64_t>(uploadLayout.rowPitch) * uploadLayout.numRows;

          BufferDesc uploadDesc = {};
          uploadDesc.size      = uploadLayout.totalSize;
          uploadDesc.usage     = ResourceUsage::Dynamic;
          uploadDesc.cpuAccess = CpuAccess::Write;
          auto uploadBuf = m_backend->createBuffer(uploadDesc);
          if (uploadBuf) {
            void* ptr = ctx->mapBuffer(uploadBuf.get(), MapMode::Write);
            if (ptr) {
              auto* dst = static_cast<uint8_t*>(ptr);
              auto* src = static_cast<const uint8_t*>(sub.pSysMem);
              for (uint32_t z = 0; z < uploadLayout.depth; z++) {
                auto* sliceSrc = src + z * sub.SysMemSlicePitch;
                auto* sliceDst = dst + z * slicePitch;
                for (uint32_t row = 0; row < uploadLayout.numRows; row++) {
                  copyUploadRow(sliceDst + row * uploadLayout.rowPitch, sliceSrc + row * sub.SysMemPitch,
                    uploadLayout.copyRowBytes, sub.SysMemPitch, desc->Format);
                }
              }
              ctx->unmapBuffer(uploadBuf.get());
              ImageSubresource subres = {};
              subres.mipLevel = mip;
              ctx->copyBufferToImage(image.get(), subres, uploadBuf.get(), 0);
            }
            ctx->retainTemporaryBuffer(std::move(uploadBuf));
          }
        }
        ctx.end();
      }
    }

    return image;
  }

  Rc<IBackendSampler> D3D11BackendDevice::createSampler(const D3D11_SAMPLER_DESC* desc) {
    if (!m_backend) return nullptr;

    SamplerDesc backendDesc = convertSamplerDesc(desc);
    overrideSamplerAnisotropy(backendDesc, UserOverrides::get().samplerAnisotropy);
    overrideSamplerLodBias(backendDesc, UserOverrides::get().samplerLodBias);
    return m_backend->createSampler(backendDesc);
  }

  Rc<IBackendShader> D3D11BackendDevice::createVertexShader(const void* bytecode, size_t size) {
    if (!m_backend) return nullptr;
    ShaderDesc desc;
    desc.stage = ShaderStage::Vertex;
    desc.bytecode = bytecode;
    desc.bytecodeSize = size;
    return m_backend->createShader(desc);
  }

  Rc<IBackendShader> D3D11BackendDevice::createPixelShader(const void* bytecode, size_t size) {
    if (!m_backend) return nullptr;
    ShaderDesc desc;
    desc.stage = ShaderStage::Pixel;
    desc.bytecode = bytecode;
    desc.bytecodeSize = size;
    return m_backend->createShader(desc);
  }

  Rc<IBackendShader> D3D11BackendDevice::createGeometryShader(const void* bytecode, size_t size) {
    if (!m_backend) return nullptr;
    ShaderDesc desc;
    desc.stage = ShaderStage::Geometry;
    desc.bytecode = bytecode;
    desc.bytecodeSize = size;
    return m_backend->createShader(desc);
  }

  Rc<IBackendShader> D3D11BackendDevice::createGeometryShaderWithStreamOutput(const void* bytecode, size_t size) {
    // Same as regular GS - the SO declaration is handled at PSO creation time
    return createGeometryShader(bytecode, size);
  }

  Rc<IBackendShader> D3D11BackendDevice::createHullShader(const void* bytecode, size_t size) {
    if (!m_backend) return nullptr;
    ShaderDesc desc;
    desc.stage = ShaderStage::Hull;
    desc.bytecode = bytecode;
    desc.bytecodeSize = size;
    return m_backend->createShader(desc);
  }

  Rc<IBackendShader> D3D11BackendDevice::createDomainShader(const void* bytecode, size_t size) {
    if (!m_backend) return nullptr;
    ShaderDesc desc;
    desc.stage = ShaderStage::Domain;
    desc.bytecode = bytecode;
    desc.bytecodeSize = size;
    return m_backend->createShader(desc);
  }

  Rc<IBackendShader> D3D11BackendDevice::createComputeShader(const void* bytecode, size_t size) {
    if (!m_backend) return nullptr;
    ShaderDesc desc;
    desc.stage = ShaderStage::Compute;
    desc.bytecode = bytecode;
    desc.bytecodeSize = size;
    return m_backend->createShader(desc);
  }
#endif

  Rc<IBackendContext> D3D11BackendDevice::createImmediateContext() {
    if (!m_backend) return nullptr;
    return m_backend->createContext();
  }

  Rc<IBackendContext> D3D11BackendDevice::createThreadedImmediateContext() {
    if (!m_backend) return nullptr;
    auto ctx = m_backend->createContext();
#ifdef DXUP_D3D12_BACKEND
    auto retireQueue = m_backend->getRetireQueue();
    if (ctx && retireQueue && csThreadEnabled()) {
      logMessage(LogLevel::Info, "Immediate context records on the CS thread");
      if (m_backendType == BackendType::D3D12)
        static_cast<d3d12::D3D12BackendContext*>(ctx.get())->setExternallySynchronized(true);
      return std::make_shared<CsThreadContext>(std::move(ctx), m_backend.get(), std::move(retireQueue));
    }
#endif
    return ctx;
  }

  Rc<IBackendContext> D3D11BackendDevice::createDeferredContext() {
    if (!m_backend) return nullptr;
#ifdef DXUP_D3D12_BACKEND
    return std::make_shared<CsThreadContext>(m_backend.get(), m_backend->getRetireQueue());
#else
    return m_backend->createContext();
#endif
  }

}
