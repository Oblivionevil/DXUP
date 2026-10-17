#pragma once

// d3d11.upscalerInject: temporal anti-aliasing with DXUP's upscalers for
// games that have none, without a profile.
//
// - The scene: draws that test depth against the depth buffer most draws
//   with a color target use (not a shadow map); its color is the target
//   they draw into most.
// - Jitter: those draws get the viewport shifted by a Halton (2, 3) offset
//   of under a pixel (no shader changes; passes without depth stay put).
// - Motion: one of the vertex shader constant buffers of the scene draws
//   holds the camera (each slot as bound at the first scene draw of a frame
//   that binds it). Of their 4x4 blocks (either layout), the one whose
//   frame-to-frame ClipToPrevClip (current inverse times previous)
//   reprojects the previous frame's color best is the camera, all
//   candidates measured in the same frames (half a second of camera
//   motion or so); motion is the camera's, from depth. Before that, while
//   the camera buffers stand still, the upscaler runs without motion; an
//   image that moves all the same (the camera is not in them) ends that.
//   Moving objects get only the camera's motion.
// - Where: before the first draw or dispatch after scene draws that reads
//   the scene color (post-processing; scene-depth draws after it, such as
//   health bars, are interface), else before the first one that follows
//   the last scene draw, learnt from earlier frames by its shader; at
//   Present when nothing follows.
// - Menus, loading screens and still cameras: the scene is looked for
//   again after a pause, and again when it is gone (new targets).
// The upscaler runs at the scene's own size (DLAA, FSR and XeSS anti-
// aliasing); dxgi.renderScale adds the speed. Camera, place and depth
// direction are kept in dxup_upscaler_inject.txt next to the DLL.

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../util/util_types.h"
#include "../d3d12/d3d12_backend_context.h"

namespace dxup {

  namespace taa_inject {

    inline float halton(uint32_t index, uint32_t base) {
      float f = 1.0f, r = 0.0f;
      for (; index; index /= base) {
        f /= float(base);
        r += f * float(index % base);
      }
      return r;
    }

    // 4x4 inverse (rows); false when singular.
    inline bool invert(const float m[16], float out[16]) {
      float inv[16];
      inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
      inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
      inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
      inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
      inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
      inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
      inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
      inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
      inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
      inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
      inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
      inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
      inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
      inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
      inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
      inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
      const double det = double(m[0]) * inv[0] + double(m[1]) * inv[4] + double(m[2]) * inv[8] + double(m[3]) * inv[12];
      if (!std::isfinite(det) || std::fabs(det) < 1e-20)
        return false;
      for (int i = 0; i < 16; i++)
        out[i] = float(inv[i] / det);
      return true;
    }

    inline void multiply(const float a[16], const float b[16], float out[16]) {
      for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
          out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    }

    // The camera matrix at offset in a buffer's bytes, rows as stored
    // (layout 0, pos * M) or transposed (layout 1, M * pos).
    inline bool matrixAt(const std::vector<uint8_t>& bytes, uint32_t offset, uint32_t layout, float m[16]) {
      if (offset + 64 > bytes.size())
        return false;
      float a[16];
      std::memcpy(a, bytes.data() + offset, 64);
      for (int i = 0; i < 16; i++) {
        if (!std::isfinite(a[i]))
          return false;
        m[i] = layout ? a[(i % 4) * 4 + i / 4] : a[i];
      }
      return true;
    }

    // ClipToPrevClip rows for the motion pass: current inverse times previous.
    inline bool clipToPrevClip(const std::vector<uint8_t>& current, const std::vector<uint8_t>& previous,
                               uint32_t offset, uint32_t layout, float rows[16]) {
      float cur[16], prev[16], inv[16];
      if (!matrixAt(current, offset, layout, cur) || !matrixAt(previous, offset, layout, prev) || !invert(cur, inv))
        return false;
      multiply(inv, prev, rows);
      for (int i = 0; i < 16; i++) {
        if (!std::isfinite(rows[i]))
          return false;
      }
      return true;
    }

    // The vertex shader constant buffers of the scene by slot and frame.
    struct CameraStore {
      static constexpr uint32_t kSlots = 14;
      std::mutex mutex;
      std::map<uint64_t, std::vector<uint8_t>> frames[kSlots];

      void put(uint32_t slot, uint64_t frame, std::vector<uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        frames[slot][frame] = std::move(bytes);
        while (frames[slot].size() > 64)
          frames[slot].erase(frames[slot].begin());
      }
      bool pair(uint32_t slot, uint64_t frame, std::vector<uint8_t>& current, std::vector<uint8_t>& previous) {
        std::lock_guard<std::mutex> lock(mutex);
        if (slot >= kSlots)
          return false;
        auto cur = frames[slot].find(frame);
        auto prev = frames[slot].find(frame - 1);
        if (cur == frames[slot].end() || prev == frames[slot].end())
          return false;
        current = cur->second;
        previous = prev->second;
        return true;
      }
    };

    struct Candidate {
      uint32_t offset = 0, layout = 0, slot = 0;
    };

    // Whether the frame's camera buffers changed against the frame before
    // in a block that could be a camera (an invertible matrix with a w
    // column other than 0, 0, 0, 1 in either layout): 1 changed, 0 stood
    // still, -1 nothing to tell.
    inline int cameraBuffersMoved(CameraStore& store, uint64_t frame) {
      int result = -1;
      std::lock_guard<std::mutex> lock(store.mutex);
      for (uint32_t slot = 0; slot < CameraStore::kSlots; slot++) {
        auto cur = store.frames[slot].find(frame);
        auto prev = store.frames[slot].find(frame - 1);
        if (cur == store.frames[slot].end() || prev == store.frames[slot].end())
          continue;
        const std::vector<uint8_t>& now = cur->second;
        const std::vector<uint8_t>& before = prev->second;
        for (size_t offset = 0; offset + 64 <= now.size(); offset += 16) {
          float a[16], inverse[16];
          std::memcpy(a, now.data() + offset, 64);
          const bool finite = std::all_of(a, a + 16, [](float v) { return std::isfinite(v); });
          const bool projective = std::fabs(a[3]) + std::fabs(a[7]) + std::fabs(a[11]) > 1e-3f
            || std::fabs(a[12]) + std::fabs(a[13]) + std::fabs(a[14]) > 1e-3f || std::fabs(a[15] - 1.0f) > 1e-3f;
          if (!finite || !projective || !invert(a, inverse))
            continue;
          if (offset + 64 > before.size() || std::memcmp(now.data() + offset, before.data() + offset, 64))
            return 1;
          result = 0;
        }
      }
      return result;
    }

  }

  class InjectTaa {
  public:
    enum class State { Observing, Calibrating, Active, Failed };

    InjectTaa() { load(); }

    State state() const { return m_state; }
    uint64_t frame() const { return m_frame; }
    const std::shared_ptr<taa_inject::CameraStore>& cameras() const { return m_cameras; }

    // The scene this frame (resources as identities only).
    ID3D11Resource* depth() const { return m_depth; }
    ID3D11Resource* color() const { return m_color; }
    bool depthInverted() const { return m_depthInverted; }

    // This frame's jitter in render pixels (x right, y down).
    void jitter(float out[2]) const {
      const uint32_t index = uint32_t(m_frame % 8) + 1;
      out[0] = taa_inject::halton(index, 2) - 0.5f;
      out[1] = taa_inject::halton(index, 3) - 0.5f;
    }

    // A draw: whether it belongs to the scene (tests depth against the
    // scene's depth buffer). Observing counts what the scene uses.
    bool observeDraw(ID3D11Resource* dsv, ID3D11Resource* rt0, bool depthTest, bool greater) {
      if (!dsv || !depthTest)
        return false;
      if (m_state == State::Observing && m_frame >= 2 && m_frame >= m_observeFrom) {
        Stats& stats = m_stats[dsv];
        stats.draws++;
        stats.greater += greater ? 1 : 0;
        if (rt0)
          stats.colors[rt0]++;
      }
      if (dsv != m_depth || !m_depth)
        return false;
      // The color target too: only seen in use this frame is it known alive.
      if (rt0 == m_color)
        m_colorSeen = true;
      return true;
    }

    // After a scene draw; and for any other draw or dispatch with its
    // shader's hash: whether DXUP's upscaler goes in before it.
    void noteSceneDraw() {
      m_sceneSeen = true;
      m_afterScene = 0;
      m_afterSceneSet = false;
    }
    bool beforeOtherEvent(uint64_t hash) {
      if (!m_sceneSeen)
        return false;
      if (!m_afterSceneSet) {
        m_afterScene = hash;
        m_afterSceneSet = true;
      }
      return (m_state == State::Active || m_state == State::Calibrating) && !m_inserted && m_haveInsert
          && m_colorSeen && hash == m_insertHash;
    }
    bool atPresent() const {
      return (m_state == State::Active || m_state == State::Calibrating) && m_sceneSeen && m_colorSeen
          && !m_inserted && m_haveInsert && m_insertHash == 0;
    }
    void markInserted() { m_inserted = true; }
    bool inserted() const { return m_inserted; }
    // While the place is learnt: the first draw or dispatch after scene
    // draws that reads the scene color (post-processing; later scene-depth
    // draws such as health bars belong to the interface).
    bool wantsColorReads() const { return m_state == State::Observing && m_depth && m_sceneSeen && !m_readSet; }
    void noteColorRead(uint64_t hash) {
      m_firstRead = hash;
      m_readSet = true;
    }
    // The vertex shader constant buffer slots still wanted this frame (each
    // from the first scene draw that binds it): all while the camera is
    // looked for, then the camera's.
    uint32_t cameraSlotsWanted() const {
      constexpr uint32_t kAll = (1u << taa_inject::CameraStore::kSlots) - 1;
      if (m_state == State::Calibrating)
        return kAll & ~m_slotsTaken;
      if (m_state == State::Active)
        return (1u << m_camera.slot) & ~m_slotsTaken;
      return 0;
    }
    void cameraSlotTaken(uint32_t slot) { m_slotsTaken |= 1u << slot; }

    // While calibrating: the candidates measured this frame.
    const std::vector<taa_inject::Candidate>& calibrationCandidates() const { return m_candidates; }
    const std::shared_ptr<d3d12::VelocityCalibration>& calibration() const { return m_calibration; }
    taa_inject::Candidate camera() const { return m_camera; }
    // Before the camera is known, while it stands still: the upscaler runs
    // with no motion. Every eighth frame the image is compared with the one
    // eight frames before (the same jitter) by the still probe.
    bool stillUpscaling() const { return m_state == State::Calibrating && m_stillMode; }
    bool stillProbeFrame() const { return (m_frame % 8) == 0; }
    // A change in the camera buffers drops the history (not for a quiet
    // image, whose buffers change all the time).
    bool resetOnCameraChange() const { return !m_quiet; }
    const std::shared_ptr<d3d12::VelocityCalibration>& stillProbe() const { return m_stillProbe; }
    // The upscaler's history does not lead up to this frame.
    bool takeHistoryReset() {
      const bool reset = m_historyReset;
      m_historyReset = false;
      return reset;
    }

    // At each Present.
    void endFrame() {
      if (m_sceneSeen && m_state == State::Observing && m_depth) {
        m_insertVotes[m_afterSceneSet ? m_afterScene : 0]++;
        if (m_readSet)
          m_readVotes[m_firstRead]++;
      }
      switch (m_state) {
        case State::Observing:   endObserveFrame(); break;
        case State::Calibrating: endCalibrateFrame(); break;
        default:                 break;
      }
      // Loading screens, menus, new targets after a resize: 300 frames
      // without the scene or without the upscaler's place, and the scene is
      // looked for again (the camera found stays known).
      if (m_state == State::Active || m_state == State::Calibrating) {
        m_lostFrames = m_sceneSeen && m_colorSeen && m_inserted ? 0 : m_lostFrames + 1;
        if (m_lostFrames >= 300)
          restart("upscalerInject: the scene is gone - looking for it again", 0);
      }
      m_frame++;
      m_sceneSeen = false;
      m_colorSeen = false;
      m_readSet = false;
      m_afterSceneSet = false;
      m_afterScene = 0;
      m_inserted = false;
      m_slotsTaken = 0;
    }

  private:
    struct Stats {
      uint32_t draws = 0, greater = 0;
      std::unordered_map<ID3D11Resource*, uint32_t> colors;
    };

    void endObserveFrame() {
      if (m_frame < m_observeFrom)
        return;
      m_observeFrames++;
      // 30 frames: the scene's depth, color, camera slot and depth direction.
      if (!m_depth && m_observeFrames >= 30) {
        // The depth buffer most draws with a color target test against
        // (not a shadow map, drawn without one).
        ID3D11Resource* best = nullptr;
        uint32_t bestDraws = 0;
        for (const auto& entry : m_stats) {
          uint32_t colored = 0;
          for (const auto& color : entry.second.colors)
            colored += color.second;
          if (colored > bestDraws) {
            bestDraws = colored;
            best = entry.first;
          }
        }
        if (!best || bestDraws < 30 * 10) {
          // Not yet a 3D scene (menus, loading screens): look again.
          uint32_t draws = 0;
          for (const auto& entry : m_stats)
            draws += entry.second.draws;
          if (draws && m_detailLogs < 8) {
            m_detailLogs++;
            logMessage(LogLevel::Warn, "upscalerInject: no scene yet (%zu depth buffers, %u depth-tested draws, at most %u "
              "with a color target on one)", m_stats.size(), draws, bestDraws);
          }
          lookAgain("upscalerInject: no 3D scene found");
          return;
        }
        const Stats& stats = m_stats[best];
        uint32_t colorDraws = 0;
        for (const auto& color : stats.colors) {
          if (color.second > colorDraws) {
            colorDraws = color.second;
            m_color = color.first;
          }
        }
        if (!m_color) {
          lookAgain("upscalerInject: the scene has no color target");
          return;
        }
        m_depth = best;
        m_depthInverted = stats.greater * 2 > stats.draws;
        logMessage(LogLevel::Warn, "upscalerInject: scene depth %p (%s, %u of %u draws), color %p",
          static_cast<void*>(m_depth), m_depthInverted ? "inverted" : "standard", colorDraws, stats.draws,
          static_cast<void*>(m_color));
        m_stats.clear();
        m_insertVotes.clear();
        m_readVotes.clear();
        m_observeFrames = 0;
        return;
      }
      // 30 more frames: what follows the scene.
      if (m_depth && m_observeFrames >= 30) {
        uint32_t votes = 0;
        for (const auto& vote : m_readVotes) {
          if (vote.second > votes) {
            votes = vote.second;
            m_insertHash = vote.first;
          }
        }
        // Without a pass that reads the scene: after its last draw.
        if (votes < 15) {
          votes = 0;
          for (const auto& vote : m_insertVotes) {
            if (vote.second > votes) {
              votes = vote.second;
              m_insertHash = vote.first;
            }
          }
        }
        m_haveInsert = votes > 0;
        if (!m_haveInsert) {
          fail("upscalerInject: no place after the scene found");
          return;
        }
        logMessage(LogLevel::Warn, "upscalerInject: upscaler before shader %016llx (%u of 30 frames)",
          static_cast<unsigned long long>(m_insertHash), votes);
        if (m_loaded) {
          m_camera = m_loadedCamera;
          m_state = State::Active;
          logMessage(LogLevel::Warn, "upscalerInject: camera from %s", path().c_str());
        } else {
          m_state = State::Calibrating;
          m_calibration = std::make_shared<d3d12::VelocityCalibration>();
        }
      }
    }

    // First the candidate blocks from the camera buffers of several frames
    // (invertible, ClipToPrevClip near identity, moving with the camera),
    // then all of them measured against the image in the same frames.
    void endCalibrateFrame() {
      updateStillMode();
      if (m_stillMode)
        return;
      if (++m_calibrateFrames > 7200) {
        // A still camera (menus): again later, from the scene on.
        restart("upscalerInject: the camera could not be measured (too little motion?) - again later", 3600);
        return;
      }
      // The search goes through every 4x4 block of every slot: twice a
      // second or so, not every frame.
      if (m_candidates.empty()) {
        if ((m_calibrateFrames % 30) == 0)
          findCandidates();
        return;
      }
      double error[d3d12::kCameraCandidates], still;
      uint32_t frames;
      {
        std::lock_guard<std::mutex> lock(m_calibration->mutex);
        std::memcpy(error, m_calibration->error, sizeof(error));
        still = m_calibration->still;
        frames = m_calibration->frames;
      }
      if (frames < 30)
        return;
      {
        std::lock_guard<std::mutex> lock(m_calibration->mutex);
        std::fill(std::begin(m_calibration->error), std::end(m_calibration->error), 0.0);
        m_calibration->still = 0.0;
        m_calibration->frames = 0;
      }
      // The best; candidates within 2 % of it describe the same motion (the
      // same matrix in two places, or one that differs only in precision),
      // and the best ranked of those wins. The next best is the best of the
      // rest.
      const size_t count = (std::min)(m_candidates.size(), size_t(d3d12::kCameraCandidates));
      size_t best = 0;
      for (size_t k = 1; k < count; k++) {
        if (error[k] < error[best])
          best = k;
      }
      const double tie = error[best] * 1.02;
      double second = 1e30;
      for (size_t k = 0; k < count; k++) {
        if (error[k] <= tie) {
          best = (std::min)(best, k);
          continue;
        }
        second = (std::min)(second, error[k]);
      }
      for (size_t k = 0; k < count; k++)
        logMessage(LogLevel::Info, "upscalerInject: candidate cb%u+%u layout %u: error %.3f, still %.3f",
          m_candidates[k].slot, m_candidates[k].offset, m_candidates[k].layout, error[k] / frames, still / frames);
      const taa_inject::Candidate winner = m_candidates[best];
      const double bestError = error[best] / frames, bestStill = still / frames, bestSecond = second / frames;
      logMessage(m_detailLogs++ < 48 ? LogLevel::Warn : LogLevel::Info,
        "upscalerInject: camera candidate cb%u+%u layout %u error %.3f (still %.3f, next %.3f)",
        winner.slot, winner.offset, winner.layout, bestError, bestStill, bestSecond);
      // Clearly better than no motion and than the others.
      if (bestError < bestStill * 0.85 && bestError < bestSecond * 0.9) {
        m_camera = winner;
        m_state = State::Active;
        m_historyReset = true;
        logMessage(LogLevel::Warn, "upscalerInject: camera matrix at VS cb%u+%u (%s); upscaling", m_camera.slot,
          m_camera.offset, m_camera.layout ? "column vectors" : "row vectors");
        save();
        return;
      }
      // Undecided: look for candidates again (more motion next time). An
      // image that hardly moves (a menu camera swaying a little, particles)
      // is upscaled meanwhile as a still one.
      if (bestStill < 0.03 && !m_quiet && !m_stillDisabled) {
        m_quiet = true;
        logMessage(LogLevel::Info, "upscalerInject: the image hardly moves (%.3f) - upscaling it as a still one",
          bestStill);
      }
      m_candidates.clear();
    }

    // Eight frames in a row with the camera buffers standing still start the
    // upscaler without motion; a change in them ends it (the motion is
    // measured), as does an image that moves all the same.
    void updateStillMode() {
      const int moved = taa_inject::cameraBuffersMoved(*m_cameras, m_frame);
      m_stillFrames = moved == 0 ? m_stillFrames + 1 : 0;
      // Upscaling a quiet image: the camera buffers change all the time,
      // only the image tells when it moves.
      if (m_stillMode && moved != 0 && !m_quiet) {
        m_stillMode = false;
        logMessage(LogLevel::Info, "upscalerInject: the camera moves - measuring it");
      }
      if (m_stillMode && m_stillProbe) {
        std::lock_guard<std::mutex> lock(m_stillProbe->mutex);
        if (m_stillProbe->frames >= 2 && m_stillProbe->still > 0.08 * m_stillProbe->frames) {
          m_stillMode = false;
          if (m_quiet) {
            // The quiet image moves: measure the camera again.
            m_quiet = false;
            logMessage(LogLevel::Info, "upscalerInject: the image moves - measuring the camera");
          } else {
            m_stillDisabled = true;
            logMessage(LogLevel::Warn, "upscalerInject: the image moves while the camera buffers stand still - "
              "no upscaling until the camera is found");
          }
        }
      }
      if (!m_stillMode && !m_stillDisabled && (m_stillFrames >= 8 || m_quiet)) {
        m_stillMode = true;
        m_historyReset = true;
        m_stillProbe = std::make_shared<d3d12::VelocityCalibration>();
        logMessage(m_detailLogs++ < 48 ? LogLevel::Warn : LogLevel::Info,
          "upscalerInject: the camera stands still - upscaling without motion until it moves");
      }
    }

    void findCandidates() {
      // Ranked: projective matrices first (a w column other than 0, 0, 0, 1:
      // the camera's clip transform rather than its view alone), then those
      // at multiples of 64 bytes (matrices packed one after the other; the
      // blocks across two of them can look alike), then by how often they
      // moved.
      std::vector<std::pair<uint32_t, taa_inject::Candidate>> scored;
      bool enough = false;
      for (uint32_t slot = 0; slot < taa_inject::CameraStore::kSlots; slot++) {
        std::map<uint64_t, std::vector<uint8_t>> frames;
        {
          std::lock_guard<std::mutex> lock(m_cameras->mutex);
          frames = m_cameras->frames[slot];
        }
        if (frames.size() < 12)
          continue;
        enough = true;
        // The latest 16 frame pairs.
        const auto first = std::next(frames.begin(), frames.size() > 17 ? frames.size() - 16 : 1);
        const std::vector<uint8_t>& latest = frames.rbegin()->second;
        const size_t size = latest.size();
        for (uint32_t offset = 0; offset + 64 <= size; offset += 16) {
          // Blocks of zeros are no camera.
          if (std::all_of(latest.begin() + offset, latest.begin() + offset + 64, [](uint8_t b) { return b == 0; }))
            continue;
          for (uint32_t layout = 0; layout < 2; layout++) {
            uint32_t pairs = 0, close = 0, moved = 0;
            for (auto it = first; it != frames.end(); ++it) {
              auto prev = frames.find(it->first - 1);
              if (prev == frames.end())
                continue;
              float rows[16];
              pairs++;
              if (!taa_inject::clipToPrevClip(it->second, prev->second, offset, layout, rows))
                continue;
              float deviation = 0.0f;
              for (int i = 0; i < 16; i++)
                deviation = (std::max)(deviation, std::fabs(rows[i] - ((i % 5) == 0 ? 1.0f : 0.0f)));
              if (deviation < 0.5f)
                close++;
              if (deviation > 1e-5f)
                moved++;
            }
            float m[16];
            const bool projective = taa_inject::matrixAt(frames.rbegin()->second, offset, layout, m)
              && (std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]) > 1e-3f || std::fabs(m[15] - 1.0f) > 1e-3f);
            if (pairs >= 8 && close * 5 >= pairs * 4 && moved * 4 >= pairs)
              scored.push_back({ (projective ? 0x20000u : 0u) + ((offset % 64) == 0 ? 0x10000u : 0u) + moved,
                { offset, layout, slot } });
          }
        }
      }
      if (!enough)
        return;
      if (scored.empty()) {
        // The camera has not moved enough yet: wait for more frames.
        std::lock_guard<std::mutex> lock(m_cameras->mutex);
        for (auto& frames : m_cameras->frames) {
          while (frames.size() > 16)
            frames.erase(frames.begin());
        }
        return;
      }
      std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (size_t i = 0; i < scored.size() && i < d3d12::kCameraCandidates; i++)
        m_candidates.push_back(scored[i].second);
      // Measurements of the frames before belong to no candidate.
      {
        std::lock_guard<std::mutex> lock(m_calibration->mutex);
        std::fill(std::begin(m_calibration->error), std::end(m_calibration->error), 0.0);
        m_calibration->still = 0.0;
        m_calibration->frames = 0;
        m_calibration->generation++;
      }
      logMessage(m_detailLogs++ < 48 ? LogLevel::Warn : LogLevel::Info, "upscalerInject: %zu camera candidates (best cb%u+%u)",
        m_candidates.size(), m_candidates[0].slot, m_candidates[0].offset);
    }

    // Another 30 frames after a pause of 300 (menus and loading screens
    // last; observing costs every draw a little).
    void lookAgain(const char* message) {
      m_stats.clear();
      m_observeFrames = 0;
      m_observeFrom = m_frame + 1 + 300;
      if (++m_observeRounds > 1000)
        fail(message);
    }

    void restart(const char* message, uint32_t pause) {
      logMessage(LogLevel::Warn, "%s", message);
      if (m_state == State::Active) {
        m_loaded = true;
        m_loadedCamera = m_camera;
      }
      m_state = State::Observing;
      m_depth = nullptr;
      m_color = nullptr;
      m_haveInsert = false;
      m_insertHash = 0;
      m_stats.clear();
      m_insertVotes.clear();
      m_readVotes.clear();
      m_observeFrames = 0;
      m_observeFrom = m_frame + 1 + pause;
      m_candidates.clear();
      m_calibrateFrames = 0;
      m_stillFrames = 0;
      m_stillMode = false;
      m_quiet = false;
      m_calibration.reset();
      m_lostFrames = 0;
    }

    void fail(const char* message) {
      m_state = State::Failed;
      logMessage(LogLevel::Warn, "%s - no injected upscaling", message);
    }

    static std::string path() {
      char file[MAX_PATH] = {};
      HMODULE module = nullptr;
      if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&taa_inject::halton), &module)
       || !GetModuleFileNameA(module, file, MAX_PATH))
        return std::string();
      std::string result = file;
      const size_t slash = result.find_last_of("\\/");
      return slash == std::string::npos ? std::string() : result.substr(0, slash + 1) + "dxup_upscaler_inject.txt";
    }

    void save() const {
      const std::string file = path();
      FILE* out = file.empty() ? nullptr : std::fopen(file.c_str(), "w");
      if (!out)
        return;
      std::fprintf(out, "%u %u %u\n", m_camera.slot, m_camera.offset, m_camera.layout);
      std::fclose(out);
    }

    void load() {
      const std::string file = path();
      FILE* in = file.empty() ? nullptr : std::fopen(file.c_str(), "r");
      if (!in)
        return;
      unsigned slot = 0, offset = 0, layout = 0;
      if (std::fscanf(in, "%u %u %u", &slot, &offset, &layout) == 3 && layout < 2 && slot < 14) {
        m_loaded = true;
        m_loadedCamera = { offset, layout, slot };
      }
      std::fclose(in);
    }

    State    m_state = State::Observing;
    uint64_t m_frame = 0;
    // Observing.
    std::unordered_map<ID3D11Resource*, Stats> m_stats;
    std::unordered_map<uint64_t, uint32_t> m_insertVotes, m_readVotes;
    uint32_t m_observeFrames = 0, m_observeRounds = 0, m_detailLogs = 0;
    uint64_t m_observeFrom = 0;
    uint32_t m_lostFrames = 0;
    // The scene.
    ID3D11Resource* m_depth = nullptr;
    ID3D11Resource* m_color = nullptr;
    bool     m_depthInverted = false;
    uint64_t m_insertHash = 0;
    bool     m_haveInsert = false;
    // This frame.
    bool     m_sceneSeen = false;
    bool     m_colorSeen = false;
    bool     m_readSet = false;
    uint64_t m_firstRead = 0;
    bool     m_afterSceneSet = false;
    uint64_t m_afterScene = 0;
    bool     m_inserted = false;
    uint32_t m_slotsTaken = 0;
    // Camera.
    std::shared_ptr<taa_inject::CameraStore> m_cameras = std::make_shared<taa_inject::CameraStore>();
    std::vector<taa_inject::Candidate> m_candidates;
    uint32_t m_calibrateFrames = 0;
    // Upscaling while the camera stands still (updateStillMode).
    uint32_t m_stillFrames = 0;
    bool     m_stillMode = false;
    bool     m_stillDisabled = false;
    bool     m_quiet = false;
    bool     m_historyReset = false;
    std::shared_ptr<d3d12::VelocityCalibration> m_stillProbe;
    std::shared_ptr<d3d12::VelocityCalibration> m_calibration;
    taa_inject::Candidate m_camera;
    bool     m_loaded = false;
    taa_inject::Candidate m_loadedCamera;
  };

}
