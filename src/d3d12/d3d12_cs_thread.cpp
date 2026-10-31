#include "d3d12_cs_thread.h"
#include "d3d12_backend_device.h"
#include "d3d12_backend_context.h"
#include "../util/util_types.h"
#include "../util/util_paths.h"

#include <array>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dxup {

  void CsWorkerState::rebindOutputMergerIfNeeded() {
    if ((rtCount || dsv) && ctx->outputMergerStateNeedsRebind())
      ctx->bindRenderTargets(rtCount, rtvs, dsv);
  }

  bool csThreadEnabled() {
#ifdef _WIN32
    static bool enabled = [] {
      if (dxupSiblingExists("dxup_disable_cs_thread.txt")) {
        logMessage(LogLevel::Info, "CS thread disabled by dxup_disable_cs_thread.txt");
        return false;
      }
      return true;
    }();
    return enabled;
#else
    return false;
#endif
  }

  namespace {
    std::mutex             g_chunkPoolMutex;
    std::vector<CsChunk*>  g_freeChunks;
    // Enough for a few frames of commands; more are freed.
    constexpr size_t       kMaxPooledChunks = 512;

    // An auto-reset event for each thread that waits for a worker.
    struct ThreadWaitEvent {
      HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      ~ThreadWaitEvent() {
        if (handle)
          CloseHandle(handle);
      }
    };

    HANDLE threadWaitEvent() {
      static thread_local ThreadWaitEvent event;
      return event.handle;
    }

    // Takes the messages sent to this thread, as a native Present would,
    // then waits for the event (none: only for the time) at most timeout ms
    // or until another message is sent; callers loop. The worker's DXGI
    // Present can send messages to the window, whose thread may be the
    // waiting one. Posted messages stay for the application.
    void waitTakingSentMessages(HANDLE event, DWORD timeout) {
      MSG msg;
      PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
      // MWMO_INPUTAVAILABLE: also for a message that arrived before the
      // wait and was seen without being taken.
      MsgWaitForMultipleObjectsEx(event ? 1 : 0, event ? &event : nullptr, timeout, QS_SENDMESSAGE,
        MWMO_INPUTAVAILABLE);
    }
  }

  // Slices taken from the allocator at once: one lock for this many maps.
  constexpr uint32_t kSliceBatch = 32;

  struct CsSliceCache {
    d3d12::D3D12UploadSuballocator* owner = nullptr;
    std::array<std::vector<d3d12::D3D12UploadSuballocator::Slice>, d3d12::D3D12UploadSuballocator::kMaxClassCount> slices;
  };

  CsChunk* allocCsChunk() {
    CsChunk* chunk = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_chunkPoolMutex);
      if (!g_freeChunks.empty()) {
        chunk = g_freeChunks.back();
        g_freeChunks.pop_back();
      }
    }
    if (!chunk)
      chunk = new CsChunk();
    chunk->inUse = true;
    return chunk;
  }

  void freeCsChunk(CsChunk* chunk) {
    if (!chunk)
      return;
    if (!chunk->inUse) {
      static std::atomic<uint32_t> s_reported{ 0 };
      if (s_reported.fetch_add(1) < 8) {
        void* frames[12] = {};
        const USHORT count = CaptureStackBackTrace(1, 12, frames, nullptr);
        logMessage(LogLevel::Error, "CS chunk %p freed twice; frames %p %p %p %p %p %p %p %p", static_cast<void*>(chunk),
          count > 0 ? frames[0] : nullptr, count > 1 ? frames[1] : nullptr, count > 2 ? frames[2] : nullptr,
          count > 3 ? frames[3] : nullptr, count > 4 ? frames[4] : nullptr, count > 5 ? frames[5] : nullptr,
          count > 6 ? frames[6] : nullptr, count > 7 ? frames[7] : nullptr);
      }
      return;
    }
    chunk->clearCommands();
    chunk->retired.clear();
    chunk->inUse = false;
    {
      std::lock_guard<std::mutex> lock(g_chunkPoolMutex);
      if (g_freeChunks.size() < kMaxPooledChunks) {
        g_freeChunks.push_back(chunk);
        return;
      }
    }
    delete chunk;
  }

  void CsChunk::reportFailedCommand(const CsCommand* cmd, const char* what) {
    static std::atomic<uint32_t> s_reported{ 0 };
    if (s_reported.fetch_add(1) < 8)
      logMessage(LogLevel::Error, "CS worker: command %p threw (%s); skipped", reinterpret_cast<void*>(cmd->invoke), what);
  }

  CsCommand* CsChunk::skipFailedCommand(CsCommand* cmd, const char* what) {
    reportFailedCommand(cmd, what);
    CsCommand* next = cmd->next;
    cmd->invoke(cmd, nullptr, CsOp::Destroy);
    return next;
  }

  CsCommandList::CsCommandList(
      std::vector<CsChunk*>               chunks,
      std::vector<IBackendQuery*>         queries,
      std::vector<BackendGpuUse*>         gpuUses,
      std::vector<std::shared_ptr<void>>  objects,
      std::unordered_set<IBackendBuffer*> ownContents,
      std::shared_ptr<BackendRetireQueue> retireQueue)
  : m_chunks(std::move(chunks))
  , m_queries(std::move(queries))
  , m_gpuUses(std::move(gpuUses))
  , m_objects(std::move(objects))
  , m_ownContents(std::move(ownContents))
  , m_retireQueue(std::move(retireQueue)) {
  }

  CsCommandList::~CsCommandList() {
    // Destroying the commands drops the references they captured. The GPU
    // may still run what the worker recorded from them: the objects retire
    // like released ones.
    for (CsChunk* chunk : m_chunks)
      freeCsChunk(chunk);
    if (m_retireQueue)
      m_retireQueue->retire(m_objects);
  }

  void CsCommandList::replay(CsWorkerState& state) const {
    // D3D11 command lists nest only as deep as applications execute them
    // into one another; far deeper means a list reaches itself.
    static thread_local uint32_t s_depth = 0;
    if (s_depth >= 64) {
      static std::atomic<uint32_t> s_reported{ 0 };
      if (s_reported.fetch_add(1) < 8)
        logMessage(LogLevel::Error, "CS command list %p nested %u deep: not replayed", static_cast<const void*>(this), s_depth);
      return;
    }
    // Also when a command throws; a list replayed by another goes back to
    // the outer one's buffers after.
    struct Depth {
      CsWorkerState& state;
      const std::unordered_set<IBackendBuffer*>* own;
      bool replaying;
      Depth(CsWorkerState& s, const std::unordered_set<IBackendBuffer*>* listOwn)
      : state(s), own(s.replayOwn), replaying(s.replaying) {
        s_depth++;
        s.replayOwn = listOwn;
        s.replaying = true;
      }
      ~Depth() {
        s_depth--;
        state.replayOwn = own;
        state.replaying = replaying;
      }
    } depth(state, &m_ownContents);
    for (CsChunk* chunk : m_chunks)
      chunk->replay(state);
  }

  void replayCsCommandList(IBackendContext* ctx, const Rc<IBackendCommandList>& list) {
    auto* commands = static_cast<const CsCommandList*>(list.get());
    if (!ctx || !commands)
      return;
    // The replayed Begin/End calls count themselves down.
    for (IBackendQuery* query : commands->queries())
      query->queuedOps.fetch_add(1, std::memory_order_relaxed);
    for (BackendGpuUse* use : commands->gpuUses())
      use->queuedOps.fetch_add(1, std::memory_order_relaxed);
    CsWorkerState state;
    // Only the D3D12 context replays command lists.
    state.ctx = static_cast<d3d12::D3D12BackendContext*>(ctx);
    ctx->clearState();
    commands->replay(state);
  }

  CsThreadContext::CsThreadContext(
      IBackendDevice*                     device,
      std::shared_ptr<BackendRetireQueue> retireQueue)
  : m_device(device)
  , m_retireQueue(std::move(retireQueue))
  , m_recording(true) {
    m_chunk = allocCsChunk();
    // Never 0, which objects not kept yet carry.
    nextRecording();
  }

  CsThreadContext::CsThreadContext(
      Rc<IBackendContext>                 real,
      IBackendDevice*                     device,
      std::shared_ptr<BackendRetireQueue> retireQueue)
  : m_real(std::move(real))
  , m_device(device)
  , m_retireQueue(std::move(retireQueue)) {
    m_workerState.ctx = static_cast<d3d12::D3D12BackendContext*>(m_real.get());
    m_chunk = allocCsChunk();
    if (m_retireQueue)
      m_retireQueue->setDeferred(true);
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_lowLatency = UserOverrides::get().lowLatency;
    m_worker = Thread([this] { workerMain(); });
  }

  CsThreadContext::~CsThreadContext() {
    if (m_recording) {
      discardRecording();
      freeCsChunk(m_chunk);
      m_chunk = nullptr;
      return;
    }

    waitForWorker();
    // Jobs of the submit thread that count frames refer to this context.
    m_real->waitForQueue();
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_stopping = true;
    }
    m_queueCv.notify_one();
    if (m_worker.joinable())
      m_worker.join();

    // Nothing is queued any more; later releases can destroy right away,
    // once the GPU is through as well: the real context keeps no
    // references of its own to what the last frames use.
    if (!processExiting())
      m_real->synchronize();
    if (m_retireQueue)
      m_retireQueue->setDeferred(false);
    if (m_sliceCache && m_sliceCache->owner) {
      for (auto& cache : m_sliceCache->slices)
        m_sliceCache->owner->free(cache);
    }
    m_stagedMaps.clear();
    freeCsChunk(m_chunk);
    m_chunk = nullptr;
    if (m_frameEvent)
      CloseHandle(m_frameEvent);
    if (m_pacingTimer)
      CloseHandle(m_pacingTimer);
  }

  void CsThreadContext::discardRecording() {
    for (CsChunk* chunk : m_recordedChunks)
      freeCsChunk(chunk);
    m_recordedChunks.clear();
    m_chunk->clearCommands();
    m_recordedQueries.clear();
    m_recordedGpuUses.clear();
    m_stagedMaps.clear();
    nextRecording();
    if (m_retireQueue)
      m_retireQueue->retire(m_recordedObjects);
    m_recordedObjects.clear();
    m_recordedOwnContents.clear();
  }

  void CsThreadContext::nextRecording() {
    static std::atomic<uint64_t> s_recordings{0};
    m_recordingId = s_recordings.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void CsThreadContext::keepObject(const BackendRetirable* object) {
    // Once per recording: the object carries the mark (binds repeat).
    if (!object || !object->keepFor(m_recordingId))
      return;
    if (std::shared_ptr<void> owner = object->retireOwner())
      m_recordedObjects.push_back(std::move(owner));
  }

  void CsThreadContext::workerMain() {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"dxup-cs");
#endif
    for (;;) {
      CsChunk* chunk = nullptr;
      // The application submits chunks in bursts; poll a while before
      // sleeping so the next chunk does not pay for a wake-up.
      if (!m_queuedChunks.load(std::memory_order_acquire)) {
        LARGE_INTEGER start, now, frequency;
        QueryPerformanceCounter(&start);
        QueryPerformanceFrequency(&frequency);
        const int64_t spinTicks = frequency.QuadPart * kWorkerSpinMicroseconds / 1000000;
        for (uint32_t i = 1; !m_queuedChunks.load(std::memory_order_acquire); i++) {
          YieldProcessor();
          if (!(i & 63)) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart - start.QuadPart > spinTicks)
              break;
          }
        }
      }
      {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_queueCv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
        if (m_queue.empty())
          return;
        chunk = m_queue.front();
        m_queue.pop_front();
        m_queuedChunks.fetch_sub(1, std::memory_order_relaxed);
      }

      try {
        chunk->executeAll(m_workerState);
        // The GPU may still use what these commands referenced.
        if (!chunk->retired.empty())
          m_real->retireAfterGpu(chunk->retired);
      } catch (const std::exception& e) {
        logMessage(LogLevel::Error, "CS worker: retiring a chunk's objects failed (%s)", e.what());
      }
      freeCsChunk(chunk);

      {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        const uint64_t executed = m_chunksExecuted.fetch_add(1, std::memory_order_release) + 1;
        if (executed >= m_syncTarget)
          wakeSyncWaiters(executed);
      }
    }
  }

  void CsThreadContext::wakeSyncWaiters(uint64_t executed) {
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < m_syncWaiters.size();) {
      if (m_syncWaiters[i].target <= executed) {
        SetEvent(m_syncWaiters[i].event);
        m_syncWaiters[i] = m_syncWaiters.back();
        m_syncWaiters.pop_back();
      } else {
        next = (std::min)(next, m_syncWaiters[i].target);
        i++;
      }
    }
    m_syncTarget = next;
  }

  void CsThreadContext::submitChunk() {
    if (m_recording) {
      if (m_chunk->empty())
        return;
      m_recordedChunks.push_back(m_chunk);
      m_chunk = allocCsChunk();
      return;
    }

    bool retired = m_retireQueue && m_retireQueue->takeRetired(m_chunk->retired);
    if (m_chunk->empty() && !retired && m_chunk->retired.empty())
      return;

    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_queue.push_back(m_chunk);
      m_queuedChunks.fetch_add(1, std::memory_order_release);
    }
    m_queueCv.notify_one();
    m_chunksSubmitted++;
    m_chunk = allocCsChunk();
  }

  void CsThreadContext::waitForWorker() {
    HitchScope hitch("app waitForWorker");
    if (m_recording)
      return;
    submitChunk();
    waitForChunk(m_chunksSubmitted);
  }

  void CsThreadContext::waitForChunk(uint64_t target) {
    if (m_chunksExecuted.load(std::memory_order_acquire) >= target)
      return;

    // The worker usually finishes shortly after the application stops
    // recording; spin briefly instead of paying for a sleep and wake-up.
    for (uint32_t i = 0; i < kSyncSpinCount; i++) {
      YieldProcessor();
      if (m_chunksExecuted.load(std::memory_order_acquire) >= target)
        return;
    }

    // The worker is gone once the process exits.
    if (processExiting())
      return;
    void* event = threadWaitEvent();
    {
      std::lock_guard<std::mutex> lock(m_syncMutex);
      if (m_chunksExecuted.load(std::memory_order_acquire) >= target)
        return;
      m_syncWaiters.push_back({ target, event });
      m_syncTarget = (std::min)(m_syncTarget, target);
    }
    while (m_chunksExecuted.load(std::memory_order_acquire) < target && !processExiting()) {
      // The worker presents, and a present can wait for the window's
      // thread, which may be this one: its messages are taken while the
      // worker runs such a callback. Not otherwise: they would run the
      // window procedure in the middle of a D3D11 call, which native D3D11
      // does not do.
      if (m_workerInCallback.load(std::memory_order_acquire))
        waitTakingSentMessages(event, 100);
      else if (event)
        WaitForSingleObject(event, 100);
      else
        Sleep(1);
    }

    // The worker removes the entry when the chunk has run; it is left when
    // the process exits.
    std::lock_guard<std::mutex> lock(m_syncMutex);
    for (size_t i = 0; i < m_syncWaiters.size(); i++) {
      if (m_syncWaiters[i].event == event && m_syncWaiters[i].target == target) {
        m_syncWaiters[i] = m_syncWaiters.back();
        m_syncWaiters.pop_back();
        m_syncTarget = UINT64_MAX;
        for (const SyncWaiter& waiter : m_syncWaiters)
          m_syncTarget = (std::min)(m_syncTarget, waiter.target);
        break;
      }
    }
  }

  // ============================================================
  // Resource binding
  // ============================================================

  void CsThreadContext::bindVertexBuffer(uint32_t slot, IBackendBuffer* buffer, uint32_t offset, uint32_t stride) {
    keep(buffer);
    emit([=](CsWorkerState& s) { s.ctx->bindVertexBuffer(slot, s.resolve(buffer), offset, stride); });
  }

  void CsThreadContext::bindIndexBuffer(IBackendBuffer* buffer, uint32_t offset, IndexType type) {
    keep(buffer);
    emit([=](CsWorkerState& s) { s.ctx->bindIndexBuffer(s.resolve(buffer), offset, type); });
  }

  void CsThreadContext::bindConstantBuffer(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer) {
    keep(buffer);
    emit([=](CsWorkerState& s) { s.ctx->bindConstantBuffer(stage, slot, s.resolve(buffer)); });
  }

  void CsThreadContext::bindConstantBufferRange(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer,
                                                uint32_t firstConstant, uint32_t numConstants) {
    keep(buffer);
    emit([=](CsWorkerState& s) {
      s.ctx->bindConstantBufferRange(stage, slot, s.resolve(buffer), firstConstant, numConstants);
    });
  }

  void CsThreadContext::bindShaderResource(ShaderStage stage, uint32_t slot, IBackendImageView* view) {
    keep(view);
    emit([=](CsWorkerState& s) { s.ctx->bindShaderResource(stage, slot, view); });
  }

  void CsThreadContext::bindShaderResource(ShaderStage stage, uint32_t slot, IBackendBufferView* view) {
    keep(view);
    emit([=](CsWorkerState& s) { s.ctx->bindShaderResource(stage, slot, view); });
  }

  void CsThreadContext::bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendImageView* view) {
    keep(view);
    emit([=](CsWorkerState& s) { s.ctx->bindUnorderedAccess(stage, slot, view); });
  }

  void CsThreadContext::bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendBufferView* view) {
    keep(view);
    emit([=](CsWorkerState& s) { s.ctx->bindUnorderedAccess(stage, slot, view); });
  }

  void CsThreadContext::setUnorderedAccessCounter(IBackendBufferView* view, uint32_t value) {
    keep(view);
    emit([=](CsWorkerState& s) { s.ctx->setUnorderedAccessCounter(view, value); });
  }

  void CsThreadContext::bindSampler(ShaderStage stage, uint32_t slot, IBackendSampler* sampler) {
    keep(sampler);
    emit([=](CsWorkerState& s) { s.ctx->bindSampler(stage, slot, sampler); });
  }

  // ============================================================
  // Stream output
  // ============================================================

  void CsThreadContext::bindStreamOutputTargets(uint32_t count, IBackendBuffer** buffers, const uint32_t* offsets) {
    std::array<IBackendBuffer*, 4> bufferCopy = {};
    std::array<uint32_t, 4> offsetCopy = {};
    const uint32_t clamped = count < 4 ? count : 4;
    for (uint32_t i = 0; i < clamped; i++) {
      bufferCopy[i] = buffers ? buffers[i] : nullptr;
      offsetCopy[i] = offsets ? offsets[i] : 0;
      keep(bufferCopy[i]);
    }
    const bool hasBuffers = buffers != nullptr;
    const bool hasOffsets = offsets != nullptr;
    emit([=](CsWorkerState& s) mutable {
      for (IBackendBuffer*& buffer : bufferCopy)
        buffer = s.resolve(buffer);
      s.ctx->bindStreamOutputTargets(
        clamped,
        hasBuffers ? bufferCopy.data() : nullptr,
        hasOffsets ? offsetCopy.data() : nullptr);
    });
  }

  uint64_t CsThreadContext::getStreamOutputFilledSize(IBackendBuffer* buffer) {
    if (m_recording)
      return 0;
    waitForQueue();
    return m_real->getStreamOutputFilledSize(buffer);
  }

  void CsThreadContext::drawAuto(IBackendBuffer* vertexBuffer, uint32_t stride) {
    keep(vertexBuffer);
    // Counts what stream output wrote when the draw is recorded, also for
    // command lists.
    emit([=](CsWorkerState& s) {
      s.rebindOutputMergerIfNeeded();
      s.ctx->drawAuto(s.resolve(vertexBuffer), stride);
    });
  }

  // ============================================================
  // Render targets and state
  // ============================================================

  void CsThreadContext::bindRenderTargets(uint32_t count, IBackendImageView** rtvs, IBackendImageView* dsv) {
    std::array<IBackendImageView*, 8> rtvCopy = {};
    const uint32_t clamped = count < 8 ? count : 8;
    for (uint32_t i = 0; i < clamped && rtvs; i++) {
      rtvCopy[i] = rtvs[i];
      keep(rtvCopy[i]);
    }
    keep(dsv);
    emit([=](CsWorkerState& s) mutable {
      s.rtCount = clamped;
      for (uint32_t i = 0; i < 8; i++)
        s.rtvs[i] = rtvCopy[i];
      s.dsv = dsv;
      s.ctx->bindRenderTargets(clamped, rtvCopy.data(), dsv);
    });
  }

  void CsThreadContext::clearState() {
    // Command lists start from the default state anyway.
    if (m_recording && m_chunk->empty() && m_recordedChunks.empty())
      return;
    emit([](CsWorkerState& s) {
      s.resetOutputMerger();
      s.ctx->clearState();
    });
  }

  void CsThreadContext::releaseBindings() {
    if (m_recording) {
      discardRecording();
      return;
    }
    emit([](CsWorkerState& s) {
      s.resetOutputMerger();
      s.ctx->releaseBindings();
    });
    waitForWorker();
  }

  void CsThreadContext::setViewports(uint32_t count, const Viewport* viewports) {
    emitArray(viewports, count, [](CsWorkerState& s, const Viewport* data, size_t n) {
      s.ctx->setViewports(static_cast<uint32_t>(n), data);
    });
  }

  void CsThreadContext::setScissorRects(uint32_t count, const Rect* rects) {
    emitArray(rects, count, [](CsWorkerState& s, const Rect* data, size_t n) {
      s.ctx->setScissorRects(static_cast<uint32_t>(n), data);
    });
  }

  void CsThreadContext::setBlendState(const BlendState& state) {
    emit([state](CsWorkerState& s) { s.ctx->setBlendState(state); });
  }

  void CsThreadContext::setRasterizerState(const RasterizerState& state) {
    emit([state](CsWorkerState& s) { s.ctx->setRasterizerState(state); });
  }

  void CsThreadContext::setDepthStencilState(const DepthStencilState& state) {
    emit([state](CsWorkerState& s) { s.ctx->setDepthStencilState(state); });
  }

  void CsThreadContext::setStencilRef(uint32_t ref) {
    emit([=](CsWorkerState& s) { s.ctx->setStencilRef(ref); });
  }

  void CsThreadContext::setBlendFactor(const float factor[4]) {
    std::array<float, 4> copy = { factor[0], factor[1], factor[2], factor[3] };
    emit([copy](CsWorkerState& s) { s.ctx->setBlendFactor(copy.data()); });
  }

  void CsThreadContext::setPrimitiveTopology(PrimitiveTopology topology) {
    emit([=](CsWorkerState& s) { s.ctx->setPrimitiveTopology(topology); });
  }

  // ============================================================
  // Pipelines
  // ============================================================

  void CsThreadContext::bindGraphicsPipeline(IBackendPipeline* pipeline) {
    emit([=](CsWorkerState& s) { s.ctx->bindGraphicsPipeline(pipeline); });
  }

  void CsThreadContext::bindComputePipeline(IBackendPipeline* pipeline) {
    emit([=](CsWorkerState& s) { s.ctx->bindComputePipeline(pipeline); });
  }


  // ============================================================
  // Draws and dispatches
  // ============================================================

  void CsThreadContext::draw(uint32_t vertexCount, uint32_t instanceCount,
                             uint32_t firstVertex, uint32_t firstInstance) {
    emit([=](CsWorkerState& s) {
      s.rebindOutputMergerIfNeeded();
      s.ctx->draw(vertexCount, instanceCount, firstVertex, firstInstance);
    });
  }

  void CsThreadContext::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    emit([=](CsWorkerState& s) {
      s.rebindOutputMergerIfNeeded();
      s.ctx->drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    });
  }

  void CsThreadContext::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    emit([=](CsWorkerState& s) { s.ctx->dispatch(x, y, z); });
  }

  void CsThreadContext::drawIndirect(IBackendBuffer* buffer, uint64_t offset) {
    keep(buffer);
    emit([=](CsWorkerState& s) {
      s.rebindOutputMergerIfNeeded();
      s.ctx->drawIndirect(s.resolve(buffer), offset);
    });
  }

  void CsThreadContext::drawIndexedIndirect(IBackendBuffer* buffer, uint64_t offset) {
    keep(buffer);
    emit([=](CsWorkerState& s) {
      s.rebindOutputMergerIfNeeded();
      s.ctx->drawIndexedIndirect(s.resolve(buffer), offset);
    });
  }

  void CsThreadContext::dispatchIndirect(IBackendBuffer* buffer, uint64_t offset) {
    keep(buffer);
    emit([=](CsWorkerState& s) { s.ctx->dispatchIndirect(s.resolve(buffer), offset); });
  }

  // ============================================================
  // Copies
  // ============================================================

  void CsThreadContext::copyBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                                   IBackendBuffer* src, uint64_t srcOffset, uint64_t size) {
    keep(dst, src);
    noteBufferGpuWrite(dst);
    BackendGpuUse* dstUse = trackGpuUse(stagingGpuUse(dst));
    BackendGpuUse* srcUse = trackGpuUse(stagingGpuUse(src));
    emit([=](CsWorkerState& s) {
      const GpuUseRelease release{ dstUse, srcUse };
      s.ctx->copyBuffer(s.resolve(dst), dstOffset, s.resolve(src), srcOffset, size);
    });
  }

  void CsThreadContext::copyImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                                  IBackendImage* src, const ImageSubresource& srcSubres) {
    keep(dst, src);
    BackendGpuUse* dstUse = trackGpuUse(stagingGpuUse(dst));
    BackendGpuUse* srcUse = trackGpuUse(stagingGpuUse(src));
    emit([=](CsWorkerState& s) {
      const GpuUseRelease release{ dstUse, srcUse };
      s.ctx->copyImage(dst, dstSubres, src, srcSubres);
    });
  }

  void CsThreadContext::copyImageRegion(IBackendImage* dst, const ImageSubresource& dstSubres,
                                        uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                                        IBackendImage* src, const ImageSubresource& srcSubres,
                                        const ImageCopyRegion* srcRegion) {
    keep(dst, src);
    const bool hasRegion = srcRegion != nullptr;
    const ImageCopyRegion region = hasRegion ? *srcRegion : ImageCopyRegion{};
    BackendGpuUse* dstUse = trackGpuUse(stagingGpuUse(dst));
    BackendGpuUse* srcUse = trackGpuUse(stagingGpuUse(src));
    emit([=](CsWorkerState& s) {
      const GpuUseRelease release{ dstUse, srcUse };
      s.ctx->copyImageRegion(dst, dstSubres, dstX, dstY, dstZ, src, srcSubres, hasRegion ? &region : nullptr);
    });
  }

  void CsThreadContext::copyBufferToImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                                          IBackendBuffer* src, uint64_t srcOffset,
                                          const ImageCopyRegion* region) {
    keep(dst, src);
    const bool hasRegion = region != nullptr;
    const ImageCopyRegion regionCopy = hasRegion ? *region : ImageCopyRegion{};
    BackendGpuUse* dstUse = trackGpuUse(stagingGpuUse(dst));
    BackendGpuUse* srcUse = trackGpuUse(stagingGpuUse(src));
    emit([=](CsWorkerState& s) {
      const GpuUseRelease release{ dstUse, srcUse };
      s.ctx->copyBufferToImage(dst, dstSubres, s.resolve(src), srcOffset, hasRegion ? &regionCopy : nullptr);
    });
  }

  void CsThreadContext::copyImageToBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                                          IBackendImage* src, const ImageSubresource& srcSubres) {
    keep(dst, src);
    noteBufferGpuWrite(dst);
    BackendGpuUse* dstUse = trackGpuUse(stagingGpuUse(dst));
    BackendGpuUse* srcUse = trackGpuUse(stagingGpuUse(src));
    emit([=](CsWorkerState& s) {
      const GpuUseRelease release{ dstUse, srcUse };
      s.ctx->copyImageToBuffer(s.resolve(dst), dstOffset, src, srcSubres);
    });
  }

  void CsThreadContext::copyStructureCount(IBackendBuffer* dst, uint64_t dstOffset, IBackendBufferView* view) {
    keep(dst, view);
    noteBufferGpuWrite(dst);
    BackendGpuUse* dstUse = trackGpuUse(stagingGpuUse(dst));
    emit([=](CsWorkerState& s) {
      const GpuUseRelease release{ dstUse };
      s.ctx->copyStructureCount(s.resolve(dst), dstOffset, view);
    });
  }

  // ============================================================
  // Mapping and updates
  // ============================================================

  void* CsThreadContext::mapBuffer(IBackendBuffer* buffer, MapMode mode) {
    if (!buffer)
      return nullptr;

    switch (mode) {
      case MapMode::Read:
      case MapMode::ReadWrite:
        // Deferred contexts cannot read back.
        if (m_recording)
          return nullptr;
        // Staging buffers: the caller waited for the commands that use this
        // one (waitForGpuUse), so other queued work need not finish; reading
        // maps the resource and touches no recording state. Other buffers
        // reflect every queued command.
        if (buffer->getDesc().usage != ResourceUsage::Staging)
          waitForQueue();
        return m_real->mapBuffer(buffer, mode);

      case MapMode::WriteDiscard: {
        const BufferDesc& desc = buffer->getDesc();
        if (desc.usage == ResourceUsage::Default || desc.usage == ResourceUsage::Immutable)
          return m_recording ? nullptr : m_real->mapBuffer(buffer, mode);

        // Queued commands may still read the buffer: write a fresh upload
        // buffer and copy it over in order when it is unmapped.
        BufferDesc stagingDesc = {};
        stagingDesc.size      = desc.size;
        stagingDesc.usage     = ResourceUsage::Dynamic;
        stagingDesc.cpuAccess = CpuAccess::Write;
        auto staging = m_device ? m_device->createBuffer(stagingDesc) : nullptr;
        if (!staging)
          return nullptr;

        void* ptr = mapDirect(staging.get(), MapMode::Write);
        if (!ptr)
          return nullptr;

        for (auto& entry : m_stagedMaps) {
          if (entry.target == buffer) {
            entry.staging = std::move(staging);
            entry.size = desc.size;
            return ptr;
          }
        }
        m_stagedMaps.push_back({ buffer, std::move(staging), desc.size });
        return ptr;
      }

      default:
        // Plain writes go to persistently mapped memory the application
        // manages itself (NO_OVERWRITE, freshly created buffers).
        return mapDirect(buffer, mode);
    }
  }

  void* CsThreadContext::mapDirect(IBackendBuffer* buffer, MapMode mode) {
#ifdef _WIN32
    // Writes to CPU-visible memory get its newest slice, as the real context
    // would return; asking it would take its lock, which the worker holds
    // while it records (NO_OVERWRITE appends waited for the worker each).
    if (mode != MapMode::Read && mode != MapMode::ReadWrite)
      if (void* data = static_cast<d3d12::D3D12BackendBuffer*>(buffer)->getAppMappedData())
        return data;
#endif
    if (!m_recording)
      return m_real->mapBuffer(buffer, mode);

#ifdef _WIN32
    auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
    if (void* data = d3d12Buffer->getAppMappedData())
      return data;
    ID3D12Resource* resource = d3d12Buffer->getResource();
    void* data = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    if (!resource || FAILED(resource->Map(0, &readRange, &data)))
      return nullptr;
    return data;
#else
    (void)buffer;
    return nullptr;
#endif
  }

  void CsThreadContext::unmapDirect(IBackendBuffer* buffer) {
    if (!m_recording) {
      m_real->unmapBuffer(buffer);
      return;
    }

#ifdef _WIN32
    auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
    if (d3d12Buffer->getPersistentMappedData())
      return;
    if (ID3D12Resource* resource = d3d12Buffer->getResource())
      resource->Unmap(0, nullptr);
#else
    (void)buffer;
#endif
  }

  void CsThreadContext::unmapBuffer(IBackendBuffer* buffer) {
    // Persistently mapped memory, as renamed dynamic buffers have, needs
    // nothing; only the real context's staged maps of GPU memory do.
    if (m_stagedMaps.empty() && !m_recording && buffer
     && static_cast<d3d12::D3D12BackendBuffer*>(buffer)->isPersistentlyMapped()) {
      // Write-combined memory: the last line of a constant buffer whose
      // size is not whole lines stays in the CPU, partly written, until the
      // next locked instruction stalls on it. Zeros past the data (what
      // D3D11 reads there) complete it.
      auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
      const BufferDesc& desc = d3d12Buffer->getDesc();
      // Other buffers in slices have room up to the slice's size.
      const uint64_t end = desc.dataSize ? desc.dataSize : desc.size;
      if (end & 63) {
        const uint64_t lineEnd = (end + 63) & ~uint64_t(63);
        const uint64_t room = desc.dataSize || !d3d12Buffer->isSuballocated() ? desc.size
          : d3d12Buffer->getSuballocator()->sliceSize(d3d12Buffer->getSizeClass());
        if (lineEnd <= room)
          if (uint8_t* data = d3d12Buffer->getAppMappedData())
            std::memset(data + end, 0, static_cast<size_t>(lineEnd - end));
      }
      return;
    }
    for (size_t i = 0; i < m_stagedMaps.size(); i++) {
      if (m_stagedMaps[i].target != buffer)
        continue;

      StagedMap staged = std::move(m_stagedMaps[i]);
      m_stagedMaps.erase(m_stagedMaps.begin() + i);

      unmapDirect(staged.staging.get());
      keep(staged.target);
      // Commands of command lists run again: keep the references.
      emit([target = staged.target, staging = std::move(staged.staging), size = staged.size](CsWorkerState& s) {
        s.ctx->copyBuffer(s.resolve(target), 0, staging.get(), 0, size);
        s.ctx->retainTemporaryBuffer(staging);
      });
      return;
    }

    unmapDirect(buffer);
  }

  void CsThreadContext::updateBuffer(IBackendBuffer* buffer, uint64_t offset, uint64_t size, const void* data) {
    keep(buffer);
    if (data && size) {
      BufferUpdateRegion region = {};
      region.offset = offset;
      region.size = size;
      region.data = data;
      if (renameConstantBuffer(buffer, &region, 1))
        return;
    }
    if (!data || !size) {
      emit([=](CsWorkerState& s) { s.ctx->updateBuffer(s.resolve(buffer), offset, size, data); });
      return;
    }

    BackendGpuUse* use = trackGpuUse(stagingGpuUse(buffer));
    emitArray(static_cast<const uint8_t*>(data), static_cast<size_t>(size),
      [buffer, offset, use](CsWorkerState& s, const uint8_t* copy, size_t n) {
        const GpuUseRelease release{ use };
        s.ctx->updateBuffer(s.resolve(buffer), offset, n, copy);
      });
  }

  void CsThreadContext::updateBufferRegions(IBackendBuffer* buffer, const BufferUpdateRegion* regions, uint32_t regionCount) {
    if (!buffer || !regions || !regionCount)
      return;
    if (renameConstantBuffer(buffer, regions, regionCount))
      return;
    keep(buffer);

    // One block: the region table followed by the data of every region.
    size_t total = sizeof(BufferUpdateRegion) * regionCount;
    for (uint32_t i = 0; i < regionCount; i++) {
      if (regions[i].data)
        total += static_cast<size_t>((regions[i].size + 7) & ~uint64_t(7));
    }

    std::vector<uint64_t> heapBlock;
    uint8_t* block = nullptr;
    const bool inlineBlock = total <= kMaxInlineData;
    if (inlineBlock) {
      if (m_chunk->remaining() < total + 256)
        submitChunk();
      block = static_cast<uint8_t*>(m_chunk->allocate(total, 8));
    } else {
      heapBlock.resize((total + 7) / 8);
      block = reinterpret_cast<uint8_t*>(heapBlock.data());
    }

    auto* table = reinterpret_cast<BufferUpdateRegion*>(block);
    uint8_t* payload = block + sizeof(BufferUpdateRegion) * regionCount;
    for (uint32_t i = 0; i < regionCount; i++) {
      table[i] = regions[i];
      if (regions[i].data && regions[i].size) {
        std::memcpy(payload, regions[i].data, static_cast<size_t>(regions[i].size));
        table[i].data = payload;
        payload += static_cast<size_t>((regions[i].size + 7) & ~uint64_t(7));
      }
    }

    BackendGpuUse* use = trackGpuUse(stagingGpuUse(buffer));
    if (inlineBlock) {
      emit([buffer, table, regionCount, use](CsWorkerState& s) {
        const GpuUseRelease release{ use };
        s.ctx->updateBufferRegions(s.resolve(buffer), table, regionCount);
      });
    } else {
      emit([buffer, regionCount, use, heapBlock = std::move(heapBlock)](CsWorkerState& s) {
        const GpuUseRelease release{ use };
        s.ctx->updateBufferRegions(s.resolve(buffer),
          reinterpret_cast<const BufferUpdateRegion*>(heapBlock.data()), regionCount);
      });
    }
  }

  void CsThreadContext::retargetBufferViews(IBackendBuffer* buffer, const std::vector<IBackendBufferView*>& views) {
    if (views.empty())
      return;
    keep(buffer);
    for (IBackendBufferView* view : views)
      keep(view);
    emit([buffer, views](CsWorkerState& s) { s.ctx->retargetBufferViews(buffer, views); });
  }

  void CsThreadContext::noteBufferRenamed(const Rc<IBackendBuffer>& previous, const Rc<IBackendBuffer>& current) {
    if (m_recording || !previous || !current)
      return;
    emit([previous, current](CsWorkerState&) { joinBufferIdentity(previous.get(), current); });
  }

  void CsThreadContext::noteListContents(IBackendBuffer* buffer) {
    if (m_recording && buffer)
      m_recordedOwnContents.insert(buffer);
  }

  void CsThreadContext::noteBufferGpuWrite(IBackendBuffer* buffer) {
    auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
    if (!d3d12Buffer || !d3d12Buffer->isRenamableConstantBuffer())
      return;
    if (m_recording)
      d3d12Buffer->blockRename();
    else
      d3d12Buffer->setRenamedData(nullptr);
  }

  bool CsThreadContext::renameConstantBuffer(IBackendBuffer* buffer, const BufferUpdateRegion* regions,
                                             uint32_t regionCount) {
    auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
    if (!d3d12Buffer || !d3d12Buffer->isRenamableConstantBuffer())
      return false;
    if (m_recording) {
      d3d12Buffer->blockRename();
      d3d12Buffer->forgetLastUpdate();
      return false;
    }
    if (d3d12Buffer->renameBlocked()) {
      d3d12Buffer->forgetLastUpdate();
      return false;
    }
    const BufferDesc& desc = d3d12Buffer->getDesc();
    const uint64_t dataSize = desc.dataSize ? desc.dataSize : desc.size;
    // Updated the ordinary way after all: the renamed contents (a partial
    // update's base, the last whole update) are not the buffer's any more.
    auto updateNormally = [d3d12Buffer] {
      d3d12Buffer->setRenamedData(nullptr);
      d3d12Buffer->forgetLastUpdate();
      return false;
    };
    bool whole = false;
    for (uint32_t i = 0; i < regionCount; i++) {
      if (!regions[i].data || !regions[i].size)
        continue;
      if (regions[i].offset >= dataSize)
        return updateNormally();
      whole |= regions[i].offset == 0 && regions[i].size >= dataSize;
    }
    // A partial update builds on the contents of the previous rename.
    uint8_t* previous = d3d12Buffer->getRenamedData();
    // The same data as the last whole update while its slice is still the
    // buffer's: nothing to do. Unreal Engine 4 sends 38% of its constant
    // buffer updates unchanged (Kena: ~300 a frame); each new slice is a new
    // address the GPU's constant caches load again (Kena: 0.15 ms a frame).
    const bool single = whole && regionCount == 1;
    if (single && d3d12Buffer->sameAsLastUpdate(regions[0].data, static_cast<size_t>(dataSize)))
      return true;
    if (!whole && !previous) {
      d3d12Buffer->setRenamedData(nullptr);
      return false;
    }
    auto* real = static_cast<d3d12::D3D12BackendContext*>(m_real.get());
    auto* allocator = &real->getBackendDevice()->getUploadSuballocator();
    if (!m_sliceCache)
      m_sliceCache = std::make_unique<CsSliceCache>();
    if (m_sliceCache->owner && m_sliceCache->owner != allocator)
      return updateNormally();
    m_sliceCache->owner = allocator;
    auto& cache = m_sliceCache->slices[allocator->sizeClassOf(desc.size)];
    if (cache.empty()) {
      d3d12::D3D12UploadSuballocator::Slice batch[kSliceBatch];
      const uint32_t count = allocator->allocateMany(real->getBackendDevice()->getDevice(), desc.size, batch, kSliceBatch);
      if (!count)
        return updateNormally();
      cache.assign(batch, batch + count);
    }
    d3d12::D3D12UploadSuballocator::Slice slice = cache.back();
    cache.pop_back();
    d3d12::D3D12UploadSuballocator::prepareSlice(slice, dataSize, true);
    // Buffers updated in parts keep a copy in cached memory: reading the
    // write-combined slice back is slow.
    uint8_t* shadow = d3d12Buffer->getRenameShadow();
    if (!whole && !shadow) {
      shadow = d3d12Buffer->createRenameShadow(static_cast<size_t>(dataSize));
      std::memcpy(shadow, previous, static_cast<size_t>(dataSize));
    }
    uint8_t* target = shadow ? shadow : slice.mapped;
    for (uint32_t i = 0; i < regionCount; i++) {
      if (!regions[i].data || !regions[i].size)
        continue;
      uint64_t size = regions[i].size;
      if (size > dataSize - regions[i].offset)
        size = dataSize - regions[i].offset;
      std::memcpy(target + regions[i].offset, regions[i].data, static_cast<size_t>(size));
    }
    if (shadow)
      std::memcpy(slice.mapped, shadow, static_cast<size_t>(dataSize));
    // The slice is write-combined memory: a line written in part waits in
    // the CPU until the next locked instruction, which then stalls. The
    // last line is completed with zeros (D3D11 reads zeros past the data).
    const uint64_t lineEnd = (std::min)((dataSize + 63) & ~uint64_t(63), allocator->sliceSize(slice.sizeClass));
    if (lineEnd > dataSize)
      std::memset(slice.mapped + dataSize, 0, static_cast<size_t>(lineEnd - dataSize));
    d3d12Buffer->setRenamedData(slice.mapped);
    if (single)
      d3d12Buffer->noteLastUpdate(regions[0].data, static_cast<size_t>(dataSize), slice.mapped);
    else
      d3d12Buffer->forgetLastUpdate();
    emit([d3d12Buffer, slice, allocator](CsWorkerState& s) {
      static_cast<d3d12::D3D12BackendContext*>(s.ctx)->setBufferSlice(d3d12Buffer, slice, allocator);
    });
    return true;
  }

  void* CsThreadContext::discardBuffer(IBackendBuffer* buffer) {
    // Command lists keep the memory of their own maps: deferred contexts
    // rename instead.
    auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
    if (m_recording || !d3d12Buffer || !d3d12Buffer->isSuballocated() || d3d12Buffer->pinnedContents)
      return nullptr;
    auto* real = static_cast<d3d12::D3D12BackendContext*>(m_real.get());
    auto* suballocator = d3d12Buffer->getSuballocator();
    if (!m_sliceCache)
      m_sliceCache = std::make_unique<CsSliceCache>();
    if (m_sliceCache->owner && m_sliceCache->owner != suballocator)
      return nullptr;
    m_sliceCache->owner = suballocator;
    const BufferDesc& desc = d3d12Buffer->getDesc();
    auto& cache = m_sliceCache->slices[d3d12Buffer->getSizeClass()];
    if (cache.empty()) {
      d3d12::D3D12UploadSuballocator::Slice batch[kSliceBatch];
      const uint32_t count = suballocator->allocateMany(real->getBackendDevice()->getDevice(), desc.size, batch, kSliceBatch);
      if (!count)
        return nullptr;
      cache.assign(batch, batch + count);
    }
    d3d12::D3D12UploadSuballocator::Slice slice = cache.back();
    cache.pop_back();
    d3d12::D3D12UploadSuballocator::prepareSlice(slice, desc.dataSize ? desc.dataSize : desc.size,
      (desc.bindFlags & BindFlags::ConstantBuffer) != BindFlags::None);
    d3d12Buffer->setAppMappedData(slice.mapped);
    emit([d3d12Buffer, slice](CsWorkerState& s) {
      static_cast<d3d12::D3D12BackendContext*>(s.ctx)->setBufferSlice(d3d12Buffer, slice);
    });
    return slice.mapped;
  }

  void CsThreadContext::writeMappedBuffer(IBackendBuffer* buffer, uint64_t offset, const void* data, size_t size,
                                          size_t padded) {
    // The memory the application would write now (a later WRITE_DISCARD
    // gives the buffer other memory; the worker hands this back only after
    // the commands queued before).
    auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
    uint8_t* mapped = d3d12Buffer && !m_recording ? d3d12Buffer->getAppMappedData() : nullptr;
    if (!mapped || !data || !size) {
      IBackendContext::writeMappedBuffer(buffer, offset, data, size, padded);
      return;
    }
    // Written now, where an application writes after Map(NO_OVERWRITE)
    // (mapDirect returns this memory: the commands queued before use other
    // parts of it, or other memory after a discard): no command, no second
    // copy on the worker. DXUP's Direct3D 9 layer writes its constants and
    // streamed vertices this way, hundreds of times a frame.
    uint8_t* target = mapped + offset;
    std::memcpy(target, data, size);
    if (padded > size)
      std::memset(target + size, 0, padded - size);
  }

  void CsThreadContext::writeMappedBufferWith(IBackendBuffer* buffer, uint64_t offset, size_t size, WriteFn fn,
                                              void* user, PackFn pack, void* packUser, size_t dataSize) {
    if (m_recording) {
      IBackendContext::writeMappedBufferWith(buffer, offset, size, fn, user, pack, packUser, dataSize);
      return;
    }
    uint8_t* target = nullptr;
    if (buffer) {
      auto* d3d12Buffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer);
      uint8_t* mapped = d3d12Buffer->getAppMappedData();
      if (!mapped) {
        IBackendContext::writeMappedBufferWith(buffer, offset, size, fn, user, pack, packUser, dataSize);
        return;
      }
      target = mapped + offset;
    }
    emitFilled(dataSize, [pack, packUser, dataSize](uint8_t* data) { if (pack && dataSize) pack(packUser, data); },
      [target, size, fn, user](CsWorkerState&, const uint8_t* data, size_t n) { fn(user, target, target ? size : 0, data, n); });
  }

  void CsThreadContext::retireAfterGpu(std::vector<std::shared_ptr<void>>& objects) {
    if (objects.empty())
      return;
    // The queued commands keep them (a command list as long as it exists:
    // each execution uses them), then the context until the GPU has run
    // the commands.
    auto kept = std::make_shared<std::vector<std::shared_ptr<void>>>(std::move(objects));
    objects.clear();
    emit([kept](CsWorkerState& s) {
      std::vector<std::shared_ptr<void>> retired = *kept;
      s.ctx->retireAfterGpu(retired);
    });
  }

  void CsThreadContext::retainTemporaryBuffer(Rc<IBackendBuffer> buffer) {
    // Slices of upload blocks are not recycled; dropping the reference
    // retires them behind the queued commands and their GPU work.
    if (buffer && static_cast<d3d12::D3D12BackendBuffer*>(buffer.get())->isSuballocated())
      return;
    const uint64_t size = buffer ? buffer->getDesc().size : 0;
    emit([buffer = std::move(buffer)](CsWorkerState& s) { s.ctx->retainTemporaryBuffer(buffer); });
    // In a 32-bit process upload memory is address space: a load must not
    // run hundreds of megabytes of texture uploads ahead of the worker. Two
    // windows of 32 MB at most: the end of the one before must have run.
    constexpr uint64_t kUploadWindowBytes = 32ull << 20;
    if (d3d12::kSmallAddressSpace && !m_recording && (m_uploadWindowBytes += size) >= kUploadWindowBytes) {
      submitChunk();
      if (m_uploadWindowChunk)
        waitForChunk(m_uploadWindowChunk);
      m_uploadWindowChunk = m_chunksSubmitted;
      m_uploadWindowBytes = 0;
    }
  }

  // ============================================================
  // Queries
  // ============================================================

  void CsThreadContext::beginQuery(IBackendQuery* query) {
    if (!query)
      return;
    if (m_recording) {
      keep(query);
      m_recordedQueries.push_back(query);
      emit([=](CsWorkerState& s) {
        const QueryRelease release{ query };
        s.ctx->beginQuery(query);
      });
      return;
    }
    // getData() reports the query as not submitted until the worker
    // recorded this.
    query->queuedOps.fetch_add(1, std::memory_order_relaxed);
    emit([=](CsWorkerState& s) {
      const QueryRelease release{ query };
      s.ctx->beginQuery(query);
    });
  }

  void CsThreadContext::endQuery(IBackendQuery* query) {
    if (!query)
      return;
    if (m_recording) {
      keep(query);
      m_recordedQueries.push_back(query);
      emit([=](CsWorkerState& s) {
        const QueryRelease release{ query };
        s.ctx->endQuery(query);
      });
      return;
    }
    query->queuedOps.fetch_add(1, std::memory_order_relaxed);
    emit([=](CsWorkerState& s) {
      const QueryRelease release{ query };
      s.ctx->endQuery(query);
    });
  }

  void CsThreadContext::setPredication(IBackendQuery* predicate, bool value) {
    keep(predicate);
    emit([=](CsWorkerState& s) { s.ctx->setPredication(predicate, value); });
  }

  void CsThreadContext::signalFence(IUnknown* fence, uint64_t value) {
    if (!fence)
      return;
    fence->AddRef();
    emit([=](CsWorkerState& s) {
      s.ctx->signalFence(fence, value);
      fence->Release();
    });
    submitChunk();
  }

  void CsThreadContext::waitFence(IUnknown* fence, uint64_t value) {
    if (!fence)
      return;
    fence->AddRef();
    emit([=](CsWorkerState& s) {
      s.ctx->waitFence(fence, value);
      fence->Release();
    });
    submitChunk();
  }

  void CsThreadContext::notifyOnCompletion(void* event) {
    emit([=](CsWorkerState& s) { s.ctx->notifyOnCompletion(event); });
    submitChunk();
  }

  void CsThreadContext::updateTileMappings(const TileMappingUpdate& update) {
    keepTiled(update.resource);
    keep(update.tilePool);
    emit([update](CsWorkerState& s) { s.ctx->updateTileMappings(update); });
  }

  void CsThreadContext::copyTileMappings(TiledResourceRef dst, const TileCoordinate& dstCoordinate,
                                         TiledResourceRef src, const TileCoordinate& srcCoordinate,
                                         const TileRegionSize& size, uint32_t flags) {
    keepTiled(dst);
    keepTiled(src);
    emit([=](CsWorkerState& s) { s.ctx->copyTileMappings(dst, dstCoordinate, src, srcCoordinate, size, flags); });
  }

  void CsThreadContext::copyTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                                  IBackendBuffer* buffer, uint64_t bufferOffset, uint32_t flags) {
    keepTiled(resource);
    keep(buffer);
    BackendGpuUse* bufferUse = trackGpuUse(stagingGpuUse(buffer));
    emit([=](CsWorkerState& s) {
      const GpuUseRelease release{ bufferUse };
      s.ctx->copyTiles(resource, coordinate, size, buffer, bufferOffset, flags);
    });
  }

  void CsThreadContext::updateTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                                    const void* data, uint32_t flags) {
    const uint32_t tiles = size.useBox ? size.width * size.height * size.depth : size.numTiles;
    if (!data || !tiles)
      return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> copy(bytes, bytes + size_t(tiles) * kTileSizeInBytes);
    keepTiled(resource);
    emit([resource, coordinate, size, flags, copy = std::move(copy)](CsWorkerState& s) {
      s.ctx->updateTiles(resource, coordinate, size, copy.data(), flags);
    });
  }

  bool CsThreadContext::resizeTilePool(IBackendBuffer* tilePool, uint64_t newSize) {
    // The heap is created here so failures reach the application; the
    // worker adds it in order with the mappings that use it.
    auto* pool = static_cast<d3d12::D3D12BackendBuffer*>(tilePool);
    auto* device = static_cast<d3d12::D3D12BackendDevice*>(m_device);
    if (!pool || !device)
      return false;
    d3d12::D3D12BackendBuffer::TileHeap heap;
    HRESULT hr = pool->createTileHeap(device->getDevice(), newSize, heap);
    if (FAILED(hr))
      return false;
    if (hr == S_OK) {
      keep(tilePool);
      auto shared = std::make_shared<d3d12::D3D12BackendBuffer::TileHeap>(std::move(heap));
      emit([pool, shared](CsWorkerState&) { pool->appendTileHeap(*shared); });
    }
    return true;
  }

  void CsThreadContext::tiledResourceBarrier() {
    emit([](CsWorkerState& s) { s.ctx->tiledResourceBarrier(); });
  }

  void CsThreadContext::setImageMinLod(IBackendImage* image, float minLod) {
    keep(image);
    emit([=](CsWorkerState& s) { s.ctx->setImageMinLod(image, minLod); });
  }

  void CsThreadContext::suspendPredication(bool suspended) {
    emit([=](CsWorkerState& s) { s.ctx->suspendPredication(suspended); });
  }

  // ============================================================
  // Synchronization and submission
  // ============================================================

  void CsThreadContext::flush() {
    // Flush does nothing on deferred contexts.
    if (m_recording)
      return;
    emit([](CsWorkerState& s) { s.ctx->flush(); });
    submitChunk();
    m_flushChunk = m_chunksSubmitted;
  }

  // Whether a flush the worker has yet to run submits everything recorded
  // so far: polling Map or GetData then queues no flush of its own.
  bool CsThreadContext::flushQueued() const {
    return m_chunk->empty() && m_flushChunk == m_chunksSubmitted
        && m_chunksExecuted.load(std::memory_order_acquire) < m_flushChunk;
  }

  bool CsThreadContext::waitForGpuUse(BackendGpuUse& use, bool doNotWait) {
    HitchScope hitch("app waitForGpuUse");
    if (m_recording)
      return true;
    auto* real = static_cast<d3d12::D3D12BackendContext*>(m_real.get());
    if (use.queuedOps.load(std::memory_order_acquire) == 0) {
      const uint64_t value = use.fenceValue.load(std::memory_order_acquire);
      if (!value || real->completedFenceValue() >= value)
        return true;
      if (doNotWait) {
        // Submit the work so that it finishes eventually.
        if (real->submittedFenceValue() < value && !flushQueued())
          flush();
        return false;
      }
    } else if (doNotWait) {
      if (!flushQueued())
        flush();
      return false;
    }
    waitForQueue();
    return m_real->waitForGpuUse(use, false);
  }

  bool CsThreadContext::renameStagingImage(IBackendImage* image) {
    if (m_recording || !image)
      return false;
    // Copies the worker has yet to record still read the current memory.
    if (image->gpuUse.queuedOps.load(std::memory_order_acquire))
      waitForWorker();
    return m_real->renameStagingImage(image);
  }

  void CsThreadContext::synchronize() {
    HitchScope hitch("app synchronize");
    if (m_recording)
      return;
    waitForQueue();
    m_real->synchronize();
  }

  void CsThreadContext::closeForSubmission() {
    if (m_recording)
      return;
    waitForQueue();
    m_real->closeForSubmission();
  }

  void CsThreadContext::submitRecordedCommands() {
    if (m_recording)
      return;
    waitForQueue();
    m_real->submitRecordedCommands();
  }

  void CsThreadContext::presentImage(IBackendImage* backBuffer) {
    emit([=](CsWorkerState& s) { s.ctx->presentImage(backBuffer); });
    submitChunk();
  }

  void CsThreadContext::endFrame() {
    const uint32_t slot = uint32_t(m_presents % kPresentHistory);
    emit([this, slot](CsWorkerState& s) {
      auto* ctx = static_cast<d3d12::D3D12BackendContext*>(s.ctx);
      m_presentFences[slot] = ctx->getLastPresentFenceValue();
      m_presentIndices[slot] = ctx->getPresentCount() ? ctx->getPresentCount() - 1 : UINT64_MAX;
      m_framesExecuted.fetch_add(1, std::memory_order_release);
      SetEvent(m_frameEvent);
    });
    submitChunk();
    m_presents++;
  }

  void CsThreadContext::waitForQueue() {
    if (m_recording)
      return;
    waitForWorker();
    m_real->waitForQueue();
  }

  void CsThreadContext::waitForFrames(uint64_t frames) {
    for (uint32_t i = 0; i < kSyncSpinCount; i++) {
      if (m_framesExecuted.load(std::memory_order_acquire) >= frames)
        return;
      YieldProcessor();
    }
    // The worker presents, and a present can wait for the window's thread,
    // which may be this one.
    while (m_framesExecuted.load(std::memory_order_acquire) < frames && !processExiting())
      waitTakingSentMessages(m_frameEvent, 100);
  }

  void CsThreadContext::waitForFrameLatency(uint32_t maxLatency) {
    LARGE_INTEGER entry = {};
    if (m_lowLatency)
      QueryPerformanceCounter(&entry);
    // The worker may still record the frame just presented, but not the
    // one before: the application runs at most a frame ahead of it.
    if (m_presents < 2) {
      if (m_lowLatency)
        paceFrame(maxLatency, entry.QuadPart);
      return;
    }
    {
      HitchScope hitch("app waitForPreviousFrame");
      waitForFrames(m_presents - 1);
    }
    // As a native Present: the frame presented maxLatency frames before the
    // latest must be done on the GPU.
    maxLatency = (std::min)(maxLatency, kPresentHistory - 2);
    if (maxLatency && m_presents > maxLatency) {
      const uint64_t frame = m_presents - 1 - maxLatency;
      HitchScope hitch("app waitForFrameLatency");
      static_cast<d3d12::D3D12BackendContext*>(m_real.get())->waitForFenceValue(m_presentFences[frame % kPresentHistory]);
    }
    if (m_lowLatency)
      paceFrame(maxLatency, entry.QuadPart);
  }

  void CsThreadContext::paceFrame(uint32_t maxLatency, int64_t entry) {
    auto* real = static_cast<d3d12::D3D12BackendContext*>(m_real.get());
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / double(frequency.QuadPart);
    if (m_pacingReturn)
      m_latencyPacer.noteCpuFrame(double(entry - m_pacingReturn) * msPerTick);
    // The frames whose GPU work is done (or not counted: frameGpuTiming
    // tells), all of which the worker has finished.
    const uint64_t done = (std::min)(m_presents > maxLatency ? m_presents - maxLatency : 0,
      m_framesExecuted.load(std::memory_order_acquire));
    for (; m_pacedFrame < done; m_pacedFrame++) {
      double busyMs = 0.0, lateMs = 0.0;
      const uint64_t index = m_presentIndices[m_pacedFrame % kPresentHistory];
      if (m_pacedFrame + kPresentHistory > m_presents && index != UINT64_MAX
       && real->frameGpuTiming(index, busyMs, lateMs))
        m_latencyPacer.noteGpuFrame(busyMs, lateMs);
    }
    const double sleepMs = m_latencyPacer.sleepMs();
    if (sleepMs > 0.0) {
      HitchScope hitch("app lowLatency sleep");
      d3d12::paceSleep(m_pacingTimer, sleepMs, [](HANDLE timer, DWORD timeout) { waitTakingSentMessages(timer, timeout); });
    }
    m_pacingSleptMs += sleepMs;
    if (++m_pacingFrames == 1000) {
      logMessage(LogLevel::Info, "low latency: held back %.3f ms a frame, margin %.2f ms, %llu frames late so far",
        m_pacingSleptMs / m_pacingFrames, m_latencyPacer.marginMs(),
        static_cast<unsigned long long>(m_latencyPacer.lateFrames()));
      m_pacingSleptMs = 0.0;
      m_pacingFrames = 0;
    }
    QueryPerformanceCounter(&now);
    m_pacingReturn = now.QuadPart;
  }

  void CsThreadContext::executeOnBackend(std::function<void(IBackendContext*)> fn) {
    if (m_recording) {
      emit([fn = std::move(fn)](CsWorkerState& s) { fn(s.ctx); });
      return;
    }
    emit([this, fn = std::move(fn)](CsWorkerState& s) {
      // DXGI's Present runs in these and can wait for the window's thread:
      // threads waiting for the worker take sent messages meanwhile.
      struct InCallback {
        std::atomic<bool>& flag;
        explicit InCallback(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_seq_cst); }
        ~InCallback() { flag.store(false, std::memory_order_release); }
      } inCallback(m_workerInCallback);
      {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        for (const SyncWaiter& waiter : m_syncWaiters)
          SetEvent(waiter.event);
      }
      fn(s.ctx);
    });
    submitChunk();
  }

  Rc<IBackendCommandList> CsThreadContext::finishCommandList() {
    if (!m_recording)
      return nullptr;

    submitChunk();
    auto list = std::make_shared<CsCommandList>(
      std::move(m_recordedChunks), std::move(m_recordedQueries), std::move(m_recordedGpuUses),
      std::move(m_recordedObjects), std::move(m_recordedOwnContents), m_retireQueue);
    m_recordedOwnContents.clear();
    m_recordedChunks.clear();
    m_recordedQueries.clear();
    m_recordedGpuUses.clear();
    m_recordedObjects.clear();
    nextRecording();
    m_stagedMaps.clear();
    return list;
  }

  void CsThreadContext::executeCommandList(const Rc<IBackendCommandList>& list) {
    auto commands = std::static_pointer_cast<CsCommandList>(list);
    if (!commands)
      return;

    if (m_recording) {
      // Nested command list: its queries count for the outer one.
      for (IBackendQuery* query : commands->queries())
        m_recordedQueries.push_back(query);
      for (BackendGpuUse* use : commands->gpuUses())
        m_recordedGpuUses.push_back(use);
    } else {
      // getData() and Map wait until the worker recorded these commands.
      for (IBackendQuery* query : commands->queries())
        query->queuedOps.fetch_add(1, std::memory_order_relaxed);
      for (BackendGpuUse* use : commands->gpuUses())
        use->queuedOps.fetch_add(1, std::memory_order_relaxed);
    }

    emit([commands](CsWorkerState& s) {
      s.resetOutputMerger();
      s.ctx->clearState();
      commands->replay(s);
    });
  }

  // ============================================================
  // Clears
  // ============================================================

  void CsThreadContext::clearRenderTarget(IBackendImageView* rtv, const float color[4]) {
    keep(rtv);
    std::array<float, 4> copy = { color[0], color[1], color[2], color[3] };
    emit([=](CsWorkerState& s) { s.ctx->clearRenderTarget(rtv, copy.data()); });
  }

  void CsThreadContext::clearDepthStencil(IBackendImageView* dsv, float depth, uint8_t stencil, uint32_t flags) {
    keep(dsv);
    emit([=](CsWorkerState& s) { s.ctx->clearDepthStencil(dsv, depth, stencil, flags); });
  }

  void CsThreadContext::clearUnorderedAccessViewUint(IBackendImageView* view, const uint32_t values[4]) {
    keep(view);
    std::array<uint32_t, 4> copy = { values[0], values[1], values[2], values[3] };
    emit([=](CsWorkerState& s) { s.ctx->clearUnorderedAccessViewUint(view, copy.data()); });
  }

  void CsThreadContext::clearUnorderedAccessViewUint(IBackendBufferView* view, const uint32_t values[4]) {
    keep(view);
    std::array<uint32_t, 4> copy = { values[0], values[1], values[2], values[3] };
    emit([=](CsWorkerState& s) { s.ctx->clearUnorderedAccessViewUint(view, copy.data()); });
  }

  void CsThreadContext::clearUnorderedAccessViewFloat(IBackendImageView* view, const float values[4]) {
    keep(view);
    std::array<float, 4> copy = { values[0], values[1], values[2], values[3] };
    emit([=](CsWorkerState& s) { s.ctx->clearUnorderedAccessViewFloat(view, copy.data()); });
  }

  void CsThreadContext::clearUnorderedAccessViewFloat(IBackendBufferView* view, const float values[4]) {
    keep(view);
    std::array<float, 4> copy = { values[0], values[1], values[2], values[3] };
    emit([=](CsWorkerState& s) { s.ctx->clearUnorderedAccessViewFloat(view, copy.data()); });
  }

  void CsThreadContext::clearBufferView(IBackendBufferView* view, const float color[4], uint32_t rectCount,
                                        const Rect* rects) {
    keep(view);
    std::array<float, 4> colorCopy = { color[0], color[1], color[2], color[3] };
    emitArray(rects, rectCount, [view, colorCopy](CsWorkerState& s, const Rect* data, size_t count) {
      s.ctx->clearBufferView(view, colorCopy.data(), static_cast<uint32_t>(count), data);
    });
  }

  void CsThreadContext::clearView(IBackendImageView* view, bool renderTarget, const float color[4],
                                  uint32_t rectCount, const Rect* rects) {
    keep(view);
    std::array<float, 4> colorCopy = { color[0], color[1], color[2], color[3] };
    emitArray(rects, rectCount, [view, renderTarget, colorCopy](CsWorkerState& s, const Rect* data, size_t count) {
      s.ctx->clearView(view, renderTarget, colorCopy.data(), static_cast<uint32_t>(count), data);
    });
  }

  // ============================================================
  // Misc
  // ============================================================

  void CsThreadContext::generateMips(IBackendImageView* srv) {
    keep(srv);
    emit([=](CsWorkerState& s) { s.ctx->generateMips(srv); });
  }

  void CsThreadContext::resolveSubresource(IBackendImage* dst, uint32_t dstSubres,
                                           IBackendImage* src, uint32_t srcSubres, DXGI_FORMAT format) {
    keep(dst, src);
    emit([=](CsWorkerState& s) { s.ctx->resolveSubresource(dst, dstSubres, src, srcSubres, format); });
  }

  void CsThreadContext::upsampleImage(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format,
                                      DXGI_FORMAT readFormat) {
    keep(dst, src);
    emit([=](CsWorkerState& s) { s.ctx->upsampleImage(dst, src, format, readFormat); });
  }

  void CsThreadContext::resolveSampleZero(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format,
                                     DXGI_FORMAT readFormat) {
    keep(dst, src);
    emit([=](CsWorkerState& s) { s.ctx->resolveSampleZero(dst, src, format, readFormat); });
  }

  void CsThreadContext::resolveFinite(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format) {
    keep(dst, src);
    emit([=](CsWorkerState& s) { s.ctx->resolveFinite(dst, src, format); });
  }

  void CsThreadContext::copyStencil(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) {
    keep(dst, src);
    emit([=](CsWorkerState& s) { s.ctx->copyStencil(dst, src, format, readFormat); });
  }

}
