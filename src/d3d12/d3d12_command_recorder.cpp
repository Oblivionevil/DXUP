#include "d3d12_command_recorder.h"
#include "../util/util_types.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32

namespace dxup::d3d12 {

  namespace {

    // Like the CS worker: calls arrive in bursts, so the record thread
    // polls a while before it sleeps, and the worker before it waits for
    // the record thread.
    constexpr int64_t kRecordSpinMicroseconds = 250;
    constexpr int64_t kDrainSpinMicroseconds = 50;

    int64_t qpcNow() {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      return now.QuadPart;
    }

    int64_t qpcTicks(int64_t microseconds) {
      static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
      }();
      return frequency * microseconds / 1000000;
    }

  }

  CommandListRecorder::~CommandListRecorder() {
    stopThread();
  }

  void CommandListRecorder::setList(ComPtr<ID3D12GraphicsCommandList> list) {
    drain();
    m_list = std::move(list);
    m_list2 = nullptr;
    if (m_list)
      m_list.As(&m_list2);
  }

  void CommandListRecorder::startThread() {
    if (m_deferred || !m_list)
      return;
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_drainedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    try {
      if (!m_wakeEvent || !m_drainedEvent)
        throw std::runtime_error("CreateEvent");
      m_writeBlock = allocateBlock(kBlockSize);
      m_readBlock = m_writeBlock;
      m_writePos = 0;
      m_readPos = 0;
      m_produced = 0;
      m_unpublishedBytes = 0;
      m_consumedLocal = 0;
      m_blocksAhead = 0;
      m_published.store(0, std::memory_order_relaxed);
      m_consumed.store(0, std::memory_order_relaxed);
      m_blocksReleased.store(0, std::memory_order_relaxed);
      m_stopping.store(false, std::memory_order_relaxed);
      m_thread = dxup::Thread([this] { threadMain(); });
      m_deferred = true;
    } catch (const std::exception& e) {
      logMessage(LogLevel::Warn, "record thread not started (%s): command lists recorded directly", e.what());
      std::free(m_writeBlock);
      m_writeBlock = nullptr;
      m_readBlock = nullptr;
      if (m_wakeEvent)
        CloseHandle(m_wakeEvent);
      if (m_drainedEvent)
        CloseHandle(m_drainedEvent);
      m_wakeEvent = nullptr;
      m_drainedEvent = nullptr;
    }
  }

  void CommandListRecorder::stopThread() {
    if (!m_deferred)
      return;
    // At process exit the record thread is gone already.
    if (processExiting()) {
      m_deferred = false;
      m_thread.detach();
      return;
    }
    drain();
    m_deferred = false;
    m_stopping.store(true, std::memory_order_seq_cst);
    SetEvent(m_wakeEvent);
    m_thread.join();

    std::free(m_writeBlock);
    m_writeBlock = nullptr;
    m_readBlock = nullptr;
    for (Block* block : m_freeBlocks)
      std::free(block);
    m_freeBlocks.clear();
    CloseHandle(m_wakeEvent);
    CloseHandle(m_drainedEvent);
    m_wakeEvent = nullptr;
    m_drainedEvent = nullptr;
  }

  CommandListRecorder::Block* CommandListRecorder::allocateBlock(size_t size) {
    const size_t capacity = (std::max)(kBlockSize, size);
    Block* block = nullptr;
    if (capacity == kBlockSize) {
      std::lock_guard<std::mutex> lock(m_freeMutex);
      if (!m_freeBlocks.empty()) {
        block = m_freeBlocks.back();
        m_freeBlocks.pop_back();
      }
    }
    if (!block) {
      block = static_cast<Block*>(std::malloc(sizeof(Block) + capacity));
      if (!block)
        throw std::bad_alloc();
      block->capacity = capacity;
    }
    block->next = nullptr;
    return block;
  }

  __declspec(noinline) void CommandListRecorder::releaseBlock(Block* block) {
    bool pooled = false;
    if (block->capacity == kBlockSize) {
      // Out of memory the block is freed: the record thread must not throw
      // (replayUntil would take it for a faulting call).
      try {
        std::lock_guard<std::mutex> lock(m_freeMutex);
        m_freeBlocks.push_back(block);
        pooled = true;
      } catch (const std::exception&) {
      }
    }
    if (!pooled)
      std::free(block);
    m_blocksReleased.store(m_blocksReleased.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  void CommandListRecorder::nextBlock(size_t size) {
    // The record thread is far behind: wait instead of taking more memory.
    if (m_blocksAhead - m_blocksReleased.load(std::memory_order_acquire) >= kMaxBlocksAhead)
      drain();
    Block* block = allocateBlock(size + sizeof(Command));
    m_writeBlock->next = block;
    Command* end = reinterpret_cast<Command*>(m_writeBlock->data() + m_writePos);
    end->replay = nullptr;
    end->size = 0;
    // Published with the calls that follow.
    m_produced++;
    m_blocksAhead++;
    m_writeBlock = block;
    m_writePos = 0;
  }

  void CommandListRecorder::logResetFailure(HRESULT hr) {
    if (FAILED(hr))
      logMessage(LogLevel::Error, "record thread: command list or allocator Reset failed (hr=0x%08x)", static_cast<unsigned>(hr));
  }

  void CommandListRecorder::wake() {
    if (m_sleeping.exchange(false, std::memory_order_acq_rel))
      SetEvent(m_wakeEvent);
  }

  void CommandListRecorder::waitForRecordThread() {
    if (processExiting())
      return;
    m_unpublishedBytes = 0;
    m_published.store(m_produced, std::memory_order_release);
    // publish() may have missed that the record thread went to sleep.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (m_sleeping.load(std::memory_order_relaxed))
      wake();

    const int64_t start = qpcNow();
    const int64_t spinTicks = qpcTicks(kDrainSpinMicroseconds);
    for (uint32_t i = 1; m_consumed.load(std::memory_order_acquire) != m_produced; i++) {
      YieldProcessor();
      if (!(i & 63) && qpcNow() - start > spinTicks)
        break;
    }
    while (m_consumed.load(std::memory_order_acquire) != m_produced) {
      m_drainWaiting.store(true, std::memory_order_seq_cst);
      if (m_consumed.load(std::memory_order_seq_cst) != m_produced)
        WaitForSingleObject(m_drainedEvent, INFINITE);
      m_drainWaiting.store(false, std::memory_order_relaxed);
    }
  }

  bool CommandListRecorder::replayUntil(uint32_t target) {
    __try {
      while (m_consumedLocal != target) {
        const Command* cmd = reinterpret_cast<const Command*>(m_readBlock->data() + m_readPos);
        if (!cmd->replay) {
          Block* done = m_readBlock;
          m_readBlock = done->next;
          m_readPos = 0;
          releaseBlock(done);
        } else {
          cmd->replay(m_list.Get(), cmd);
          m_readPos += cmd->size;
        }
        m_consumedLocal++;
      }
      return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // The call that raised it is skipped, as the safe* wrappers of
      // direct recording do.
      const Command* cmd = reinterpret_cast<const Command*>(m_readBlock->data() + m_readPos);
      logMessage(LogLevel::Error, "record thread: call %p CRASHED (SEH=0x%08X); skipped",
        reinterpret_cast<void*>(cmd->replay), GetExceptionCode());
      m_readPos += cmd->size;
      m_consumedLocal++;
      return false;
    }
  }

  void CommandListRecorder::threadMain() {
    SetThreadDescription(GetCurrentThread(), L"dxup-record");
    for (;;) {
      uint32_t target = m_published.load(std::memory_order_acquire);
      if (target == m_consumedLocal) {
        const int64_t start = qpcNow();
        const int64_t spinTicks = qpcTicks(kRecordSpinMicroseconds);
        for (uint32_t i = 1; (target = m_published.load(std::memory_order_acquire)) == m_consumedLocal; i++) {
          YieldProcessor();
          if (!(i & 63) && qpcNow() - start > spinTicks)
            break;
        }
      }
      if (target == m_consumedLocal) {
        m_sleeping.store(true, std::memory_order_seq_cst);
        target = m_published.load(std::memory_order_seq_cst);
        if (target == m_consumedLocal) {
          if (m_stopping.load(std::memory_order_seq_cst))
            return;
          WaitForSingleObject(m_wakeEvent, INFINITE);
        }
        m_sleeping.store(false, std::memory_order_relaxed);
        continue;
      }

      while (!replayUntil(target)) { }
      m_consumed.store(m_consumedLocal, std::memory_order_seq_cst);
      if (m_drainWaiting.load(std::memory_order_seq_cst))
        SetEvent(m_drainedEvent);
    }
  }

}

#endif
