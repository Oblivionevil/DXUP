// The D3D9 worker thread (see d3d9_worker.h).

#include "d3d9_worker.h"

#include "../util/util_config.h"
#include "../util/util_types.h"

#include <algorithm>
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
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_chunk = new Chunk();
    m_queue.push_back(m_chunk);
    m_chunksStarted = 1;
    m_begin = m_pos = m_chunk->data;
    m_end = m_begin + kPublishBytes;
    m_thread = dxup::Thread([this] { workerMain(); });
  }

  Worker::~Worker() {
    if (processExiting()) {
      // The thread is gone with the others; what it held stays.
      m_thread.detach();
      return;
    }
    synchronize();
    m_stopping.store(true, std::memory_order_seq_cst);
    SetEvent(m_wakeEvent);
    m_thread.join();
    // Released while the worker made the last calls: deleted here.
    for (const auto& entry : m_retired)
      entry.first(entry.second);
    for (Chunk* chunk : m_freeChunks)
      delete chunk;
    // The one being recorded is the worker's after it made the others.
    if (std::find(m_queue.begin(), m_queue.end(), m_chunk) == m_queue.end())
      delete m_chunk;
    for (Chunk* chunk : m_queue)
      delete chunk;
    if (m_doneEvent)
      CloseHandle(m_doneEvent);
    if (m_wakeEvent)
      CloseHandle(m_wakeEvent);
  }

  uint8_t* Worker::reserve(uint32_t size) {
    if (size > kChunkSize) {
      logMessage(LogLevel::Error, "D3D9 worker: a command of %u bytes does not fit a chunk", size);
      std::terminate();
    }
    if (size_t(m_begin + kChunkSize - m_pos) < size)
      nextChunk();
    else
      publish();
    const size_t room = size_t(m_begin + kChunkSize - m_pos);
    m_end = m_pos + (std::min)(room, (std::max)(size_t(size), kPublishBytes));
    return m_pos;
  }

  void Worker::wakeWorker() {
    if (m_workerSleeping.load(std::memory_order_seq_cst))
      SetEvent(m_wakeEvent);
  }

  void Worker::publish() {
    const uint32_t bytes = uint32_t(m_pos - m_begin);
    if (m_chunk->published.load(std::memory_order_relaxed) == bytes)
      return;
    m_chunk->published.store(bytes, std::memory_order_seq_cst);
    wakeWorker();
  }

  void Worker::nextChunk() {
    Chunk* next = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_freeChunks.empty()) {
        next = m_freeChunks.back();
        m_freeChunks.pop_back();
      }
    }
    if (!next)
      next = new Chunk();
    next->published.store(0, std::memory_order_relaxed);
    next->consumed.store(0, std::memory_order_relaxed);
    // Queued before this one is sealed: the worker finds it when done.
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.push_back(next);
    }
    m_chunksStarted++;
    m_chunk->published.store(uint32_t(m_pos - m_begin) | kSealed, std::memory_order_seq_cst);
    wakeWorker();
    m_chunk = next;
    m_begin = m_pos = next->data;
    m_end = m_begin + kPublishBytes;
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

  bool Worker::caughtUp() const {
    return m_chunksFinished.load(std::memory_order_seq_cst) + 1 == m_chunksStarted
        && m_chunk->consumed.load(std::memory_order_seq_cst) == uint32_t(m_pos - m_begin);
  }

  void Worker::synchronize() {
    recordRetired();
    publish();
    // Calls recorded during the wait (window procedures it gives sent
    // messages) are published and waited for too.
    waitFor([&] {
      publish();
      return caughtUp();
    });
  }

  void Worker::endFrame() {
    const uint64_t frame = ++m_framesRecorded;
    emit([this, frame](legacy::Device&) { m_framesDone.store(frame, std::memory_order_seq_cst); });
    recordRetired();
    publish();
    // dxgi.lowLatency holds the next frame back in the present, which runs
    // on the worker: the application waits for it, not a frame behind.
    static const uint64_t ahead = UserOverrides::get().lowLatency ? 0 : 1;
    waitFor([&] { return m_framesDone.load(std::memory_order_seq_cst) + ahead >= frame; });
  }

  Worker::Chunk* Worker::popChunk() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
      return nullptr;
    Chunk* chunk = m_queue.front();
    m_queue.pop_front();
    return chunk;
  }

  void Worker::execute(Chunk* chunk, uint32_t from, uint32_t to) {
    static std::atomic<uint32_t> s_failures{0};
    for (uint8_t* at = chunk->data + from; at < chunk->data + to;) {
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

  bool Worker::waitForCommands(Chunk* chunk, uint32_t seen) {
    const int64_t until = microsecondsNow() + kWorkerSpinMicroseconds;
    for (uint32_t i = 0;; i++) {
      if (chunk->published.load(std::memory_order_acquire) != seen)
        return true;
      if (m_stopping.load(std::memory_order_relaxed))
        return false;
      YieldProcessor();
      if ((i & 63) == 63 && microsecondsNow() >= until)
        break;
    }
    m_workerSleeping.store(true, std::memory_order_seq_cst);
    while (chunk->published.load(std::memory_order_seq_cst) == seen && !m_stopping.load(std::memory_order_seq_cst))
      WaitForSingleObject(m_wakeEvent, 100);
    m_workerSleeping.store(false, std::memory_order_relaxed);
    return chunk->published.load(std::memory_order_acquire) != seen || !m_stopping.load(std::memory_order_relaxed);
  }

  void Worker::workerMain() {
    SetThreadDescription(GetCurrentThread(), L"dxup-d3d9");
    Chunk* chunk = popChunk();
    uint32_t done = 0;
    while (chunk) {
      const uint32_t published = chunk->published.load(std::memory_order_acquire);
      const uint32_t end = published & ~kSealed;
      if (done < end) {
        execute(chunk, done, end);
        done = end;
        chunk->consumed.store(done, std::memory_order_seq_cst);
        if (m_waiting.load(std::memory_order_seq_cst))
          SetEvent(m_doneEvent);
        continue;
      }
      if (published & kSealed) {
        // The next one was queued before this was sealed.
        Chunk* next = popChunk();
        freeChunk(chunk);
        m_chunksFinished.fetch_add(1, std::memory_order_seq_cst);
        if (m_waiting.load(std::memory_order_seq_cst))
          SetEvent(m_doneEvent);
        chunk = next;
        done = 0;
        continue;
      }
      if (!waitForCommands(chunk, published))
        return;
    }
  }

}
