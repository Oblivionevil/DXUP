#pragma once

#ifdef _WIN32
#include <d3d12.h>
#include "../util/com_ptr.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <windows.h>
#include <share.h>
#include <string>
#include <vector>
#include <unordered_set>

namespace dxup {

  // What the GPU work since a barrier batch touched: render targets and
  // unordered access views it wrote, destinations transitioned to (copies,
  // resolves), shader resources it read. A later batch that transitions none
  // of it need not wait for that work - with legacy barriers it waits anyway
  // (a 4K pass costs ~2.3 us more), so such a batch is better moved to the one
  // before (see D3D12BackendContext's barrier hoisting).
  class BarrierAccessTracker {
  public:
    enum Kind : int { Written = 0, Read = 1, Neither = 2 };

    // Noted when bound; written by the next draw or dispatch.
    void noteWrite(ID3D12Resource* resource) { addUnique(m_pendingWrites, resource); }
    // The bound render targets and depth buffer (written by every draw).
    void setTargetWrites(ID3D12Resource* const* resources, size_t count) { m_targetWrites.assign(resources, resources + count); }
    // Shader resources the next command reads.
    void noteRead(ID3D12Resource* resource) {
      if (resource)
        m_read.insert(resource);
    }
    // A draw or dispatch ran.
    void commit(bool draw) {
      for (ID3D12Resource* resource : m_pendingWrites)
        m_written.insert(resource);
      m_pendingWrites.clear();
      if (draw) {
        for (ID3D12Resource* resource : m_targetWrites)
          m_written.insert(resource);
      }
    }
    // Shader reads seen since the last anchor: bindings that did not change
    // need not be noted again.
    bool readsCurrent(uint64_t bindingSerial) const { return m_readSerial == bindingSerial && !m_readsCleared; }
    void setReadSerial(uint64_t bindingSerial) { m_readSerial = bindingSerial; m_readsCleared = false; }

    Kind classify(const D3D12_RESOURCE_BARRIER* barriers, size_t count) const {
      Kind kind = Neither;
      for (size_t i = 0; i < count; i++) {
        const auto& b = barriers[i];
        ID3D12Resource* resource = b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? b.Transition.pResource
          : b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? b.UAV.pResource : nullptr;
        if (!resource || m_written.count(resource))
          return Written;
        if (m_read.count(resource))
          kind = Read;
      }
      return kind;
    }

    // A batch stays where it is: what came before it is ordered by it.
    void anchor(const D3D12_RESOURCE_BARRIER* barriers, size_t count) {
      m_written.clear();
      m_read.clear();
      m_readsCleared = true;
      addWrites(barriers, count);
    }
    // What a batch transitions is used by the work after it: written in
    // write states, read in the others (also by DXUP's own passes, which
    // the tracker does not see otherwise).
    void addWrites(const D3D12_RESOURCE_BARRIER* barriers, size_t count) {
      constexpr D3D12_RESOURCE_STATES kWrites = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST
        | D3D12_RESOURCE_STATE_STREAM_OUT;
      for (size_t i = 0; i < count; i++) {
        const auto& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
          continue;
        if (b.Transition.StateAfter & kWrites)
          m_written.insert(b.Transition.pResource);
        else
          m_read.insert(b.Transition.pResource);
      }
    }
    void clear() {
      m_written.clear();
      m_read.clear();
      m_readsCleared = true;
      m_pendingWrites.clear();
    }

  private:
    static bool contains(const std::vector<ID3D12Resource*>& list, ID3D12Resource* resource) {
      return std::find(list.begin(), list.end(), resource) != list.end();
    }
    static void addUnique(std::vector<ID3D12Resource*>& list, ID3D12Resource* resource) {
      if (resource && !contains(list, resource))
        list.push_back(resource);
    }
    std::unordered_set<ID3D12Resource*> m_written, m_read;
    std::vector<ID3D12Resource*> m_pendingWrites, m_targetWrites;
    uint64_t m_readSerial = 0;
    bool m_readsCleared = true;
  };

  // Diagnostic GPU pass profiler, enabled by dxup_gpu_profile.txt next to the
  // DLL ("<first frame> <frame count>", default "2000 300"; "go <frame count>"
  // starts once dxup_gpu_profile_go.txt appears next to it). Writes a GPU
  // timestamp whenever the kind of work changes (draws into other render
  // targets, another compute shader, the present) and reports the GPU time of
  // each pass in dxup_gpu_profile_out.txt.
  class GpuProfiler {
  public:
    enum class Kind : uint8_t { Present, Draw, Compute, Internal };

    struct Targets {
      const void* key = nullptr;
      uint32_t width = 0, height = 0, samples = 0, count = 0;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, depthFormat = DXGI_FORMAT_UNKNOWN;
    };

    void init(ID3D12Device* device, ID3D12CommandQueue* queue, const std::wstring& markerPath,
              const std::wstring& outPath) {
      FILE* marker = _wfsopen(markerPath.c_str(), L"r", _SH_DENYNO);
      if (!marker)
        return;
      unsigned first = 2000, count = 300;
      char go[3] = {};
      if (fscanf(marker, "%2[go] %u", go, &count) == 2 && go[0] == 'g' && go[1] == 'o') {
        m_goPath = markerPath.substr(0, markerPath.size() - 4) + L"_go.txt";
        first = ~0u;
      } else {
        rewind(marker);
        if (fscanf(marker, "%u %u", &first, &count) < 1)
          first = 2000;
      }
      fclose(marker);
      D3D12_QUERY_HEAP_DESC heapDesc = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kMaxQueries, 0 };
      D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = kMaxQueries * sizeof(uint64_t);
      desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_heap)))
       || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback)))
       || FAILED(queue->GetTimestampFrequency(&m_frequency)))
        return;
      m_queue = queue;
      m_firstFrame = first;
      m_frameCount = count ? count : 1;
      m_outPath = outPath;
      m_enabled = true;
    }

    bool enabled() const { return m_enabled; }
    bool capturing() const { return m_capturing; }

    void setTargets(const Targets& targets) { m_targets = targets; }

    void onDraw(ID3D12GraphicsCommandList* list) {
      if (m_cur.kind != Kind::Draw || m_cur.targets.key != m_targets.key) {
        if (!begin(list, Kind::Draw))
          return;
        m_cur.targets = m_targets;
      } else if (m_batchSinceDraw) {
        // Barriers between two draws of one pass: the GPU drains the pass.
        m_cur.midBatches++;
        for (const auto& key : m_lastBatchKeys)
          m_midKinds[key]++;
      }
      m_batchSinceDraw = false;
      m_cur.draws++;
    }

    void onDispatch(ID3D12GraphicsCommandList* list, uint64_t shaderHash) {
      if (m_cur.kind != Kind::Compute || m_cur.shader != shaderHash) {
        if (!begin(list, Kind::Compute))
          return;
        m_cur.shader = shaderHash;
      } else if (m_batchSinceDraw) {
        m_cur.midBatches++;
        for (const auto& key : m_lastBatchKeys)
          m_midKinds[key]++;
      }
      m_batchSinceDraw = false;
      m_cur.dispatches++;
    }

    // DXUP's own GPU work (resolves, forced MSAA passes) into `target`: a
    // pass of its own.
    void onInternal(ID3D12GraphicsCommandList* list, const char* name, ID3D12Resource* target, DXGI_FORMAT format) {
      if (!begin(list, Kind::Internal))
        return;
      m_cur.name = name;
      if (target) {
        const D3D12_RESOURCE_DESC desc = target->GetDesc();
        m_cur.targets.width = uint32_t(desc.Width);
        m_cur.targets.height = desc.Height;
        m_cur.targets.samples = desc.SampleDesc.Count;
      }
      m_cur.targets.format = format;
      m_cur.copies++;
    }

    void onCopy() { m_cur.copies++; }
    void onClear() { m_cur.clears++; }

    // A barrier batch classified by BarrierAccessTracker (0 written, 1 read,
    // 2 neither: it could have gone with the batch before).
    void countBatch(int kind) { m_batchKinds[kind < 0 || kind > 2 ? 0 : kind]++; }
    // Why a batch that could have gone with the batch before stayed where
    // it is (barrier hoisting).
    void countUnmoved(const char* reason) { m_unmovedReasons[reason]++; }

    void onBarriers(const D3D12_RESOURCE_BARRIER* barriers, size_t count) {
      m_cur.barrierCalls++;
      if (!m_batchSinceDraw)
        m_lastBatchKeys.clear();
      m_batchSinceDraw = true;
      m_cur.barriers += static_cast<uint32_t>(count);
      for (size_t i = 0; i < count; i++) {
        char key[160];
        const auto& b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
          const D3D12_RESOURCE_DESC desc = b.Transition.pResource->GetDesc();
          if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            snprintf(key, sizeof(key), "%-7s buffer %llu: %x -> %x", kindName(m_cur.kind),
              static_cast<unsigned long long>(desc.Width), unsigned(b.Transition.StateBefore),
              unsigned(b.Transition.StateAfter));
          else
            snprintf(key, sizeof(key), "%-7s image %llux%u fmt %u mips %u sub %s: %x -> %x", kindName(m_cur.kind),
              static_cast<unsigned long long>(desc.Width), desc.Height, unsigned(desc.Format), desc.MipLevels,
              b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? "all" : "one",
              unsigned(b.Transition.StateBefore), unsigned(b.Transition.StateAfter));
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && b.UAV.pResource) {
          const D3D12_RESOURCE_DESC desc = b.UAV.pResource->GetDesc();
          snprintf(key, sizeof(key), "%-7s uav barrier %s %llux%u fmt %u", kindName(m_cur.kind),
            desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? "buffer" : "image",
            static_cast<unsigned long long>(desc.Width), desc.Height, unsigned(desc.Format));
        } else {
          snprintf(key, sizeof(key), "%-7s %s", kindName(m_cur.kind),
            b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? "uav barrier (all)" : "aliasing barrier");
        }
        m_barrierKinds[key]++;
        m_lastBatchKeys.push_back(std::string(key) + " | in " + describe(m_cur));
      }
    }

    // A command list starts recording (after a submission).
    void onListBegin(ID3D12GraphicsCommandList* list) {
      if (!m_capturing || !timestamp(list))
        return;
      m_submit = Submit();
      m_submit.start = m_next - 1;
    }

    // The list was executed; callers are module offsets of the code that
    // submitted it.
    void onExecuted(uint64_t caller0, uint64_t caller1) {
      if (!m_submitPending)
        return;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      m_submit.cpu = now.QuadPart;
      m_submit.caller[0] = caller0;
      m_submit.caller[1] = caller1;
      m_submits.push_back(m_submit);
      m_submit = Submit();
      m_submitPending = false;
    }

    // Resolves the timestamps the list wrote; called before it is closed.
    void beforeClose(ID3D12GraphicsCommandList* list) {
      if (m_capturing && timestamp(list)) {
        m_submit.end = m_next - 1;
        m_submit.frame = m_frame;
        m_submitPending = true;
      }
      if (m_resolved < m_next) {
        list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_resolved, m_next - m_resolved,
          m_readback.Get(), uint64_t(m_resolved) * sizeof(uint64_t));
        m_resolved = m_next;
      }
      if (m_capturing)
        m_cur.submits++;
    }

    // Called at the start of each present, while the list records.
    void onPresent(ID3D12GraphicsCommandList* list, uint64_t nextFence) {
      if (!m_enabled)
        return;
      if (m_overflowed && !m_resultFence) {
        m_resultFence = nextFence;
        return;
      }
      m_frame++;
      if (!m_goPath.empty() && !(m_frame & 63) && GetFileAttributesW(m_goPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m_firstFrame = m_frame + 1;
        m_goPath.clear();
      }
      if (m_capturing && m_frame == m_firstFrame + m_frameCount) {
        end(list);
        m_capturing = false;
        m_resultFence = nextFence;
        return;
      }
      if (m_frame == m_firstFrame) {
        m_capturing = true;
        m_queue->GetClockCalibration(&m_calibrationGpu, &m_calibrationCpu);
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        m_qpcFrequency = frequency.QuadPart;
      }
      if (m_capturing) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        m_frameCpu[m_frame] = now.QuadPart;
        begin(list, Kind::Present);
      }
    }

    bool wantsResults() const { return m_enabled && !m_capturing && m_resultFence; }

    void writeResults(uint64_t completedFence) {
      if (completedFence < m_resultFence)
        return;
      m_resultFence = 0;
      m_enabled = false;
      const uint64_t* ticks = nullptr;
      D3D12_RANGE range = { 0, size_t(m_next) * sizeof(uint64_t) };
      if (FAILED(m_readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint64_t**>(&ticks)))))
        return;
      report(ticks);
      D3D12_RANGE none = { 0, 0 };
      m_readback->Unmap(0, &none);
      m_passes.clear();
      m_passes.shrink_to_fit();
      m_heap.Reset();
      m_readback.Reset();
    }

  private:
    static constexpr uint32_t kMaxQueries = 1u << 18;

    struct Submit {
      uint32_t frame = 0, start = UINT32_MAX, end = UINT32_MAX;
      int64_t cpu = 0;
      uint64_t caller[2] = {};
    };

    struct Pass {
      uint32_t frame = 0, query = 0;
      Kind kind = Kind::Present;
      Targets targets;
      uint64_t shader = 0;
      const char* name = nullptr;
      uint32_t draws = 0, dispatches = 0, copies = 0, clears = 0, barriers = 0, barrierCalls = 0, submits = 0, midBatches = 0;
    };

    bool timestamp(ID3D12GraphicsCommandList* list) {
      if (m_next >= kMaxQueries)
        return false;
      list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_next++);
      return true;
    }

    bool begin(ID3D12GraphicsCommandList* list, Kind kind) {
      if (!m_capturing)
        return false;
      if (!timestamp(list)) {
        m_capturing = false;
        m_open = false;
        m_end = m_next - 1;
        m_overflowed = true;
        return false;
      }
      if (m_open)
        m_passes.push_back(m_cur);
      m_cur = Pass();
      m_cur.kind = kind;
      m_cur.frame = m_frame;
      m_cur.query = m_next - 1;
      m_open = true;
      return true;
    }

    void end(ID3D12GraphicsCommandList* list) {
      if (m_open && timestamp(list))
        m_passes.push_back(m_cur);
      m_open = false;
      m_end = m_next - 1;
      m_cur = Pass();
    }

    static const char* kindName(Kind kind) {
      switch (kind) {
        case Kind::Draw:    return "draw";
        case Kind::Compute: return "compute";
        case Kind::Internal: return "dxup";
        default:            return "present";
      }
    }

    std::string describe(const Pass& pass) const {
      char text[160];
      if (pass.kind == Kind::Draw)
        snprintf(text, sizeof(text), "draw %ux%u fmt %u x%u rts %u ds %u", pass.targets.width,
          pass.targets.height, unsigned(pass.targets.format), pass.targets.samples, pass.targets.count,
          unsigned(pass.targets.depthFormat));
      else if (pass.kind == Kind::Compute)
        snprintf(text, sizeof(text), "compute cs %016llx", static_cast<unsigned long long>(pass.shader));
      else if (pass.kind == Kind::Internal)
        snprintf(text, sizeof(text), "dxup %s %ux%u fmt %u x%u", pass.name ? pass.name : "?", pass.targets.width,
          pass.targets.height, unsigned(pass.targets.format), pass.targets.samples);
      else
        snprintf(text, sizeof(text), "present");
      return text;
    }

    void report(const uint64_t* ticks) {
      FILE* out = _wfsopen(m_outPath.c_str(), L"w", _SH_DENYNO);
      if (!out)
        return;
      const double msPerTick = 1000.0 / double(m_frequency);
      auto duration = [&](size_t i) {
        const uint32_t endQuery = i + 1 < m_passes.size() ? m_passes[i + 1].query : m_end;
        const uint64_t a = ticks[m_passes[i].query], b = ticks[endQuery];
        return b > a ? double(b - a) * msPerTick : 0.0;
      };

      // Per frame totals; the median frame is listed pass by pass.
      std::map<uint32_t, std::pair<size_t, size_t>> frames;
      for (size_t i = 0; i < m_passes.size(); i++) {
        auto it = frames.find(m_passes[i].frame);
        if (it == frames.end())
          frames[m_passes[i].frame] = { i, i + 1 };
        else
          it->second.second = i + 1;
      }
      std::vector<std::pair<double, uint32_t>> totals;
      for (const auto& frame : frames) {
        double sum = 0.0;
        for (size_t i = frame.second.first; i < frame.second.second; i++)
          sum += duration(i);
        totals.push_back({ sum, frame.first });
      }
      std::sort(totals.begin(), totals.end());
      double all = 0.0;
      for (const auto& t : totals)
        all += t.first;
      fprintf(out, "frames %zu, GPU ms per frame: avg %.3f, min %.3f, median %.3f, max %.3f\n", totals.size(),
        totals.empty() ? 0.0 : all / totals.size(), totals.empty() ? 0.0 : totals.front().first,
        totals.empty() ? 0.0 : totals[totals.size() / 2].first, totals.empty() ? 0.0 : totals.back().first);

      // Average per frame of each pass description, largest first.
      struct Sum { double ms = 0.0; uint64_t passes = 0, draws = 0, dispatches = 0, copies = 0, clears = 0,
        barriers = 0, barrierCalls = 0, submits = 0, midBatches = 0; };
      std::map<std::string, Sum> sums;
      for (size_t i = 0; i < m_passes.size(); i++) {
        auto& sum = sums[describe(m_passes[i])];
        sum.ms += duration(i);
        sum.passes++;
        sum.draws += m_passes[i].draws;
        sum.dispatches += m_passes[i].dispatches;
        sum.copies += m_passes[i].copies;
        sum.clears += m_passes[i].clears;
        sum.barriers += m_passes[i].barriers;
        sum.barrierCalls += m_passes[i].barrierCalls;
        sum.submits += m_passes[i].submits;
        sum.midBatches += m_passes[i].midBatches;
      }
      std::vector<std::pair<std::string, Sum>> sorted(sums.begin(), sums.end());
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
      const double n = totals.empty() ? 1.0 : double(totals.size());
      fprintf(out, "barrier batches per frame: %.1f transition what the work since the batch before wrote, %.1f what it "
        "only read, %.1f neither (could go with the batch before)\n", double(m_batchKinds[0]) / n, double(m_batchKinds[1]) / n,
        double(m_batchKinds[2]) / n);
      fprintf(out, "\nper frame by pass kind: ms passes draws dispatches copies clears barriers batches submits mid-pass | pass\n");
      for (const auto& s : sorted)
        fprintf(out, "%7.3f %6.1f %7.1f %6.1f %6.1f %6.1f %7.1f %6.1f %5.2f %5.1f | %s\n", s.second.ms / n,
          s.second.passes / n, s.second.draws / n, s.second.dispatches / n, s.second.copies / n,
          s.second.clears / n, s.second.barriers / n, s.second.barrierCalls / n, s.second.submits / n,
          s.second.midBatches / n, s.first.c_str());

      if (!m_unmovedReasons.empty()) {
        fprintf(out, "batches that could have gone with the batch before, per frame, by why they did not:");
        for (const auto& [reason, count] : m_unmovedReasons)
          fprintf(out, " %s %.1f;", reason.c_str(), double(count) / n);
        fprintf(out, "\n");
      }
      std::vector<std::pair<std::string, uint64_t>> kinds(m_barrierKinds.begin(), m_barrierKinds.end());
      std::sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      fprintf(out, "\nbarriers per frame (pass kind, resource, before -> after):\n");
      for (const auto& k : kinds)
        fprintf(out, "%8.1f %s\n", k.second / n, k.first.c_str());
      std::vector<std::pair<std::string, uint64_t>> midKinds(m_midKinds.begin(), m_midKinds.end());
      std::sort(midKinds.begin(), midKinds.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      fprintf(out, "\nbarriers between two draws or dispatches of one pass, per frame:\n");
      for (const auto& k : midKinds)
        fprintf(out, "%8.1f %s\n", k.second / n, k.first.c_str());

      if (!totals.empty()) {
        const uint32_t median = totals[totals.size() / 2].second;
        const auto range = frames[median];
        fprintf(out, "\nmedian frame %u: ms draws dispatches copies clears barriers batches submits | pass\n", median);
        for (size_t i = range.first; i < range.second; i++) {
          const Pass& p = m_passes[i];
          fprintf(out, "%3zu %7.3f %5u %4u %4u %3u %4u %4u %2u | %s\n", i - range.first, duration(i), p.draws,
            p.dispatches, p.copies, p.clears, p.barriers, p.barrierCalls, p.submits, describe(p).c_str());
        }
        reportSubmits(out, ticks, median, n);
      }
      fclose(out);
    }

    ComPtr<ID3D12QueryHeap> m_heap;
    ComPtr<ID3D12Resource>  m_readback;
    uint64_t m_frequency = 1;
    bool m_enabled = false, m_capturing = false, m_open = false, m_overflowed = false, m_batchSinceDraw = false;
    std::vector<std::string> m_lastBatchKeys;
    uint64_t m_batchKinds[3] = {};
    std::map<std::string, uint64_t> m_midKinds;
    uint32_t m_frame = 0, m_firstFrame = 0, m_frameCount = 0;
    uint32_t m_next = 0, m_resolved = 0, m_end = 0;
    uint64_t m_resultFence = 0;
    std::wstring m_outPath;
    std::wstring m_goPath;
    Targets m_targets;
    Pass m_cur;
    std::vector<Pass> m_passes;
    std::map<std::string, uint64_t> m_barrierKinds;
    std::map<std::string, uint64_t> m_unmovedReasons;
    ID3D12CommandQueue* m_queue = nullptr;
    uint64_t m_calibrationGpu = 0, m_calibrationCpu = 0;
    int64_t m_qpcFrequency = 1;
    Submit m_submit;
    bool m_submitPending = false;
    std::vector<Submit> m_submits;
    std::map<uint32_t, int64_t> m_frameCpu;

    void reportSubmits(FILE* out, const uint64_t* ticks, uint32_t medianFrame, double frames) {
      // GPU ticks on the CPU clock, in ms.
      auto gpuMs = [&](uint64_t tick) {
        return (double(m_calibrationCpu) + (double(tick) - double(m_calibrationGpu)) * double(m_qpcFrequency)
          / double(m_frequency)) * 1000.0 / double(m_qpcFrequency);
      };
      auto cpuMs = [&](int64_t qpc) { return double(qpc) * 1000.0 / double(m_qpcFrequency); };
      std::map<std::string, std::pair<double, uint64_t>> idleBySubmitter;
      double idleTotal = 0.0, waitedTotal = 0.0;
      uint64_t starved = 0;
      fprintf(out, "\nsubmissions of median frame %u: cpu submit, gpu start, gpu end, idle before (ms from frame start) | callers\n",
        medianFrame);
      for (size_t i = 1; i < m_submits.size(); i++) {
        const Submit& s = m_submits[i];
        const Submit& p = m_submits[i - 1];
        if (s.start == UINT32_MAX || s.end == UINT32_MAX || p.end == UINT32_MAX)
          continue;
        const double start = gpuMs(ticks[s.start]), end = gpuMs(ticks[s.end]), previousEnd = gpuMs(ticks[p.end]);
        const double idle = start > previousEnd ? start - previousEnd : 0.0;
        const double submitted = cpuMs(s.cpu);
        idleTotal += idle;
        char key[64];
        snprintf(key, sizeof(key), "+0x%llx <- +0x%llx", static_cast<unsigned long long>(s.caller[0]),
          static_cast<unsigned long long>(s.caller[1]));
        auto& entry = idleBySubmitter[key];
        entry.first += idle;
        entry.second++;
        // The GPU waited for this submission: it started right after it.
        if (idle > 0.02 && start - submitted < 0.25) {
          starved++;
          waitedTotal += idle;
        }
        if (s.frame == medianFrame) {
          const double origin = cpuMs(m_frameCpu[medianFrame]);
          fprintf(out, "%8.3f %8.3f %8.3f %7.3f | %s\n", submitted - origin, start - origin, end - origin, idle, key);
        }
      }
      fprintf(out, "\nGPU idle between submissions per frame: %.3f ms (%.3f ms in %.1f gaps that ended right after their submission)\n",
        idleTotal / frames, waitedTotal / frames, double(starved) / frames);
      std::vector<std::pair<std::string, std::pair<double, uint64_t>>> sorted(idleBySubmitter.begin(), idleBySubmitter.end());
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
      fprintf(out, "idle per frame by the submission ending it: ms submissions | callers (module offsets)\n");
      for (const auto& s : sorted)
        fprintf(out, "%7.3f %6.1f | %s\n", s.second.first / frames, double(s.second.second) / frames, s.first.c_str());
    }
  };

}
#endif
