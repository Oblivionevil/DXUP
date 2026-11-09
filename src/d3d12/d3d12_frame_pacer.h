#pragma once

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include "../util/com_ptr.h"
#include "../util/util_config.h"
#include "../util/util_paths.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

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
  // In some games one frame takes milliseconds longer at a fixed rate of its
  // own, whatever the frame rate (Heroes of the Storm's replays every 64 ms;
  // normal play does not): the first frame that begins after such a tick's
  // time. From each frame's start
  // and CPU time TickPredictor finds the period and phase of those ticks, so
  // that the latency limiter can begin the frames before a tick early enough
  // for the tick frame to present on time (FrameLimiter::waitMs). Times in ms
  // on the QPC clock.
  class TickPredictor {
  public:
    struct Next {
      double lo = 0.0, hi = 0.0;      // the tick's time lies in [lo, hi]
      double tickCpuMs = 0.0;         // a tick frame's CPU time (90th percentile)
      double normalCpuMs = 0.0;       // another frame's (90th percentile)
      double normalMedianMs = 0.0;    // and its median
    };

    // A frame that began at start, after the frame before began at
    // prevStart, and took cpuMs until its Present.
    void noteFrame(double prevStart, double start, double cpuMs) {
      m_intervals[m_frames % kFrames] = start - prevStart;
      m_cpu[m_frames++ % kFrames] = cpuMs;
      if (m_frames < kFrames)
        return;
      if ((m_frames & 63) == 0)
        refreshThresholds();
      if (cpuMs <= m_spikeMs || start <= prevStart || start - prevStart > kMaxFrameMs)
        return;
      m_spikes[m_spikeCount++ % kSpikes] = { prevStart, start, cpuMs };
      m_lastSpike = start;
      // Every fourth spike, every 16th while no period held for a while: a
      // search across periods takes up to some 0.4 ms.
      if (m_spikeCount >= kMinSpikes && (m_spikeCount & (m_failedFits >= 8 ? 15 : 3)) == 0) {
        fit();
        m_failedFits = m_valid ? 0 : m_failedFits + 1;
      }
    }

    // The first tick after `after` (when the frame before began), as long as
    // a steady period holds and ticks still come (the latest within a few
    // periods of now).
    bool next(double after, double now, Next& next) const {
      if (!m_valid || now - m_lastSpike > 3.0 * m_period)
        return false;
      const double k = std::ceil((after - (m_base + m_phaseHi)) / m_period);
      next.lo = m_base + m_phaseLo + k * m_period;
      next.hi = m_base + m_phaseHi + k * m_period;
      if (next.hi <= after) {
        next.lo += m_period;
        next.hi += m_period;
      }
      next.tickCpuMs = m_tickCpuMs;
      next.normalCpuMs = m_normalCpuMs;
      next.normalMedianMs = m_normalMedianMs;
      return true;
    }

    bool valid() const { return m_valid; }
    double periodMs() const { return m_valid ? m_period : 0.0; }
    double windowMs() const { return m_valid ? m_phaseHi - m_phaseLo : 0.0; }

  private:
    static constexpr uint32_t kFrames = 256;
    static constexpr uint32_t kSpikes = 48;
    static constexpr uint32_t kMinSpikes = 16;
    static constexpr double   kMaxFrameMs = 50.0;
    // Steps of 0.002 ms the periods as many spikes agree at may lie apart.
    static constexpr int      kPeriodSteps = 50;

    struct Spike {
      double lo, hi, cpu;   // the frame before began at lo, the tick frame at hi
    };

    void refreshThresholds() {
      double sorted[kFrames];
      std::copy(m_cpu, m_cpu + kFrames, sorted);
      std::sort(sorted, sorted + kFrames);
      const double median = sorted[kFrames / 2];
      m_spikeMs = median + (std::max)(1.5, median * 0.5);
      // The other frames' times, without the spikes.
      const uint32_t normal = uint32_t(std::upper_bound(sorted, sorted + kFrames, m_spikeMs) - sorted);
      if (normal) {
        m_normalCpuMs = sorted[normal * 9 / 10];
        m_normalMedianMs = sorted[normal / 2];
      }
      std::copy(m_intervals, m_intervals + kFrames, sorted);
      std::nth_element(sorted, sorted + kFrames / 2, sorted + kFrames);
      m_frameMs = sorted[kFrames / 2];
    }

    // Finds the period around the spikes' median spacing at which the most
    // spikes agree on the phase of the ticks: each spike says the tick lay
    // after the frame before began and no later than the tick frame began.
    void fit() {
      const uint32_t count = uint32_t((std::min<uint64_t>)(m_spikeCount, kSpikes));
      Spike spikes[kSpikes];
      for (uint32_t i = 0; i < count; i++)
        spikes[i] = m_spikes[(m_spikeCount - count + i) % kSpikes];
      double gaps[kSpikes], cpus[kSpikes];
      for (uint32_t i = 0; i < count; i++) {
        gaps[i] = i ? spikes[i].hi - spikes[i - 1].hi : 0.0;
        cpus[i] = spikes[i].cpu;
      }
      std::sort(gaps + 1, gaps + count);
      std::sort(cpus, cpus + count);
      const double spacing = gaps[1 + (count - 1) / 2];
      const double frame = m_frameMs;
      m_valid = false;
      // Ticks are slower than frames, and a tick frame must take clearly
      // longer than the others for planning to help.
      if (frame <= 0.0 || spacing < 2.0 * frame || spacing > 2000.0)
        return;
      m_tickCpuMs = cpus[count * 9 / 10];
      if (cpus[count / 2] < m_normalCpuMs + 1.0)
        return;
      const double base = spikes[count - 1].hi;
      Agreement best;
      const bool refine = m_period > 0.0 && std::abs(m_period - spacing) < frame;
      if (refine)
        best = agreement(spikes, count, base, m_period, best);
      // Steps of 0.002 ms around the period found before, if it still
      // holds; else first coarse steps (0.05 ms, at most 100) across the
      // spacing give or take a frame. (A period costs some 3 us.)
      double around = 0.02;
      if (!refine || best.count < count * 9 / 10) {
        const double coarse = (std::max)(0.05, frame / 50.0);
        for (double period = spacing - frame; period <= spacing + frame; period += coarse) {
          const Agreement a = agreement(spikes, count, base, period, best);
          if (a.better(best))
            best = a;
        }
        around = coarse;
      }
      const double center = best.period;
      for (double period = center - around; period <= center + around; period += 0.002) {
        const Agreement a = agreement(spikes, count, base, period, best);
        if (a.better(best))
          best = a;
      }
      if (best.count < count * 85 / 100)
        return;
      // Every period as many spikes agree at, and the next tick's window
      // over all of them: with spikes at few phases of the frames (ticks
      // every so many frames exactly) the windows span most of a frame, and
      // the narrowest of them says nothing. The narrowest lies at an end of
      // those periods (the spikes' windows tilt against each other), the
      // middle is the better guess.
      double lo = base + best.period + best.phaseLo, hi = base + best.period + best.phaseHi;
      double periods[2] = { best.period, best.period };
      for (int side = 0; side < 2; side++) {
        const double direction = side ? 0.002 : -0.002;
        int steps = 0;
        for (; steps < kPeriodSteps; steps++) {
          const Agreement a = agreement(spikes, count, base, best.period + direction * (steps + 1), best);
          if (a.count < best.count)
            break;
          periods[side] = a.period;
          lo = (std::min)(lo, base + a.period + a.phaseLo);
          hi = (std::max)(hi, base + a.period + a.phaseHi);
        }
        if (steps == kPeriodSteps)
          return;
      }
      if (hi - lo > frame * 0.5)
        return;
      Agreement middle = agreement(spikes, count, base, 0.5 * (periods[0] + periods[1]), Agreement());
      if (middle.count < best.count)
        middle = best;
      if (middle.phaseHi - middle.phaseLo > frame * 0.25)
        return;
      m_period = middle.period;
      m_base = base;
      m_phaseLo = middle.phaseLo;
      m_phaseHi = middle.phaseHi;
      m_valid = true;
    }

    struct Agreement {
      uint32_t count = 0;
      double   period = 0.0, phaseLo = 0.0, phaseHi = 0.0;
      bool better(const Agreement& other) const {
        return count > other.count || (count == other.count && phaseHi - phaseLo < other.phaseHi - other.phaseLo);
      }
    };

    // How many spikes agree on one phase of the ticks at this period, and the
    // phase window they leave (relative to base).
    // Only windows of a point at least as good as `beat` are worked out.
    static Agreement agreement(const Spike* spikes, uint32_t count, double base, double period, const Agreement& beat) {
      // Each spike's window (lo, hi], moved by whole periods next to base,
      // where the windows of the spikes that agree overlap.
      double lo[kSpikes], hi[kSpikes], los[kSpikes], his[kSpikes];
      for (uint32_t i = 0; i < count; i++) {
        const double k = std::round((base - spikes[i].hi) / period);
        hi[i] = his[i] = spikes[i].hi + k * period - base;
        lo[i] = los[i] = hi[i] - (spikes[i].hi - spikes[i].lo);
      }
      std::sort(los, los + count);
      std::sort(his, his + count);
      // The point inside the most windows lies at a window's end: inside
      // are the windows that begin before it, but for those that end before.
      auto inside = [&](double x) {
        return uint32_t(std::lower_bound(los, los + count, x) - los) - uint32_t(std::lower_bound(his, his + count, x) - his);
      };
      uint32_t most = 0;
      for (uint32_t c = 0; c < count; c++)
        most = (std::max)(most, inside(his[c]));
      Agreement best;
      best.period = period;
      best.count = most;
      best.phaseLo = -1e9;
      best.phaseHi = 1e9;
      if (most < beat.count)
        return best;
      best.count = 0;
      // Of the points inside that many, the one whose windows leave the
      // narrowest phase.
      for (uint32_t c = 0; c < count; c++) {
        if ((c && his[c] == his[c - 1]) || inside(his[c]) != most)
          continue;
        Agreement a;
        a.count = most;
        a.period = period;
        a.phaseLo = -1e9;
        a.phaseHi = 1e9;
        for (uint32_t i = 0; i < count; i++) {
          if (lo[i] < his[c] && his[c] <= hi[i]) {
            a.phaseLo = (std::max)(a.phaseLo, lo[i]);
            a.phaseHi = (std::min)(a.phaseHi, hi[i]);
          }
        }
        if (a.better(best))
          best = a;
      }
      return best;
    }

    double   m_cpu[kFrames] = {};
    double   m_intervals[kFrames] = {};   // from the frame before's start
    double   m_frameMs = 0.0;             // their median
    uint64_t m_frames = 0;
    Spike    m_spikes[kSpikes] = {};
    uint64_t m_spikeCount = 0;
    double   m_spikeMs = 1e9;
    double   m_normalCpuMs = 0.0, m_normalMedianMs = 0.0, m_tickCpuMs = 0.0;
    double   m_lastSpike = 0.0;
    uint32_t m_failedFits = 0;
    bool     m_valid = false;
    double   m_period = 0.0, m_base = 0.0, m_phaseLo = 0.0, m_phaseHi = 0.0;
  };

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
    // from the moment it may go. With ticks (the latency limiter of a
    // fixed-step game loop) the frames before a tick begin early enough, one
    // after the other, that the tick frame, which takes longer, still
    // presents on time; each frame that began early waits at Present for the
    // time it would have presented at (presentTarget).
    double waitMs(int64_t now, const TickPredictor* ticks = nullptr) {
      if (!m_step)
        return 0.0;
      const int64_t due = (std::max)(m_next, m_released + m_shortestStep);
      int64_t release = due;
      m_presentTarget = 0;
      TickPredictor::Next tick;
      if (ticks && m_released && ticks->next(double(m_released) * m_msPerTick, double(now) * m_msPerTick, tick)) {
        const double step = double(m_step) * m_msPerTick;
        const double grid = double(due) * m_msPerTick;
        const double longer = tick.tickCpuMs - tick.normalMedianMs;
        // The tick frame: the first whose start on the grid, earlier by how
        // much longer it takes, lies after the tick, with a margin: the
        // game's tick may come a little later than the earliest moment the
        // spikes allow (its phase wanders by half a millisecond).
        int frames = 0;
        while (frames < kTickHorizon && grid + frames * step - longer < tick.hi + kTickLateMarginMs)
          frames++;
        if (frames < kTickHorizon) {
          double start = grid + frames * step - longer;
          // The frames before it begin before the tick, each done before
          // the next begins.
          for (int i = frames - 1; i >= 0; i--)
            start = (std::min)({ grid + i * step, start - tick.normalCpuMs, tick.lo - kTickMarginMs });
          const int64_t early = int64_t(start / m_msPerTick);
          if (early < due) {
            release = early;
            m_presentTarget = due + int64_t(tick.normalMedianMs / m_msPerTick);
            m_earlyFrames++;
          }
        }
      }
      const int64_t released = (std::max)(release, now);
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

    // The QPC time before which the frame just let go should not present
    // (0: whenever it is ready), and how many frames began early so far.
    int64_t presentTarget() const { return m_presentTarget; }
    uint64_t earlyFrames() const { return m_earlyFrames; }

    // On the presenting thread: waits until the QPC time target (0: not at
    // all), for a frame the latency limiter began early.
    void holdUntil(int64_t target) {
      if (!target)
        return;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if (now.QuadPart < target)
        paceSleep(m_timer, double(target - now.QuadPart) * m_msPerTick,
          [](HANDLE timer, DWORD timeout) { WaitForSingleObject(timer, timeout); });
    }

  private:
    // Frames ahead a tick is planned for, how long before the tick the
    // frames before it begin and how long after it the tick frame at least.
    static constexpr int    kTickHorizon = 8;
    static constexpr double kTickMarginMs = 0.25;
    static constexpr double kTickLateMarginMs = 0.6;

    double   m_msPerTick = 0.0;
    int64_t  m_step = 0;
    int64_t  m_shortestStep = 0;
    int64_t  m_next = 0;
    int64_t  m_released = 0;
    int64_t  m_presentTarget = 0;
    uint64_t m_earlyFrames = 0;
    HANDLE   m_timer = nullptr;
  };

  // dxup_pacing_dump.txt holding a count N: once dxup_pacing_dump_go.txt lies
  // next to the DLL too, the next N frames' times go to
  // dxup_pacing_dump_out.csv (ms from the first frame's start): when the
  // application began each frame (its Present before returned), when it
  // presented it, how long DXUP held the frame back before it began, when
  // the real Present returned and when the frame's GPU work ended.
  class PacingDump {
  public:
    void init() {
      FILE* file = dxupOpenSibling("dxup_pacing_dump.txt", "r");
      if (!file)
        return;
      unsigned count = 0;
      if (fscanf_s(file, "%u", &count) != 1)
        count = 0;
      fclose(file);
      m_records.resize((std::min)(count, 1u << 20));
      LARGE_INTEGER frequency;
      QueryPerformanceFrequency(&frequency);
      m_msPerTick = 1000.0 / double(frequency.QuadPart);
    }

    // The frame the application begins now (start), the frame before it
    // presented at entry, and how long the start was held back.
    void noteStart(uint64_t frame, int64_t start, int64_t entry, double sleptMs) {
      if (m_records.empty())
        return;
      if (m_first == UINT64_MAX) {
        if ((frame & 255) != 0 || !dxupSiblingExists("dxup_pacing_dump_go.txt"))
          return;
        m_first = frame;
      }
      if (frame > m_first && frame - 1 - m_first < m_records.size())
        m_records[frame - 1 - m_first].entry = entry;
      if (frame - m_first < m_records.size()) {
        m_records[frame - m_first].start = start;
        m_records[frame - m_first].sleptMs = sleptMs;
      }
    }

    // A frame the worker presented and the GPU finished.
    void noteDone(uint64_t frame, int64_t presented, int64_t gpuEnd) {
      if (m_records.empty() || m_first == UINT64_MAX || frame < m_first)
        return;
      if (frame - m_first < m_records.size()) {
        m_records[frame - m_first].presented = presented;
        m_records[frame - m_first].gpuEnd = gpuEnd;
      }
      if (frame - m_first + 1 == m_records.size())
        write();
    }

  private:
    struct Record {
      int64_t start = 0, entry = 0, presented = 0, gpuEnd = 0;
      double  sleptMs = 0.0;
    };

    void write() {
      FILE* out = dxupOpenSibling("dxup_pacing_dump_out.csv", "w");
      if (out) {
        const int64_t origin = m_records[0].start;
        auto ms = [&](int64_t t) { return t ? double(t - origin) * m_msPerTick : -1.0; };
        fprintf(out, "frame,start,entry,slept,presented,gpuEnd\n");
        for (size_t i = 0; i < m_records.size(); i++) {
          const Record& r = m_records[i];
          fprintf(out, "%zu,%.3f,%.3f,%.3f,%.3f,%.3f\n", i, ms(r.start), ms(r.entry), r.sleptMs, ms(r.presented), ms(r.gpuEnd));
        }
        fclose(out);
      }
      m_records.clear();
      m_records.shrink_to_fit();
    }

    std::vector<Record> m_records;
    uint64_t            m_first = UINT64_MAX;
    double              m_msPerTick = 0.0;
  };

  // A frame behind the grid of the limiter before the real Present follows the
  // one before at this share of a step at the least.
  constexpr double kPresentLimiterShortestStep = 0.9;

  // The latency limiter plans for the ticks of fixed-step game loops, but
  // with dxup_no_tick_plan.txt next to the DLL (for comparison).
  inline bool tickPlanning() {
    static const bool off = dxupSiblingExists("dxup_no_tick_plan.txt");
    return !off;
  }

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
