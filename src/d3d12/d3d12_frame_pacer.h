#pragma once

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include "../util/com_ptr.h"
#include "../util/util_config.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace dxup::d3d12 {

#ifdef _WIN32
  // dxgi.lowLatency: when each frame's GPU work began and ended, from two
  // timestamps per frame (the begin at the top of the first command list
  // after a present, the end at the present), and when that first list was
  // submitted. The recording thread writes them; the application thread
  // reads a frame's once the fence of its present completed.
  class FrameTimer {
  public:
    void init(ID3D12Device* device, ID3D12CommandQueue* queue) {
      D3D12_QUERY_HEAP_DESC heapDesc = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrames * 2, 0 };
      D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = kFrames * 2 * sizeof(uint64_t);
      desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      void* mapped = nullptr;
      LARGE_INTEGER qpcFrequency;
      QueryPerformanceFrequency(&qpcFrequency);
      if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_heap)))
       || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback)))
       || FAILED(queue->GetTimestampFrequency(&m_frequency)) || !m_frequency
       || FAILED(m_readback->Map(0, nullptr, &mapped))) {
        m_heap.Reset();
        m_readback.Reset();
        return;
      }
      m_queue = queue;
      m_qpcFrequency = qpcFrequency.QuadPart;
      m_ticks = static_cast<const volatile uint64_t*>(mapped);
      for (auto& frame : m_frames)
        frame.store(UINT64_MAX, std::memory_order_relaxed);
      measureCalibrationError(device);
    }

    bool enabled() const { return m_heap.Get() != nullptr; }

    // The top of a new command list: the begin of frame `m_armed`, the first
    // time after its present armed it.
    template<typename List>
    void onListBegin(List& list) {
      if (!m_heap || m_armed == UINT64_MAX)
        return;
      list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot(m_armed) * 2);
      m_begun = m_armed;
      m_unsubmitted = m_armed;
      m_armed = UINT64_MAX;
    }

    // A command list went to the queue: the first of a frame notes when.
    void onSubmitted() {
      if (m_unsubmitted == UINT64_MAX)
        return;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      m_submitted[slot(m_unsubmitted)] = now.QuadPart;
      m_unsubmitted = UINT64_MAX;
    }

    // At the present of `frame`, before the list with its last work closes:
    // its end and the resolve of both. The next list begins frame + 1.
    template<typename List>
    void onPresent(List& list, uint64_t frame) {
      if (!m_heap)
        return;
      const uint32_t index = slot(frame);
      if (m_begun == frame && m_unsubmitted != frame) {
        // GPU ticks against QPC, renewed now and then (the clocks drift).
        if (!m_calibrationAge--) {
          m_queue->GetClockCalibration(&m_calibrationGpu, &m_calibrationCpu);
          m_calibrationAge = 15;
        }
        m_frameCalibrationGpu[index] = m_calibrationGpu;
        m_frameCalibrationCpu[index] = m_calibrationCpu;
        list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2 + 1);
        list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2, 2, m_readback.Get(),
          uint64_t(index) * 2 * sizeof(uint64_t));
        m_frames[index].store(frame, std::memory_order_release);
      } else {
        m_frames[index].store(UINT64_MAX, std::memory_order_release);
      }
      m_armed = frame + 1;
    }

    struct Times {
      double  busyMs = 0.0;   // from the begin to the end of its GPU work
      int64_t submitted = 0;  // QPC of its first list's submission
      int64_t ended = 0;      // QPC of its GPU work's end
    };

    // Any thread, once the fence of `frame`'s present completed; false if it
    // has none (no work, or overwritten).
    bool read(uint64_t frame, Times& times) const {
      if (!m_ticks)
        return false;
      const uint32_t index = slot(frame);
      if (m_frames[index].load(std::memory_order_acquire) != frame)
        return false;
      const uint64_t begin = m_ticks[index * 2];
      const uint64_t end = m_ticks[index * 2 + 1];
      if (end <= begin)
        return false;
      times.busyMs = double(end - begin) * 1000.0 / double(m_frequency);
      times.submitted = m_submitted[index];
      times.ended = toQpc(end, m_frameCalibrationGpu[index], m_frameCalibrationCpu[index]) - m_calibrationError;
      return true;
    }

    int64_t qpcFrequency() const { return m_qpcFrequency; }

  private:
    static constexpr uint32_t kFrames = 64;
    static uint32_t slot(uint64_t frame) { return uint32_t(frame % kFrames); }

    int64_t toQpc(uint64_t ticks, uint64_t calibrationGpu, uint64_t calibrationCpu) const {
      return int64_t(calibrationCpu)
        + int64_t(double(int64_t(ticks - calibrationGpu)) * double(m_qpcFrequency) / double(m_frequency));
    }

    // GetClockCalibration need not be on the clock of the queue's timestamps:
    // with AMD's driver (RX 9070 XT) a timestamp converted with it lies 2.9 ms
    // ahead of the moment its fence completes. A timestamp cannot be later
    // than its fence is seen: a few small lists on the idle queue tell how
    // much later it reads, the closest of them being the least delayed.
    void measureCalibrationError(ID3D12Device* device) {
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      ComPtr<ID3D12Fence> fence;
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
       || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
       || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return;
      list->Close();
      int64_t error = INT64_MIN;
      for (uint32_t i = 1; i <= kCalibrationLists; i++) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
          return;
        list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, m_readback.Get(), 0);
        if (FAILED(list->Close()))
          return;
        uint64_t calibrationGpu = 0, calibrationCpu = 0;
        if (FAILED(m_queue->GetClockCalibration(&calibrationGpu, &calibrationCpu)))
          return;
        ID3D12CommandList* lists[] = { list.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        if (FAILED(m_queue->Signal(fence.Get(), i)))
          return;
        // Spinning: a wake-up would be what it measures.
        LARGE_INTEGER seen, deadline;
        QueryPerformanceCounter(&deadline);
        deadline.QuadPart += m_qpcFrequency / 10;
        while (fence->GetCompletedValue() < i) {
          QueryPerformanceCounter(&seen);
          if (seen.QuadPart > deadline.QuadPart)
            return;
          YieldProcessor();
        }
        QueryPerformanceCounter(&seen);
        error = (std::max)(error, toQpc(m_ticks[0], calibrationGpu, calibrationCpu) - seen.QuadPart);
      }
      if (error > 0)
        m_calibrationError = error;
    }

    static constexpr uint32_t kCalibrationLists = 8;
    int64_t                  m_calibrationError = 0;   // QPC ticks

    ComPtr<ID3D12QueryHeap>  m_heap;
    ComPtr<ID3D12Resource>   m_readback;
    ID3D12CommandQueue*      m_queue = nullptr;
    const volatile uint64_t* m_ticks = nullptr;
    uint64_t                 m_frequency = 0;
    int64_t                  m_qpcFrequency = 0;
    // Recording thread.
    uint64_t                 m_armed = UINT64_MAX;
    uint64_t                 m_begun = UINT64_MAX;
    uint64_t                 m_unsubmitted = UINT64_MAX;
    uint64_t                 m_calibrationGpu = 0;
    uint64_t                 m_calibrationCpu = 0;
    uint32_t                 m_calibrationAge = 0;
    // Per slot, published by m_frames.
    int64_t                  m_submitted[kFrames] = {};
    uint64_t                 m_frameCalibrationGpu[kFrames] = {};
    uint64_t                 m_frameCalibrationCpu[kFrames] = {};
    std::atomic<uint64_t>    m_frames[kFrames];   // the frame a slot holds
  };
#endif

#ifdef _WIN32
  // Holds the calling thread back `ms`: on a high-resolution waitable timer
  // (created in `timer` the first time) up to the last half millisecond,
  // which it spins. wait(timer, timeoutMs) waits for the timer; a caller
  // can take sent messages meanwhile (it returns early then, and the sleep
  // goes on).
  template<typename Wait>
  void paceSleep(HANDLE& timer, double ms, Wait&& wait) {
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    const int64_t target = now.QuadPart + int64_t(ms * double(frequency.QuadPart) / 1000.0);
    const int64_t spin = frequency.QuadPart / 2000;
    if (!timer) {
      // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803), else the
      // default timer resolution.
      timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
      if (!timer)
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    while (timer) {
      QueryPerformanceCounter(&now);
      const int64_t left = target - spin - now.QuadPart;
      if (left <= 0)
        break;
      LARGE_INTEGER due;
      due.QuadPart = -int64_t(double(left) * 1e7 / double(frequency.QuadPart));
      if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        break;
      wait(timer, DWORD(left * 1000 / frequency.QuadPart) + 1);
    }
    do {
      YieldProcessor();
      QueryPerformanceCounter(&now);
    } while (now.QuadPart < target);
  }
#endif

#ifdef _WIN32
  // dxgi.maxFrameRate: at most so many frames a second, on a grid of even
  // steps, so that the average stays at the limit. A frame later than a step
  // lets the next one catch up, or starts the grid anew.
  class FrameLimiter {
  public:
    // shortestStep: how close to the frame before one may follow while
    // catching up, in steps.
    FrameLimiter(double framesPerSecond, double shortestStep) {
      LARGE_INTEGER frequency;
      QueryPerformanceFrequency(&frequency);
      m_msPerTick = 1000.0 / double(frequency.QuadPart);
      m_step = framesPerSecond > 0.0 ? int64_t(double(frequency.QuadPart) / framesPerSecond) : 0;
      m_shortestStep = int64_t(double(m_step) * shortestStep);
    }
    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;
    ~FrameLimiter() {
      if (m_timer)
        CloseHandle(m_timer);
    }

    bool enabled() const { return m_step != 0; }

    // How long this frame has to wait from `now` (QPC); the next is counted
    // from the moment it may go.
    double waitMs(int64_t now) {
      if (!m_step)
        return 0.0;
      const int64_t due = (std::max)(m_next, m_released + m_shortestStep);
      const int64_t released = (std::max)(due, now);
      m_released = released;
      m_next = (std::max)(m_next, released - m_step) + m_step;
      return double(released - now) * m_msPerTick;
    }

    // Waits for that moment. wait as for paceSleep.
    template<typename Wait>
    void hold(Wait&& wait) {
      if (!m_step)
        return;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      const double ms = waitMs(now.QuadPart);
      if (ms > 0.0)
        paceSleep(m_timer, ms, wait);
    }

  private:
    double  m_msPerTick = 0.0;
    int64_t m_step = 0;
    int64_t m_shortestStep = 0;
    int64_t m_next = 0;
    int64_t m_released = 0;
    HANDLE  m_timer = nullptr;
  };

  // A frame behind the grid of the limiter before the real Present follows the
  // one before at this share of a step at the least.
  constexpr double kPresentLimiterShortestStep = 0.9;

  // Before the real Present, on the thread that presents.
  inline double smoothFrameRate() {
    const UserOverrides& overrides = UserOverrides::get();
    return overrides.frameLimiter == FrameLimiterMode::Smooth ? double(overrides.maxFrameRate) : 0.0;
  }
  // At the start of the application's next frame.
  inline double latencyFrameRate() {
    const UserOverrides& overrides = UserOverrides::get();
    return overrides.frameLimiter == FrameLimiterMode::Latency ? double(overrides.maxFrameRate) : 0.0;
  }
#endif

  // dxgi.lowLatency on the application thread: how long to hold the next
  // frame back, so that its first command list reaches the GPU just before
  // the GPU is done with the frame before (Reflex-like). Without it a
  // GPU-bound game starts a frame as soon as the frame latency allows and its
  // input waits a frame in the queue. A frame whose first list reached the
  // GPU only after the frame before was done (the GPU waited for it: held back
  // too long) widens the margin; frames on time narrow it again, so it settles
  // where the GPU just stays busy. Waits for the display (VSync) do not count.
  class LatencyPacer {
  public:
    // A frame the GPU finished: its GPU time and how long after the end of
    // the frame before its work arrived (0 if before).
    void noteGpuFrame(double busyMs, double lateMs) {
      m_gpu[m_gpuCount++ % kHistory] = busyMs;
      // Grows at once, shrinks by a quarter percent a frame (about one frame
      // in forty late): at most a frame's GPU time, beyond which nothing is
      // held back anyway. By little at a time: a single frame late by
      // milliseconds (the game hitched) says nothing about the next, whose
      // input would wait for hundreds of frames.
      if (lateMs > kLateMs) {
        const double step = (std::min)((std::max)(kMarginStepMs, lateMs), kMarginMaxStepMs);
        m_marginMs = (std::min)(m_marginMs + step, (std::max)(busyMs, kMinMarginMs));
        m_late++;
      } else {
        m_marginMs = (std::max)(m_marginMs - (std::max)(kMarginDecayMs, m_marginMs * kMarginDecay), kMinMarginMs);
      }
    }

    // The application's work for a frame: from the return of the previous
    // Present to this one.
    void noteCpuFrame(double ms) { m_cpu[m_cpuCount++ % kHistory] = ms; }

    // How long to hold the next frame back now, sinceEndMs after the end of
    // the GPU work of the latest frame done (0 when not GPU-bound: the
    // application with its slow frames has no time to spare). The GPU then
    // works on the frame after that for its usual time; the application may
    // still be recording while the GPU begins the next one.
    double sleepMs(double sinceEndMs) const {
      if (m_gpuCount < kHistory || m_cpuCount < kHistory)
        return 0.0;
      const double gpu = percentile(m_gpu, 0.5);
      const double cpu = percentile(m_cpu, 0.75);
      if (gpu - cpu - m_marginMs <= kMinSleepMs)
        return 0.0;
      const double sleep = gpu - m_marginMs - sinceEndMs;
      return sleep > kMinSleepMs ? (std::min)(sleep, gpu) : 0.0;
    }

    double marginMs() const { return m_marginMs; }
    uint64_t lateFrames() const { return m_late; }

  private:
    static constexpr uint32_t kHistory = 16;
    static constexpr double kLateMs = 0.05;
    static constexpr double kMinSleepMs = 0.05;
    static constexpr double kMarginStepMs = 0.25;
    static constexpr double kMarginMaxStepMs = 0.5;
    static constexpr double kMarginDecay = 0.0025;
    static constexpr double kMarginDecayMs = 0.005;
    static constexpr double kMinMarginMs = 0.3;

    static double percentile(const double (&values)[kHistory], double p) {
      double sorted[kHistory];
      std::copy(values, values + kHistory, sorted);
      std::sort(sorted, sorted + kHistory);
      return sorted[(std::min)(uint32_t(p * kHistory), kHistory - 1)];
    }

    double   m_gpu[kHistory] = {};
    double   m_cpu[kHistory] = {};
    uint32_t m_gpuCount = 0;
    uint32_t m_cpuCount = 0;
    double   m_marginMs = 1.0;
    uint64_t m_late = 0;
  };
}
