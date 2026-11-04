#pragma once

// The draw history of glitch reports (dxup_frame_dump.txt): each draw and clear
// of the immediate context leaves a compact record, the last frames' stay, and
// a frame that flickered is logged against the one before.

#include "../directx/directx_backend.h"
#include "../util/util_types.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dxup {

  enum class RecordOp : uint8_t { Draw, DrawIndexed, DrawAuto, DrawIndirect, Dispatch, ClearColor, ClearDepth };

  struct DrawRecord {
    RecordOp    op = RecordOp::Draw;
    bool        dropped = false;   // the pipeline could not be prepared: not drawn
    bool        swapChainTarget = false;
    uint8_t     targets = 0;       // bound render targets
    uint32_t    count = 0, instances = 0, start = 0;
    int32_t     base = 0;
    uint64_t    vs = 0, ps = 0;    // compute shader in ps for dispatches
    const void* target = nullptr;  // render target 0 (or the cleared view)
    const void* depth = nullptr;   // depth-stencil view
    uint16_t    width = 0, height = 0, format = 0;
    const void* srv[4] = {};
    uint16_t    srvWidth[4] = {}, srvHeight[4] = {}, srvFormat[4] = {};
    const void* cb[2] = {};        // vertex and pixel (or compute) b0
    uint32_t    cbFirst[2] = {};
    const void* blend = nullptr;
    const void* depthState = nullptr;
    const void* raster = nullptr;
    uint32_t    stencilRef = 0;
    float       viewport[2] = {};
    float       value[4] = {};     // clear color, or depth, stencil, flags
  };

  inline void describeImageView(IBackendImageView* view, uint16_t& width, uint16_t& height, uint16_t& format) {
    if (!view || !view->getImage())
      return;
    const ImageDesc& desc = view->getImage()->getDesc();
    width = uint16_t((std::min)(desc.width, 65535u));
    height = uint16_t((std::min)(desc.height, 65535u));
    format = uint16_t(view->getDesc().format ? view->getDesc().format : desc.format);
  }

  // NaN in GPU results the game reads back: shows as black or missing pixels
  // and spreads through the game's own math.
  inline void checkReadbackForNan(uint32_t& m_nanReadbacks, ID3D11Resource* resource, const void* data, UINT rowPitch, UINT rows, UINT width,
                                  DXGI_FORMAT format, bool buffer) {
    if (!data || m_nanReadbacks >= 16)
      return;
    UINT components = 0;
    bool half = false;
    switch (format) {
      case DXGI_FORMAT_R32_FLOAT: components = 1; break;
      case DXGI_FORMAT_R32G32_FLOAT: components = 2; break;
      case DXGI_FORMAT_R32G32B32A32_FLOAT: components = 4; break;
      case DXGI_FORMAT_R16_FLOAT: components = 1; half = true; break;
      case DXGI_FORMAT_R16G16_FLOAT: components = 2; half = true; break;
      case DXGI_FORMAT_R16G16B16A16_FLOAT: components = 4; half = true; break;
      default: return;
    }
    // Buffers: as floats (structured readbacks), up to 64 KB.
    const UINT values = buffer ? (std::min)(width, 65536u) / 4 : width * components;
    for (UINT y = 0; y < (buffer ? 1u : (std::min)(rows, 4096u)); y++) {
      const uint8_t* row = static_cast<const uint8_t*>(data) + size_t(y) * rowPitch;
      for (UINT i = 0; i < values; i++) {
        bool nan;
        if (half) {
          uint16_t h;
          std::memcpy(&h, row + size_t(i) * 2, 2);
          nan = (h & 0x7c00) == 0x7c00 && (h & 0x3ff);
        } else {
          float f;
          std::memcpy(&f, row + size_t(i) * 4, 4);
          nan = f != f;
        }
        if (!nan)
          continue;
        // Buffers of other data: only when they hold floats elsewhere too.
        m_nanReadbacks++;
        logMessage(LogLevel::Warn, "glitch: the game read NaN from GPU results: %s %p (%ux%u format %u), at %u, %u",
          buffer ? "buffer" : "texture", static_cast<void*>(resource), width, rows, unsigned(format),
          i / (components ? components : 1), y);
        return;
      }
    }
  }

  inline std::string describeRecord(const DrawRecord& r) {
    static const char* kOps[] = { "Draw", "DrawIndexed", "DrawAuto", "DrawIndirect", "Dispatch", "ClearColor", "ClearDepth" };
    char text[640];
    if (r.op == RecordOp::ClearColor || r.op == RecordOp::ClearDepth) {
      sprintf_s(text, "%s %p %ux%u/%u value=%g,%g,%g,%g", kOps[uint32_t(r.op)], r.target, r.width, r.height, r.format,
        r.value[0], r.value[1], r.value[2], r.value[3]);
      return text;
    }
    int length = sprintf_s(text, "%s%s n=%u x%u start=%u base=%d %s=%016llX", kOps[uint32_t(r.op)],
      r.dropped ? " DROPPED" : "", r.count, r.instances, r.start, r.base, r.op == RecordOp::Dispatch ? "cs" : "ps",
      static_cast<unsigned long long>(r.ps));
    if (r.op != RecordOp::Dispatch)
      length += sprintf_s(text + length, sizeof(text) - length, " vs=%016llX rt=%p %ux%u/%u targets=%u ds=%p vp=%gx%g blend=%p dss=%p rs=%p ref=%u",
        static_cast<unsigned long long>(r.vs), r.target, r.width, r.height, r.format, r.targets, r.depth,
        r.viewport[0], r.viewport[1], r.blend, r.depthState, r.raster, r.stencilRef);
    for (uint32_t slot = 0; slot < 4; slot++) {
      if (r.srv[slot])
        length += sprintf_s(text + length, sizeof(text) - length, " t%u=%p %ux%u/%u", slot, r.srv[slot],
          r.srvWidth[slot], r.srvHeight[slot], r.srvFormat[slot]);
    }
    sprintf_s(text + length, sizeof(text) - length, " cb=%p+%u,%p+%u", r.cb[0], r.cbFirst[0], r.cb[1], r.cbFirst[1]);
    return text;
  }

  // The same call in two frames: kind, shaders and the target's size and
  // format (the swap chain's buffers take turns from frame to frame).
  inline bool sameCall(const DrawRecord& a, const DrawRecord& b) {
    return a.op == b.op && a.vs == b.vs && a.ps == b.ps && a.width == b.width && a.height == b.height
        && a.format == b.format;
  }

  // What else differs between two records of the same call.
  inline std::string recordChanges(const DrawRecord& before, const DrawRecord& now) {
    std::string changes;
    char text[160];
    if (before.target != now.target && !(before.swapChainTarget && now.swapChainTarget)) {
      sprintf_s(text, " rt %p -> %p", before.target, now.target);
      changes += text;
    }
    if (before.dropped != now.dropped)
      changes += now.dropped ? " now DROPPED" : " was dropped";
    if (before.count != now.count || before.instances != now.instances || before.start != now.start || before.base != now.base) {
      sprintf_s(text, " count %u x%u start %u base %d -> %u x%u start %u base %d", before.count, before.instances,
        before.start, before.base, now.count, now.instances, now.start, now.base);
      changes += text;
    }
    for (uint32_t slot = 0; slot < 4; slot++) {
      if (before.srv[slot] != now.srv[slot]) {
        sprintf_s(text, " t%u %p %ux%u/%u -> %p %ux%u/%u", slot, before.srv[slot], before.srvWidth[slot],
          before.srvHeight[slot], before.srvFormat[slot], now.srv[slot], now.srvWidth[slot], now.srvHeight[slot],
          now.srvFormat[slot]);
        changes += text;
      }
    }
    for (uint32_t stage = 0; stage < 2; stage++) {
      if (before.cb[stage] != now.cb[stage]) {
        sprintf_s(text, " cb%u %p -> %p", stage, before.cb[stage], now.cb[stage]);
        changes += text;
      }
    }
    if (before.depth != now.depth) {
      sprintf_s(text, " ds %p -> %p", before.depth, now.depth);
      changes += text;
    }
    if (before.blend != now.blend || before.depthState != now.depthState || before.raster != now.raster
     || before.stencilRef != now.stencilRef) {
      sprintf_s(text, " states %p,%p,%p,%u -> %p,%p,%p,%u", before.blend, before.depthState, before.raster,
        before.stencilRef, now.blend, now.depthState, now.raster, now.stencilRef);
      changes += text;
    }
    if (std::memcmp(before.value, now.value, sizeof(now.value)) != 0) {
      sprintf_s(text, " value %g,%g,%g,%g -> %g,%g,%g,%g", before.value[0], before.value[1], before.value[2],
        before.value[3], now.value[0], now.value[1], now.value[2], now.value[3]);
      changes += text;
    }
    return changes;
  }

  class DrawHistory {
  public:
    static constexpr uint32_t kHistoryFrames = 8;

    void add(const DrawRecord& record) { m_history[m_historyFrame % kHistoryFrames].push_back(record); }
    void nextFrame() {
      m_historyFrame++;
      m_history[m_historyFrame % kHistoryFrames].clear();
    }
    uint32_t frame() const { return m_historyFrame; }

    // The number of calls of the frame presented `back` presents ago.
    size_t calls(uint32_t back) const {
      if (back >= kHistoryFrames || m_historyFrame < back)
        return SIZE_MAX;
      return m_history[(m_historyFrame - back) % kHistoryFrames].size();
    }

    // The calls of the frame presented `back` presents ago (0: the frame
    // being presented) against the frame before it, into the log (glitch
    // `event` of the swap chain, at its present `present`). With `full`,
    // also every call of the glitch frame.
    void log(uint32_t back, uint32_t present, uint32_t event, bool full) {
      if (back + 1 >= kHistoryFrames || m_historyFrame < back + 1)
        return;
      const auto& now = m_history[(m_historyFrame - back) % kHistoryFrames];
      const auto& before = m_history[(m_historyFrame - back - 1) % kHistoryFrames];
      uint32_t dropped = 0;
      for (const auto& r : now)
        dropped += r.dropped ? 1u : 0u;
      logMessage(LogLevel::Warn, "glitch %u: the frame before (present %u) has %zu calls, the glitch frame (present %u) %zu, %u of them dropped; differences:",
        event, present - 1, before.size(), present, now.size(), dropped);
      // Calls match in order by kind, shaders and target; a call with no
      // match among the next 64 is in one frame only.
      constexpr size_t kWindow = 64;
      uint32_t lines = 0, suppressed = 0;
      auto line = [&](const char* what, size_t index, const std::string& text) {
        if (lines++ < 200)
          logMessage(LogLevel::Warn, "glitch %u:   %s #%zu %s", event, what, index, text.c_str());
        else
          suppressed++;
      };
      size_t i = 0, j = 0;
      while (i < before.size() || j < now.size()) {
        if (i < before.size() && j < now.size() && sameCall(before[i], now[j])) {
          const std::string changes = recordChanges(before[i], now[j]);
          if (!changes.empty())
            line("changed", j, describeRecord(now[j]) + " |" + changes);
          i++, j++;
          continue;
        }
        size_t aheadNow = j, aheadBefore = i;
        while (aheadNow < now.size() && aheadNow < j + kWindow && !(i < before.size() && sameCall(before[i], now[aheadNow])))
          aheadNow++;
        if (i < before.size() && aheadNow < now.size() && aheadNow < j + kWindow) {
          for (; j < aheadNow; j++)
            line("only in the glitch frame", j, describeRecord(now[j]));
          continue;
        }
        while (aheadBefore < before.size() && aheadBefore < i + kWindow && !(j < now.size() && sameCall(before[aheadBefore], now[j])))
          aheadBefore++;
        if (j < now.size() && aheadBefore < before.size() && aheadBefore < i + kWindow) {
          for (; i < aheadBefore; i++)
            line("only in the frame before", i, describeRecord(before[i]));
          continue;
        }
        if (i < before.size())
          line("only in the frame before", i, describeRecord(before[i])), i++;
        if (j < now.size())
          line("only in the glitch frame", j, describeRecord(now[j])), j++;
      }
      if (suppressed)
        logMessage(LogLevel::Warn, "glitch %u:   %u more differences", event, suppressed);
      if (!lines)
        logMessage(LogLevel::Warn, "glitch %u:   none: the same calls with the same objects (the glitch comes from the data or the GPU)", event);
      if (full) {
        for (size_t index = 0; index < now.size() && index < 4000; index++)
          logMessage(LogLevel::Warn, "glitch %u: call #%zu %s", event, index, describeRecord(now[index]).c_str());
      }
    }

  private:
    std::vector<DrawRecord> m_history[kHistoryFrames];
    uint32_t                m_historyFrame = 0;
  };

}
