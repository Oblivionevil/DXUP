#pragma once

#include "../directx/directx_backend.h"
#include "../util/util_thread.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace dxup::d3d12 {
  class D3D12BackendContext;
}

namespace dxup {

  // The immediate context records its backend commands on a worker thread
  // unless dxup_disable_cs_thread.txt exists next to the DLL.
  bool csThreadEnabled();

  // State the worker keeps next to the context it records into. The worker
  // calls the D3D12 context directly, without virtual calls.
  struct CsWorkerState {
    d3d12::D3D12BackendContext* ctx = nullptr;
    uint32_t            rtCount = 0;
    IBackendImageView*  rtvs[8] = {};
    IBackendImageView*  dsv = nullptr;

    // Transitions and copies can move a bound render target out of its
    // render target state; bind the output merger again before drawing.
    inline void rebindOutputMergerIfNeeded();

    void resetOutputMerger() {
      rtCount = 0;
      for (auto& rtv : rtvs)
        rtv = nullptr;
      dsv = nullptr;
    }
  };

  enum class CsOp : uint8_t { Run, RunAndDestroy, Destroy };

  // A recorded command; its lambda follows in the chunk. One function
  // pointer instead of virtual functions keeps the header small, and the
  // worker runs and frees each command with one call (most lambdas need no
  // destruction at all).
  struct CsCommand {
    void (*invoke)(CsCommand* command, CsWorkerState* state, CsOp op) = nullptr;
    CsCommand* next = nullptr;
  };

  template<typename Fn>
  struct CsTypedCommand final : CsCommand {
    explicit CsTypedCommand(Fn&& fn) : m_fn(std::move(fn)) {
      invoke = [](CsCommand* command, CsWorkerState* state, CsOp op) {
        auto* self = static_cast<CsTypedCommand*>(command);
        if (op != CsOp::Destroy)
          self->m_fn(*state);
        if constexpr (!std::is_trivially_destructible_v<Fn>) {
          if (op != CsOp::Run)
            self->~CsTypedCommand();
        }
      };
    }

    Fn m_fn;
  };

  // Commands and their inline data live in fixed-size chunks that are
  // recycled, so recording does not allocate.
  class CsChunk {
  public:
    static constexpr size_t kSize = 16384;

    template<typename Fn>
    bool push(Fn&& fn) {
      using Cmd = CsTypedCommand<std::decay_t<Fn>>;
      static_assert(sizeof(Cmd) <= kSize, "command too large for a chunk");
      void* ptr = allocate(sizeof(Cmd), alignof(Cmd));
      if (!ptr)
        return false;

      auto* cmd = new (ptr) Cmd(std::forward<Fn>(fn));
      if (m_tail)
        m_tail->next = cmd;
      else
        m_head = cmd;
      m_tail = cmd;
      return true;
    }

    void* allocate(size_t size, size_t align) {
      size_t offset = (m_used + align - 1) & ~(align - 1);
      if (offset + size > kSize)
        return nullptr;
      m_used = offset + size;
      return m_data + offset;
    }

    size_t remaining() const { return kSize - m_used; }
    bool empty() const { return !m_head; }

    // Objects retired while these commands were queued stay in retired for
    // the caller to hand on. A command that throws is logged and skipped,
    // as the record thread skips a call that faults: the worker has to go
    // on, or the application waits for it forever.
    void executeAll(CsWorkerState& state) {
      CsCommand* cmd = m_head;
      while (cmd) {
        try {
          while (cmd) {
            CsCommand* next = cmd->next;
            cmd->invoke(cmd, &state, CsOp::RunAndDestroy);
            cmd = next;
          }
        } catch (const std::exception& e) {
          cmd = skipFailedCommand(cmd, e.what());
        } catch (...) {
          cmd = skipFailedCommand(cmd, "unknown exception");
        }
      }
      m_head = nullptr;
      m_tail = nullptr;
      m_used = 0;
    }

    // Runs the commands and keeps them, for command lists executed again.
    void replay(CsWorkerState& state) {
      for (CsCommand* cmd = m_head; cmd; cmd = cmd->next)
        cmd->invoke(cmd, &state, CsOp::Run);
    }

    void clearCommands() {
      CsCommand* cmd = m_head;
      while (cmd) {
        CsCommand* next = cmd->next;
        cmd->invoke(cmd, nullptr, CsOp::Destroy);
        cmd = next;
      }
      m_head = nullptr;
      m_tail = nullptr;
      m_used = 0;
    }

    std::vector<std::shared_ptr<void>> retired;

    // Whether the chunk is handed out by the pool: a chunk freed twice
    // would end up with two owners.
    bool inUse = false;

  private:
    // Logs the exception, destroys the command and returns the next one.
    static CsCommand* skipFailedCommand(CsCommand* cmd, const char* what);

    alignas(64) unsigned char m_data[kSize];
    size_t      m_used = 0;
    CsCommand*  m_head = nullptr;
    CsCommand*  m_tail = nullptr;
  };

  // Chunks are shared by all contexts and command lists.
  CsChunk* allocCsChunk();
  void freeCsChunk(CsChunk* chunk);

  // Commands recorded by a deferred context. It keeps references to the
  // backend objects its commands use, which the application may release
  // while it still executes the list; when the list goes, they retire
  // behind the commands queued so far.
  class CsCommandList final : public IBackendCommandList {
  public:
    CsCommandList(
      std::vector<CsChunk*>               chunks,
      std::vector<IBackendQuery*>         queries,
      std::vector<BackendGpuUse*>         gpuUses,
      std::vector<std::shared_ptr<void>>  objects,
      std::shared_ptr<BackendRetireQueue> retireQueue);
    ~CsCommandList() override;

    void replay(CsWorkerState& state) const;
    const std::vector<IBackendQuery*>& queries() const { return m_queries; }
    // Staging resources the commands use, one entry per command.
    const std::vector<BackendGpuUse*>& gpuUses() const { return m_gpuUses; }

  private:
    std::vector<CsChunk*>               m_chunks;
    std::vector<IBackendQuery*>         m_queries;
    std::vector<BackendGpuUse*>         m_gpuUses;
    std::vector<std::shared_ptr<void>>  m_objects;
    std::shared_ptr<BackendRetireQueue> m_retireQueue;
  };

  // Executes a command list directly on a context that records on the
  // calling thread.
  void replayCsCommandList(IBackendContext* ctx, const Rc<IBackendCommandList>& list);

  // ============================================================
  // Immediate context that records its backend commands on a worker
  // thread, like DXVK's CS thread. The same class records deferred
  // contexts: their commands are kept in a command list instead. The application thread only encodes
  // commands; calls that return data wait for the worker first.
  // ============================================================
  class CsThreadContext final : public IBackendContext {
  public:
    // Immediate context recording into real on a worker thread.
    CsThreadContext(
      Rc<IBackendContext>                 real,
      IBackendDevice*                     device,
      std::shared_ptr<BackendRetireQueue> retireQueue);
    // Deferred context recording command lists.
    CsThreadContext(
      IBackendDevice*                     device,
      std::shared_ptr<BackendRetireQueue> retireQueue);
    ~CsThreadContext() override;

    void bindVertexBuffer(uint32_t slot, IBackendBuffer* buffer, uint32_t offset, uint32_t stride) override;
    void bindIndexBuffer(IBackendBuffer* buffer, uint32_t offset, IndexType type) override;
    void bindConstantBuffer(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer) override;
    void bindConstantBufferRange(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer,
                                 uint32_t firstConstant, uint32_t numConstants) override;
    void bindShaderResource(ShaderStage stage, uint32_t slot, IBackendImageView* view) override;
    void bindShaderResource(ShaderStage stage, uint32_t slot, IBackendBufferView* view) override;
    void bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendImageView* view) override;
    void bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendBufferView* view) override;
    void setUnorderedAccessCounter(IBackendBufferView* view, uint32_t value) override;
    void bindSampler(ShaderStage stage, uint32_t slot, IBackendSampler* sampler) override;

    void bindStreamOutputTargets(uint32_t count, IBackendBuffer** buffers, const uint32_t* offsets) override;
    uint64_t getStreamOutputFilledSize(IBackendBuffer* buffer) override;
    void drawAuto(IBackendBuffer* vertexBuffer, uint32_t stride) override;

    void bindRenderTargets(uint32_t count, IBackendImageView** rtvs, IBackendImageView* dsv) override;
    bool outputMergerStateNeedsRebind() const override { return false; }
    bool outputMergerStateCanNeedRebind() const override { return false; }

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

    void bindGraphicsPipeline(IBackendPipeline* pipeline) override;
    void bindComputePipeline(IBackendPipeline* pipeline) override;

    void draw(uint32_t vertexCount, uint32_t instanceCount,
              uint32_t firstVertex, uint32_t firstInstance) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override;

    void drawIndirect(IBackendBuffer* buffer, uint64_t offset) override;
    void drawIndexedIndirect(IBackendBuffer* buffer, uint64_t offset) override;
    void dispatchIndirect(IBackendBuffer* buffer, uint64_t offset) override;

    void copyBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                    IBackendBuffer* src, uint64_t srcOffset, uint64_t size) override;
    void copyImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                   IBackendImage* src, const ImageSubresource& srcSubres) override;
    void copyImageRegion(IBackendImage* dst, const ImageSubresource& dstSubres,
                         uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                         IBackendImage* src, const ImageSubresource& srcSubres,
                         const ImageCopyRegion* srcRegion) override;
    void copyBufferToImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                           IBackendBuffer* src, uint64_t srcOffset,
                           const ImageCopyRegion* region) override;
    void copyImageToBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                           IBackendImage* src, const ImageSubresource& srcSubres) override;
    void copyStructureCount(IBackendBuffer* dst, uint64_t dstOffset, IBackendBufferView* view) override;

    void* mapBuffer(IBackendBuffer* buffer, MapMode mode) override;
    void unmapBuffer(IBackendBuffer* buffer) override;
    void updateBuffer(IBackendBuffer* buffer, uint64_t offset, uint64_t size, const void* data) override;
    void updateBufferRegions(IBackendBuffer* buffer, const BufferUpdateRegion* regions, uint32_t regionCount) override;
    void retainTemporaryBuffer(Rc<IBackendBuffer> buffer) override;
    void retireAfterGpu(std::vector<std::shared_ptr<void>>& objects) override;
    void* discardBuffer(IBackendBuffer* buffer) override;
    void writeMappedBuffer(IBackendBuffer* buffer, uint64_t offset, const void* data, size_t size, size_t padded) override;
    void writeMappedBufferWith(IBackendBuffer* buffer, uint64_t offset, size_t size, WriteFn fn, void* user,
                               PackFn pack, void* packUser, size_t dataSize) override;
    // Writes an update of a renamable constant buffer to a new upload slice;
    // false when the update takes the GPU copy.
    bool renameConstantBuffer(IBackendBuffer* buffer, const BufferUpdateRegion* regions, uint32_t regionCount);
    // The GPU writes the buffer: the application thread no longer knows its
    // contents.
    void noteBufferGpuWrite(IBackendBuffer* buffer);
    // Upload slices taken in batches for WRITE_DISCARD on the application
    // thread.
    std::unique_ptr<struct CsSliceCache> m_sliceCache;
    void retargetBufferViews(IBackendBuffer* buffer, const std::vector<IBackendBufferView*>& views) override;

    void beginQuery(IBackendQuery* query) override;
    void endQuery(IBackendQuery* query) override;
    void setPredication(IBackendQuery* predicate, bool value) override;
    void suspendPredication(bool suspended) override;
    void setImageMinLod(IBackendImage* image, float minLod) override;
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

    void flush() override;
    void synchronize() override;
    bool waitForGpuUse(BackendGpuUse& use, bool doNotWait) override;
    bool renameStagingImage(IBackendImage* image) override;

    void closeForSubmission() override;
    void submitRecordedCommands() override;

    void presentImage(IBackendImage* backBuffer) override;
    void waitForFrameLatency(uint32_t maxLatency) override;
    void endFrame() override;
    void executeOnBackend(std::function<void(IBackendContext*)> fn) override;
    void waitForWorker() override;
    void waitForQueue() override;
    bool flushPending() const override { return flushQueued(); }
    Rc<IBackendCommandList> finishCommandList() override;
    void executeCommandList(const Rc<IBackendCommandList>& list) override;

    void clearRenderTarget(IBackendImageView* rtv, const float color[4]) override;
    void clearDepthStencil(IBackendImageView* dsv, float depth, uint8_t stencil, uint32_t flags) override;
    void clearUnorderedAccessViewUint(IBackendImageView* view, const uint32_t values[4]) override;
    void clearUnorderedAccessViewUint(IBackendBufferView* view, const uint32_t values[4]) override;
    void clearUnorderedAccessViewFloat(IBackendImageView* view, const float values[4]) override;
    void clearUnorderedAccessViewFloat(IBackendBufferView* view, const float values[4]) override;
    void clearBufferView(IBackendBufferView* view, const float color[4], uint32_t rectCount, const Rect* rects) override;
    void clearView(IBackendImageView* view, bool renderTarget, const float color[4],
                   uint32_t rectCount, const Rect* rects) override;

    void generateMips(IBackendImageView* srv) override;
    void upsampleImage(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) override;
    void resolveSampleZero(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) override;
    void resolveFinite(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format) override;
    void copyStencil(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) override;
    void resolveSubresource(IBackendImage* dst, uint32_t dstSubres,
                            IBackendImage* src, uint32_t srcSubres, DXGI_FORMAT format) override;

  private:
    // Inline copies above this size go to the heap instead of the chunk.
    static constexpr size_t kMaxInlineData = 8192;
    // About 100-200 us of spinning before a synchronization blocks.
    static constexpr uint32_t kSyncSpinCount = 4096;
    // Polling before the worker sleeps. A frame's chunks arrive tens of
    // microseconds apart: staying awake through those gaps spares the
    // application a wake-up (a system call) for every chunk.
    static constexpr int64_t kWorkerSpinMicroseconds = 250;

    struct StagedMap {
      IBackendBuffer*     target = nullptr;
      Rc<IBackendBuffer>  staging;
      uint64_t            size = 0;
    };

    Rc<IBackendContext>                   m_real;
    IBackendDevice*                       m_device;
    std::shared_ptr<BackendRetireQueue>   m_retireQueue;

    // Deferred context state
    bool                        m_recording = false;
    std::vector<CsChunk*>       m_recordedChunks;
    std::vector<IBackendQuery*> m_recordedQueries;
    std::vector<BackendGpuUse*> m_recordedGpuUses;
    // References to the objects the recorded commands use, and those
    // objects, once each.
    std::vector<std::shared_ptr<void>>          m_recordedObjects;
    std::unordered_set<const BackendRetirable*> m_keptObjects;

    // Application thread
    CsChunk*                m_chunk = nullptr;
    uint64_t                m_chunksSubmitted = 0;
    // The chunk the last flush was submitted with.
    uint64_t                m_flushChunk = 0;
    // 32-bit processes: temporary upload memory (texture uploads) queued
    // since the last window ended, and the chunk that ended it.
    uint64_t                m_uploadWindowBytes = 0;
    uint64_t                m_uploadWindowChunk = 0;
    std::vector<StagedMap>  m_stagedMaps;

    // Worker queue
    std::mutex              m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<CsChunk*>    m_queue;
    std::atomic<uint32_t>   m_queuedChunks{0};
    bool                    m_stopping = false;

    // Worker progress
    struct SyncWaiter {
      uint64_t target;
      void*    event;
    };
    std::mutex              m_syncMutex;
    std::atomic<uint64_t>   m_chunksExecuted{0};
    // Threads waiting for a chunk, with an event each, and the earliest of
    // their chunks (guarded by m_syncMutex): the worker wakes a thread once
    // its chunk ran, not after every chunk.
    std::vector<SyncWaiter> m_syncWaiters;
    uint64_t                m_syncTarget = UINT64_MAX;
    // The worker runs a callback of executeOnBackend, such as DXGI's
    // Present; it wakes the waiting threads when it starts one.
    std::atomic<bool>       m_workerInCallback{false};

    // Frames: ended by the application thread, and done on the worker,
    // which stores the fence value each frame's GPU work completes with
    // before it counts the frame and signals the event.
    static constexpr uint32_t kPresentHistory = 16;
    uint64_t                m_presents = 0;
    std::atomic<uint64_t>   m_framesExecuted{0};
    uint64_t                m_presentFences[kPresentHistory] = {};
    void*                   m_frameEvent = nullptr;
    void waitForChunk(uint64_t chunk);
    void waitForFrames(uint64_t frames);
    // Worker: wakes the threads whose chunk ran.
    void wakeSyncWaiters(uint64_t executed);

    CsWorkerState           m_workerState;
    Thread                  m_worker;

    void workerMain();
    void submitChunk();
    bool flushQueued() const;

    // Counts a command that uses a staging resource until the worker has
    // recorded it; Map waits for that.
    BackendGpuUse* trackGpuUse(BackendGpuUse* use) {
      if (!use)
        return nullptr;
      if (m_recording)
        m_recordedGpuUses.push_back(use);
      else
        use->queuedOps.fetch_add(1, std::memory_order_relaxed);
      return use;
    }
    static void releaseGpuUse(BackendGpuUse* use) {
      if (use)
        use->queuedOps.fetch_sub(1, std::memory_order_release);
    }
    void discardRecording();
    // Deferred contexts: the command list keeps these objects alive.
    template<typename... Objects>
    void keep(const Objects*... objects) {
      if (m_recording)
        (keepObject(objects), ...);
    }
    void keepObject(const BackendRetirable* object);
    void keepTiled(const TiledResourceRef& ref) { keep(ref.buffer, ref.image); }
    void* mapDirect(IBackendBuffer* buffer, MapMode mode);
    void unmapDirect(IBackendBuffer* buffer);

    template<typename Fn>
    void emit(Fn&& fn) {
      if (!m_chunk->push(std::forward<Fn>(fn))) {
        submitChunk();
        m_chunk->push(std::forward<Fn>(fn));
      }
    }

    // fill(data) writes bytes into the chunk the command lands in, which
    // records fn(state, data, bytes). Sizes above kMaxInlineData go through
    // a heap copy.
    template<typename Fill, typename Fn>
    void emitFilled(size_t bytes, Fill&& fill, Fn&& fn) {
      if (bytes > kMaxInlineData) {
        std::vector<uint8_t> copy(bytes);
        fill(copy.data());
        emit([fn = std::forward<Fn>(fn), copy = std::move(copy)](CsWorkerState& state) mutable {
          fn(state, static_cast<const uint8_t*>(copy.data()), copy.size());
        });
        return;
      }
      if (m_chunk->remaining() < bytes + sizeof(Fn) + 256)
        submitChunk();
      auto* data = static_cast<uint8_t*>(m_chunk->allocate(bytes ? bytes : 1, 8));
      fill(data);
      emit([fn = std::forward<Fn>(fn), data, bytes](CsWorkerState& state) mutable {
        fn(state, static_cast<const uint8_t*>(data), bytes);
      });
    }

    // Copies count items into the chunk the command lands in and records
    // fn(state, copy). Sizes above kMaxInlineData go through a heap copy.
    template<typename T, typename Fn>
    void emitArray(const T* items, size_t count, Fn&& fn) {
      static_assert(std::is_trivially_copyable<T>::value, "inline data must be trivially copyable");
      const size_t bytes = sizeof(T) * count;
      if (!items || !count) {
        emit([fn = std::forward<Fn>(fn)](CsWorkerState& state) mutable {
          fn(state, static_cast<const T*>(nullptr), size_t(0));
        });
        return;
      }

      if (bytes > kMaxInlineData) {
        std::vector<T> copy(items, items + count);
        emit([fn = std::forward<Fn>(fn), copy = std::move(copy)](CsWorkerState& state) mutable {
          fn(state, copy.data(), copy.size());
        });
        return;
      }

      // Room for the data, the command and alignment padding.
      if (m_chunk->remaining() < bytes + sizeof(Fn) + 256)
        submitChunk();

      auto* copy = static_cast<T*>(m_chunk->allocate(bytes, alignof(T) < 8 ? 8 : alignof(T)));
      std::memcpy(copy, items, bytes);
      emit([fn = std::forward<Fn>(fn), copy, count](CsWorkerState& state) mutable {
        fn(state, static_cast<const T*>(copy), count);
      });
    }
  };

}
