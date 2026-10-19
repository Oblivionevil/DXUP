#pragma once

#include "../directx/directx_backend.h"
#include "../util/util_types.h"
#include "../util/util_thread.h"
#include "d3d12_dxbc.h"
#include <cstring>
#include <cstdio>
#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

#ifdef _WIN32
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../util/com_ptr.h"
#include "d3d12_placed_allocator.h"
#include "d3d12_shared_sync.h"

#endif

namespace dxup::d3d12 {

  static constexpr uint32_t kD3D12ShaderStageCount = static_cast<uint32_t>(ShaderStage::Count);

  struct ShaderResourceLimits {
    bool known = false;
    uint32_t cbvCount = 0;
    uint32_t srvCount = 0;
    uint32_t samplerCount = 0;
    uint32_t uavCount = 0;
    // The dimension each t# (first 128 bytes) and u# (64 after) register is
    // declared with (dxbc::resourceDimensions); shared by every shader with
    // the same declarations and kept for the process. Null when unknown.
    const uint8_t* dimensions = nullptr;
    // How the code accesses each u# register (dxbc::UavAccess bits),
    // interned like the dimensions. Null when unknown: every declared view
    // may be read and written.
    const uint16_t* uavAccess = nullptr;
    // The constant buffer slot the shader's immediate constant buffer moved
    // to (dxbc::icbToConstantBuffer), and the buffer DXUP binds there.
    uint32_t icbSlot = UINT32_MAX;
    uint64_t icbAddress = 0;
    uint32_t icbSize = 0;
  };

  using PipelineResourceLimits = std::array<ShaderResourceLimits, kD3D12ShaderStageCount>;

  class D3D12DescriptorHeapManager;

  // ============================================================
  // Forward Declarations
  // ============================================================
  class D3D12BackendDevice;
  class D3D12BackendContext;
  class D3D12BackendBuffer;

  // Unique, increasing version of descriptor contents. CPU descriptor
  // handles are reused once freed: caches of shader-visible tables tell
  // descriptors apart by handle and version.
  uint64_t nextDescriptorVersion();

  class D3D12BackendImage;

  // ============================================================
  // Root signature layout
  // ============================================================

  // The first constant buffer slots of each stage are root descriptors: a
  // GPU address each, no descriptor to create per draw. The other slots
  // use a descriptor table starting at the first register after them.
  // Order: vertex, hull, domain, geometry, pixel, compute.
  constexpr uint32_t kRootCbvSlots[6] = { 4, 2, 2, 2, 4, 8 };
  // Graphics: tables 0-19 (CBV, SRV, sampler, UAV per stage), then the
  // root CBVs stage after stage. Compute: tables 0-3, then its root CBVs.
  constexpr uint32_t kGraphicsRootCbvBase = 20;
  constexpr uint32_t kComputeRootCbvBase = 4;
  constexpr uint32_t kGraphicsRootCbvCount = 14;
  inline uint32_t graphicsRootCbvIndex(uint32_t stage, uint32_t slot) {
    uint32_t index = kGraphicsRootCbvBase;
    for (uint32_t i = 0; i < stage; i++)
      index += kRootCbvSlots[i];
    return index + slot;
  }
  // Changes whenever the layout changes; part of the PSO disk cache key.
  constexpr size_t kRootSignatureLayoutVersion = 0x52434256u;
  // Mixed into the PSO disk cache key of pipelines whose pixel shader
  // outputs DXUP masked (dxbc::maskOutputWrites).
  constexpr size_t kPixelOutputMaskVersion = 0x504f4d31u;

  // ============================================================
  // Upload heap suballocation
  // ============================================================

  // Small dynamic buffers are slices of large persistently mapped upload
  // heap blocks. A committed resource each would take 64 KB and a driver
  // call for every WRITE_DISCARD rename. Slices are sized in powers of two
  // from 512 bytes (placed footprint alignment) to 64 KB.
  // Small buffers as slices of large blocks: creating a resource for each
  // costs a kernel allocation. Upload blocks are mapped; default blocks are
  // shared by their slices, which share the block's resource state too.
  class D3D12UploadSuballocator {
  public:
    static constexpr uint64_t kChunkSize = 4ull << 20;
    static constexpr uint64_t kMaxSlice = 64ull << 10;
    static constexpr uint32_t kMaxClassCount = 9;

    explicit D3D12UploadSuballocator(D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD)
    : m_heapType(heapType)
    , m_minSlice(heapType == D3D12_HEAP_TYPE_UPLOAD ? 512 : 256) { }

    // Upload blocks in video memory the CPU writes through the PCIe BAR
    // (resizable BAR): shaders read dynamic constant, vertex and index
    // buffers from local memory instead of system memory, as D3D11
    // drivers place them. Set before the first allocation.
    void useGpuUploadHeap() { m_gpuUploadHeap = true; }
    bool usesGpuUploadHeap() const { return m_gpuUploadHeap; }
    // Cached system memory instead of write-combined: CPU writes need no
    // drain at the next locked instruction, the GPU snoops its reads.
    void useWriteBack() { m_writeBack = true; }
    bool usesWriteBack() const { return m_writeBack; }
    static void uploadHeapProperties(D3D12_HEAP_PROPERTIES& props, bool gpuUpload, bool writeBack) {
      if (writeBack) {
        props.Type = D3D12_HEAP_TYPE_CUSTOM;
        props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        props.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      } else {
        props.Type = gpuUpload ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_UPLOAD;
      }
    }

    struct Slice {
      ID3D12Resource*        resource = nullptr;
      uint64_t               offset = 0;
      uint8_t*               mapped = nullptr;
      uint32_t               sizeClass = 0;
      // The bytes from the start that earlier users may have written; the
      // rest is zero, as new blocks are.
      uint32_t               written = 0;
      // State of the block, for default blocks.
      D3D12_RESOURCE_STATES* state = nullptr;
    };

    bool allocate(ID3D12Device* device, uint64_t size, Slice& slice);
    // Several slices of one size under one lock; returns how many.
    uint32_t allocateMany(ID3D12Device* device, uint64_t size, Slice* slices, uint32_t count);
    void free(const Slice& slice);
    void free(const std::vector<Slice>& slices);
    // Gives back blocks no slice of which was needed for a while: a burst
    // (Trine 4 renames 250 MB of 64 KB buffers while it loads) would keep
    // its blocks in video memory for good. Called at present.
    void trim();
    uint64_t sliceSize(uint32_t sizeClass) const { return m_minSlice << sizeClass; }
    // Blocks and free slices per size class (dxup_memory_stats.txt).
    std::string describe() {
      std::string text;
      std::lock_guard<std::mutex> lock(m_mutex);
      std::lock_guard<std::mutex> returnedLock(m_returnedMutex);
      for (uint32_t c = 0; c < kMaxClassCount; c++) {
        if (!m_chunkCount[c])
          continue;
        char part[96];
        std::snprintf(part, sizeof(part), "%s%llu B: %u blocks, %zu of %llu free", text.empty() ? "" : "; ",
          static_cast<unsigned long long>(sliceSize(c)), m_chunkCount[c], m_free[c].size() + m_returned[c].size(),
          static_cast<unsigned long long>(m_chunkCount[c] * (kChunkSize / sliceSize(c))));
        text += part;
      }
      return text;
    }
    // Upload blocks stay mapped; default blocks are GPU memory.
    bool isMapped() const { return m_heapType == D3D12_HEAP_TYPE_UPLOAD; }

    // Hands a mapped slice to a buffer whose application data is dataSize
    // bytes. A constant buffer reads 0 past its data in D3D11: what earlier
    // users wrote there is cleared, and only that.
    static void prepareSlice(Slice& slice, uint64_t dataSize, bool zeroTail) {
      const uint32_t size = static_cast<uint32_t>(dataSize);
      if (zeroTail) {
        if (slice.mapped && slice.written > size)
          std::memset(slice.mapped + size, 0, size_t(slice.written - size));
        slice.written = size;
      } else if (slice.written < size) {
        slice.written = size;
      }
    }

    uint32_t sizeClassOf(uint64_t size) const {
      uint32_t sizeClass = 0;
      while ((m_minSlice << sizeClass) < size)
        sizeClass++;
      return sizeClass;
    }

  private:
    const D3D12_HEAP_TYPE m_heapType;
    const uint64_t        m_minSlice;
    bool                  m_gpuUploadHeap = false;
    bool                  m_writeBack = false;
    std::vector<std::unique_ptr<D3D12_RESOURCE_STATES>> m_chunkStates;
    // Allocation and release happen on different threads (the application
    // renames, the CS worker destroys retired buffers): released slices
    // collect apart and move over in one step when a class runs dry.
    std::mutex                                 m_mutex;
    std::vector<ComPtr<ID3D12Resource>>        m_chunks;
    std::array<uint32_t, kMaxClassCount>       m_chunkCount = {};
    // Slices handed out and not back yet, and the most of them at once in
    // this and the two trim periods before.
    std::array<std::atomic<int64_t>, kMaxClassCount> m_used = {};
    std::array<int64_t, kMaxClassCount>        m_peak = {};
    std::array<std::array<int64_t, 2>, kMaxClassCount> m_earlierPeaks = {};
    std::atomic<uint64_t>                      m_lastTrim{ 0 };
    // Blocks trim gave up, released a few at each present.
    std::vector<ComPtr<ID3D12Resource>>        m_releasing;
    std::atomic<size_t>                        m_releasingCount{ 0 };
    std::array<std::vector<Slice>, kMaxClassCount> m_free;
    std::mutex                                 m_returnedMutex;
    std::array<std::vector<Slice>, kMaxClassCount> m_returned;
  };

  // ============================================================
  // D3D12 Backend Buffer
  // ============================================================
  class D3D12BackendBuffer : public IBackendBuffer {
  public:
#ifdef _WIN32
    // A slice of an upload block, returned to the allocator when destroyed.
    D3D12BackendBuffer(const BufferDesc& desc, D3D12UploadSuballocator* allocator,
                       const D3D12UploadSuballocator::Slice& slice);
#endif
    D3D12BackendBuffer(
#ifdef _WIN32
      ComPtr<ID3D12Resource>   resource,
#endif
      const BufferDesc&        desc);
    ~D3D12BackendBuffer() override;

    const BufferDesc& getDesc() const final { return m_desc; }

#ifdef _WIN32
    ID3D12Resource* getResource() const { return m_resource.Get(); }
    // Where the buffer starts in its resource (slices of upload blocks).
    uint64_t getOffset() const { return m_offset; }
    bool isSuballocated() const { return m_suballocator != nullptr; }

    // A slice can be replaced in place (WRITE_DISCARD). The application
    // thread writes the newest slice; the recording thread switches over
    // in order with the commands and hands back the previous one.
    uint8_t* getAppMappedData() const {
      return m_appMapped ? m_appMapped : static_cast<uint8_t*>(m_persistentMappedData);
    }
    void setAppMappedData(uint8_t* mapped) { m_appMapped = mapped; }
    // A slice of another allocator (constant buffer renames) changes how the
    // buffer is tracked: upload slices stay in GENERIC_READ.
    D3D12UploadSuballocator::Slice replaceSlice(const D3D12UploadSuballocator::Slice& slice,
                                                D3D12UploadSuballocator* owner = nullptr) {
      D3D12UploadSuballocator::Slice previous = m_slice;
      if (owner && owner != m_suballocator) {
        m_suballocator = owner;
        m_sharedState = slice.state;
        m_fixedState = !slice.state;
        m_persistentlyMapped = owner->isMapped();
        m_sizeClass = slice.sizeClass;
        if (!slice.state)
          m_currentState = D3D12_RESOURCE_STATE_GENERIC_READ;
      }
      m_slice = slice;
      m_resource = slice.resource;
      m_offset = slice.offset;
      m_gpuAddress = slice.resource->GetGPUVirtualAddress() + slice.offset;
      m_persistentMappedData = slice.mapped;
      return previous;
    }
    D3D12UploadSuballocator* getSuballocator() const { return m_suballocator; }

    // Default constant buffers the application replaces with UpdateSubresource
    // move to upload slices, renamed for every update like WRITE_DISCARD: a
    // copy into GPU memory needs a barrier pair around it, which drains the
    // GPU (Unreal Engine 4 updates its global constants before most draws).
    // GPU writes move them back to a default slice.
    bool isRenamableConstantBuffer() const { return m_renamableConstantBuffer; }
    void setRenamableConstantBuffer() { m_renamableConstantBuffer = true; }
    // On an upload slice now (recording thread).
    bool usesUploadSlice() const { return m_renamableConstantBuffer && m_fixedState; }
    // The newest contents the application thread wrote to an upload slice,
    // or null when the GPU may have written the buffer since.
    uint8_t* getRenamedData() const { return m_renamedData; }
    uint8_t* getRenameShadow() const { return m_renameShadow.get(); }
    uint8_t* createRenameShadow(size_t size) {
      m_renameShadow = std::make_unique<uint8_t[]>(size);
      return m_renameShadow.get();
    }
    void setRenamedData(uint8_t* data) { m_renamedData = data; }
    // The data of the last whole update, while the slice it went to is the
    // current one: an update with the same data needs no new slice.
    bool sameAsLastUpdate(const void* data, size_t size) const {
      return m_lastUpdateSlice && m_lastUpdateSlice == m_renamedData && m_lastUpdate.size() == size
        && std::memcmp(m_lastUpdate.data(), data, size) == 0;
    }
    void noteLastUpdate(const void* data, size_t size, uint8_t* slice) {
      m_lastUpdate.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
      m_lastUpdateSlice = slice;
    }
    void forgetLastUpdate() { m_lastUpdateSlice = nullptr; }
    // Deferred contexts write the buffer: no more renames.
    bool renameBlocked() const { return m_renameBlocked.load(std::memory_order_relaxed); }
    void blockRename() { m_renameBlocked.store(true, std::memory_order_relaxed); }
    // The slice size class of a suballocated buffer; every slice it gets
    // has this one.
    uint32_t getSizeClass() const { return m_sizeClass; }
    // Memory the CPU writes in place for as long as the buffer lives.
    bool isPersistentlyMapped() const { return m_persistentlyMapped; }
    D3D12_GPU_VIRTUAL_ADDRESS getGpuAddress() const {
      return m_gpuAddress;
    }
    void* getPersistentMappedData() const { return m_persistentMappedData; }

    D3D12_RESOURCE_STATES getCurrentState() const { return m_sharedState ? *m_sharedState : m_currentState; }
    void setCurrentState(D3D12_RESOURCE_STATES state) {
      if (m_sharedState)
        *m_sharedState = state;
      else
        m_currentState = state;
    }
    // A slice of a default block: its state is the block's.
    bool hasSharedState() const { return m_sharedState != nullptr; }
    // Upload heap buffers stay in GENERIC_READ; others are tracked.
    bool hasFixedState() const { return m_fixedState; }
    void setFixedState(bool fixed) { m_fixedState = fixed; }

    // Buffers shared with other devices.
    D3D12SharedSync* getSharedSync() const { return m_sharedSync.get(); }
    const std::shared_ptr<D3D12SharedSync>& sharedSync() const { return m_sharedSync; }
    void setSharedSync(std::shared_ptr<D3D12SharedSync> sync) { m_sharedSync = std::move(sync); }

    // Stream output filled size in bytes, like D3D11 kept per buffer.
    // Created the first time the buffer is bound as a stream output target.
    ID3D12Resource* getStreamOutputCounter(ID3D12Device* device);
    ID3D12Resource* streamOutputCounter() const { return m_soCounter.Get(); }
    D3D12_RESOURCE_STATES m_soCounterState = D3D12_RESOURCE_STATE_COPY_DEST;

    // Tile pools have no resource, only heaps: the first one sized at
    // creation, one more for each time the pool grew.
    struct TileHeap {
      ComPtr<ID3D12Heap> heap;
      uint32_t           firstTile = 0;
      uint32_t           tileCount = 0;
    };
    std::vector<TileHeap> m_tileHeaps;
    uint64_t              m_tilePoolSize = 0;

    // Creates the heap that grows the pool to newSize; appendTileHeap adds
    // it in order with the commands that use the pool.
    HRESULT createTileHeap(ID3D12Device* device, uint64_t newSize, TileHeap& heap);
    // Tiles covered by created heaps, appended or not yet.
    uint32_t              m_reservedTiles = 0;
    void appendTileHeap(TileHeap heap);
#endif

  private:
#ifdef _WIN32
    ComPtr<ID3D12Resource>    m_resource;
    // Queried once: binds read it for every draw.
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuAddress = 0;
    void*                     m_persistentMappedData = nullptr;
    D3D12_RESOURCE_STATES     m_currentState = D3D12_RESOURCE_STATE_COMMON;
    bool                      m_fixedState = false;
    uint64_t                  m_offset = 0;
    D3D12UploadSuballocator*  m_suballocator = nullptr;
    D3D12UploadSuballocator::Slice m_slice;
    uint32_t                  m_sizeClass = 0;
    bool                      m_persistentlyMapped = false;
    D3D12_RESOURCE_STATES*    m_sharedState = nullptr;
    bool                      m_renamableConstantBuffer = false;
    uint8_t*                  m_renamedData = nullptr;
    std::unique_ptr<uint8_t[]> m_renameShadow;
    std::vector<uint8_t>      m_lastUpdate;
    uint8_t*                  m_lastUpdateSlice = nullptr;
    std::atomic<bool>         m_renameBlocked{ false };
    uint8_t*                  m_appMapped = nullptr;
    ComPtr<ID3D12Resource>    m_soCounter;
    std::shared_ptr<D3D12SharedSync> m_sharedSync;
#endif
    BufferDesc                m_desc;
  };

  // ============================================================
  // D3D12 Backend Image
  // ============================================================
  class D3D12BackendImageView;

  class D3D12BackendImage : public IBackendImage, public std::enable_shared_from_this<D3D12BackendImage> {
  public:
#ifdef _WIN32
    // Shader resource views of this image, rewritten when its min LOD
    // changes, and every view of a retargetable image. Views register when
    // created and leave when destroyed.
    std::mutex                           m_srvViewMutex;
    std::vector<D3D12BackendImageView*>  m_srvViews;
#endif

    D3D12BackendImage(
#ifdef _WIN32
      ComPtr<ID3D12Resource>   resource,
#endif
      const ImageDesc&         desc,
      bool                     simultaneousAccess = false);

#ifdef _WIN32
    // A render target whose memory comes with its first write (a reserved
    // resource without tiles until then, which reads as zero): the tiles it
    // still needs, 0 once it has them (D3D12BackendDevice::commitLazyImage).
    std::atomic<uint32_t>                m_lazyTiles{ 0 };
    bool isLazyUncommitted() const { return m_lazyTiles.load(std::memory_order_acquire) != 0; }
#endif

    const ImageDesc& getDesc() const override { return m_desc; }

#ifdef _WIN32
    ID3D12Resource* getResource() const { return m_resource.Get(); }
    DXGI_FORMAT getFormat() const { return m_desc.format; }
    // A swap chain buffer the application draws into directly: another
    // real buffer after every Present (D3D12BackendContext::retargetImage).
    // Its render target views may take the other format of the family, as
    // swap chain buffers allow.
    void setRetargetable() { m_retargetable = true; }
    bool isRetargetable() const { return m_retargetable; }
    void retarget(ComPtr<ID3D12Resource> resource, D3D12_RESOURCE_STATES state) {
      m_resource = std::move(resource);
      setCurrentState(state);
    }
    bool allowsSimultaneousAccess() const { return m_simultaneousAccess; }

    // Images shared with other devices.
    D3D12SharedSync* getSharedSync() const { return m_sharedSync.get(); }
    const std::shared_ptr<D3D12SharedSync>& sharedSync() const { return m_sharedSync; }
    void setSharedSync(std::shared_ptr<D3D12SharedSync> sync) { m_sharedSync = std::move(sync); }

    // Staging images are a buffer in CPU-visible memory holding every
    // subresource as a placed footprint of the texture they stand for;
    // copies address them through the footprints and Map returns the
    // memory directly.
    struct StagingLayout {
      D3D12_RESOURCE_DESC                             textureDesc = {};
      std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
      std::vector<UINT>                               numRows;
      std::vector<UINT64>                             rowSizes;
    };
    void initStaging(std::shared_ptr<D3D12BackendBuffer> buffer, StagingLayout layout, uint8_t* mapped,
                     ID3D12Device* device);
    // Swaps in memory holding the current contents; the old memory stays a
    // spare until the GPU passed readFence (renameStagingImage).
    bool renameStaging(uint64_t readFence, uint64_t completedFence);
    bool isStaging() const { return m_stagingBuffer != nullptr; }
    D3D12BackendBuffer* getStagingBuffer() const { return m_stagingBuffer.get(); }
    const StagingLayout& getStagingLayout() const { return m_staging; }
    // The texture description, also for staging images.
    D3D12_RESOURCE_DESC getTextureDesc() const {
      return m_stagingBuffer ? m_staging.textureDesc : m_resource->GetDesc();
    }

    bool isCpuAccessible() const override { return m_stagingBuffer != nullptr; }
    uint8_t* mapStaging(uint32_t subresource, MapMode mode, ImageSubresourceLayout& layout) override;
    void unmapStaging(uint32_t subresource) override;

    D3D12_RESOURCE_STATES getCurrentState() const { return m_currentState; }
    void setCurrentState(D3D12_RESOURCE_STATES state) {
      m_currentState = state;
      m_isUniformState = true;
      for (auto& subresourceState : m_subresourceStates)
        subresourceState = state;
    }
    bool hasUniformState() const { return m_isUniformState; }
    uint32_t getSubresourceCount() const { return static_cast<uint32_t>(m_subresourceStates.size()); }
    uint32_t getMipLevelCount() const { return m_desc.mipLevels ? m_desc.mipLevels : 1u; }
    uint32_t getArraySubresourceCount() const {
      return m_desc.type == ImageType::Tex3D
        ? 1u
        : (m_desc.arraySize ? m_desc.arraySize : 1u);
    }
    uint32_t getPlaneCount() const { return m_planeCount; }
    uint32_t calcSubresource(uint32_t mipLevel, uint32_t arrayLayer, uint32_t planeSlice = 0) const {
      return mipLevel
        + arrayLayer * getMipLevelCount()
        + planeSlice * getMipLevelCount() * getArraySubresourceCount();
    }
    D3D12_RESOURCE_STATES getSubresourceState(uint32_t subresource) const {
      return subresource < m_subresourceStates.size()
        ? m_subresourceStates[subresource]
        : m_currentState;
    }
    void setSubresourceState(uint32_t subresource, D3D12_RESOURCE_STATES state) {
      if (subresource >= m_subresourceStates.size()) {
        m_currentState = state;
        return;
      }

      m_subresourceStates[subresource] = state;
      updateUniformState();
    }
#endif

  private:
#ifdef _WIN32
    void updateUniformState() {
      if (m_subresourceStates.empty()) {
        m_isUniformState = true;
        return;
      }

      m_currentState = m_subresourceStates[0];
      m_isUniformState = true;

      for (auto subresourceState : m_subresourceStates) {
        if (subresourceState != m_currentState) {
          m_isUniformState = false;
          break;
        }
      }
    }

    ComPtr<ID3D12Resource>    m_resource;
    std::shared_ptr<D3D12BackendBuffer> m_stagingBuffer;
    StagingLayout             m_staging;
    uint8_t*                  m_mappedData = nullptr;
    // Renamed staging memory the GPU may still read, reused once it passed
    // the fence.
    struct StagingSpare {
      std::shared_ptr<D3D12BackendBuffer> buffer;
      uint8_t*                            mapped = nullptr;
      uint64_t                            fence = 0;
    };
    std::vector<StagingSpare> m_stagingSpares;
    ComPtr<ID3D12Device>      m_stagingDevice;
    uint32_t                  m_stagingMaps = 0;
    // Depth-stencil staging images: D3D11 maps depth and stencil
    // interleaved (4 or 8 bytes a texel), D3D12 keeps them in two planes.
    uint32_t                  m_interleavedTexelSize = 0;
    std::vector<std::vector<uint8_t>> m_interleaved;
    std::vector<uint8_t>      m_interleavedWritable;
    D3D12_RESOURCE_STATES     m_currentState = D3D12_RESOURCE_STATE_COMMON;
    std::vector<D3D12_RESOURCE_STATES> m_subresourceStates;
    uint32_t                  m_planeCount = 1;
    bool                      m_isUniformState = true;
    bool                      m_simultaneousAccess = false;
    bool                      m_retargetable = false;
    std::shared_ptr<D3D12SharedSync> m_sharedSync;
#endif
    ImageDesc                 m_desc;
  };

  // ============================================================
  // D3D12 Backend Buffer View
  // ============================================================
  class D3D12BackendBufferView : public IBackendBufferView {
  public:
    D3D12BackendBufferView(
      IBackendBuffer*          buffer,
      const BufferViewDesc&    desc
    , Rc<IBackendBuffer>       counterBuffer = nullptr
    , D3D12DescriptorHeapManager* heapMgr = nullptr
#ifdef _WIN32
    , ID3D12Device*            device = nullptr
    , D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = {}
    , D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = {}
    , D3D12_CPU_DESCRIPTOR_HANDLE nullSrvHandle = {}
    , D3D12_CPU_DESCRIPTOR_HANDLE nullUavHandle = {}
#endif
    );

    ~D3D12BackendBufferView();

    const BufferViewDesc& getDesc() const override { return m_desc; }
    IBackendBuffer* getBuffer() const override { return m_buffer; }
  IBackendBuffer* getCounterBuffer() const override { return m_counterBuffer.get(); }
    bool retargetBuffer(IBackendBuffer* buffer) override;
    uint64_t getDescriptorVersion() const override { return m_descriptorVersion; }

#ifdef _WIN32
    D3D12_CPU_DESCRIPTOR_HANDLE getSrvHandle() const { return m_srvHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE getUavHandle() const { return m_uavHandle; }
    // A raw view of the same bytes, for clearing structured buffers, which
    // D3D12 cannot clear through their own view. Created on first use.
    D3D12_CPU_DESCRIPTOR_HANDLE getRawClearUavHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE getNullSrvHandle() const { return m_nullSrvHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE getNullUavHandle() const { return m_nullUavHandle; }
#endif

  private:
    IBackendBuffer*           m_buffer;
    BufferViewDesc            m_desc;
    Rc<IBackendBuffer>        m_counterBuffer;
    D3D12DescriptorHeapManager* m_heapMgr = nullptr;
    uint64_t                  m_descriptorVersion = nextDescriptorVersion();
#ifdef _WIN32
    ID3D12Device*             m_device = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_uavHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_rawClearUavHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullUavHandle = {};
#endif
  };

  // ============================================================
  // D3D12 Backend Image View
  // ============================================================
  class D3D12BackendImageView : public IBackendImageView {
  public:
    D3D12BackendImageView(
      IBackendImage*           image,
      const ImageViewDesc&     desc,
      D3D12DescriptorHeapManager* heapMgr
#ifdef _WIN32
    , D3D12_CPU_DESCRIPTOR_HANDLE handle = {}
    , D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = {}
#endif
    );

    ~D3D12BackendImageView();

    const ImageViewDesc& getDesc() const override { return m_desc; }
    IBackendImage* getImage() const override { return m_image; }

#ifdef _WIN32
    D3D12_CPU_DESCRIPTOR_HANDLE getHandle() const { return m_handle; }
  D3D12_CPU_DESCRIPTOR_HANDLE getNullHandle() const { return m_nullHandle; }
    void setRtvHandle(D3D12_CPU_DESCRIPTOR_HANDLE h) { m_rtvHandle = h; }
    void setDsvHandle(D3D12_CPU_DESCRIPTOR_HANDLE h) { m_dsvHandle = h; }
    void setUavHandle(D3D12_CPU_DESCRIPTOR_HANDLE h) { m_uavHandle = h; }
  void setNullUavHandle(D3D12_CPU_DESCRIPTOR_HANDLE h) { m_nullUavHandle = h; }
    D3D12_CPU_DESCRIPTOR_HANDLE getRtvHandle() const { return m_rtvHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE getDsvHandle() const { return m_dsvHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE getUavHandle() const { return m_uavHandle; }
  D3D12_CPU_DESCRIPTOR_HANDLE getNullUavHandle() const { return m_nullUavHandle; }
    bool isReadOnlyDepth() const {
      return static_cast<uint32_t>(m_desc.flags & ImageViewFlags::ReadOnlyDepth) != 0;
    }
    bool isReadOnlyStencil() const {
      return static_cast<uint32_t>(m_desc.flags & ImageViewFlags::ReadOnlyStencil) != 0;
    }
    bool hasReadOnlyDepthStencil() const {
      return isReadOnlyDepth() || isReadOnlyStencil();
    }
    bool isReadOnlyDepthStencil() const {
      return isReadOnlyDepth() && isReadOnlyStencil();
    }

    uint64_t getDescriptorVersion() const override { return m_descriptorVersion; }

    // Keeps the SRV description so the descriptor can be rewritten with
    // another min LOD clamp, and registers with the image.
    void trackShaderResourceView(ID3D12Device* device, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc);
    // Rewrites the SRV with the clamp. Called by the recording context.
    void setMinLodClamp(float minLod);
    // Views of a retargetable image keep their descriptions (registered
    // with the image) and are written again for its new resource.
    void trackRetarget(ID3D12Device* device, const D3D12_RENDER_TARGET_VIEW_DESC* rtvDesc);
    void rewriteForImage();
#endif

  private:
    IBackendImage*            m_image;
    // Views keep their image: retired together, the image may otherwise be
    // destroyed first, and the view's destructor unregisters from it.
    std::shared_ptr<D3D12BackendImage> m_imageOwner;
    ImageViewDesc             m_desc;
    D3D12DescriptorHeapManager* m_heapMgr = nullptr;
#ifdef _WIN32
    ID3D12Device*             m_srvDevice = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC m_srvDesc = {};
    ID3D12Device*             m_retargetDevice = nullptr;
    bool                      m_hasRtvDesc = false;
    D3D12_RENDER_TARGET_VIEW_DESC m_rtvDesc = {};
    uint64_t                  m_descriptorVersion = nextDescriptorVersion();
#endif
#ifdef _WIN32
  D3D12_CPU_DESCRIPTOR_HANDLE m_nullHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_handle    = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_uavHandle = {};
  D3D12_CPU_DESCRIPTOR_HANDLE m_nullUavHandle = {};
#endif
  };

  // ============================================================
  // D3D12 Backend Sampler
  // ============================================================
  class D3D12BackendSampler : public IBackendSampler {
  public:
    D3D12BackendSampler(
      const SamplerDesc&       desc,
      D3D12DescriptorHeapManager* heapMgr = nullptr
#ifdef _WIN32
      , D3D12_CPU_DESCRIPTOR_HANDLE handle = {}
#endif
    );

    ~D3D12BackendSampler();

    const SamplerDesc& getDesc() const override { return m_desc; }

#ifdef _WIN32
    D3D12_CPU_DESCRIPTOR_HANDLE getHandle() const { return m_handle; }
#endif
    uint64_t getDescriptorVersion() const { return m_descriptorVersion; }

  private:
    SamplerDesc               m_desc;
    uint64_t                  m_descriptorVersion = nextDescriptorVersion();
    D3D12DescriptorHeapManager* m_heapMgr = nullptr;
#ifdef _WIN32
    D3D12_CPU_DESCRIPTOR_HANDLE m_handle = {};
#endif
  };

  // ============================================================
  // D3D12 Backend Shader
  // ============================================================
  class D3D12BackendShader : public IBackendShader {
  public:
    D3D12BackendShader(const ShaderDesc& desc);

    ShaderStage getStage() const override { return m_stage; }
    const void* getBytecode() const override { return m_bytecode.data(); }
    size_t getBytecodeSize() const override { return m_bytecode.size(); }
    uint64_t getBytecodeHash() const override { return m_bytecodeHash; }
    uint32_t getPixelShaderTraits() const override { return m_pixelShaderTraits; }
    const uint64_t* getShaderResourceSlots() const override { return m_srvSlotsKnown ? m_srvSlots : nullptr; }
    // The immediate constant buffer, now read from constant buffer slot
    // getIcbSlot() (UINT32_MAX: none).
    uint32_t getIcbSlot() const { return m_icbSlot; }
    const std::vector<uint32_t>& getIcbData() const { return m_icbData; }

  private:
    ShaderStage               m_stage;
    uint32_t                  m_icbSlot = UINT32_MAX;
    std::vector<uint32_t>     m_icbData;
    std::vector<uint8_t>      m_bytecode;
    uint64_t                  m_bytecodeHash = 0;
    uint32_t                  m_pixelShaderTraits = 0;
    uint64_t                  m_srvSlots[2] = {};
    bool                      m_srvSlotsKnown = false;
  };

  // ============================================================
  // D3D12 Backend Graphics Pipeline
  // ============================================================
  class D3D12BackendGraphicsPipeline : public IBackendPipeline {
  public:
    D3D12BackendGraphicsPipeline(
#ifdef _WIN32
      ComPtr<ID3D12PipelineState>  pso,
      ID3D12RootSignature*         rootSig,
#endif
      PrimitiveTopology            topology,
      const PipelineResourceLimits& resourceLimits = {},
      uint32_t                     activeStageMask = 0x1Fu);

#ifdef _WIN32
    ID3D12PipelineState* getPSO() const { return m_current.load(std::memory_order_acquire); }
    // The same pipeline compiled better (masked outputs, built later on a
    // compiler thread): draws bind it from then on. The first one stays for
    // command lists that still use it.
    void replacePSO(ComPtr<ID3D12PipelineState> pso);
    ID3D12RootSignature* getRootSignature() const { return m_rootSignature; }
    const PipelineResourceLimits& getResourceLimits() const { return m_resourceLimits; }
    uint32_t getActiveStageMask() const { return m_activeStageMask; }
#endif
    PrimitiveTopology getTopology() const { return m_topology; }
    // The vertex buffer slot inputs missing from the input layout read; it
    // must stay unbound while the pipeline draws. UINT32_MAX: none.
    uint32_t getNullVertexSlot() const { return m_nullVertexSlot; }
    void setNullVertexSlot(uint32_t slot) { m_nullVertexSlot = slot; }
#ifdef _WIN32
    // Stands for a pipeline that compiles in the background
    // (createGraphicsPipelineDeferred); D3D12BackendDevice::resolvePipeline
    // gives the compiled one.
    bool isPending() const { return m_pending; }
#endif

  private:
#ifdef _WIN32
    friend class D3D12BackendDevice;
    // Pending pipelines: what to compile (until it is compiled, under
    // m_resolveMutex), and the compiled pipeline, kept by the device's cache.
    bool                         m_pending = false;
    std::shared_ptr<void>        m_pendingDesc;
    std::atomic<D3D12BackendGraphicsPipeline*> m_resolved{nullptr};
    std::atomic<bool>            m_failed{false};
    std::mutex                   m_resolveMutex;
    ComPtr<ID3D12PipelineState>  m_pso;
    ComPtr<ID3D12PipelineState>  m_replacement;   // under m_resolveMutex
    std::atomic<ID3D12PipelineState*> m_current{nullptr};
    ID3D12RootSignature*         m_rootSignature = nullptr;
    PipelineResourceLimits       m_resourceLimits = {};
    uint32_t                     m_activeStageMask = 0x1Fu;
#endif
    uint32_t                     m_nullVertexSlot = UINT32_MAX;
    PrimitiveTopology            m_topology;
  };

  // ============================================================
  // D3D12 Backend Compute Pipeline
  // ============================================================
  class D3D12BackendComputePipeline : public IBackendPipeline {
  public:
    D3D12BackendComputePipeline(
#ifdef _WIN32
      ComPtr<ID3D12PipelineState>  pso,
      ID3D12RootSignature*         rootSig,
      const ShaderResourceLimits&  resourceLimits = {}
#endif
    );

#ifdef _WIN32
    ID3D12PipelineState* getPSO() const { return m_pso.Get(); }
    ID3D12RootSignature* getRootSignature() const { return m_rootSignature; }
    const ShaderResourceLimits& getResourceLimits() const { return m_resourceLimits; }
#endif
    // Bytecode hash of the shader, for diagnostics.
    uint64_t getShaderHash() const { return m_shaderHash; }
    void setShaderHash(uint64_t hash) { m_shaderHash = hash; }

  private:
#ifdef _WIN32
    ComPtr<ID3D12PipelineState>  m_pso;
    ID3D12RootSignature*         m_rootSignature = nullptr;
    ShaderResourceLimits         m_resourceLimits = {};
#endif
    uint64_t                     m_shaderHash = 0;
  };

  // ============================================================
  // D3D12 Backend Query
  // ============================================================
  class D3D12BackendContext;

#ifdef _WIN32
  struct D3D12QuerySlot {
    ID3D12QueryHeap*  heap = nullptr;
    uint32_t          index = 0;
    ID3D12Resource*   readback = nullptr;
    uint64_t          readbackOffset = 0;
    const uint8_t*    data = nullptr;
  };

  // A query result to copy into readback memory when the command list is
  // submitted; resolving them together keeps the GPU from serializing on
  // one small copy per query.
  struct D3D12QueryResolve {
    ID3D12QueryHeap*  heap = nullptr;
    D3D12_QUERY_TYPE  type = D3D12_QUERY_TYPE_OCCLUSION;
    uint32_t          index = 0;
    ID3D12Resource*   readback = nullptr;
    uint64_t          readbackOffset = 0;
  };

  // Query heaps with persistently mapped readback buffers, handed out one
  // slot at a time. Slots are reused in command order, so a slot can go back
  // to the pool as soon as its query object is gone.
  class D3D12QueryPool {
  public:
    bool allocate(ID3D12Device* device, D3D12_QUERY_HEAP_TYPE type, D3D12QuerySlot& slot);
    void free(D3D12_QUERY_HEAP_TYPE type, const D3D12QuerySlot& slot);

    static uint32_t resultSize(D3D12_QUERY_HEAP_TYPE type);

  private:
    static constexpr uint32_t kSlotsPerPage = 256;

    struct Page {
      ComPtr<ID3D12QueryHeap> heap;
      ComPtr<ID3D12Resource>  readback;
      const uint8_t*          mapped = nullptr;
    };

    struct Pool {
      std::vector<Page>           pages;
      std::vector<D3D12QuerySlot> freeSlots;
    };

    std::mutex  m_mutex;
    Pool        m_pools[4];
  };
#endif

  // ============================================================
  // D3D12 Backend Query
  //
  // Recorded by one context at a time. Occlusion, pipeline statistics and
  // stream output queries that stay active across a command list
  // submission are split into segments whose results are summed.
  // ============================================================
  class D3D12BackendQuery : public IBackendQuery {
    friend class D3D12BackendContext;
  public:
    D3D12BackendQuery(D3D12BackendDevice* device, const QueryDesc& desc);
    ~D3D12BackendQuery() override;

    QueryType getType() const override { return m_desc.type; }
    QueryStatus getData(QueryData& data) override;

    const QueryDesc& getDesc() const { return m_desc; }

  private:
    D3D12BackendDevice*   m_device;
    QueryDesc             m_desc;

#ifdef _WIN32
    // Begin/End bracket GPU work (occlusion, statistics).
    bool hasGpuScope() const;
    D3D12_QUERY_HEAP_TYPE heapType() const;
    D3D12_QUERY_TYPE gpuQueryType(uint32_t slotInSegment) const;
    uint32_t slotsPerSegment() const;
    bool beginSegment(ID3D12GraphicsCommandList* list);
    void endSegment(ID3D12GraphicsCommandList* list, std::vector<D3D12QueryResolve>& resolves);
    bool writeTimestamp(ID3D12GraphicsCommandList* list, std::vector<D3D12QueryResolve>& resolves);
    ID3D12Resource* predicateBuffer();

    // Recording state, owned by the recording thread.
    std::vector<D3D12QuerySlot> m_slots;
    uint32_t                    m_segments = 0;
    D3D12BackendContext*        m_context = nullptr;  // while active or used as predicate
    ComPtr<ID3D12Resource>      m_predicateBuffer;
    D3D12_RESOURCE_STATES       m_predicateState = D3D12_RESOURCE_STATE_COPY_DEST;
    bool                        m_predicateValid = false;

    // Published to getData() when the query is ended.
    std::atomic<bool>           m_active{false};
    std::atomic<bool>           m_issued{false};
    std::atomic<uint64_t>       m_fenceValue{0};
    ComPtr<ID3D12Fence>         m_fence;
    std::shared_ptr<std::atomic<uint64_t>> m_submittedFence;
    uint32_t                    m_issuedSegments = 0;

    // Bumped by every End; getData() caches the result of the last one it
    // saw finished, so polling does not touch the fence again.
    std::atomic<uint64_t>       m_issueSerial{0};
    uint64_t                    m_cachedSerial = 0;
    QueryData                   m_cachedData;
#endif
  };

  // ============================================================
  // Descriptor Heap Manager
  // ============================================================
  // CPU descriptors of views and samplers. Each type grows by heap pages
  // when one is full; freed descriptors are reused.
  class D3D12DescriptorHeapManager {
  public:
#ifdef _WIN32
    void init(ID3D12Device* device);

    D3D12_CPU_DESCRIPTOR_HANDLE allocateRtv() { return allocate(m_rtv); }
    D3D12_CPU_DESCRIPTOR_HANDLE allocateDsv() { return allocate(m_dsv); }
    D3D12_CPU_DESCRIPTOR_HANDLE allocateCbvSrvUav() { return allocate(m_cbvSrvUav); }
    D3D12_CPU_DESCRIPTOR_HANDLE allocateSampler() { return allocate(m_sampler); }

    void freeRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle) { free(m_rtv, handle); }
    void freeDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle) { free(m_dsv, handle); }
    void freeCbvSrvUav(D3D12_CPU_DESCRIPTOR_HANDLE handle) { free(m_cbvSrvUav, handle); }
    void freeSampler(D3D12_CPU_DESCRIPTOR_HANDLE handle) { free(m_sampler, handle); }

  private:
    struct Pool {
      D3D12_DESCRIPTOR_HEAP_TYPE                type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      uint32_t                                  pageSize = 0;
      uint32_t                                  descriptorSize = 0;
      std::vector<ComPtr<ID3D12DescriptorHeap>> pages;
      SIZE_T                                    next = 0;
      uint32_t                                  remaining = 0;
      std::vector<SIZE_T>                       freeList;
    };

    void initPool(Pool& pool, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t pageSize);
    D3D12_CPU_DESCRIPTOR_HANDLE allocate(Pool& pool);
    void free(Pool& pool, D3D12_CPU_DESCRIPTOR_HANDLE handle);

    std::mutex     m_mutex;
    ID3D12Device*  m_device = nullptr;
    Pool           m_rtv;
    Pool           m_dsv;
    Pool           m_cbvSrvUav;
    Pool           m_sampler;
#endif
  };

  // ============================================================
  // D3D12 Backend Device
  // ============================================================
  class D3D12BackendDevice : public IBackendDevice {
  public:
    // adapterLuid selects the adapter the application asked for; without it
    // the first hardware adapter is used.
    explicit D3D12BackendDevice(const LUID* adapterLuid = nullptr);
    ~D3D12BackendDevice();

    // IBackendDevice implementation
    Rc<IBackendBuffer> createBuffer(const BufferDesc& desc) override;
    Rc<IBackendImage> createImage(const ImageDesc& desc) override;
    Rc<IBackendImage> createStagingImage(const ImageDesc& desc, D3D12_RESOURCE_DESC textureDesc);
    Rc<IBackendSampler> createSampler(const SamplerDesc& desc) override;

    Rc<IBackendBufferView> createBufferView(IBackendBuffer* buffer, const BufferViewDesc& desc) override;
    Rc<IBackendImageView> createImageView(IBackendImage* image, const ImageViewDesc& desc) override;

    Rc<IBackendShader> createShader(const ShaderDesc& desc) override;
    // The constant buffer standing for an immediate constant buffer of the
    // shader with this bytecode hash, into a stage's limits.
    void attachIcb(ShaderResourceLimits& limits, const void* bytecode, size_t size, uint64_t hash);
    Rc<IBackendPipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    Rc<IBackendPipeline> createComputePipeline(const ComputePipelineDesc& desc) override;
    bool asyncPipelines() const override { return m_asyncDraws && !m_compileThreads.empty(); }
    uint32_t takeFailedPipelineDraws() override { return m_failedPipelineDraws.exchange(0, std::memory_order_relaxed); }
    void noteFailedPipelineDraw() { m_failedPipelineDraws.fetch_add(1, std::memory_order_relaxed); }
    std::atomic<uint32_t> m_failedPipelineDraws{0};
    bool compilesInBackground() const override { return !m_compileThreads.empty(); }
    Rc<IBackendPipeline> requestGraphicsPipeline(const GraphicsPipelineDesc& desc, bool& pending) override;
    void requestComputePipeline(const ComputePipelineDesc& desc) override;
    Rc<IBackendPipeline> createGraphicsPipelineDeferred(const GraphicsPipelineDesc& desc) override;
#ifdef _WIN32
    // The compiled pipeline of a pending one (itself for others), waiting
    // for it or compiling it here; null when it failed to compile.
    D3D12BackendGraphicsPipeline* resolvePipeline(D3D12BackendGraphicsPipeline* pipeline);
#endif

    Rc<IBackendQuery> createQuery(const QueryDesc& desc) override;
    Rc<IBackendContext> createContext() override;
    Rc<IBackendContext> createCopyContext() override;
    Rc<IBackendContext> beginInitialData() override;
    void endInitialData(IBackendContext* context) override;
    // Submits the recorded initial data; immediate contexts call it before
    // they submit.
    void submitInitialData();

    bool checkFeatureSupport(BackendFeature feature) override;
    BackendLimits getLimits() override;
    BackendType getBackendType() const override { return BackendType::D3D12; }
    uint32_t checkMultisampleQualityLevels(DXGI_FORMAT format, uint32_t sampleCount) override;

#ifdef _WIN32
    ID3D12Device* getDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* getCommandQueue() const { return m_commandQueue.Get(); }
  IDXGIFactory4* getDxgiFactory() const { return m_dxgiFactory.Get(); }
  IDXGIAdapter1* getAdapter() const { return m_adapter.Get(); }
    void* getNativeCommandQueue() override { return m_commandQueue.Get(); }
    std::shared_ptr<BackendRetireQueue> getRetireQueue() const override { return m_retireQueue; }
    ID3D12RootSignature* getCommonRootSignature() const { return m_graphicsRootSignature.Get(); }
    ID3D12RootSignature* getGraphicsRootSignature() const { return m_graphicsRootSignature.Get(); }
    // 64 KB of zeros: unbound constant buffers read 0 in D3D11.
    D3D12_GPU_VIRTUAL_ADDRESS getZeroConstantBufferAddress() const {
      return m_zeroConstantBuffer ? m_zeroConstantBuffer->GetGPUVirtualAddress() : 0;
    }
    // Null views of each resource dimension a shader declares (index as in
    // ShaderResourceLimits::dimensions): unbound slots read 0 in D3D11, and
    // D3D12 needs the null view to match the declaration. The default
    // sampler is what D3D11 samples with when no sampler is bound.
    D3D12_CPU_DESCRIPTOR_HANDLE getNullSrv(uint8_t dimension) const {
      return m_nullSrvs[dimension < 13 ? dimension : 0];
    }
    D3D12_CPU_DESCRIPTOR_HANDLE getNullUav(uint8_t dimension) const {
      return m_nullUavs[dimension < 13 ? dimension : 0];
    }
    D3D12_CPU_DESCRIPTOR_HANDLE getDefaultSampler() const { return m_defaultSampler; }
    ID3D12RootSignature* getComputeRootSignature() const { return m_computeRootSignature.Get(); }
    D3D12DescriptorHeapManager& getDescriptorHeapManager() { return m_descriptorHeapManager; }
    ID3D12CommandSignature* getDrawIndirectSignature() const { return m_drawIndirectSignature.Get(); }
    ID3D12CommandSignature* getDrawIndexedIndirectSignature() const { return m_drawIndexedIndirectSignature.Get(); }
    ID3D12CommandSignature* getDispatchIndirectSignature() const { return m_dispatchIndirectSignature.Get(); }
    bool isGpuSyncValidationEnabled() const { return m_gpuSyncValidationEnabled; }
    bool isDebugLayerEnabled() const { return m_debugLayerEnabled; }
    // Logs the errors the debug layer found since the last call.
    void drainDebugMessages();
    HRESULT flushAndCheckGpu(const char* tag);
    bool recycleTemporaryBuffer(Rc<IBackendBuffer> buffer);
#endif

  private:
#ifdef _WIN32
    ComPtr<IDXGIFactory4>        m_dxgiFactory;
    ComPtr<IDXGIAdapter1>        m_adapter;
    ComPtr<ID3D12Device>         m_device;
    ComPtr<ID3D12CommandQueue>   m_commandQueue;
    ComPtr<ID3D12Fence>          m_fence;
    uint64_t                     m_fenceValue = 0;
    HANDLE                       m_fenceEvent = nullptr;

    ComPtr<ID3D12RootSignature>  m_graphicsRootSignature;
    ComPtr<ID3D12Resource>       m_zeroConstantBuffer;
    ComPtr<ID3D12RootSignature>  m_computeRootSignature;
    D3D12DescriptorHeapManager   m_descriptorHeapManager;
    bool                         m_debugLayerEnabled = false;
    bool                         m_gpuSyncValidationEnabled = false;

    struct RecycledBufferKey {
      uint64_t size = 0;
      uint32_t structStride = 0;
      uint32_t usage = 0;
      uint32_t bindFlags = 0;
      uint32_t cpuAccess = 0;
      uint32_t miscFlags = 0;

      bool operator==(const RecycledBufferKey& other) const {
        return size == other.size
            && structStride == other.structStride
            && usage == other.usage
            && bindFlags == other.bindFlags
            && cpuAccess == other.cpuAccess
            && miscFlags == other.miscFlags;
      }
    };
    struct RecycledBufferKeyHash {
      size_t operator()(const RecycledBufferKey& key) const {
        size_t hash = static_cast<size_t>(key.size ^ (key.size >> 32));
        hash = (hash * 16777619u) ^ key.structStride;
        hash = (hash * 16777619u) ^ key.usage;
        hash = (hash * 16777619u) ^ key.bindFlags;
        hash = (hash * 16777619u) ^ key.cpuAccess;
        hash = (hash * 16777619u) ^ key.miscFlags;
        return hash;
      }
    };
    std::unordered_map<RecycledBufferKey, std::vector<Rc<IBackendBuffer>>, RecycledBufferKeyHash> m_recycledBuffers;
    std::mutex m_recycledBufferMutex;
    size_t m_recycledBufferCount = 0;

    static bool isReusableUploadBuffer(const BufferDesc& desc);
    static RecycledBufferKey makeRecycledBufferKey(const BufferDesc& desc);
    Rc<IBackendBuffer> acquireRecycledBuffer(const BufferDesc& desc);

    void createDevice();

    D3D12UploadSuballocator   m_uploadSuballocator{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12UploadSuballocator   m_defaultSuballocator{ D3D12_HEAP_TYPE_DEFAULT };
    std::shared_ptr<D3D12PlacedAllocator> m_textureAllocator =
      std::make_shared<D3D12PlacedAllocator>(D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES, 16ull << 20);
    // Streamed vertex and index buffers reach tens of megabytes; a committed
    // resource that size takes a millisecond or more to create.
    std::shared_ptr<D3D12PlacedAllocator> m_bufferAllocator =
      std::make_shared<D3D12PlacedAllocator>(D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, 32ull << 20);
    // Dynamic buffers that cannot be upload slices (shader views): upload
    // heaps.
    std::shared_ptr<D3D12PlacedAllocator> m_dynamicBufferAllocator =
      std::make_shared<D3D12PlacedAllocator>(D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, 32ull << 20, D3D12_HEAP_TYPE_UPLOAD);
  public:
    // Memory no longer needed goes back (upload blocks, empty placed heaps);
    // at present, each at most every 10 seconds.
    void trimIdleMemory() {
      m_uploadSuballocator.trim();
      m_textureAllocator->trim();
      m_bufferAllocator->trim();
      m_dynamicBufferAllocator->trim();
    }
    D3D12UploadSuballocator& getUploadSuballocator() { return m_uploadSuballocator; }
    // Graphics and compute pipelines made so far (dxup_memory_stats.txt).
    std::pair<size_t, size_t> pipelineCounts() {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      return { m_psoCache.size(), m_computePsoCache.size() };
    }
    D3D12UploadSuballocator& getDefaultSuballocator() { return m_defaultSuballocator; }
    // Returns slices to the allocator they came from (default slices carry
    // their block's state).
    void freeSlices(const std::vector<D3D12UploadSuballocator::Slice>& slices);
  private:

    // One copy context collects the initial data of new resources.
    std::mutex                m_initDataMutex;
    Rc<IBackendContext>       m_initDataContext;
    std::atomic<bool>         m_initDataPending{false};
    uint32_t                  m_initDataRecorded = 0;
    bool acceptsAdapter(const DXGI_ADAPTER_DESC1& desc) const;
    uint32_t m_vendorId = 0;
  public:
    // PCI vendor of the adapter (0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel).
    uint32_t getVendorId() const { return m_vendorId; }
  public:
    // Gives a lazily committed render target its memory before the first
    // command that writes it: a heap of its own mapped onto all its tiles.
    // True when this call mapped them; the caller then initializes the
    // target (D3D12BackendContext::commitLazyImage).
    bool commitLazyImage(D3D12BackendImage* image);
  private:
    // Whether render targets are committed on their first write
    // (d3d12.lazyTargets; NVIDIA by default, where reserved targets render
    // as fast as committed ones).
    bool m_lazyTargets = false;
    bool m_resourceHeapTier2 = false;
    // D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT (Direct3D 9's anisotropic
    // filter with point mips); without it such samplers filter mips linearly.
    bool m_anisotropicPointMip = false;
    std::mutex m_lazyCommitMutex;
    // A reserved render target for the description, or nothing when it
    // does not qualify; tiles is the count it needs.
    ComPtr<ID3D12Resource> createLazyTarget(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                                            const D3D12_CLEAR_VALUE* clearValue, uint32_t& tiles);
    bool m_hasAdapterLuid = false;
    LUID m_adapterLuid = {};
    // dxgi.adapter or DXUP_ADAPTER: part of the adapter's name, or "warp".
    std::string m_adapterChoice;
    void createCommandQueue();
    void createFence();
    void createCommonRootSignature();
    void createCommandSignatures();
    void waitForGpu();

    ComPtr<ID3D12CommandSignature> m_drawIndirectSignature;
    ComPtr<ID3D12CommandSignature> m_drawIndexedIndirectSignature;
    ComPtr<ID3D12CommandSignature> m_dispatchIndirectSignature;

    // PSO caches — avoid recompiling graphics or compute state when D3D11
    // switches back to a previously used shader/state combination.
    struct PsoCacheKey {
      uint64_t vsHash; uint64_t psHash; uint64_t gsHash; uint64_t hsHash; uint64_t dsHash;
      uint64_t vsSize; uint64_t psSize; uint64_t gsSize; uint64_t hsSize; uint64_t dsSize;
      uint64_t inputLayoutHash; uint32_t numInputElements;
      uint64_t blendHash; uint64_t rsHash; uint64_t dssHash;
      PrimitiveTopology topology;
      IndexType stripCutIndexType;
      uint32_t numRenderTargets;
      DXGI_FORMAT rtvFormats[8];
      DXGI_FORMAT dsvFormat;
      uint32_t sampleCount; uint32_t sampleMask;
      uint64_t soHash;
      uint32_t sampleShading;
      bool operator==(const PsoCacheKey& o) const {
        if (vsHash != o.vsHash || psHash != o.psHash || gsHash != o.gsHash
         || hsHash != o.hsHash || dsHash != o.dsHash
         || vsSize != o.vsSize || psSize != o.psSize || gsSize != o.gsSize
         || hsSize != o.hsSize || dsSize != o.dsSize
         || inputLayoutHash != o.inputLayoutHash || numInputElements != o.numInputElements
         || blendHash != o.blendHash || rsHash != o.rsHash || dssHash != o.dssHash
         || topology != o.topology || stripCutIndexType != o.stripCutIndexType
         || numRenderTargets != o.numRenderTargets || dsvFormat != o.dsvFormat
         || sampleCount != o.sampleCount || sampleMask != o.sampleMask || soHash != o.soHash
         || sampleShading != o.sampleShading)
          return false;

        for (uint32_t i = 0; i < 8; i++) {
          if (rtvFormats[i] != o.rtvFormats[i])
            return false;
        }
        return true;
      }
    };
    // Fast hash for in-memory lookups, which happen on every pipeline change.
    struct PsoCacheHash {
      size_t operator()(const PsoCacheKey& k) const {
        uint64_t h = 0x9e3779b97f4a7c15ull;
        auto mix = [&h](uint64_t value) {
          h = (h ^ value) * 0xff51afd7ed558ccdull;
          h ^= h >> 32;
        };
        mix(k.vsHash); mix(k.psHash); mix(k.gsHash); mix(k.hsHash); mix(k.dsHash);
        mix(k.vsSize); mix(k.psSize); mix(k.gsSize); mix(k.hsSize); mix(k.dsSize);
        mix(k.inputLayoutHash); mix(k.numInputElements);
        mix(k.blendHash); mix(k.rsHash); mix(k.dssHash);
        mix(static_cast<uint64_t>(k.topology)); mix(static_cast<uint64_t>(k.stripCutIndexType));
        mix(k.numRenderTargets);
        for (uint32_t i = 0; i < 8; i++)
          mix(static_cast<uint64_t>(k.rtvFormats[i]));
        mix(static_cast<uint64_t>(k.dsvFormat)); mix(k.sampleCount); mix(k.sampleMask);
        mix(k.soHash);
        mix(k.sampleShading);
        return static_cast<size_t>(h);
      }
    };
    // Byte-wise hash that names on-disk PSO cache files. Only computed when
    // a pipeline is compiled; kept unchanged so existing caches stay valid.
    struct PsoDiskKeyHash {
      size_t operator()(const PsoCacheKey& k) const {
        size_t h = 14695981039346656037ULL;
        auto append = [&h](const void* value, size_t size) {
          const auto* bytes = static_cast<const uint8_t*>(value);
          for (size_t i = 0; i < size; i++) {
            h ^= bytes[i];
            h = fnv64Multiply(h);
          }
        };

        append(&k.vsHash, sizeof(k.vsHash)); append(&k.psHash, sizeof(k.psHash));
        append(&k.gsHash, sizeof(k.gsHash)); append(&k.hsHash, sizeof(k.hsHash));
        append(&k.dsHash, sizeof(k.dsHash)); append(&k.vsSize, sizeof(k.vsSize));
        append(&k.psSize, sizeof(k.psSize)); append(&k.gsSize, sizeof(k.gsSize));
        append(&k.hsSize, sizeof(k.hsSize)); append(&k.dsSize, sizeof(k.dsSize));
        append(&k.inputLayoutHash, sizeof(k.inputLayoutHash));
        append(&k.numInputElements, sizeof(k.numInputElements));
        append(&k.blendHash, sizeof(k.blendHash)); append(&k.rsHash, sizeof(k.rsHash));
        append(&k.dssHash, sizeof(k.dssHash)); append(&k.topology, sizeof(k.topology));
        append(&k.stripCutIndexType, sizeof(k.stripCutIndexType));
        append(&k.numRenderTargets, sizeof(k.numRenderTargets));
        for (uint32_t i = 0; i < 8; i++)
          append(&k.rtvFormats[i], sizeof(k.rtvFormats[i]));
        append(&k.dsvFormat, sizeof(k.dsvFormat));
        append(&k.sampleCount, sizeof(k.sampleCount));
        append(&k.sampleMask, sizeof(k.sampleMask));
        append(&k.soHash, sizeof(k.soHash));
        // Only then: cache files of other pipelines keep their names.
        if (k.sampleShading)
          append(&k.sampleShading, sizeof(k.sampleShading));
        return h;
      }
    };
    std::unordered_map<PsoCacheKey, Rc<IBackendPipeline>, PsoCacheHash> m_psoCache;
    struct ComputePsoCacheKey {
      uint64_t csHash = 0;
      uint64_t csSize = 0;

      bool operator==(const ComputePsoCacheKey& other) const {
        return csHash == other.csHash && csSize == other.csSize;
      }
    };
    struct ComputePsoCacheHash {
      size_t operator()(const ComputePsoCacheKey& key) const {
        size_t hash = 14695981039346656037ULL;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(&key);
        for (size_t i = 0; i < sizeof(key); i++) {
          hash ^= data[i];
          hash = fnv64Multiply(hash);
        }
        return hash;
      }
    };
    std::unordered_map<ComputePsoCacheKey, Rc<IBackendPipeline>, ComputePsoCacheHash> m_computePsoCache;
    // Pending pipelines handed out, kept as long as the device: contexts
    // refer to them by pointer.
    std::unordered_map<PsoCacheKey, Rc<IBackendPipeline>, PsoCacheHash> m_pendingPsos;
    std::mutex m_psoCacheMutex;
    // Pipelines some thread compiles. Their result enters the cache (null
    // when compiling failed) and m_psoCompiledCv wakes the threads waiting
    // for it; both under m_psoCacheMutex.
    std::unordered_set<PsoCacheKey, PsoCacheHash> m_compilingPsos;
    std::unordered_set<ComputePsoCacheKey, ComputePsoCacheHash> m_compilingComputePsos;
    std::condition_variable m_psoCompiledCv;
    PsoCacheKey graphicsPipelineKey(const GraphicsPipelineDesc& desc) const;
    ComputePsoCacheKey computePipelineKey(const ComputePipelineDesc& desc) const;
    // Compile a pipeline marked as compiling, cache it and wake the waiters.
    Rc<IBackendPipeline> compileGraphicsPipeline(const GraphicsPipelineDesc& desc, const PsoCacheKey& key);
    Rc<IBackendPipeline> compileComputePipeline(const ComputePipelineDesc& desc, const ComputePsoCacheKey& key);
    // masksLater: a pipeline whose masked outputs (d3d12.pixelOutputMasks)
    // give the driver shaders it has not compiled starts without the masks
    // and gets them later (queueMaskedPipeline).
    Rc<IBackendPipeline> buildGraphicsPipeline(const GraphicsPipelineDesc& desc, const PsoCacheKey& key,
                                               bool masksLater = true);
    void queueMaskedPipeline(const GraphicsPipelineDesc& desc, const PsoCacheKey& key,
                             const std::shared_ptr<D3D12BackendGraphicsPipeline>& pipeline);
    Rc<IBackendPipeline> buildComputePipeline(const ComputePipelineDesc& desc, const ComputePsoCacheKey& key);

    // Background compiler threads (d3d11.asyncShaders, precompiled pipelines).
    // A queued job is run by whoever claims it first: a compiler thread, or
    // a thread that needs the pipeline now instead of waiting behind the
    // queue.
    struct CompileJob {
      std::atomic<bool> claimed{false};
      // Queued again in front because a draw waits for it.
      std::atomic<bool> bumped{false};
      std::function<void()> run;
    };
    std::unordered_map<PsoCacheKey, std::shared_ptr<CompileJob>, PsoCacheHash> m_queuedPsoJobs;
    std::unordered_map<ComputePsoCacheKey, std::shared_ptr<CompileJob>, ComputePsoCacheHash> m_queuedComputeJobs;
    std::vector<Thread> m_compileThreads;
    std::deque<std::shared_ptr<CompileJob>> m_compileJobs;
    std::mutex m_compileMutex;
    std::condition_variable m_compileCv;
    bool m_stopCompileThreads = false;
    // Draws skip pipelines that still compile (d3d11.asyncShaders).
    bool m_asyncDraws = false;
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrvs[13] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullUavs[13] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_defaultSampler = {};
    void createNullDescriptors();
    // Started with the device; the list does not change afterwards.
    void startCompileThreads();
    void runCompileThread(bool burst);
    // Compiler threads of their own; the burst ones take jobs only while
    // many wait (loading screens create shaders by the thousand).
    static constexpr size_t kBurstBacklog = 32;
    void stopCompileThreads();
    // False when no thread takes the job. Urgent jobs (a draw waits for
    // them) go before the precompiled ones.
    bool enqueueCompile(std::shared_ptr<CompileJob> job, bool urgent = true);

    // Pipeline state cache (d3d12_pipeline_cache.cpp).
    struct DecodedPipeline;
    struct StoredPipeline {
      std::vector<uint8_t> record;
      // Shaders of the pipeline not created yet.
      uint8_t missing = 0;
    };
    std::mutex m_stateCacheMutex;
    std::string m_stateCachePath;
    // Fixed once the device is created; `missing` changes under the mutex.
    std::vector<StoredPipeline> m_storedPipelines;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_storedPipelinesByShader;
    // Pixel shaders with a stored pipeline that shades per sample, and with
    // one that shades per pixel.
    std::unordered_set<uint64_t> m_storedSampleShadingShaders;
    std::unordered_set<uint64_t> m_storedPixelRateShaders;
    std::unordered_map<uint64_t, std::weak_ptr<D3D12BackendShader>> m_stateCacheShaders;
    std::unordered_set<PsoCacheKey, PsoCacheHash> m_storedPipelineKeys;
    // File end after the last complete record, and the size the file had,
    // when it was read; the disk writer owns the handle. Other devices and
    // processes append to the same file.
    size_t m_stateCacheValidSize = 0;
    size_t m_stateCacheLoadedSize = 0;
    HANDLE m_stateCacheHandle = INVALID_HANDLE_VALUE;
    void loadPipelineStateCache();
    // True when stored pipelines use the shader (they compile in the
    // background once all their shaders exist).
    // True when stored pipelines use the shader; for a pixel shader,
    // perPixelMissing / perSampleMissing: none of them shades per pixel /
    // per sample.
    bool noteShaderCreated(const std::shared_ptr<D3D12BackendShader>& shader, bool& perPixelMissing,
                           bool& perSampleMissing);
    // Compiles a new vertex or pixel shader in the background with a
    // stand-in partner, so its first real pipeline finds it compiled.
    // Warms the stage (perPixel; a pixel shader's per-sample variant too when
    // supersampling may run it: perSample).
    void warmShaderStage(const std::shared_ptr<D3D12BackendShader>& shader, bool perPixel = true, bool perSample = true);
    void compileStageProbe(ShaderStage stage, const std::vector<uint8_t>& bytecode, uint32_t pixelShaderTraits,
                           bool perPixel, bool perSample);
    // Copies, not the compiler's blobs: those count references without
    // atomics, and the compiler threads share these.
    using ProbeBytecode = std::shared_ptr<const std::vector<uint8_t>>;
    static ProbeBytecode compileProbe(const char* source, size_t size, const char* name, const char* target);
    ProbeBytecode probePartnerVs(const dxbc::Signature& pixelInputs);
    std::mutex m_probeMutex;
    ProbeBytecode m_probePs;
    bool m_probePsCompiled = false;
    std::unordered_map<std::string, ProbeBytecode> m_probeVs;
    // The stage bytecode the driver compiled in this process (warm-ups and
    // pipelines): another pipeline with it takes ~0.2 ms, not ~10 ms.
    std::unordered_set<uint64_t> m_warmedStages;
    static uint64_t stageHash(const void* bytecode, size_t size);
    void noteDriverStages(const D3D12_SHADER_BYTECODE& vs, const D3D12_SHADER_BYTECODE& ps);
    bool driverHasStages(const D3D12_SHADER_BYTECODE& vs, const D3D12_SHADER_BYTECODE& ps);
    void logSlowPipeline(const GraphicsPipelineDesc& desc, uint64_t us);
    void queuePrecompile(uint32_t index);
    void storePipelineRecord(const GraphicsPipelineDesc& desc, const PsoCacheKey& key);
    void appendPipelineRecord(const std::vector<uint8_t>& record);
    void closePipelineStateCache();

    // Pipeline compile statistics, logged with dxup_hitch_trace.txt every
    // 256 compiles and when the device goes away.
    struct PsoStats {
      std::atomic<uint32_t> compiles{0};
      std::atomic<uint32_t> diskHits{0};
      std::atomic<uint32_t> precompiled{0};
      std::atomic<uint32_t> warmed{0};
      std::atomic<uint32_t> masksLater{0};   // pipelines that got their masked outputs later
      std::atomic<uint32_t> waits{0};
      std::atomic<uint64_t> compileUs{0};
      std::atomic<uint64_t> maxUs{0};
      std::atomic<uint64_t> waitUs{0};
      std::atomic<int64_t>  loggedAt{0};   // hitch timestamp of the last stats line
    } m_psoStats;
    uint64_t notePsoCompile(int64_t start);
    void notePsoWait(int64_t start);
    void logPsoStats(const char* when);

    // PSO disk cache. Newly compiled blobs are serialized by a background
    // worker so GetCachedBlob and file I/O stay off frame-critical threads.
    struct DeferredPsoDiskWrite {
      size_t keyHash = 0;
      ComPtr<ID3D12PipelineState> pso;
      // Or a pipeline description for the state cache.
      std::vector<uint8_t> record;
    };
    std::string m_psoDiskCacheDir;
    // Pipelines are compiled from arbitrary threads (deferred contexts, shader
    // creation), so blobs are handed out under a lock and removed on use.
    std::unordered_map<size_t, std::vector<uint8_t>> m_psoDiskBlobs;
    // The files are only listed at start and read when their pipeline
    // compiles (mostly on a compiler thread).
    std::unordered_set<size_t> m_psoDiskFiles;
    std::mutex m_psoDiskBlobMutex;

    std::shared_ptr<BackendRetireQueue> m_retireQueue;
    std::deque<DeferredPsoDiskWrite> m_deferredPsoDiskWrites;
    std::unordered_set<size_t> m_deferredPsoDiskWriteKeys;
    std::mutex m_psoDiskWriteMutex;
    std::condition_variable m_psoDiskWriteCv;
    Thread m_psoDiskWriter;
    struct IcbBuffer {
      uint32_t           slot = UINT32_MAX;
      Rc<IBackendBuffer> buffer;
      uint64_t           address = 0;
      uint32_t           size = 0;
    };
    std::mutex                                m_icbMutex;
    std::unordered_map<uint64_t, IcbBuffer>   m_icbBuffers;
    bool m_stopPsoDiskWriter = false;
    void initPsoDiskCache();
    void loadPsoDiskCache();
    std::vector<uint8_t> takePsoDiskBlob(size_t keyHash);
    void deferPsoBlobWrite(size_t keyHash, ID3D12PipelineState* pso);
    void runPsoDiskWriter();
    void stopPsoDiskWriter();
    void savePsoBlobToDisk(size_t keyHash, ID3D12PipelineState* pso);
#endif

  public:
#ifdef _WIN32
    D3D12QueryPool& getQueryPool() { return m_queryPool; }
    uint64_t getTimestampFrequency();
#endif

  private:
#ifdef _WIN32
    D3D12QueryPool m_queryPool;
    std::atomic<uint64_t> m_timestampFrequency{0};
#endif
  };

}
