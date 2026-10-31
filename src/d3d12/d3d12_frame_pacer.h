#pragma once

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include "../util/com_ptr.h"
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
      times.ended = m_frameCalibrationCpu[index]
        + int64_t(double(int64_t(end - m_frameCalibrationGpu[index])) * double(m_qpcFrequency) / double(m_frequency));
      return true;
    }

    int64_t qpcFrequency() const { return m_qpcFrequency; }

  private:
    static constexpr uint32_t kFrames = 64;
    static uint32_t slot(uint64_t frame) { return uint32_t(frame % kFrames); }

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

  // dxgi.lowLatency on the application thread: how long to hold the next
  // frame back, so that its work reaches the GPU just before the GPU is done
  // with the frame before (Reflex-like). Without it a GPU-bound game starts
  // a frame as soon as the frame latency allows and its input waits a frame
  // in the queue. A frame whose work reached the GPU only after the frame
  // before was done (the GPU waited for it: held back too long) widens the
  // margin; frames on time narrow it again, so it settles where the GPU just
  // stays busy. Waits for the display (VSync) do not count.
  class LatencyPacer {
  public:
    // A frame the GPU finished: its GPU time and how long after the end of
    // the frame before its work arrived (0 if before).
    void noteGpuFrame(double busyMs, double lateMs) {
      m_gpu[m_gpuCount++ % kHistory] = busyMs;
      // Grows at once, shrinks by a percent a frame: at most a frame's GPU
      // time, beyond which nothing is held back anyway.
      if (lateMs > kLateMs) {
        m_marginMs = (std::min)(m_marginMs + (std::max)(kMarginStepMs, lateMs), (std::max)(busyMs, kMinMarginMs));
        m_late++;
      } else {
        m_marginMs = (std::max)(m_marginMs - (std::max)(kMarginDecayMs, m_marginMs * 0.01), kMinMarginMs);
      }
    }

    // The application's work for a frame: from the return of the previous
    // Present to this one.
    void noteCpuFrame(double ms) { m_cpu[m_cpuCount++ % kHistory] = ms; }

    // How long to hold the next frame back now (0 when not GPU-bound).
    double sleepMs() const {
      if (m_gpuCount < kHistory || m_cpuCount < kHistory)
        return 0.0;
      // The GPU's usual frame against the application's slow ones.
      const double gpu = percentile(m_gpu, 0.5);
      const double cpu = percentile(m_cpu, 0.75);
      const double sleep = gpu - cpu - m_marginMs;
      return sleep > 0.05 ? (std::min)(sleep, gpu) : 0.0;
    }

    double marginMs() const { return m_marginMs; }
    uint64_t lateFrames() const { return m_late; }

  private:
    static constexpr uint32_t kHistory = 16;
    static constexpr double kLateMs = 0.05;
    static constexpr double kMarginStepMs = 0.25;
    static constexpr double kMarginDecayMs = 0.02;
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
