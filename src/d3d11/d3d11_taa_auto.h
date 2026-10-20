#pragma once

// d3d11.upscalerAuto: finds a game's temporal anti-aliasing pass without a
// profile, for DXUP's upscalers to run in its place.
//
// The pass is the compute dispatch that reads back, in the next frame, a
// texture it wrote (its history), together with a depth and a velocity
// texture, and writes a color texture at least as large as its color input;
// of several, the last in the frame (temporal SSR or AO run earlier). Over
// the following frames the context hands the pass's constant buffers over;
// in them the jitter is the float pair that follows the Halton (2, 3)
// sequence, the view size a constant integer pair of the color's aspect,
// and ClipToPrevClip the matrix close to identity that moves with the
// camera. A float velocity texture (other engines than Unreal Engine 4)
// is then read in whichever of six ways (UV, NDC, pixels, either sign)
// reprojects the previous frame's color best, measured on the GPU while
// the game's pass still runs. The analysis runs on a thread of its own;
// the result is kept in dxup_upscaler_auto.txt next to the DLL for the
// next start.

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../util/util_types.h"
#include "../d3d12/d3d12_backend_context.h"

namespace dxup {

  namespace taa_auto {

    inline float halton(uint32_t index, uint32_t base) {
      float f = 1.0f, r = 0.0f;
      while (index) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
      }
      return r;
    }

    // Per frame, the bytes of each constant buffer slot (empty when unbound).
    using FrameSnapshot = std::vector<std::vector<uint8_t>>;

    inline bool floatAt(const FrameSnapshot& frame, uint32_t slot, uint32_t offset, float& value) {
      if (slot >= frame.size() || offset + 4 > frame[slot].size())
        return false;
      std::memcpy(&value, frame[slot].data() + offset, 4);
      return std::isfinite(value);
    }

    struct Jitter {
      uint32_t slot = 0, offset = 0;
      float    scale[2] = { 1.0f, 1.0f };    // to render pixels
      bool     found = false;
    };

    // The float pair that follows Halton (2, 3) minus 0.5 with some period,
    // phase and scale (render pixels, UV or NDC units).
    inline Jitter findJitter(const std::vector<FrameSnapshot>& frames, float renderWidth, float renderHeight) {
      Jitter best;
      float bestError = 0.02f;
      const size_t n = frames.size();
      if (n < 16)
        return best;
      const uint32_t slots = uint32_t(frames[0].size());
      std::vector<float> x(n), y(n);
      for (uint32_t slot = 0; slot < slots; slot++) {
        const uint32_t size = uint32_t(frames[0][slot].size());
        for (uint32_t offset = 0; offset + 8 <= size; offset += 4) {
          bool usable = true;
          uint32_t changes = 0;
          float maxAbs = 0.0f;
          for (size_t i = 0; i < n && usable; i++) {
            usable = floatAt(frames[i], slot, offset, x[i]) && floatAt(frames[i], slot, offset + 4, y[i])
                  && std::fabs(x[i]) <= 1.0f && std::fabs(y[i]) <= 1.0f;
            if (usable && i && (x[i] != x[i - 1] || y[i] != y[i - 1]))
              changes++;
            maxAbs = (std::max)(maxAbs, (std::max)(std::fabs(x[i]), std::fabs(y[i])));
          }
          if (!usable || changes < (n - 1) * 3 / 4 || maxAbs == 0.0f)
            continue;
          // Period and phase of the sequence; x may use base 2 or 3.
          for (uint32_t period = 4; period <= 256; period = period < 64 ? period + 1 : period * 2) {
            for (uint32_t phase = 0; phase < period; phase++) {
              for (uint32_t swap = 0; swap < 2; swap++) {
                double sxh = 0, shh = 0, syk = 0, skk = 0, sxx = 0, syy = 0;
                for (size_t i = 0; i < n; i++) {
                  const uint32_t index = uint32_t((phase + i) % period) + 1;
                  const double h = halton(index, swap ? 3 : 2) - 0.5;
                  const double k = halton(index, swap ? 2 : 3) - 0.5;
                  sxh += x[i] * h; shh += h * h; syk += y[i] * k; skk += k * k;
                  sxx += double(x[i]) * x[i]; syy += double(y[i]) * y[i];
                }
                if (shh <= 0 || skk <= 0 || sxx <= 0 || syy <= 0)
                  continue;
                const double sx = sxh / shh, sy = syk / skk;
                // Residual energy relative to the values' energy.
                const double ex = (sxx - sx * sxh) / sxx, ey = (syy - sy * syk) / syy;
                const float error = float((std::max)(ex, ey));
                if (error < bestError) {
                  bestError = error;
                  best.found = true;
                  best.slot = slot;
                  best.offset = offset;
                  // Units: pixels (scale near 1), UV (1 / width) or NDC (2 / width, y up).
                  const double ratio = std::fabs(sx) * renderWidth;
                  if (std::fabs(sx) >= 0.25) {
                    best.scale[0] = best.scale[1] = 1.0f;
                  } else if (ratio > 1.5 && ratio < 2.5) {
                    best.scale[0] = renderWidth * 0.5f;
                    best.scale[1] = -renderHeight * 0.5f;
                  } else {
                    best.scale[0] = renderWidth;
                    best.scale[1] = renderHeight;
                  }
                }
              }
            }
          }
        }
      }
      return best;
    }

    struct ViewSize {
      uint32_t slot = 0, offset = 0;
      bool     found = false;
    };

    // A constant integer pair no larger than the color texture, at least a
    // quarter of it, of the output's aspect; better with its reciprocals next.
    inline ViewSize findViewSize(const std::vector<FrameSnapshot>& frames, float texWidth, float texHeight,
                                 float outWidth, float outHeight) {
      ViewSize best;
      int bestScore = 0;
      if (frames.empty() || outHeight <= 0.0f)
        return best;
      const float aspect = outWidth / outHeight;
      const FrameSnapshot& first = frames[0];
      for (uint32_t slot = 0; slot < first.size(); slot++) {
        for (uint32_t offset = 0; offset + 8 <= first[slot].size(); offset += 4) {
          float a = 0.0f, b = 0.0f;
          if (!floatAt(first, slot, offset, a) || !floatAt(first, slot, offset + 4, b))
            continue;
          if (a < texWidth * 0.25f || a > texWidth + 0.5f || b < texHeight * 0.25f || b > texHeight + 0.5f
           || std::fabs(a - std::round(a)) > 1e-3f || std::fabs(b - std::round(b)) > 1e-3f
           || std::fabs(a / b - aspect) > aspect * 0.02f)
            continue;
          bool constant = true;
          for (const FrameSnapshot& frame : frames) {
            float fa = 0.0f, fb = 0.0f;
            constant = constant && floatAt(frame, slot, offset, fa) && floatAt(frame, slot, offset + 4, fb)
                    && fa == a && fb == b;
          }
          if (!constant)
            continue;
          int score = 1;
          float ra = 0.0f, rb = 0.0f;
          if (floatAt(first, slot, offset + 8, ra) && floatAt(first, slot, offset + 12, rb)
           && std::fabs(ra * a - 1.0f) < 0.01f && std::fabs(rb * b - 1.0f) < 0.01f)
            score = 2;
          if (score > bestScore) {
            bestScore = score;
            best.found = true;
            best.slot = slot;
            best.offset = offset;
          }
        }
      }
      return best;
    }

    struct Matrix {
      uint32_t slot = 0, offset = 0;
      bool     found = false;
      bool     moved = false;    // differed from identity in some frame
    };

    // ClipToPrevClip: 16 floats close to identity in most frames, not
    // exactly identity in all of them once the camera moved.
    inline Matrix findClipToPrevClip(const std::vector<FrameSnapshot>& frames) {
      Matrix best;
      uint32_t bestMoved = 0;
      if (frames.empty())
        return best;
      const FrameSnapshot& first = frames[0];
      for (uint32_t slot = 0; slot < first.size(); slot++) {
        for (uint32_t offset = 0; offset + 64 <= first[slot].size(); offset += 16) {
          uint32_t closeFrames = 0, moved = 0, readable = 0;
          for (const FrameSnapshot& frame : frames) {
            float m[16];
            bool ok = true;
            for (uint32_t i = 0; i < 16 && ok; i++)
              ok = floatAt(frame, slot, offset + i * 4, m[i]);
            if (!ok)
              break;
            readable++;
            float deviation = 0.0f;
            for (uint32_t i = 0; i < 16; i++)
              deviation = (std::max)(deviation, std::fabs(m[i] - ((i % 5) == 0 ? 1.0f : 0.0f)));
            if (deviation < 0.3f)
              closeFrames++;
            if (deviation > 1e-5f)
              moved++;
          }
          if (readable != frames.size() || closeFrames * 5 < readable * 4)
            continue;
          if (!best.found || moved > bestMoved) {
            best.found = true;
            best.slot = slot;
            best.offset = offset;
            best.moved = moved > 0;
            bestMoved = moved;
          }
        }
      }
      return best;
    }

  }

  class AutoTaa {
  public:
    // A texture a dispatch reads or writes.
    struct TextureUse {
      ID3D11Resource* resource = nullptr;
      DXGI_FORMAT     viewFormat = DXGI_FORMAT_UNKNOWN;
      UINT            width = 0, height = 0;
      UINT            slot = 0;
      bool            depth = false;          // a depth-stencil texture
    };

    struct Profile {
      uint64_t shaderHash = 0;
      UINT     colorSlot = 0, depthSlot = 0, velocitySlot = 0, outputSlot = 0;
      bool     viewSizeFromTexture = true;
      UINT     viewSizeBuffer = 0, viewSizeOffset = 0;
      UINT     jitterBuffer = 0, jitterOffset = 0;
      float    jitterScale[2] = { 1.0f, 1.0f };
      UINT     clipBuffer = 0, clipOffset = 0;
      // 0: Unreal Engine 4's encoded velocity; 1: float velocity read as
      // candidate velocityCandidate (d3d12::velocityCandidateScale).
      UINT     velocityMode = 0, velocityCandidate = 0;
    };

    enum class State { Searching, Sampling, Analyzing, Calibrating, Found, Failed };

    AutoTaa() { loadProfile(); }

    ~AutoTaa() {
      if (m_analysis.joinable())
        m_analysis.join();
    }

    State state() const { return m_state; }
    // Searched that many frames without seeing any pass that reads back its
    // own output: the game has no TAA (d3d11.upscalerInject takes over).
    bool noTaaFor(uint32_t frames) const {
      return m_state == State::Searching && !m_everCandidate && m_searchFrames >= frames;
    }
    // While calibrating: the pass whose velocity is measured, and where the
    // worker adds the measurements.
    const Profile& candidateProfile() const { return m_profile; }
    const std::shared_ptr<d3d12::VelocityCalibration>& calibration() const { return m_calibration; }
    const Profile* profile() const { return m_state == State::Found ? &m_profile : nullptr; }

    // Every compute dispatch while searching or sampling.
    void observeDispatch(uint64_t hash, const TextureUse* srvs, size_t srvCount, const TextureUse* uavs, size_t uavCount) {
      if (m_state != State::Searching || !hash || !uavCount)
        return;
      m_dispatchIndex++;
      auto& written = m_writtenNow[hash];
      for (size_t i = 0; i < uavCount; i++)
        written.insert(uavs[i].resource);
      auto previous = m_writtenBefore.find(hash);
      if (previous == m_writtenBefore.end())
        return;
      // Reads what it (or a copy of it) wrote the frame before.
      const TextureUse* history = nullptr;
      for (size_t i = 0; i < srvCount && !history; i++) {
        if (previous->second.count(srvs[i].resource))
          history = &srvs[i];
      }
      if (!history)
        return;
      Candidate candidate;
      if (!classify(hash, srvs, srvCount, uavs, uavCount, history, candidate))
        return;
      candidate.order = m_dispatchIndex;
      m_candidatesNow[hash] = candidate;
    }

    // CopyResource: copies of what a dispatch wrote count as its writes.
    void noteCopy(ID3D11Resource* dst, ID3D11Resource* src) {
      if (m_state != State::Searching)
        return;
      for (auto* written : { &m_writtenNow, &m_writtenBefore }) {
        for (auto& entry : *written) {
          if (entry.second.count(src)) {
            entry.second.insert(dst);
            return;
          }
        }
      }
    }

    // The profile's pass ran (the upscaler replaced it or tried to).
    void noteProfileUsed() { m_framesUnused = 0; }

    // While sampling: whether this dispatch's constant buffers are wanted
    // (the pass's first dispatch in a frame).
    bool wantsSnapshot(uint64_t hash) {
      if (m_state != State::Sampling || hash != m_profile.shaderHash || m_snapshotTaken)
        return false;
      m_snapshotTaken = true;
      return true;
    }

    // Filled on the worker thread.
    struct SnapshotSink {
      std::mutex                          mutex;
      std::vector<taa_auto::FrameSnapshot> frames;
      std::vector<uint64_t>               frameIndex;
    };
    std::shared_ptr<SnapshotSink> sink() const { return m_sink; }
    uint64_t frameIndex() const { return m_frame; }

    // At each Present.
    void endFrame() {
      m_frame++;
      m_snapshotTaken = false;
      switch (m_state) {
        case State::Searching:  endSearchFrame(); break;
        case State::Sampling:   endSampleFrame(); break;
        case State::Analyzing:  endAnalysisFrame(); break;
        case State::Calibrating: endCalibrationFrame(); break;
        case State::Found:
          // A stored profile of another version of the game: search again.
          if (m_fromFile && ++m_framesUnused > 3600) {
            logMessage(LogLevel::Warn, "upscalerAuto: the stored pass %016llx did not run for a minute, searching again",
              static_cast<unsigned long long>(m_profile.shaderHash));
            m_fromFile = false;
            m_state = State::Searching;
          }
          break;
        default:                break;
      }
    }

  private:
    struct Candidate {
      Profile  profile;
      uint64_t order = 0;
      float    renderWidth = 0, renderHeight = 0;    // color texture
      float    outputWidth = 0, outputHeight = 0;
      DXGI_FORMAT velocityFormat = DXGI_FORMAT_UNKNOWN;
    };

    static bool isDepthRead(DXGI_FORMAT format) {
      return format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || format == DXGI_FORMAT_R32_FLOAT
          || format == DXGI_FORMAT_R16_UNORM || format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    }
    static bool isVelocityFormat(DXGI_FORMAT format) {
      return format == DXGI_FORMAT_R16G16_UNORM || format == DXGI_FORMAT_R16G16B16A16_UNORM
          || format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R16G16_SNORM
          || format == DXGI_FORMAT_R32G32_FLOAT;
    }
    // Unreal Engine 4's encoding (0.499 * 0.5 * v + 0.5, x = 0 static), the
    // one DXUP's motion pass decodes.
    static bool isEncodedVelocity(DXGI_FORMAT format) {
      return format == DXGI_FORMAT_R16G16_UNORM || format == DXGI_FORMAT_R16G16B16A16_UNORM;
    }
    static bool isColorFormat(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_R32G32B32A32_FLOAT:
          return true;
        default:
          return false;
      }
    }

    // Slots of the pass's inputs and output, or false when it does not look
    // like temporal anti-aliasing.
    static bool classify(uint64_t hash, const TextureUse* srvs, size_t srvCount, const TextureUse* uavs,
                         size_t uavCount, const TextureUse* history, Candidate& out) {
      const TextureUse* depth = nullptr;
      const TextureUse* velocity = nullptr;
      const TextureUse* color = nullptr;
      for (size_t i = 0; i < srvCount; i++) {
        const TextureUse& use = srvs[i];
        if (&use == history || use.resource == history->resource)
          continue;
        if (use.depth && isDepthRead(use.viewFormat)) {
          if (!depth)
            depth = &use;
        } else if (isVelocityFormat(use.viewFormat)) {
          if (!velocity)
            velocity = &use;
        } else if (isColorFormat(use.viewFormat)) {
          // The largest color input that is not the history.
          if (!color || use.width * use.height > color->width * color->height)
            color = &use;
        }
      }
      if (!depth || !velocity || !color)
        return false;
      // The output: a color UAV of at least the color's size; one that is not
      // read back as history if there are two.
      const TextureUse* output = nullptr;
      for (size_t i = 0; i < uavCount; i++) {
        const TextureUse& use = uavs[i];
        if (!isColorFormat(use.viewFormat) || use.width < color->width || use.height < color->height)
          continue;
        if (!output || (output->resource == history->resource && use.resource != history->resource))
          output = &use;
      }
      if (!output)
        return false;
      out.profile = Profile();
      out.profile.shaderHash = hash;
      out.profile.colorSlot = color->slot;
      out.profile.depthSlot = depth->slot;
      out.profile.velocitySlot = velocity->slot;
      out.profile.outputSlot = output->slot;
      out.renderWidth = float(color->width);
      out.renderHeight = float(color->height);
      out.outputWidth = float(output->width);
      out.outputHeight = float(output->height);
      out.velocityFormat = velocity->viewFormat;
      return true;
    }

    void endSearchFrame() {
      // The last candidate of the frame; the same pass several frames in a row.
      const Candidate* last = nullptr;
      for (const auto& entry : m_candidatesNow) {
        if (!last || entry.second.order > last->order)
          last = &entry.second;
      }
      m_searchFrames++;
      if (last)
        m_everCandidate = true;
      if (last && last->profile.shaderHash == m_streakHash) {
        m_streak++;
      } else {
        m_streak = last ? 1 : 0;
        m_streakHash = last ? last->profile.shaderHash : 0;
      }
      if (last && m_streak >= 4) {
        m_candidate = *last;
        m_profile = last->profile;
        m_floatVelocity = !isEncodedVelocity(last->velocityFormat);
        m_state = State::Sampling;
        logMessage(LogLevel::Warn,
          "upscalerAuto: TAA pass %016llx: color t%u (%.0fx%.0f), depth t%u, velocity t%u (fmt %u), output u%u "
          "(%.0fx%.0f); reading its constants", m_profile.shaderHash, m_profile.colorSlot, last->renderWidth,
          last->renderHeight, m_profile.depthSlot, m_profile.velocitySlot, unsigned(last->velocityFormat),
          m_profile.outputSlot, last->outputWidth, last->outputHeight);
      }
      m_writtenBefore.swap(m_writtenNow);
      m_writtenNow.clear();
      m_candidatesNow.clear();
      m_dispatchIndex = 0;
    }

    void endSampleFrame() {
      size_t count;
      {
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        // Consecutive frames only: a gap (menu, loading) starts over.
        auto& frames = m_sink->frames;
        auto& index = m_sink->frameIndex;
        for (size_t i = 1; i < index.size(); i++) {
          if (index[i] != index[i - 1] + 1) {
            frames.erase(frames.begin(), frames.begin() + i);
            index.erase(index.begin(), index.begin() + i);
            i = 0;
          }
        }
        count = frames.size();
      }
      if (++m_sampleFrames > 3000) {
        fail("upscalerAuto: the TAA pass's constants could not be read (no CPU copy?) - the game's pass keeps running");
        return;
      }
      if (count < 48)
        return;
      std::vector<taa_auto::FrameSnapshot> frames;
      {
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        frames = m_sink->frames;
        m_sink->frames.clear();
        m_sink->frameIndex.clear();
      }
      m_state = State::Analyzing;
      m_analysisDone = false;
      const Candidate candidate = m_candidate;
      m_analysis = std::thread([this, frames = std::move(frames), candidate]() {
        Profile profile = candidate.profile;
        const auto viewSize = taa_auto::findViewSize(frames, candidate.renderWidth, candidate.renderHeight,
          candidate.outputWidth, candidate.outputHeight);
        float renderWidth = candidate.renderWidth, renderHeight = candidate.renderHeight;
        if (viewSize.found) {
          profile.viewSizeFromTexture = false;
          profile.viewSizeBuffer = viewSize.slot;
          profile.viewSizeOffset = viewSize.offset;
          taa_auto::floatAt(frames[0], viewSize.slot, viewSize.offset, renderWidth);
          taa_auto::floatAt(frames[0], viewSize.slot, viewSize.offset + 4, renderHeight);
        }
        const auto jitter = taa_auto::findJitter(frames, renderWidth, renderHeight);
        const auto clip = taa_auto::findClipToPrevClip(frames);
        if (jitter.found) {
          profile.jitterBuffer = jitter.slot;
          profile.jitterOffset = jitter.offset;
          profile.jitterScale[0] = jitter.scale[0];
          profile.jitterScale[1] = jitter.scale[1];
        }
        if (clip.found) {
          profile.clipBuffer = clip.slot;
          profile.clipOffset = clip.offset;
        }
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result = profile;
        m_resultJitter = jitter.found;
        m_resultClip = clip.found;
        m_resultClipMoved = clip.moved;
        m_resultViewSize = viewSize.found;
        m_analysisDone = true;
      });
    }

    void endAnalysisFrame() {
      if (!m_analysisDone)
        return;
      if (m_analysis.joinable())
        m_analysis.join();
      std::lock_guard<std::mutex> lock(m_resultMutex);
      if (!m_resultJitter) {
        fail("upscalerAuto: no Halton jitter in the TAA pass's constants - the game's pass keeps running");
        return;
      }
      if (m_floatVelocity) {
        // The velocity's reading next, measured against the image.
        m_profile = m_result;
        m_profile.velocityMode = 1;
        m_calibration = std::make_shared<d3d12::VelocityCalibration>();
        m_calibrationFrames = 0;
        m_state = State::Calibrating;
        logMessage(LogLevel::Warn, "upscalerAuto: measuring how the pass's float velocity reads");
        return;
      }
      if (!m_resultClip || !m_resultClipMoved) {
        // Not before the camera has moved: sample again.
        if (++m_analysisRounds < 20) {
          m_state = State::Sampling;
          return;
        }
        if (!m_resultClip) {
          fail("upscalerAuto: no ClipToPrevClip in the TAA pass's constants - the game's pass keeps running");
          return;
        }
      }
      m_profile = m_result;
      m_state = State::Found;
      logMessage(LogLevel::Warn,
        "upscalerAuto: profile %016llx: view size %s cb%u+%u, jitter cb%u+%u (x %.3f, %.3f), ClipToPrevClip cb%u+%u",
        m_profile.shaderHash, m_profile.viewSizeFromTexture ? "texture" : "", m_profile.viewSizeBuffer,
        m_profile.viewSizeOffset, m_profile.jitterBuffer, m_profile.jitterOffset, m_profile.jitterScale[0],
        m_profile.jitterScale[1], m_profile.clipBuffer, m_profile.clipOffset);
      saveProfile();
    }

    // Enough frames with motion: the candidate that reprojects clearly best,
    // and clearly better than no motion.
    void endCalibrationFrame() {
      double error[6], still;
      uint32_t frames;
      {
        std::lock_guard<std::mutex> lock(m_calibration->mutex);
        std::memcpy(error, m_calibration->error, sizeof(error));
        still = m_calibration->still;
        frames = m_calibration->frames;
      }
      if (++m_calibrationFrames > 3600) {
        fail("upscalerAuto: the velocity's reading stayed unclear (too little motion?) - the game's pass keeps running");
        return;
      }
      if (frames < 30)
        return;
      uint32_t best = 0;
      for (uint32_t k = 1; k < 6; k++) {
        if (error[k] < error[best])
          best = k;
      }
      uint32_t second = best ? 0 : 1;
      for (uint32_t k = 0; k < 6; k++) {
        if (k != best && error[k] < error[second])
          second = k;
      }
      logMessage(LogLevel::Info, "upscalerAuto: velocity errors over %u frames: still %.3f, %.3f %.3f %.3f %.3f %.3f %.3f",
        frames, still / frames, error[0] / frames, error[1] / frames, error[2] / frames, error[3] / frames,
        error[4] / frames, error[5] / frames);
      if (error[best] < still * 0.85 && error[best] < error[second] * 0.9) {
        m_profile.velocityCandidate = best;
        m_state = State::Found;
        static const char* const kNames[] = { "UV, current - previous", "UV, previous - current",
          "NDC, current - previous", "NDC, previous - current", "pixels, current - previous", "pixels, previous - current" };
        logMessage(LogLevel::Warn, "upscalerAuto: profile %016llx: velocity %s; view size %s cb%u+%u, jitter cb%u+%u "
          "(x %.3f, %.3f)", m_profile.shaderHash, kNames[best], m_profile.viewSizeFromTexture ? "texture" : "",
          m_profile.viewSizeBuffer, m_profile.viewSizeOffset, m_profile.jitterBuffer, m_profile.jitterOffset,
          m_profile.jitterScale[0], m_profile.jitterScale[1]);
        saveProfile();
        return;
      }
      // Undecided: measure afresh.
      std::lock_guard<std::mutex> lock(m_calibration->mutex);
      std::fill(m_calibration->error, m_calibration->error + 6, 0.0);
      m_calibration->still = 0.0;
      m_calibration->frames = 0;
    }

    void fail(const char* message) {
      m_state = State::Failed;
      logMessage(LogLevel::Warn, "%s", message);
    }

    template<typename... Args>
    void logOnce(const char* format, Args... args) {
      if (m_logged)
        return;
      m_logged = true;
      logMessage(LogLevel::Warn, format, args...);
    }

    static std::string profilePath() {
      char path[MAX_PATH] = {};
      HMODULE module = nullptr;
      if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&taa_auto::halton), &module)
       || !GetModuleFileNameA(module, path, MAX_PATH))
        return std::string();
      std::string result = path;
      const size_t slash = result.find_last_of("\\/");
      return slash == std::string::npos ? std::string() : result.substr(0, slash + 1) + "dxup_upscaler_auto.txt";
    }

    void saveProfile() const {
      const std::string path = profilePath();
      FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
      if (!file)
        return;
      const Profile& p = m_profile;
      std::fprintf(file, "%016llx %u %u %u %u %d %u %u %u %u %.9g %.9g %u %u %u %u\n",
        static_cast<unsigned long long>(p.shaderHash), p.colorSlot, p.depthSlot, p.velocitySlot, p.outputSlot,
        p.viewSizeFromTexture ? 1 : 0, p.viewSizeBuffer, p.viewSizeOffset, p.jitterBuffer, p.jitterOffset,
        p.jitterScale[0], p.jitterScale[1], p.clipBuffer, p.clipOffset, p.velocityMode, p.velocityCandidate);
      std::fclose(file);
    }

    void loadProfile() {
      const std::string path = profilePath();
      FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
      if (!file)
        return;
      Profile p;
      unsigned long long hash = 0;
      int fromTexture = 1;
      const int read = std::fscanf(file, "%llx %u %u %u %u %d %u %u %u %u %f %f %u %u %u %u", &hash, &p.colorSlot,
        &p.depthSlot, &p.velocitySlot, &p.outputSlot, &fromTexture, &p.viewSizeBuffer, &p.viewSizeOffset,
        &p.jitterBuffer, &p.jitterOffset, &p.jitterScale[0], &p.jitterScale[1], &p.clipBuffer, &p.clipOffset,
        &p.velocityMode, &p.velocityCandidate);
      std::fclose(file);
      // Slots past the pipeline's (a damaged file) are not used.
      constexpr UINT kSrvs = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
      constexpr UINT kBuffers = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
      if ((read != 14 && read != 16) || !hash || p.velocityMode > 1 || p.velocityCandidate > 5
       || p.colorSlot >= kSrvs || p.depthSlot >= kSrvs || p.velocitySlot >= kSrvs || p.outputSlot >= D3D11_1_UAV_SLOT_COUNT
       || p.viewSizeBuffer >= kBuffers || p.jitterBuffer >= kBuffers || p.clipBuffer >= kBuffers
       || p.viewSizeOffset >= 65536 || p.jitterOffset >= 65536 || p.clipOffset >= 65536)
        return;
      p.shaderHash = hash;
      p.viewSizeFromTexture = fromTexture != 0;
      m_profile = p;
      m_state = State::Found;
      m_fromFile = true;
      logMessage(LogLevel::Info, "upscalerAuto: profile %016llx from %s", hash, path.c_str());
    }

    State    m_state = State::Searching;
    Profile  m_profile;
    uint64_t m_frame = 0;
    // Searching: what each compute shader wrote this and the previous frame.
    std::unordered_map<uint64_t, std::unordered_set<ID3D11Resource*>> m_writtenNow, m_writtenBefore;
    std::unordered_map<uint64_t, Candidate> m_candidatesNow;
    uint64_t m_dispatchIndex = 0;
    uint64_t m_streakHash = 0;
    uint32_t m_streak = 0;
    Candidate m_candidate;
    // Sampling and analysis.
    std::shared_ptr<SnapshotSink> m_sink = std::make_shared<SnapshotSink>();
    bool     m_snapshotTaken = false;
    uint32_t m_sampleFrames = 0;
    uint32_t m_analysisRounds = 0;
    std::thread m_analysis;
    std::atomic<bool> m_analysisDone{ false };
    std::mutex m_resultMutex;
    Profile  m_result;
    bool     m_resultJitter = false, m_resultClip = false, m_resultClipMoved = false, m_resultViewSize = false;
    bool     m_logged = false;
    bool     m_floatVelocity = false;
    bool     m_everCandidate = false;
    uint32_t m_searchFrames = 0;
    std::shared_ptr<d3d12::VelocityCalibration> m_calibration;
    uint32_t m_calibrationFrames = 0;
    bool     m_fromFile = false;
    uint32_t m_framesUnused = 0;
  };

}
