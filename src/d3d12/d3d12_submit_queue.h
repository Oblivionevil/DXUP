#pragma once

#ifdef _WIN32

#include "../util/util_thread.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

#include <d3d12.h>

namespace dxup::d3d12 {

  // Everything that goes onto the device's command queue, in order: command
  // lists with their fence signals, waits and tile mappings. Normally each
  // job runs at once on the thread that hands it in. With d3d12.submitThread
  // a thread of its own ("dxup-submit") runs them in the order they came,
  // while the CS worker goes on recording. DXGI's Present stays on the
  // worker, which waits for the queue first: on this thread NVIDIA's
  // Present spun until the GPU caught up and held the next frame's lists
  // back (Trine 4: 4.5 % fewer frames, a third more CPU time).
  //
  // Jobs hand in fence values ahead: a wait for a value signaled by a job
  // still queued returns once the thread ran it and the GPU got there. The
  // thread never waits for the worker, the application or a window, so
  // such waits end, and waiting for jobs takes no window messages.
  class SubmitQueue {
  public:
    using Job = std::function<void(ID3D12CommandQueue* queue)>;

    SubmitQueue() = default;
    ~SubmitQueue();

    SubmitQueue(const SubmitQueue&) = delete;
    SubmitQueue& operator=(const SubmitQueue&) = delete;

    void init(ID3D12Device* device, ID3D12CommandQueue* queue);

    // The thread; stop() runs what is queued first. Without it, and once
    // it has ended, jobs run on the caller's thread.
    void start();
    void stop();
    bool threaded() const { return m_threaded.load(std::memory_order_acquire); }

    // Runs or queues the job; its ticket says when it ran.
    uint64_t enqueue(Job job);

    bool done(uint64_t ticket) const { return m_done.load(std::memory_order_acquire) >= ticket; }
    // Waits until the job of the ticket ran.
    void waitFor(uint64_t ticket);
    void drain() { waitFor(m_issued.load(std::memory_order_acquire)); }

    // A job's ExecuteCommandLists or Signal failed, or the device was
    // removed after it: the first such result (S_OK while none).
    HRESULT lostReason() const { return m_lostReason.load(std::memory_order_acquire); }
    // Called by jobs after the queue calls whose results they cannot hand
    // back.
    void checkResult(HRESULT hr, const char* what);
    void checkDevice(const char* what);
    // The work handed in cannot go on (a list was lost): the device is
    // removed, so the application learns it as after a GPU fault
    // (GetDeviceRemovedReason, Present) and its waits end.
    void removeDevice(const char* what);

  private:
    uint64_t runHere(Job& job);
    void threadMain();

    ID3D12Device*         m_device = nullptr;
    ID3D12CommandQueue*   m_queue = nullptr;
    std::atomic<bool>     m_threaded{false};

    std::mutex            m_mutex;
    std::condition_variable m_wake;
    std::deque<std::pair<uint64_t, Job>> m_jobs;
    bool                  m_stopping = false;
    bool                  m_exited = false;     // the thread took its last jobs
    std::atomic<uint64_t> m_issued{0};
    std::atomic<uint64_t> m_done{0};
    std::atomic<HRESULT>  m_lostReason{S_OK};

    std::mutex            m_doneMutex;
    std::condition_variable m_doneCv;
    std::atomic<uint32_t> m_waiters{0};

    dxup::Thread          m_thread;
  };

}

#endif
