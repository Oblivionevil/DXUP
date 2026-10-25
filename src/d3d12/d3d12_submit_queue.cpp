#include "d3d12_submit_queue.h"
#include "../util/util_types.h"

#include <chrono>

#ifdef _WIN32

namespace dxup::d3d12 {

  SubmitQueue::~SubmitQueue() {
    stop();
  }

  void SubmitQueue::init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    m_device = device;
    m_queue = queue;
  }

  void SubmitQueue::start() {
    if (threaded() || !m_queue)
      return;
    try {
      m_stopping = false;
      m_exited = false;
      m_thread = dxup::Thread([this] { threadMain(); });
    } catch (const std::exception& e) {
      logMessage(LogLevel::Warn, "submit thread not started (%s)", e.what());
      return;
    }
    m_threaded.store(true, std::memory_order_release);
  }

  void SubmitQueue::stop() {
    if (!threaded())
      return;
    // At process exit the thread is gone already.
    if (processExiting()) {
      m_threaded.store(false, std::memory_order_release);
      m_thread.detach();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_wake.notify_one();
    m_thread.join();
    m_threaded.store(false, std::memory_order_release);
  }

  uint64_t SubmitQueue::runHere(Job& job) {
    const uint64_t ticket = m_issued.fetch_add(1, std::memory_order_acq_rel) + 1;
    job(m_queue);
    uint64_t done = m_done.load(std::memory_order_relaxed);
    while (done < ticket && !m_done.compare_exchange_weak(done, ticket, std::memory_order_release)) { }
    return ticket;
  }

  uint64_t SubmitQueue::enqueue(Job job) {
    if (!threaded())
      return runHere(job);
    uint64_t ticket = 0;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      // A job handed in while stop() ends the thread runs here: queued, it
      // would never run.
      if (!m_exited) {
        // fetch_add as runHere's: callers running jobs here while start()
        // turns the thread on get tickets of their own.
        ticket = m_issued.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_jobs.emplace_back(ticket, std::move(job));
      }
    }
    if (!ticket)
      return runHere(job);
    m_wake.notify_one();
    return ticket;
  }

  void SubmitQueue::waitFor(uint64_t ticket) {
    if (done(ticket) || !threaded())
      return;
    for (uint32_t i = 0; i < 4096; i++) {
      if (done(ticket))
        return;
      YieldProcessor();
    }
    m_waiters.fetch_add(1, std::memory_order_seq_cst);
    {
      std::unique_lock<std::mutex> lock(m_doneMutex);
      while (!done(ticket) && !processExiting())
        m_doneCv.wait_for(lock, std::chrono::milliseconds(10));
    }
    m_waiters.fetch_sub(1, std::memory_order_acq_rel);
  }

  void SubmitQueue::checkResult(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr))
      return;
    HRESULT expected = S_OK;
    if (m_lostReason.compare_exchange_strong(expected, hr, std::memory_order_acq_rel))
      logMessage(LogLevel::Error, "submit thread: %s failed (hr=0x%08x)", what, static_cast<unsigned>(hr));
  }

  void SubmitQueue::checkDevice(const char* what) {
    if (m_device)
      checkResult(m_device->GetDeviceRemovedReason(), what);
  }

  void SubmitQueue::threadMain() {
    SetThreadDescription(GetCurrentThread(), L"dxup-submit");
    std::deque<std::pair<uint64_t, Job>> jobs;
    // No polling before sleeping: in Trine 4 it took a core and gained
    // nothing.
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
        if (m_jobs.empty()) {
          m_exited = true;
          return;
        }
        jobs.swap(m_jobs);
      }
      for (auto& job : jobs) {
        job.second(m_queue);
        job.second = nullptr;
        // Only ever up: a job run on its caller's thread (while start() or
        // stop() switched) may have finished a later ticket.
        uint64_t done = m_done.load(std::memory_order_relaxed);
        while (done < job.first && !m_done.compare_exchange_weak(done, job.first, std::memory_order_seq_cst)) { }
        if (m_waiters.load(std::memory_order_seq_cst)) {
          { std::lock_guard<std::mutex> lock(m_doneMutex); }
          m_doneCv.notify_all();
        }
      }
      jobs.clear();
    }
  }

}

#endif
