#pragma once

#include "d3d12_backend_device.h"
#include "d3d12_gpu_profiler.h"
#include "d3d12_command_recorder.h"
#include "../upscale/upscaler.h"
#include "../util/util_config.h"
#include <future>
#include <map>
#include <vector>
#include <array>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace dxup::d3d12 {

  // Maximum number of bound resources per stage
  constexpr uint32_t kGraphicsShaderStages = 5;
  constexpr uint32_t kShaderStages         = static_cast<uint32_t>(ShaderStage::Count);
  constexpr uint32_t kMaxConstantBuffers  = 14;
  constexpr uint32_t kMaxShaderResources  = 128;
  constexpr uint32_t kMaxSamplers         = 16;
  constexpr uint32_t kMaxUAVs             = 64;
  constexpr uint32_t kMaxVertexBuffers    = 32;
  constexpr uint32_t kMaxRenderTargets    = 8;
  constexpr uint32_t kMaxViewports        = 16;
  constexpr uint32_t kMaxQueriesPerHeap   = 4096;
  // Applications flush several times a frame: enough allocators that a
  // reused one belongs to an earlier frame.
  constexpr uint32_t kCommandAllocatorRingSize = 8;
  // Shader-visible descriptor heaps are split into partitions that are
  // filled one after the other across submissions.
  constexpr uint32_t kDescriptorPartitionCount = 3;
  constexpr uint32_t kShaderVisibleCbvSrvUavHeapSize = 1000000;
  // The heap a context starts with: games take 1-3 thousand descriptors a
  // frame (Trine 4, Kena), and the partitions of this one last 20 frames and
  // more. It grows to the full size once a partition is needed again before
  // the GPU is done with it (32 MB instead of 6).
  constexpr uint32_t kInitialCbvSrvUavHeapSize = 3 * 65536;
  constexpr uint32_t kShaderVisibleSamplerHeapSize = 2048;
  // The CBV/SRV/UAV descriptors left before ensureDescriptorHeapSpace moves
  // on: a draw's tables at most (CBV 70 + SRV 640 + UAV 320 = 1030).
  constexpr uint32_t kCbvSrvUavDescriptorReserve = 1100;
  constexpr size_t kMaxCachedCbvDescriptors = 16384;

  // Flush after this many draw/dispatch ops to bound command list size.
  // Higher = fewer flushes = less GPU sync overhead, but larger command lists.
  constexpr uint32_t kDrawOpsFlushThreshold = 8192;
  // Early submission while the GPU has nothing to do (submitIfGpuIdle).
  constexpr uint32_t kEarlySubmitInterval = 128;
  // Images DXUP's own passes read: the state shader resource bindings use,
  // so that the application reading them next needs no transition.
  constexpr D3D12_RESOURCE_STATES kShaderReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr uint32_t kEarlySubmitMinDraws = 512;
  // Upload buffers a command list may collect before it is submitted (less
  // in a 32-bit process: their memory is address space).
  constexpr uint64_t kPendingUploadFlushThreshold = (sizeof(void*) == 4 ? 32ull : 128ull) * 1024ull * 1024ull;
  // The upload ring's slot per command allocator: 8 MB each keeps the ring
  // at the memory three 16 MB slots took; larger requests grow their slot.
  // 2 MB in a 32-bit process, where upload memory is address space, and a
  // grown slot goes back to that once its allocator comes round again.
  constexpr uint64_t kDefaultUploadScratchCapacity = (sizeof(void*) == 4 ? 2ull : 8ull) * 1024ull * 1024ull;

  // ============================================================
  // D3D12 Backend Context
  // ============================================================
  // A game's temporal anti-aliasing pass replaced by an upscaler: its
  // inputs at render resolution, its output, and where its constant buffers
  // hold the camera data (Unreal Engine 4 conventions). The constants are
  // read when the request runs, from the buffer contents of that moment.
  struct UpscaleRequest {
    struct Constant {
      D3D12BackendBuffer* buffer = nullptr;
      uint32_t            offset = 0;       // bytes
    };
    upscale::Kind       kind = upscale::Kind::Off;
    D3D12BackendImage*  color = nullptr;
    D3D12BackendImage*  depth = nullptr;
    D3D12BackendImage*  velocity = nullptr;
    D3D12BackendImage*  output = nullptr;
    DXGI_FORMAT         depthFormat = DXGI_FORMAT_UNKNOWN;     // the game's view formats
    DXGI_FORMAT         velocityFormat = DXGI_FORMAT_UNKNOWN;
    Constant            viewSize;          // float2: render width, height (none: the color's size)
    Constant            jitter;            // float2: render pixels
    Constant            clipToPrevClip;    // four float4 rows
    float               jitterScale[2] = { 1.0f, 1.0f };
    float               sharpness = 0.0f;
    // 0: Unreal Engine 4's encoded velocity, camera motion from depth and
    // ClipToPrevClip where it is 0; 1: velocity read as candidate
    // velocityCandidate (velocityCandidateScale) is the motion everywhere.
    uint32_t            velocityMode = 0;
    uint32_t            velocityCandidate = 0;
    // Values instead of constant buffer reads (DXUP's own jitter and the
    // camera motion it worked out; velocityMode 2: camera motion only).
    bool                directJitter = false;
    float               jitterValue[2] = {};
    bool                directClip = false;
    float               clipRows[16] = {};
    // Scene color with values above 1, depth 1 at the near plane.
    bool                hdr = true;
    bool                depthInverted = true;
    // Only set up the upscaler (its creation takes a while), nothing drawn.
    bool                prepareOnly = false;
    // The frames before do not lead up to this one: no history.
    bool                reset = false;
  };

  // d3d11.upscalerAuto: which reading of a float velocity texture matches
  // the image. Errors of reprojecting the previous frame's color with each
  // candidate scale (UV, NDC and pixels, either sign; see
  // velocityCandidateScale) against no motion, summed over frames whose
  // sparse grid had enough moving pixels. Filled on the worker.
  // The camera candidates d3d11.upscalerInject measures in the same frames.
  constexpr uint32_t kCameraCandidates = 24;
  struct VelocityCalibration {
    std::mutex mutex;
    double     error[kCameraCandidates] = {};   // calibrateVelocity: the first 6
    // calibrateCamera: the candidate set (measurements made for an earlier
    // one are dropped).
    uint32_t   generation = 0;
    double     still = 0.0;
    uint32_t   frames = 0;
  };
  // Candidate k as a scale from velocity to render pixels (previous minus
  // current) for a view of width x height.
  inline void velocityCandidateScale(uint32_t k, float width, float height, float scale[2]) {
    const float sign = (k & 1) ? 1.0f : -1.0f;     // even: current minus previous
    switch (k >> 1) {
      case 0:  scale[0] = sign * width;        scale[1] = sign * height;        break;    // UV
      case 1:  scale[0] = sign * width * 0.5f; scale[1] = -sign * height * 0.5f; break;   // NDC, y up
      default: scale[0] = sign;                scale[1] = sign;                 break;    // pixels
    }
  }

  // The upscalers live in d3d11.dll only (their SDKs would bloat dxgi.dll,
  // which links the backend too): d3d11.dll hands its factory over here.
  using UpscalerFactory = std::unique_ptr<upscale::Upscaler> (*)(const upscale::CreateInfo&, std::string&);
  void setUpscalerFactory(UpscalerFactory factory);

  class D3D12BackendContext final : public IBackendContext {
  public:
    D3D12BackendContext(D3D12BackendDevice* device, bool copyOnly = false);
    D3D12BackendDevice* getBackendDevice() const { return m_device; }
    ~D3D12BackendContext();

    // IBackendContext - Resource Binding
    void bindVertexBuffer(uint32_t slot, IBackendBuffer* buffer, uint32_t offset, uint32_t stride) override;
    void bindIndexBuffer(IBackendBuffer* buffer, uint32_t offset, IndexType type) override;
    void bindConstantBuffer(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer) override;
    void bindConstantBufferRange(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer,
                                  uint32_t firstConstant, uint32_t numConstants) override;
    void bindShaderResource(ShaderStage stage, uint32_t slot, IBackendImageView* view) override;
    void bindShaderResource(ShaderStage stage, uint32_t slot, IBackendBufferView* view) override;
    void bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendImageView* view) override;
    void bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendBufferView* view) override;
    void bindSampler(ShaderStage stage, uint32_t slot, IBackendSampler* sampler) override;

    // IBackendContext - Render Targets
    void bindRenderTargets(uint32_t count, IBackendImageView** rtvs, IBackendImageView* dsv) override;
    bool outputMergerStateNeedsRebind() const override {
#ifdef _WIN32
      return m_outputMergerRebindRequired.load(std::memory_order_relaxed);
#else
      return false;
#endif
    }

    // IBackendContext - Stream Output
    void bindStreamOutputTargets(uint32_t count, IBackendBuffer** buffers, const uint32_t* offsets) override;
    uint64_t getStreamOutputFilledSize(IBackendBuffer* buffer) override;
    void drawAuto(IBackendBuffer* vertexBuffer, uint32_t stride) override;

    // IBackendContext - State
    void clearState() override;
    void releaseBindings() override;
    void setViewports(uint32_t count, const Viewport* viewports) override;
    void setScissorRects(uint32_t count, const Rect* rects) override;
    void setBlendState(const BlendState& state) override;
    void setRasterizerState(const RasterizerState& state) override;
    void setDepthStencilState(const DepthStencilState& state) override;
    void setStencilRef(uint32_t ref) override;
    void setBlendFactor(const float factor[4]) override;
    void setPrimitiveTopology(PrimitiveTopology topology) override;

    // IBackendContext - Pipeline
    void bindGraphicsPipeline(IBackendPipeline* pipeline) override;
    void bindComputePipeline(IBackendPipeline* pipeline) override;

    // IBackendContext - Draw Commands
    void draw(uint32_t vertexCount, uint32_t instanceCount,
              uint32_t firstVertex, uint32_t firstInstance) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override;

    void drawIndirect(IBackendBuffer* buffer, uint64_t offset) override;
    void drawIndexedIndirect(IBackendBuffer* buffer, uint64_t offset) override;
    void dispatchIndirect(IBackendBuffer* buffer, uint64_t offset) override;

    // IBackendContext - Copy Operations
    void copyBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                   IBackendBuffer* src, uint64_t srcOffset, uint64_t size) override;
    void copyWholeResource(IBackendImage* dst, IBackendImage* src);

    bool waitForGpuUse(BackendGpuUse& use, bool doNotWait) override;
    bool renameStagingImage(IBackendImage* image) override;
    // Thread-safe: the fence value the GPU reached and the last submitted.
    uint64_t completedFenceValue() const { return m_fence ? m_fence->GetCompletedValue() : UINT64_MAX; }
    uint64_t submittedFenceValue() const { return m_submittedFence->load(std::memory_order_acquire); }
    void copyImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                  IBackendImage* src, const ImageSubresource& srcSubres) override;
    void copyImageRegion(IBackendImage* dst, const ImageSubresource& dstSubres,
               uint32_t dstX, uint32_t dstY, uint32_t dstZ,
               IBackendImage* src, const ImageSubresource& srcSubres,
               const ImageCopyRegion* srcRegion) override;
    void copyBufferToImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                           IBackendBuffer* src, uint64_t srcOffset,
                           const ImageCopyRegion* region = nullptr) override;
    void copyImageToBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                           IBackendImage* src, const ImageSubresource& srcSubres) override;
    void copyStructureCount(IBackendBuffer* dst, uint64_t dstOffset, IBackendBufferView* view) override;

    // IBackendContext - UAV Counter
    void setUnorderedAccessCounter(IBackendBufferView* view, uint32_t value) override;

    // IBackendContext - Resource Mapping
    void* mapBuffer(IBackendBuffer* buffer, MapMode mode) override;
    void unmapBuffer(IBackendBuffer* buffer) override;
    void updateBuffer(IBackendBuffer* buffer, uint64_t offset, uint64_t size, const void* data) override;
    void updateBufferRegions(IBackendBuffer* buffer, const BufferUpdateRegion* regions, uint32_t regionCount) override;
    void retainTemporaryBuffer(Rc<IBackendBuffer> buffer) override;
    void retireAfterGpu(std::vector<std::shared_ptr<void>>& objects) override;
    void* discardBuffer(IBackendBuffer* buffer) override;
    bool uploadImageSubresource(IBackendImage* dst, const ImageSubresource& dstSubres,
                                const void* data, uint32_t rowPitch, uint32_t depthPitch) override;
    // Switches a buffer to a new slice in order with the recorded commands;
    // the previous slice returns once the GPU finished them.
    void setBufferSlice(D3D12BackendBuffer* buffer, const D3D12UploadSuballocator::Slice& slice,
                        D3D12UploadSuballocator* owner = nullptr);
    // A renamed constant buffer about to be written by the GPU moves back to
    // a default slice holding its contents.
    void prepareBufferGpuWrite(D3D12BackendBuffer* buffer) {
      if (buffer && buffer->usesUploadSlice())
        moveToDefaultSlice(buffer);
    }
    void moveToDefaultSlice(D3D12BackendBuffer* buffer);
    // Binds the constant buffer again wherever it is bound, after its memory
    // moved.
    void rebindConstantBuffer(D3D12BackendBuffer* buffer);

    // IBackendContext - Queries
    void beginQuery(IBackendQuery* query) override;
    void endQuery(IBackendQuery* query) override;
    void setPredication(IBackendQuery* predicate, bool value) override;
    void executeCommandList(const Rc<IBackendCommandList>& list) override;
    void suspendPredication(bool suspended) override;
    // DXUP's own copies and clears run whatever the application's predicate
    // says: pausePredication returns whether to resume it after them.
    bool pausePredication();
    void resumePredication(bool paused);
    void setImageMinLod(IBackendImage* image, float minLod) override;
    void retargetImage(IBackendImage* image, IUnknown* resource) override;
    void retargetBufferViews(IBackendBuffer* buffer, const std::vector<IBackendBufferView*>& views) override;
    void trackSwapChainBuffer(IUnknown* resource) override;
    void signalFence(IUnknown* fence, uint64_t value) override;
    void waitFence(IUnknown* fence, uint64_t value) override;
    void notifyOnCompletion(void* event) override;
    void updateTileMappings(const TileMappingUpdate& update) override;
    void copyTileMappings(TiledResourceRef dst, const TileCoordinate& dstCoordinate,
                          TiledResourceRef src, const TileCoordinate& srcCoordinate,
                          const TileRegionSize& size, uint32_t flags) override;
    void copyTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                   IBackendBuffer* buffer, uint64_t bufferOffset, uint32_t flags) override;
    void updateTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                     const void* data, uint32_t flags) override;
    bool resizeTilePool(IBackendBuffer* tilePool, uint64_t newSize) override;
    void tiledResourceBarrier() override;
    // Called when a query this context recorded is destroyed.
    void forgetQuery(D3D12BackendQuery* query);

    // IBackendContext - Synchronization
    void flush() override;
    void synchronize() override;
    void presentImage(IBackendImage* backBuffer) override;
    void waitForFrameLatency(uint32_t maxLatency) override;
    // The fence value that completes with the latest present (recording
    // thread), and a wait for a fence value any one other thread may call.
    uint64_t getLastPresentFenceValue() const;
    void waitForFenceValue(uint64_t value);

    // FPS overlay blit — copies RGBA pixel data onto a target image (must be in COPY_DEST)
    void blitPixels(IBackendImage* dst, uint32_t dstX, uint32_t dstY,
                    uint32_t width, uint32_t height, const uint32_t* rgbaPixels);

    // Deferred context support
    void closeForSubmission() override;
    void submitRecordedCommands() override;

    // IBackendContext - Clear
    void clearRenderTarget(IBackendImageView* rtv, const float color[4]) override;
    void clearDepthStencil(IBackendImageView* dsv, float depth, uint8_t stencil, uint32_t flags) override;
    void clearUnorderedAccessViewUint(IBackendImageView* view, const uint32_t values[4]) override;
    void clearUnorderedAccessViewUint(IBackendBufferView* view, const uint32_t values[4]) override;
    void clearUnorderedAccessViewFloat(IBackendImageView* view, const float values[4]) override;
    void clearUnorderedAccessViewFloat(IBackendBufferView* view, const float values[4]) override;
    void clearBufferView(IBackendBufferView* view, const float color[4], uint32_t rectCount, const Rect* rects) override;
    void clearView(IBackendImageView* view, bool renderTarget, const float color[4],
                   uint32_t rectCount, const Rect* rects) override;

    // IBackendContext - Misc
    void generateMips(IBackendImageView* srv) override;
#ifdef _WIN32
    // Runs the upscaler in place of the game's pass. False when it could
    // not (no upscaler for this kind): nothing is recorded then, and the
    // caller runs the game's own pass.
    bool runUpscaler(const UpscaleRequest& request);
    // Adds this frame's reprojection errors (read back a few frames later)
    // to the calibration; keeps the color for the next frame's.
    void calibrateVelocity(D3D12BackendImage* color, DXGI_FORMAT colorFormat, D3D12BackendImage* velocity,
                           DXGI_FORMAT velocityFormat, float viewWidth, float viewHeight,
                           const std::shared_ptr<VelocityCalibration>& calibration);
    // d3d11.upscalerInject (games without TAA): the reprojection errors of up
    // to kCameraCandidates camera motions (ClipToPrevClip rows each; error[k])
    // from the scene's depth, as calibrateVelocity's.
    void calibrateCamera(D3D12BackendImage* color, DXGI_FORMAT colorFormat, D3D12BackendImage* depth,
                         DXGI_FORMAT depthFormat, uint32_t count, const float (*clipRows)[16],
                         const std::shared_ptr<VelocityCalibration>& calibration, uint32_t generation);
    // The upscaler on a scene color the game drew without TAA: the color
    // copied in, the result (in a texture of DXUP's that allows UAVs) copied
    // back; request.color and output are filled in here.
    bool runInjectedUpscaler(D3D12BackendImage* sceneColor, UpscaleRequest request);
    // dxup.conf dxgi.upscale: src (single-sampled) scaled up to fill dst,
    // then sharpened (sharpness 0-1, 0 off); dst ends in COPY_DEST.
    bool scaleImage(IBackendImage* dst, IBackendImage* src, float sharpness);
#endif
    void upsampleImage(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) override;
    void resolveSampleZero(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) override;
    void resolveFinite(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format) override;
    void copyStencil(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) override;
    void resolveSubresource(IBackendImage* dst, uint32_t dstSubres,
                             IBackendImage* src, uint32_t srcSubres, DXGI_FORMAT format) override;

    // Set when a CS thread is the only thread recording into this context
    // and serializes every other access; recording then skips the mutex.
    // Retired backend objects then also outlive the GPU work (see
    // retireAfterGpu), so bound resources need no references of their own.
    // The immediate context then records on a thread of its own with
    // d3d12.recordThread.
    void setExternallySynchronized(bool enabled);

    void afterSubmitted(std::function<void()> fn) override;
    void waitForQueue() override;

  private:
    D3D12BackendDevice* m_device;

    // Thread-safety: protects all command list recording operations
    std::recursive_mutex m_contextMutex;
    bool m_externallySynchronized = false;

    // Rects of the ClearView call in progress; empty clears everything.
    std::vector<D3D12_RECT> m_clearRects;

    // Queries between Begin and End; split into segments when the command
    // list is submitted while they are active.
    std::vector<D3D12BackendQuery*> m_activeQueries;
    D3D12BackendQuery* m_predicate = nullptr;
    bool m_predicateValue = false;
    bool m_predicationApplied = false;
    bool m_predicationSuspended = false;
    // Highest fence value signaled for a submitted command list.
    std::shared_ptr<std::atomic<uint64_t>> m_submittedFence = std::make_shared<std::atomic<uint64_t>>(0);

    // Query results resolved together when the command list is submitted.
    std::vector<D3D12QueryResolve> m_pendingQueryResolves;
    void resolvePendingQueries();
    void suspendActiveQueries();
    void resumeActiveQueries();
    void applyPredication();
    void transitionPredicateBuffer(D3D12BackendQuery* query, D3D12_RESOURCE_STATES state);
    ID3D12Resource* transitionTiledResource(TiledResourceRef resource, D3D12_RESOURCE_STATES state);

    class ContextLock {
    public:
      explicit ContextLock(D3D12BackendContext& ctx)
      : m_mutex(ctx.m_externallySynchronized ? nullptr : &ctx.m_contextMutex) {
        if (m_mutex)
          m_mutex->lock();
      }
      ~ContextLock() {
        if (m_mutex)
          m_mutex->unlock();
      }
      ContextLock(const ContextLock&) = delete;
      ContextLock& operator=(const ContextLock&) = delete;

    private:
      std::recursive_mutex* m_mutex;
    };

    // Set once device is removed — all operations become no-ops
    // Read by mapBuffer on the application thread while the CS thread records.
    std::atomic<bool> m_deviceRemoved{false};

    // Copy-only contexts skip ring allocator (always sync after each flush)
    bool m_copyOnly = false;

    // Diagnostic markers are immutable for a context lifetime. Cache them once
    // so production draw calls do not repeatedly enter static-local helpers.
    bool m_perfStatsEnabled = false;
    bool m_perfTimingEnabled = false;
    bool m_infoDiagnosticsEnabled = false;
    bool m_recentOpsEnabled = false;
    bool m_barrierStatsEnabled = false;

#ifdef _WIN32
    // Command allocator ring for overlapping CPU recording and GPU execution.
    std::array<ComPtr<ID3D12CommandAllocator>, kCommandAllocatorRingSize> m_commandAllocators;
    std::array<uint64_t, kCommandAllocatorRingSize> m_commandAllocatorFenceValues = {};
    uint32_t m_commandAllocatorCount = 1;
    uint32_t m_currentAllocatorIndex = 0;
    ComPtr<ID3D12CommandAllocator> m_currentAllocator;

    // Every call on the command list goes through it: straight to D3D12,
    // or with d3d12.recordThread to a thread of its own, drained before
    // the list is closed and wherever a CPU descriptor it reads changes.
    CommandListRecorder m_cmdList;
    // With the submit thread, the lists it executes while the next records:
    // a list is reset once the job of its ticket ran (nextCommandList).
    static constexpr uint32_t kCommandListPoolSize = 3;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kCommandListPoolSize> m_commandLists;
    std::array<uint64_t, kCommandListPoolSize> m_commandListTickets = {};
    uint32_t m_commandListCount = 1;
    uint32_t m_currentCommandList = 0;
    bool     m_commandListPoolMade = false;
    void nextCommandList();
    // D3D11 orders unordered access between draws and dispatches: one sees
    // what the previous wrote. Each resource accessed through a UAV since its
    // last UAV barrier keeps how it was accessed (dxbc::UavAccess bits);
    // before an operation, a bound UAV whose access conflicts with that gets
    // a barrier. Reads after reads, and atomics of one kind that ignore
    // their results, need none.
    void orderUavAccess(bool compute);

    // Barrier hoisting. Batches that transition only textures the GPU work
    // since an earlier batch (the anchor) has not touched could have gone
    // with that batch; with legacy barriers each batch drains the GPU (Kena:
    // 111 of 245 batches a frame). One frame in 256 learns which ones, by
    // the anchor's contents (two frames in a row: only what both see); the
    // frames after add those transitions to the anchor, where the textures
    // still have the learned state, so the later batch has nothing left. The state tracker stays authoritative: a
    // texture used otherwise in between is transitioned back as usual.
    struct HoistEntry {
      std::weak_ptr<D3D12BackendImage> image;
      uint32_t subresource;
      D3D12_RESOURCE_STATES before, after;
    };
    bool accessTracking() const { return m_gpuProfiler.capturing() || m_hoistLearning; }
    void trackCommand(bool compute);
    void noteLearnedImage(D3D12BackendImage* image) {
      if (m_hoistLearning)
        m_learnImages[image->getResource()] = image->weak_from_this();
    }
    uint64_t barrierBatchSignature() const;
    void handleBarrierBatch();
    void hoistFrameEnd();
    // The barriers before a draw or dispatch of the application: the only
    // ones learned transitions are added to (DXUP's own passes rely on the
    // states they asked for).
    void flushCommandBarriers() {
      m_hoistHere = true;
      flushBarriers();
      m_hoistHere = false;
    }
    bool m_hoistHere = false;
    std::vector<ID3D12Resource*> m_hoistBound;
    // GPU profiler: why the batch before had no learned transitions to add.
    const char* m_hoistAnchorNote = nullptr;
    std::vector<ID3D12Resource*> m_hoistSkipped;
    BarrierAccessTracker m_accessTracker;
    // Changes with every shader resource binding (to skip unchanged ones).
    uint64_t m_srvBindingSerial = 1;
    const bool m_hoistEnabled = UserOverrides::get().barrierHoisting;
    bool m_hoistLearning = false;
    uint64_t m_hoistFrames = 0;
    uint64_t m_hoistAnchor = 0;
    std::unordered_map<uint64_t, std::vector<HoistEntry>> m_hoistPlan, m_hoistLearned, m_hoistFirstLearned;
    std::unordered_map<ID3D12Resource*, std::weak_ptr<D3D12BackendImage>> m_learnImages;
    // writer: the pipeline of the draw or dispatch (null for clears and
    // copies), for d3d11.relaxedUavBarriers.
    void orderUavAccess(ID3D12Resource* resource, uint16_t access, ID3D12PipelineState* writer = nullptr);
    std::vector<ID3D12Resource*> m_uavWritten;
    std::vector<uint16_t> m_uavWrittenAccess;
    std::vector<ID3D12PipelineState*> m_uavWrittenBy;
    const bool m_relaxedUavBarriers = UserOverrides::get().relaxedUavBarriers;

    // Copies, clears and render target binds transition images whose views
    // stay bound for shader access, which transitions only when a binding
    // changes. Before a draw or dispatch the bound views of its pipeline
    // return to the states they are accessed in.
    void prepareShaderAccess(bool compute);
    void restoreBoundImageStates(bool compute);
    void ensureImageViewState(D3D12BackendImageView* view, D3D12_RESOURCE_STATES needed);
    void markBoundImageStatesDirty() {
      m_graphicsImageStatesDirty = true;
      m_computeImageStatesDirty = true;
    }
    std::array<std::array<D3D12BackendImageView*, kMaxShaderResources>, kShaderStages> m_srvImageViews = {};
    std::array<std::array<D3D12BackendImageView*, kMaxUAVs>, kShaderStages> m_uavImageViews = {};
    bool m_graphicsImageStatesDirty = false;
    bool m_computeImageStatesDirty = false;

    // After a copy wrote to a buffer, puts it back into the states of its
    // current bindings; binding only transitions when the binding changes.
    void restoreBoundBufferState(D3D12BackendBuffer* buffer);
    // A buffer a copy reads or writes returns to the states of its bindings
    // when the copy is recorded: one bound for unordered access stays in
    // that state for the next dispatch otherwise.
    struct BoundBufferRestore {
      D3D12BackendContext* context;
      D3D12BackendBuffer*  buffer;
      bool                 restore;
      ~BoundBufferRestore() {
        if (restore)
          context->restoreBoundBufferState(buffer);
      }
    };
    BoundBufferRestore restoreAfterCopy(D3D12BackendBuffer* buffer, bool always) {
      const bool restore = buffer && !buffer->hasFixedState()
        && (always || (buffer->getCurrentState() & D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
      return { this, buffer, restore };
    }
    // One barrier for every subresource when they share a state; false when
    // they do not (or implicit promotion applies), for per-subresource ones.
    bool transitionWholeImage(D3D12BackendImage* image, D3D12_RESOURCE_STATES newState);

    // Records that the commands being recorded use a staging resource;
    // markGpuRead when they only read it.
    void markGpuUse(BackendGpuUse* use) {
      if (use && !m_copyOnly) {
        use->fenceValue.store(m_fenceValue + 1, std::memory_order_release);
        use->writeFenceValue.store(m_fenceValue + 1, std::memory_order_release);
        m_stagingWritePending = true;
      }
    }
    void markGpuRead(BackendGpuUse* use) {
      if (use && !m_copyOnly)
        use->fenceValue.store(m_fenceValue + 1, std::memory_order_release);
    }
    void copyImageRegionStaging(D3D12BackendImage* dst, const ImageSubresource& dstSubres,
                                uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                                D3D12BackendImage* src, const ImageSubresource& srcSubres,
                                const ImageCopyRegion* srcRegion);
    void copyFootprintRows(ID3D12Resource* dst, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& dstFootprint,
                           uint32_t dstRowsPerSlice, uint64_t dstX, uint32_t dstRow, uint32_t dstZ,
                           ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& srcFootprint,
                           uint32_t srcRowsPerSlice, uint64_t srcX, uint32_t srcRow, uint32_t srcZ,
                           uint64_t rowBytes, uint32_t rows, uint32_t slices);

    ComPtr<ID3D12Fence>               m_fence;
    uint64_t                          m_fenceValue = 0;
    uint64_t                          m_lastSignaledFenceValue = 0;
    HANDLE                            m_fenceEvent = nullptr;
    // Waits until the GPU reached the value; false when it ended because
    // the device is lost (a Signal the submit thread could not make never
    // brings its value).
    bool waitForFence(uint64_t value);

    // Fence value covering each recent frame's work, indexed by present
    // count, for the D3D11 maximum frame latency (at most 16 frames).
    // More than the 16 frames DXGI lets queue: frame n - 1 - 16 must still be there.
    static constexpr uint32_t         kPresentFenceHistory = 32;
    std::array<uint64_t, kPresentFenceHistory> m_presentFenceValues = {};
    uint64_t                          m_presentCount = 0;
    HANDLE                            m_frameLatencyEvent = nullptr;

    // Shader-visible descriptor heaps for binding
    ComPtr<ID3D12DescriptorHeap>     m_shaderVisibleCbvSrvUavHeap;
    ComPtr<ID3D12DescriptorHeap>     m_shaderVisibleSamplerHeap;
    uint32_t                         m_cbvSrvUavHeapSize = kShaderVisibleCbvSrvUavHeapSize;
    // Heaps replaced by a bigger one, until the GPU is done with them.
    std::vector<std::pair<ComPtr<ID3D12DescriptorHeap>, uint64_t>> m_retiredDescriptorHeaps;
    ComPtr<ID3D12DescriptorHeap> createShaderVisibleCbvSrvUavHeap(uint32_t size);
    // False when no bigger heap could be made: nothing changed then.
    bool growDescriptorHeap();
    uint32_t                          m_cbvSrvUavHeapOffset = 0;
    uint32_t                          m_cbvSrvUavHeapBaseOffset = 0;
    uint32_t                          m_cbvSrvUavDescSize   = 0;
    uint32_t                          m_samplerDescSize     = 0;

    // Pending resource barriers
    std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;
    struct CommonDecayEntry {
      D3D12BackendImage* image;
      uint32_t subresource;
    };
    std::vector<CommonDecayEntry> m_commonDecaySubresources;

    // State rollback: track all state transitions in the current command list
    // session so we can revert them if Close() fails
    struct StateRollbackEntry {
      D3D12BackendImage* image;
      uint32_t subresource; // UINT_MAX = whole-image via setCurrentState
      D3D12_RESOURCE_STATES oldState;
    };
    std::vector<StateRollbackEntry> m_stateRollback;

    struct BufferRollbackEntry {
      D3D12BackendBuffer* buffer;
      D3D12_RESOURCE_STATES oldState;
    };
    std::vector<BufferRollbackEntry> m_bufferStateRollback;

    // Barrier audit: all barriers submitted during current command list recording
    std::vector<D3D12_RESOURCE_BARRIER> m_emittedBarriers;
    // flushBarriers' batches when a resource has several transitions.
    std::vector<D3D12_RESOURCE_BARRIER> m_barrierBatch;
    std::vector<std::pair<ID3D12Resource*, UINT>> m_barrierBatchResources;
    void foldPendingBarriers();
    // The reflected limits of the bound shader of a stage; null without one.
    const ShaderResourceLimits* activeResourceLimits(uint32_t stageIdx) const {
      if (m_isComputePipeline)
        return &m_currentComputeResourceLimits;
      return stageIdx < m_currentGraphicsResourceLimits.size() ? &m_currentGraphicsResourceLimits[stageIdx] : nullptr;
    }
    // Commands being recorded write a staging resource (submitIfGpuIdle).
    bool m_stagingWritePending = false;
    // DRED (dxup_dred.txt): which shader a hung dispatch runs. Each dispatch
    // writes its shader to a CPU-visible ring before it starts and its
    // number once it ended (WriteBufferImmediate); read after a hang.
    bool m_dredMarkers = false;
    uint64_t m_computeShaderHash = 0;
    ComPtr<ID3D12Resource> m_breadcrumbBuffer;
    ComPtr<ID3D12GraphicsCommandList2> m_breadcrumbList;
    const uint32_t* m_breadcrumbs = nullptr;
    uint32_t m_dispatchSerial = 0;
    static constexpr uint32_t kBreadcrumbSlots = 16;
    void initDispatchBreadcrumbs();
    void beginDispatchBreadcrumb(uint32_t x = 0, uint32_t y = 0, uint32_t z = 0) {
      if (m_breadcrumbList) {
        writeDispatchBreadcrumb(true);
        noteDispatch(x, y, z);
      }
    }
    // What the dispatches of the ring use, for the hang report.
    struct DispatchRecord {
      uint32_t serial = 0;
      uint32_t groups[3] = {};
      uint64_t fence = 0;
      ID3D12Resource* uav = nullptr;
      D3D12_RESOURCE_DESC uavDesc = {};
      ID3D12Resource* srv = nullptr;
      D3D12_RESOURCE_DESC srvDesc = {};
    };
    std::array<DispatchRecord, 16> m_dispatchRecords = {};
    void noteDispatch(uint32_t x, uint32_t y, uint32_t z);
    void endDispatchBreadcrumb() {
      if (m_breadcrumbList)
        writeDispatchBreadcrumb(false);
    }
    void writeDispatchBreadcrumb(bool begin);
    void logDispatchBreadcrumbs();
    // Submits what is recorded when the GPU ran out of work, checked every
    // kEarlySubmitInterval draws once kEarlySubmitMinDraws are recorded.
    // Otherwise a frame reaches the GPU only when it is presented: a CPU-
    // bound game leaves the GPU idle meanwhile, and a Map that waits for
    // work of the frame (staging uploads read back the next frame) waits
    // for the whole frame. A busy GPU gets no extra submissions.
    //
    // A copy to a staging resource (a readback) is submitted with the next
    // draw: Map then waits until the GPU did the copy, as with D3D11
    // drivers, not until it finished the whole frame.
    void submitIfGpuIdle() {
      if (m_stagingWritePending) {
        flush();
        return;
      }
      if ((m_drawOpsSinceFlush & (kEarlySubmitInterval - 1)) || m_drawOpsSinceFlush < kEarlySubmitMinDraws || m_copyOnly)
        return;
      if (m_fence && m_fence->GetCompletedValue() >= m_lastSignaledFenceValue)
        flush();
    }
    // The root bindings of one bind point were reset.
    void invalidateRootCache(bool compute);
    // AMD: the root signature is set again on every graphics/compute switch.
    bool m_resetRootOnModeSwitch = false;

    // Staged WriteDiscard: maps to a staging buffer, copies on unmap.
    // Prevents CPU writing to a buffer the GPU might still be reading.
    struct StagedMap {
      IBackendBuffer* target = nullptr;    // original buffer being mapped
      Rc<IBackendBuffer> staging;          // temp upload buffer
      void* mappedPtr = nullptr;           // mapped pointer into staging
      uint64_t size = 0;                   // size of the staging buffer
      bool requiresUnmap = false;
    };
    // One per mapped buffer: without a CS thread several threads may each
    // have a buffer mapped at once.
    std::vector<StagedMap> m_stagedMaps;

    // Keep temporary upload resources alive until GPU finishes
    std::vector<Rc<IBackendBuffer>> m_pendingUploads;  // current CL's uploads
    uint64_t m_pendingUploadBytes = 0;
    struct FencedUpload {
      uint64_t fenceValue;
      Rc<IBackendBuffer> buffer;
    };
    std::vector<FencedUpload> m_inFlightUploads;  // tagged with fence value

    struct FencedRetired {
      uint64_t fenceValue;
      std::vector<std::shared_ptr<void>> objects;
    };
    std::vector<std::shared_ptr<void>> m_pendingRetired;
    std::vector<FencedRetired> m_inFlightRetired;
    // A resource of the context's own that it replaces: the old one goes
    // once the GPU finished the commands recorded so far, which may still
    // use it (holdBatchResource keeps nothing behind a CS thread).
    void retireResource(ComPtr<ID3D12Resource>& resource);

    struct FencedSlices {
      uint64_t fenceValue;
      std::vector<D3D12UploadSuballocator::Slice> slices;
    };
    std::vector<D3D12UploadSuballocator::Slice> m_pendingSlices;
    std::vector<FencedSlices> m_inFlightSlices;


    // Deferred resource destruction: hold AddRef'd ID3D12Resource* for each
    // command list batch so resources aren't freed while the GPU is still
    // reading them.  Resources are AddRef'd at bind time and at flush time
    // (snapshot of all currently bound resources), tagged with the fence
    // value, then Released when the fence completes.
    std::vector<ID3D12Resource*> m_batchResourceRefs;  // current batch
    static constexpr uint32_t kBatchResourceRefTableSize = 16384;
    std::array<ID3D12Resource*, kBatchResourceRefTableSize> m_batchResourceRefTable = {};
    std::vector<uint32_t> m_batchResourceRefOccupiedSlots;
    uint32_t m_batchResourceRefTableOccupancy = 0;
    uint64_t m_resourceBatchGeneration = 1;
    uint64_t m_boundResourceSnapshotGeneration = 0;
    struct FencedResourceBatch {
      uint64_t fenceValue;
      std::vector<ID3D12Resource*> resources;  // pre-AddRef'd
    };
    std::vector<FencedResourceBatch> m_inFlightResourceBatches;

    // Auto-flush: count copy ops and flush periodically to avoid GPU TDR
    uint32_t m_copyOpsSinceFlush = 0;

    // Auto-flush: count draw/dispatch ops per command list to prevent large batches
    uint32_t m_drawOpsSinceFlush = 0;
    // Occlusion queries ended since the last submission, and the first
    // render target (or depth view) of the pass recorded now.
    uint32_t m_occlusionEndsSinceFlush = 0;
    const void* m_passTargetKey = nullptr;

    // Current pipeline state
    ID3D12PipelineState*   m_currentPSO           = nullptr;
    ID3D12RootSignature*   m_currentRootSignature  = nullptr;
    ID3D12RootSignature*   m_computeRootSignature  = nullptr;
    bool                   m_isComputePipeline     = false;
    // The bound graphics pipeline failed to compile: draws are skipped.
    bool                   m_graphicsPipelineFailed = false;

    // Tracked command list state for re-binding after resetCommandList()
    struct TrackedVB {
      D3D12_VERTEX_BUFFER_VIEW view = {};
      bool                     bound = false;
      ID3D12Resource*          resourceHeld = nullptr;
      uint64_t                 resourceHeldGeneration = 0;
    };
    std::array<TrackedVB, kMaxVertexBuffers> m_trackedVBs = {};
    // Kept unbound for the current pipeline's inputs missing from its
    // input layout (UINT32_MAX: none).
    uint32_t m_nullVertexSlot = UINT32_MAX;
    // Every tracked vertex buffer was bound again (command list reset,
    // foreign commands), the null slot's too: the next graphics pipeline
    // unbinds it. Apart from m_psoDirtyAfterFlush, which dispatches clear.
    bool m_vertexBuffersRebound = false;
    void applyNullVertexSlot(uint32_t slot);
    D3D12_INDEX_BUFFER_VIEW  m_trackedIB = {};
    bool                     m_trackedIBBound = false;
    ID3D12Resource*          m_trackedIBResourceHeld = nullptr;
    uint64_t                 m_trackedIBResourceHeldGeneration = 0;
    uint32_t                 m_trackedViewportCount = 0;
    D3D12_VIEWPORT           m_trackedViewports[kMaxViewports] = {};
    uint32_t                 m_trackedScissorCount = 0;
    D3D12_RECT               m_trackedScissors[kMaxViewports] = {};
    float                    m_trackedBlendFactor[4] = { 1.f, 1.f, 1.f, 1.f };
    uint32_t                 m_trackedStencilRef = 0;
    D3D_PRIMITIVE_TOPOLOGY   m_trackedTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    // Tracked render targets for re-binding after command list reset
    D3D12_CPU_DESCRIPTOR_HANDLE m_trackedRTVs[kMaxRenderTargets] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_trackedDSV = {};
    uint32_t                 m_trackedRTVCount = 0;
    bool                     m_trackedDSVBound = false;
    bool                     m_trackedOmBindingValid = true;
    std::atomic<bool>        m_outputMergerRebindRequired = true;
    ID3D12Resource*          m_trackedRTVResourceHeld[kMaxRenderTargets] = {};
    uint64_t                 m_trackedRTVResourceHeldGeneration[kMaxRenderTargets] = {};
    ID3D12Resource*          m_trackedDSVResourceHeld = nullptr;
    uint64_t                 m_trackedDSVResourceHeldGeneration = 0;

    // Per-stage CBV binding state (for descriptor table approach)
    struct CbvSlot {
      D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
      UINT sizeInBytes = 0;
      bool bound = false;
      ID3D12Resource* resourceHeld = nullptr;
      uint64_t resourceHeldGeneration = 0;
      // What was bound, so that a renamed buffer is bound again at its new
      // memory; no range is the whole buffer.
      D3D12BackendBuffer* buffer = nullptr;
      uint32_t firstConstant = 0;
      uint32_t numConstants = 0;
    };
    struct DescriptorSlot {
      D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
      D3D12_CPU_DESCRIPTOR_HANDLE nullHandle = {};
      uint64_t descriptorVersion = 0;
      bool bound = false;
      ID3D12Resource* resourceHeld = nullptr;
      uint64_t resourceHeldGeneration = 0;
      // The counter of the bound UAV (null without one).
      ID3D12Resource* counterResourceHeld = nullptr;
      uint64_t counterResourceHeldGeneration = 0;
    };
    std::array<std::array<CbvSlot, kMaxConstantBuffers>, kShaderStages> m_cbvBindings = {};
    // Upper bounds of the bound constant buffer slots, so scans skip the
    // empty tails. Binding raises them, clearState resets them.
    std::array<uint32_t, kShaderStages> m_cbvBindingEnd = {};
    std::array<std::array<DescriptorSlot, kMaxShaderResources>, kShaderStages> m_srvBindings = {};
    // Upper bound of the SRV slots bound since the last clear, per stage.
    std::array<uint32_t, kShaderStages> m_srvBindingEnd = {};
    std::array<uint32_t, kShaderStages> m_uavBindingEnd = {};
    std::array<std::array<DescriptorSlot, kMaxUAVs>, kShaderStages> m_uavBindings = {};
    std::array<std::array<DescriptorSlot, kMaxSamplers>, kShaderStages> m_samplerBindings = {};
    struct CbvTableCacheEntry {
      uint32_t descriptorCount = 0;
      uint64_t fingerprint = 0;
      std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxConstantBuffers> gpuAddresses = {};
      std::array<UINT, kMaxConstantBuffers> sizes = {};
      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
    };
    // SRV and UAV tables keep their source descriptors packed in a pool
    // per stage: entries stay small however many slots a table has.
    struct TableSource {
      SIZE_T   handle;
      uint64_t version;
    };
    struct SrvTableCacheEntry {
      uint32_t descriptorCount = 0;
      uint32_t first = 0;
      uint64_t fingerprint = 0;
      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
    };
    using UavTableCacheEntry = SrvTableCacheEntry;
    // Sampler tables stay in the shader-visible sampler heap while they are
    // reused: D3D12 allows 2048 samplers there, applications use few
    // distinct tables, and a ring rewrote them every frame and waited for
    // the GPU each time it came around. A table's place is reused once the
    // GPU finished the last submission that used it, least recently used
    // first. Offset 0 holds the null sampler.
    struct SamplerTable {
      uint32_t offset = 0;
      uint32_t count = 0;
      uint64_t lastUse = 0;
      uint64_t fingerprint = 0;
      std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxSamplers> handles = {};
      std::array<uint64_t, kMaxSamplers> versions = {};
    };
    std::vector<SamplerTable>            m_samplerTables;
    std::vector<uint32_t>                m_freeSamplerTableIds;
    std::unordered_map<uint64_t, uint32_t> m_samplerTableIds;
    std::map<uint32_t, uint32_t>         m_freeSamplerRanges;
    uint32_t                             m_freeSamplerCount = 0;
    // Samplers of the tables the open command list uses.
    uint32_t                             m_samplerDescriptorsInBatch = 0;
    void initSamplerTables();
    uint32_t acquireSamplerTable(uint64_t fingerprint, uint32_t count,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE* handles, const uint64_t* versions);
    bool allocateSamplerRange(uint32_t count, uint32_t& offset);
    void freeSamplerTable(uint32_t id);
    bool evictSamplerTables(uint32_t count);
    struct CbvDescriptorKey {
      D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
      UINT sizeInBytes = 0;

      bool operator==(const CbvDescriptorKey& other) const {
        return gpuAddress == other.gpuAddress && sizeInBytes == other.sizeInBytes;
      }
    };
    struct CbvDescriptorKeyHash {
      size_t operator()(const CbvDescriptorKey& key) const {
#if defined(_M_IX86)
        uint32_t mixed = uint32_t(key.gpuAddress) ^ uint32_t(key.gpuAddress >> 32) * 0x9e3779b9u ^ key.sizeInBytes * 0x85ebca6bu;
        mixed ^= mixed >> 16;
        mixed *= 0xc2b2ae35u;
        mixed ^= mixed >> 13;
        return mixed;
#else
        uint64_t mixed = static_cast<uint64_t>(key.gpuAddress) ^ (static_cast<uint64_t>(key.sizeInBytes) << 32);
        mixed ^= mixed >> 33;
        mixed *= 0xff51afd7ed558ccdULL;
        mixed ^= mixed >> 33;
        return static_cast<size_t>(mixed);
#endif
      }
    };
    struct DescriptorFingerprintHash {
      size_t operator()(uint64_t fingerprint) const noexcept {
        // Descriptor-table fingerprints are already well distributed. MSVC's
        // default integer hash runs another byte-wise FNV pass, which is pure
        // overhead on every cache lookup.
        return static_cast<size_t>(fingerprint);
      }
    };
    std::array<std::vector<CbvTableCacheEntry>, kShaderStages> m_cbvTableCache;
    std::array<std::vector<SrvTableCacheEntry>, kShaderStages> m_srvTableCache;
    std::array<std::vector<UavTableCacheEntry>, kShaderStages> m_uavTableCache;
    std::array<std::vector<TableSource>, kShaderStages> m_srvTableSources;
    std::array<std::vector<TableSource>, kShaderStages> m_uavTableSources;
    // Fingerprint-to-entry indices keep descriptor-table lookup O(1) while
    // retaining the full equality check for collision safety.
    // The caches are rebuilt for every descriptor heap partition, i.e. every
    // frame, so a node-based map allocated once per cached table. This
    // direct-mapped index never allocates and clears in O(1) by bumping a
    // generation. A collision only evicts an index slot; callers still
    // compare the full entry before reusing a table.
    class DescriptorTableIndex {
    public:
      struct Slot {
        uint64_t first = 0;
        size_t   second = 0;
      };

      const Slot* find(uint64_t fingerprint) const {
        const size_t index = slotIndex(fingerprint);
        return m_generations[index] == m_generation && m_slots[index].first == fingerprint
          ? &m_slots[index]
          : nullptr;
      }

      const Slot* end() const { return nullptr; }

      size_t& operator[](uint64_t fingerprint) {
        const size_t index = slotIndex(fingerprint);
        m_generations[index] = m_generation;
        m_slots[index].first = fingerprint;
        return m_slots[index].second;
      }

      void clear() {
        if (++m_generation == 0) {
          m_generations.fill(0);
          m_generation = 1;
        }
      }

      void reserve(size_t) { }

    private:
      static constexpr size_t kSlots = 1024;

      static size_t slotIndex(uint64_t fingerprint) {
        return static_cast<size_t>(fingerprint ^ (fingerprint >> 32)) & (kSlots - 1);
      }

      std::array<Slot, kSlots>     m_slots = {};
      std::array<uint32_t, kSlots> m_generations = {};
      uint32_t                     m_generation = 1;
    };
    std::array<DescriptorTableIndex, kShaderStages> m_cbvTableCacheLookup;
    std::array<DescriptorTableIndex, kShaderStages> m_srvTableCacheLookup;
    std::array<DescriptorTableIndex, kShaderStages> m_uavTableCacheLookup;
    std::unordered_map<CbvDescriptorKey, D3D12_CPU_DESCRIPTOR_HANDLE, CbvDescriptorKeyHash> m_cbvDescriptorCache;
    std::array<std::array<ID3D12Resource*, kMaxShaderResources>, kShaderStages> m_currentSrvResources = {};
    std::array<std::array<DXGI_FORMAT, kMaxShaderResources>, kShaderStages> m_currentSrvFormats = {};
    std::array<std::array<ID3D12Resource*, kMaxUAVs>, kShaderStages> m_currentUavResources = {};
    std::array<std::array<DXGI_FORMAT, kMaxUAVs>, kShaderStages> m_currentUavFormats = {};
    std::array<ID3D12Resource*, kMaxRenderTargets> m_currentRenderTargetResources = {};
    uint32_t m_currentRenderTargetCount = 0;
    ID3D12Resource* m_currentDepthStencilResource = nullptr;
    bool m_cbvDirty = false;
    bool m_srvDirty = false;
    bool m_uavDirty = false;
    bool m_samplerDirty = false;
    std::array<bool, kShaderStages> m_cbvStageDirty = {};
    std::array<bool, kShaderStages> m_srvStageDirty = {};
    std::array<bool, kShaderStages> m_uavStageDirty = {};
    std::array<bool, kShaderStages> m_samplerStageDirty = {};
    uint32_t m_descriptorDirtyStageMask = 0;
    bool m_psoDirtyAfterFlush = false;
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 20> m_currentGraphicsRootTables = {};
    // Addresses in the root CBVs, 0 while unknown (after a root signature set).
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kGraphicsRootCbvCount> m_currentGraphicsRootCbvs = {};
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 8> m_currentComputeRootCbvs = {};
    std::array<bool, 20> m_currentGraphicsRootTableValid = {};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> m_currentComputeRootTables = {};
    std::array<bool, 4> m_currentComputeRootTableValid = {};
    PipelineResourceLimits m_currentGraphicsResourceLimits = {};
    ShaderResourceLimits m_currentComputeResourceLimits = {};
    uint32_t m_currentGraphicsStageMask = 0x1Fu;

    void commitCbvBindings();
    void commitSrvBindings();
    void commitUavBindings();
    void commitSamplerBindings();
    void commitDescriptorBindings();
    void invalidateRootTableCache();
    // After a pass of DXUP's own on the graphics pipeline: the application's
    // root signature, viewports, scissors and topology again, its pipeline
    // at the next draw or dispatch.
    void restoreAfterInternalPass();
    bool graphicsRootTableNeedsSet(uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) const;
    bool computeRootTableNeedsSet(uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) const;
    void noteGraphicsRootTable(uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    void noteComputeRootTable(uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    D3D12_CPU_DESCRIPTOR_HANDLE getOrCreateCachedCbvDescriptor(D3D12_GPU_VIRTUAL_ADDRESS gpuAddress, UINT sizeInBytes);
    uint32_t clampDescriptorCountToShader(uint32_t stageIdx, uint32_t descriptorCount, uint32_t ShaderResourceLimits::*member) const;

    // Query heaps
    ComPtr<ID3D12QueryHeap> m_occlusionQueryHeap;
    ComPtr<ID3D12QueryHeap> m_timestampQueryHeap;
    ComPtr<ID3D12QueryHeap> m_pipelineStatsQueryHeap;
    ComPtr<ID3D12QueryHeap> m_streamOutputQueryHeap;

    void transitionResource(D3D12BackendBuffer* buffer, D3D12_RESOURCE_STATES newState);
    void transitionResource(D3D12BackendImage* image, D3D12_RESOURCE_STATES newState);
    // Maps a lazily committed render target's tiles and clears it to zero.
    void commitLazyImage(D3D12BackendImage* image);
    void transitionResource(D3D12BackendImage* image, const ImageSubresource& subresource, D3D12_RESOURCE_STATES newState);
    void transitionResource(D3D12BackendImage* image, const D3D12BackendImageView* view, D3D12_RESOURCE_STATES newState);
    void transitionSubresource(D3D12BackendImage* image, uint32_t subresource, D3D12_RESOURCE_STATES newState);

    // Resources shared with other devices that this batch uses. Shared
    // images leave the batch in COMMON, the state other devices find them
    // in; the batch waits for the last submission of another device that
    // used one of them.
    void noteSharedUse(D3D12BackendImage* image) {
      if (image->getSharedSync()
       && std::find(m_sharedImagesInBatch.begin(), m_sharedImagesInBatch.end(), image) == m_sharedImagesInBatch.end())
        m_sharedImagesInBatch.push_back(image);
    }
    void noteSharedUse(D3D12BackendBuffer* buffer) {
      if (buffer->getSharedSync()
       && std::find(m_sharedBuffersInBatch.begin(), m_sharedBuffersInBatch.end(), buffer) == m_sharedBuffersInBatch.end())
        m_sharedBuffersInBatch.push_back(buffer);
    }
    void prepareSharedSubmit();
    void publishSharedSubmit();
    std::vector<D3D12BackendImage*>  m_sharedImagesInBatch;
    std::vector<D3D12BackendBuffer*> m_sharedBuffersInBatch;
    void markOutputMergerRebindRequired(ID3D12Resource* resource);
    void markImageForCommonDecay(D3D12BackendImage* image, uint32_t subresource, D3D12_RESOURCE_STATES promotedState);
    void decayPromotedImageStates();
    void releaseCompletedUploads();
    void flushBarriers();
    void resetCommandList();
    void initNullDescriptorsAndRootTables();
    void resetBindingTracking();
    void recordRecentOp(const char* fmt, ...);
    // Records only when recent ops are on, without evaluating the arguments
    // otherwise (bind paths).
#define DXUP_RECENT_OP(...) do { if (m_recentOpsEnabled) recordRecentOp(__VA_ARGS__); } while (0)
    // Each plane of a depth-stencil view into its own state.
    void transitionDepthStencilPlanes(D3D12BackendImage* image, const D3D12BackendImageView* view,
                                      D3D12_RESOURCE_STATES depthState, D3D12_RESOURCE_STATES stencilState);
    void markDescriptorStageDirty(uint32_t stage);
    void markAllDescriptorStagesDirty();
    // A UAV binding of the stage changed (the pixel stage's: every graphics
    // stage's tables, see commitUavBindings).
    void markUavBindingDirty(uint32_t stage);
    void perfRecordFlush(uint32_t drawOps, uint32_t copyOps, uint32_t cbvSrvUavOff, uint32_t samplerOff,
                         double execMs, double totalMs);
    void perfRecordResetWait(double waitMs);
    void perfRecordPresent(double presentWaitMs);
    void holdBatchResource(ID3D12Resource* res);
    void holdBatchResourceOnce(ID3D12Resource* res, ID3D12Resource*& heldResource, uint64_t& heldGeneration);
    void clearBatchResourceRefTable();
    void snapshotBoundResources();
    void releaseCompletedResourceBatches(uint64_t completedFenceValue);
    void releaseAllResourceBatches();
    bool ensureUploadScratchCapacity(uint64_t capacity);
    void warnLowAddressSpace();
    bool allocateUploadScratch(uint64_t size, uint64_t alignment,
                               ID3D12Resource** resource, uint64_t* offset, void** ptr);

    D3D12_CPU_DESCRIPTOR_HANDLE allocateShaderVisibleCbvSrvUav();
    void ensureDescriptorHeapSpace();
    uint32_t cbvSrvUavPartitionSize() const;
    uint32_t cbvSrvUavPartitionEnd() const;
    void resetDescriptorHeapPartition();
    uint32_t descriptorPartitionCount() const;
    // Moves descriptor allocation to the next partition once the GPU is done
    // with what that partition held.
    void switchDescriptorPartition();
    uint32_t m_descriptorPartition = 0;
    // Shader-visible descriptors taken in all, and the most in one frame
    // (dxup_memory_stats.txt).
    uint64_t m_descriptorsTaken = 0, m_descriptorsAtPresent = 0, m_descriptorsFrameMax = 0;
    // Fence value of the last submission using each partition, and the
    // partitions the open command list uses.
    std::array<uint64_t, kDescriptorPartitionCount> m_descriptorPartitionFences = {};
    uint32_t m_partitionsInBatch = 1;

    // Mipmap generation resources
    bool m_mipGenInitialized = false;
    ComPtr<ID3D12RootSignature> m_mipGenRootSignature;
    // Upscaling (runUpscaler): the upscaler, the motion vector pass and the
    // motion and depth textures it writes at render resolution.
    std::unique_ptr<upscale::Upscaler> m_upscaler;
    // Created on a thread of its own (FSR and XeSS compile their pipelines
    // for hundreds of milliseconds): the game's pass runs until it is there.
    // Not std::async: its threads take the game's default stack size.
    struct PendingUpscaler {
      std::unique_ptr<upscale::Upscaler> upscaler;
      std::string error;
    };
    std::future<PendingUpscaler> m_upscalerPending;
    Thread                      m_upscalerThread;
    upscale::Kind               m_upscalerKind = upscale::Kind::Off;
    uint32_t                    m_upscalerSize[4] = {};   // render and output width, height
    bool                        m_upscalerFailed = false;
    bool                        m_upscalerReset = true;
    int64_t                     m_upscalerLastTime = 0;
    ComPtr<ID3D12RootSignature> m_upscaleMotionRootSignature;
    ComPtr<ID3D12PipelineState> m_upscaleMotionPso;
    ComPtr<ID3D12Resource>      m_upscaleMotion;
    ComPtr<ID3D12Resource>      m_upscaleDepth;
    uint32_t                    m_upscaleTextureSize[2] = {};
    bool createUpscaleMotionPass();
    // calibrateVelocity: the previous frame's color, a ring of result slots
    // (cleared from zeros, copied to readback memory) and their fences.
    ComPtr<ID3D12RootSignature> m_velocityCalRootSignature;
    ComPtr<ID3D12PipelineState> m_velocityCalPso;
    ComPtr<ID3D12Resource>      m_velocityCalPrevious;
    ComPtr<ID3D12Resource>      m_velocityCalResults;
    ComPtr<ID3D12Resource>      m_velocityCalZeros;
    ComPtr<ID3D12Resource>      m_velocityCalReadback;
    uint64_t                    m_velocityCalFence[16] = {};
    uint32_t                    m_velocityCalSlot = 0;
    uint32_t                    m_cameraCalGeneration[16] = {};
    // created: the previous color is new (nothing to compare with yet).
    bool createVelocityCalibration(D3D12BackendImage* color, bool& created);
    ComPtr<ID3D12RootSignature> m_cameraCalRootSignature;
    ComPtr<ID3D12PipelineState> m_cameraCalPso;
    ComPtr<ID3D12Resource>      m_cameraCalRows;       // per result slot: the candidates' rows
    float*                      m_cameraCalRowsData = nullptr;
    Rc<IBackendImage>           m_injectInput;
    // A depth texture shaders cannot read (typed, as forward renderers make
    // them): a copy of it in its typeless format that they can.
    Rc<IBackendImage>           m_injectDepth;
    D3D12BackendImage* shaderReadableDepth(D3D12BackendImage* depth);
    Rc<IBackendImage>           m_injectOutput;
    uint32_t                    m_upscalerSettings = 0;
    bool ensureUpscaleTextures(uint32_t width, uint32_t height);
    // Everything a foreign library may have changed on the command list.
    void restoreAfterForeignCommands();
    // scaleImage: pass 0 scales, pass 1 sharpens (d3d12_backend_scale.cpp).
    ComPtr<ID3DBlob> m_scalePassPs[2];
    std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> m_scalePsoCache;
    Rc<IBackendImage> m_scaleIntermediate;
    ID3D12PipelineState* getOrCreateScalePso(uint32_t pass, DXGI_FORMAT format);
    void drawScalePass(ID3D12PipelineState* pso, D3D12BackendImage* dst, DXGI_FORMAT dstFormat,
                       D3D12BackendImage* src, DXGI_FORMAT srcFormat, uint32_t constant);
    ComPtr<ID3DBlob> m_mipGenVsBytecode;
    // Downsampling shaders for 1D, 2D and 3D images (all as arrays but 3D).
    std::array<ComPtr<ID3DBlob>, 3> m_mipGenPsBytecode;
    std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> m_mipGenPsoCache;
    // Reused for every mip: render target views are read when bound.
    D3D12_CPU_DESCRIPTOR_HANDLE m_mipGenRtv = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_lazyClearRtv = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullRtv = {};   // holes between render targets
    void initMipGenResources();

    // Forced multisampling passes: a fullscreen triangle reads one image and
    // writes another, on the mip generation root signature.
    enum class MsaaPass : uint32_t { UpsampleColor, UpsampleDepth, ResolveDepth, ResolveColor, ResolveFinite };
    ComPtr<ID3DBlob> m_msaaPassPs[5];
    // ResolveFinite per source sample count (1 << index): with the count
    // known the shader issues all its loads at once, as fast as the hardware
    // resolve; a loop over the count taken from the texture is not.
    ComPtr<ID3DBlob> m_msaaFinitePs[5];
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> m_msaaPassPsoCache;
    D3D12_CPU_DESCRIPTOR_HANDLE m_msaaPassDsv = {};
    ID3D12PipelineState* getOrCreateMsaaPassPso(MsaaPass pass, DXGI_FORMAT format, uint32_t samples,
                                                uint32_t srcSamples);
    void runMsaaPass(MsaaPass pass, D3D12BackendImage* dst, D3D12BackendImage* src, DXGI_FORMAT format,
                     DXGI_FORMAT readFormat);
    // copyStencil: without SV_StencilRef (NVIDIA) one draw per stencil bit
    // sets it where the source has it, after a clear to 0. Pixel shaders for
    // a single-sampled and a multisampled source.
    ComPtr<ID3DBlob> m_stencilPassPs[2];
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> m_stencilPassPsoCache;
    ID3D12PipelineState* getOrCreateStencilPassPso(DXGI_FORMAT format, uint32_t samples, bool msSource, uint32_t bit);
    // Stencil resolves into a single-sampled texture: a compute pass writes
    // sample 0's stencil into a buffer, which is copied into the stencil
    // plane - one pass instead of a clear and eight draws (Kena 4K: 87 ->
    // 38 us). False when the plane's layout does not allow it. (Depth the
    // same way is slower than the depth-export pass: 66 vs 48 us.)
    bool resolveStencilPlane(D3D12BackendImage* dst, D3D12BackendImage* src, DXGI_FORMAT readFormat);
    ComPtr<ID3D12RootSignature> m_stencilResolveRootSignature;
    ComPtr<ID3D12PipelineState> m_stencilResolvePso;
    ComPtr<ID3D12Resource> m_stencilResolveBuffer;
    uint64_t m_stencilResolveBufferSize = 0;
    bool m_stencilResolveFailed = false;
    ID3D12PipelineState* getOrCreateMipGenPso(DXGI_FORMAT format, uint32_t dimension);

    std::array<std::array<char, 256>, 512> m_recentOps = {};
    uint32_t m_recentOpWriteIndex = 0;
    uint32_t m_recentOpCount = 0;

    // Marker-gated performance counters, emitted only when dxup_perf_stats.txt
    // exists beside the DLL.
    uint64_t m_perfIntervalPresents = 0;
    uint64_t m_perfIntervalFlushes = 0;
    uint64_t m_perfIntervalWorkFlushes = 0;
    uint64_t m_perfIntervalDrawOps = 0;
    uint64_t m_perfIntervalCopyOps = 0;
    uint64_t m_perfIntervalUpdateBufferCalls = 0;
    uint64_t m_perfIntervalUpdateBytes = 0;
    uint64_t m_perfIntervalUpdateConstantBufferCalls = 0;
    uint64_t m_perfIntervalUpdateIaBufferCalls = 0;
    uint64_t m_perfIntervalUpdateSrvUavBufferCalls = 0;
    uint64_t m_perfIntervalUpdateSrvOnlyBufferCalls = 0;
    uint64_t m_perfIntervalUpdateUavCapableBufferCalls = 0;
    uint64_t m_perfIntervalUpdateOtherBufferCalls = 0;
    uint64_t m_perfIntervalDirectBufferUpdateCalls = 0;
    uint64_t m_perfIntervalImmediateBufferUpdateCalls = 0;
    uint64_t m_perfIntervalCopiedBufferUpdateCalls = 0;
    uint64_t m_perfIntervalMapReadCalls = 0;
    uint64_t m_perfIntervalMapWriteCalls = 0;
    uint64_t m_perfIntervalStagedMaps = 0;
    uint64_t m_perfIntervalCopyBufferCalls = 0;
    uint64_t m_perfIntervalCopyImageCalls = 0;
    uint64_t m_perfIntervalCopyImageRegionCalls = 0;
    uint64_t m_perfIntervalCopyBufferToImageCalls = 0;
    uint64_t m_perfIntervalCopyImageToBufferCalls = 0;
    uint64_t m_perfIntervalStreamOutputReadbacks = 0;
    uint64_t m_perfIntervalDescriptorFlushes = 0;
    uint64_t m_perfIntervalBatchResourceRefs = 0;
    uint64_t m_perfIntervalPsoBinds = 0;
    uint64_t m_perfIntervalPsoSkips = 0;
    uint64_t m_perfIntervalRootTableSets = 0;
    uint64_t m_perfIntervalDescriptorCopies = 0;
    uint64_t m_perfIntervalDescriptorCacheHits = 0;
    uint64_t m_perfIntervalCbvCreates = 0;
    uint64_t m_perfIntervalBarrierCalls = 0;
    uint64_t m_perfIntervalBarrierTransitions = 0;
    uint64_t m_perfIntervalBarrierBufferTransitions = 0;
    uint64_t m_perfIntervalBarrierImageTransitions = 0;
    uint64_t m_perfIntervalBarrierToCopyDest = 0;
    uint64_t m_perfIntervalBarrierFromCopyDest = 0;
    uint64_t m_perfIntervalBarrierToSrv = 0;
    uint64_t m_perfIntervalBarrierToRenderTarget = 0;
    uint64_t m_perfIntervalBarrierToUav = 0;
    uint64_t m_perfIntervalBarrierToDepth = 0;
    uint32_t m_perfIntervalMaxCbvSrvUavOff = 0;
    uint32_t m_perfIntervalMaxSamplerOff = 0;
    double m_perfIntervalFlushExecMs = 0.0;
    double m_perfIntervalFlushTotalMs = 0.0;
    double m_perfIntervalResetWaitMs = 0.0;
    double m_perfIntervalPresentWaitMs = 0.0;
    double m_perfIntervalDescriptorCommitMs = 0.0;
    double m_perfIntervalCommitCbvMs = 0.0;
    double m_perfIntervalCommitSrvMs = 0.0;
    double m_perfIntervalCommitUavMs = 0.0;
    double m_perfIntervalCommitSamplerMs = 0.0;
    double m_perfIntervalSnapshotMs = 0.0;
    double m_perfIntervalFlushBarriersMs = 0.0;
    double m_perfIntervalCloseExecuteMs = 0.0;
    double m_perfIntervalResetCommandListMs = 0.0;
    double m_perfIntervalDeviceCheckMs = 0.0;
    double m_perfIntervalReleaseCompletedMs = 0.0;
    double m_perfIntervalUploadBookkeepingMs = 0.0;
    double m_perfIntervalPostDeviceCheckMs = 0.0;
    double m_perfIntervalSignalMs = 0.0;
    double m_perfIntervalBatchMoveMs = 0.0;
    double m_perfIntervalUploadReleaseMs = 0.0;
    double m_perfIntervalResourceBatchReleaseMs = 0.0;
    uint64_t m_perfIntervalReleasedResourceRefs = 0;
    uint64_t m_perfIntervalCompletedUploads = 0;
    uint64_t m_perfIntervalRecycledUploads = 0;
    int64_t m_perfIntervalStartTicks = 0;
    int64_t m_perfCounterFrequency = 0;

    // Stream output state
    uint32_t m_soBufferCount = 0;
    D3D12_STREAM_OUTPUT_BUFFER_VIEW m_soViews[4] = {};
    ID3D12Resource* m_soResourceHeld[4] = {};
    uint64_t m_soResourceHeldGeneration[4] = {};
    D3D12BackendBuffer* m_soBuffers[4] = {};
    ComPtr<ID3D12Resource> m_soReadback;          // filled size readback
    void transitionStreamOutputCounter(D3D12BackendBuffer* buffer, D3D12_RESOURCE_STATES state);

    // Overlay upload buffer (persistent, mapped once)

    // Per-command-allocator upload scratch buffers for frequent
    // UpdateSubresource buffer updates. A slot is reset only after the matching
    // allocator's fence has completed.
    std::array<ComPtr<ID3D12Resource>, kCommandAllocatorRingSize> m_uploadScratchBuffers;
    std::array<void*, kCommandAllocatorRingSize> m_uploadScratchMapped = {};
    std::array<uint64_t, kCommandAllocatorRingSize> m_uploadScratchCapacities = {};
    std::array<uint64_t, kCommandAllocatorRingSize> m_uploadScratchOffsets = {};

    // Deferred context: true after closeForSubmission()
    bool m_closedForSubmission = false;
    GpuProfiler m_gpuProfiler;
#endif
  };

}
