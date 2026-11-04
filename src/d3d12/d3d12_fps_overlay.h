#pragma once

// The frame rate overlay (dxup_show_fps_overlay.txt next to the DLL), of DXUP's
// swap chains.

#include "d3d12_backend_context.h"
#include "../util/util_types.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace dxup {

  class FpsOverlay {
  public:
    void count() {
      if (!m_fpsTimerInit) {
        QueryPerformanceFrequency(&m_fpsFreq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        m_fpsLastTime = static_cast<double>(now.QuadPart) / m_fpsFreq.QuadPart;
        m_fpsTimerInit = true;
      }
      m_fpsFrameCount++;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      double t = static_cast<double>(now.QuadPart) / m_fpsFreq.QuadPart;
      double elapsed = t - m_fpsLastTime;
      if (elapsed >= 1.0) {
        m_currentFps = static_cast<float>(m_fpsFrameCount / elapsed);
        logMessage(LogLevel::Info, "FPS: %.1f", m_currentFps);
        m_fpsFrameCount = 0;
        m_fpsLastTime = t;
      }
    }

    // The counter as 8-bit RGBA texels (green on gray reads the same as
    // BGRA), on targets of that size of texel only, which it fits on.
    void render(d3d12::D3D12BackendContext* ctx, IBackendImage* target) {
      const ImageDesc& targetDesc = target->getDesc();
      switch (targetDesc.format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
          break;
        default:
          return;
      }
      if (targetDesc.sampleCount > 1)
        return;
      const uint32_t targetW = targetDesc.width, targetH = targetDesc.height;
      // 5x7 bitmap font: 0-9, space, F, P, S, period
      static const uint8_t font[][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
        {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 2
        {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space (10)
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F (11)
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P (12)
        {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // S (13)
        {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // . (14)
      };
      auto ci = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c == 'F') return 11; if (c == 'P') return 12;
        if (c == 'S') return 13; if (c == '.') return 14;
        return 10;
      };

      char fpsStr[16];
      snprintf(fpsStr, sizeof(fpsStr), "%.1f FPS", m_currentFps);
      uint32_t textLen = static_cast<uint32_t>(strlen(fpsStr));

      constexpr uint32_t S = 2, CW = 5*S, CH = 7*S, GAP = S, PAD = 4;
      uint32_t overlayW = (textLen * (CW + GAP) + PAD * 2 + 3) & ~3u;
      uint32_t overlayH = CH + PAD * 2;
      if (targetW < overlayW + 8 || targetH < overlayH + 8)
        return;

      std::vector<uint32_t> px(overlayW * overlayH, 0xFF1A1A1A); // dark bg

      for (uint32_t i = 0; i < textLen; i++) {
        int fi = ci(fpsStr[i]);
        uint32_t bx = PAD + i * (CW + GAP);
        for (uint32_t cy = 0; cy < 7; cy++) {
          uint8_t row = font[fi][cy];
          for (uint32_t cx = 0; cx < 5; cx++) {
            if (row & (0x10 >> cx)) {
              for (uint32_t sy = 0; sy < S; sy++)
                for (uint32_t sx = 0; sx < S; sx++) {
                  uint32_t x = bx + cx*S + sx, y = PAD + cy*S + sy;
                  if (x < overlayW && y < overlayH)
                    px[y * overlayW + x] = 0xFF00FF00; // green
                }
            }
          }
        }
      }

      ctx->blitPixels(target, targetW - overlayW - 8, 8, overlayW, overlayH, px.data());
    }

  private:
    uint32_t m_fpsFrameCount = 0;
    double   m_fpsLastTime   = 0.0;
    float    m_currentFps    = 0.0f;
    bool     m_fpsTimerInit  = false;
    LARGE_INTEGER m_fpsFreq  = {};
  };

}
