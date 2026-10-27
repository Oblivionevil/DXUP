// The D3D9 worker thread (see d3d9_worker.h).

#include "d3d9_worker.h"

#include "../util/util_types.h"

#include <exception>

namespace dxup::d3d9 {

  namespace {
    // Spins before the worker sleeps (a game records the next call within
    // microseconds), and before the application's thread waits.
    constexpr int64_t  kWorkerSpinMicroseconds = 250;
    constexpr uint32_t kWaitSpinCount = 4096;
    // Chunks kept for reuse; more are freed.
    constexpr size_t   kMaxFreeChunks = 64;

    int64_t microsecondsNow() {
      static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
      }();
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      return now.QuadPart * 1000000 / frequency;
    }

    // Takes the messages sent to this thread, then waits for the event at
    // most timeout ms or until another message is sent (callers loop).
    // Posted messages stay for the application.
    void waitTakingSentMessages(HANDLE event, DWORD timeout) {
      MSG msg;
      PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
      MsgWaitForMultipleObjectsEx(1, &event, timeout, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
    }
  }

  Worker::Worker(legacy::Device* core) : m_core(core) {
    m_doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_chunk = new Chunk();
    m_begin = m_pos = m_chunk->data;
    m_end = m_chunk->data + kChunkSize;
    m_thread = dxup::Thread([this] { workerMain(); });
  }

  Worker::~Worker() {
    if (processExiting()) {
      // The thread is gone with the others; what it held stays.
      m_thread.detach();
      return;
    }
    synchronize();
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_wake.notify_one();
    m_thread.join();
    // Released while the worker made the last calls: deleted here.
    for (const auto& entry : m_retired)
      entry.first(entry.second);
    for (Chunk* chunk : m_freeChunks)
      delete chunk;
    delete m_chunk;
    if (m_doneEvent)
      CloseHandle(m_doneEvent);
  }

  uint8_t* Worker::reserve(uint32_t size) {
    if (size > kChunkSize) {
      logMessage(LogLevel::Error, "D3D9 worker: a command of %u bytes does not fit a chunk", size);
      std::terminate();
    }
    submitChunk();
    return m_pos;
  }

  void Worker::submitChunk() {
    if (m_pos == m_begin)
      return;
    m_chunk->used = m_pos;
    Chunk* next = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.push_back(m_chunk);
      if (!m_freeChunks.empty()) {
        next = m_freeChunks.back();
        m_freeChunks.pop_back();
      }
    }
    m_queued.fetch_add(1, std::memory_order_release);
    m_wake.notify_one();
    m_submitted++;
    if (!next)
      next = new Chunk();
    m_chunk = next;
    m_begin = m_pos = next->data;
    m_end = next->data + kChunkSize;
  }

  void Worker::retire(void (*destroy)(void*), void* object) {
    std::lock_guard<std::mutex> lock(m_retireMutex);
    m_retired.emplace_back(destroy, object);
    m_anyRetired.store(true, std::memory_order_relaxed);
  }

  void Worker::recordRetired() {
    if (!m_anyRetired.load(std::memory_order_relaxed))
      return;
    std::vector<std::pair<void (*)(void*), void*>> retired;
    {
      std::lock_guard<std::mutex> lock(m_retireMutex);
      retired.swap(m_retired);
      m_anyRetired.store(false, std::memory_order_relaxed);
    }
    if (retired.empty())
      return;
    emit([retired = std::move(retired)](legacy::Device&) {
      for (const auto& entry : retired)
        entry.first(entry.second);
    });
  }

  void Worker::submit() {
    recordRetired();
    submitChunk();
  }

  template<typename Done>
  void Worker::waitFor(Done done) {
    if (done())
      return;
    for (uint32_t i = 0; i < kWaitSpinCount; i++) {
      YieldProcessor();
      if (done())
        return;
    }
    for (;;) {
      m_waiting.store(true, std::memory_order_seq_cst);
      if (done() || processExiting())
        break;
      waitTakingSentMessages(m_doneEvent, 100);
    }
    m_waiting.store(false, std::memory_order_relaxed);
  }

  void Worker::synchronize() {
    submit();
    const uint64_t target = m_submitted;
    waitFor([&] { return m_executed.load(std::memory_order_seq_cst) >= target; });
  }

  void Worker::endFrame() {
    const uint64_t frame = ++m_framesRecorded;
    emit([this, frame](legacy::Device&) { m_framesDone.store(frame, std::memory_order_seq_cst); });
    submit();
    waitFor([&] { return m_framesDone.load(std::memory_order_seq_cst) + 1 >= frame; });
  }

  Worker::Chunk* Worker::takeChunk() {
    if (m_queued.load(std::memory_order_acquire) == 0) {
      const int64_t until = microsecondsNow() + kWorkerSpinMicroseconds;
      for (uint32_t i = 0; m_queued.load(std::memory_order_acquire) == 0; i++) {
        YieldProcessor();
        if ((i & 63) == 63 && microsecondsNow() >= until)
          break;
      }
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_wake.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
    if (m_queue.empty())
      return nullptr;
    Chunk* chunk = m_queue.front();
    m_queue.pop_front();
    m_queued.fetch_sub(1, std::memory_order_relaxed);
    return chunk;
  }

  void Worker::execute(Chunk* chunk) {
    static std::atomic<uint32_t> s_failures{0};
    for (uint8_t* at = chunk->data; at < chunk->used;) {
      auto* command = reinterpret_cast<Command*>(at);
      const uint32_t size = command->size;
      // A failing call is skipped: the application waits for the worker.
      try {
        command->invoke(command, *m_core);
      } catch (const std::exception& e) {
        if (s_failures.fetch_add(1) < 8)
          logMessage(LogLevel::Error, "D3D9 worker: a call failed: %s", e.what());
      } catch (...) {
        if (s_failures.fetch_add(1) < 8)
          logMessage(LogLevel::Error, "D3D9 worker: a call failed");
      }
      at += size;
    }
  }

  void Worker::freeChunk(Chunk* chunk) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_freeChunks.size() < kMaxFreeChunks) {
        m_freeChunks.push_back(chunk);
        return;
      }
    }
    delete chunk;
  }

  void Worker::workerMain() {
    SetThreadDescription(GetCurrentThread(), L"dxup-d3d9");
    while (Chunk* chunk = takeChunk()) {
      execute(chunk);
      freeChunk(chunk);
      m_executed.fetch_add(1, std::memory_order_seq_cst);
      if (m_waiting.load(std::memory_order_seq_cst))
        SetEvent(m_doneEvent);
    }
  }

}
