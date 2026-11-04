#pragma once

// Saved frames and flicker detection of glitch reports (dxup_frame_dump.txt),
// for what DXUP's swap chains present.

#include "d3d11_draw_history.h"
#include "../util/com_ptr.h"
#include "../util/util_paths.h"
#include "../util/util_types.h"

#include <d3d11.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dxup {

  // An 8-bit RGBA or BGRA texture as a BMP file next to the DLL.
  inline bool writeTextureBitmap(ID3D11Device* device, ID3D11Texture2D* source, const char* name) {
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                   || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                   || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if ((!rgba && !bgra) || desc.SampleDesc.Count > 1) {
      logMessage(LogLevel::Warn, "frame dump: format %d samples %u not supported",
        static_cast<int>(desc.Format), desc.SampleDesc.Count);
      return false;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())))
      return false;
    device->GetImmediateContext(context.GetAddressOf());
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return false;

    FILE* file = dxupOpenSibling(name, "wb");
    if (file) {
      const uint32_t imageSize = desc.Width * desc.Height * 4;
      BITMAPFILEHEADER fileHeader = {};
      BITMAPINFOHEADER infoHeader = {};
      fileHeader.bfType = 0x4D42;
      fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
      fileHeader.bfSize = fileHeader.bfOffBits + imageSize;
      infoHeader.biSize = sizeof(infoHeader);
      infoHeader.biWidth = LONG(desc.Width);
      infoHeader.biHeight = -LONG(desc.Height);
      infoHeader.biPlanes = 1;
      infoHeader.biBitCount = 32;
      infoHeader.biSizeImage = imageSize;
      fwrite(&fileHeader, sizeof(fileHeader), 1, file);
      fwrite(&infoHeader, sizeof(infoHeader), 1, file);
      std::vector<uint8_t> row(desc.Width * 4);
      for (UINT y = 0; y < desc.Height; y++) {
        const auto* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; x++) {
          row[x * 4 + 0] = src[x * 4 + (rgba ? 2 : 0)];
          row[x * 4 + 1] = src[x * 4 + 1];
          row[x * 4 + 2] = src[x * 4 + (rgba ? 0 : 2)];
          row[x * 4 + 3] = 255;
        }
        fwrite(row.data(), row.size(), 1, file);
      }
      fclose(file);
    }
    context->Unmap(staging.Get(), 0);
    return file != nullptr;
  }

  // Each presented frame goes down to a grid of about 60 x 34 tiles on the
  // GPU (the last levels of a mip chain); the grid comes back a few frames
  // later without waiting. Tiles that go dark (or bright) for one or two
  // frames while the frames around them agree are a flicker, not a camera
  // cut or a fade: the swap chain logs where, saves the glitch frame and
  // the one before as dxup_glitch_<present>.bmp, and the immediate context
  // logs the calls of both frames against each other (LogDrawHistory).
  class GlitchDetector {
    ID3D11Device* m_device = nullptr;
    DrawHistory*  m_history = nullptr;
    static constexpr uint32_t    kGlitchSmall = 4;          // grids on their way back
    static constexpr uint32_t    kGlitchGrids = 6;          // grids kept
    static constexpr uint32_t    kGlitchFrames = 6;         // full frames kept
    struct GlitchGrid {
      uint32_t             present = 0;
      std::vector<uint8_t> luma;
    };
    ComPtr<ID3D11Texture2D>      m_glitchMips;
    ComPtr<ID3D11ShaderResourceView> m_glitchMipsView;
    ComPtr<ID3D11Texture2D>      m_glitchSmall[kGlitchSmall];
    uint32_t                     m_glitchSmallOf[kGlitchSmall] = {};
    ComPtr<ID3D11Texture2D>      m_glitchFrames[kGlitchFrames];
    uint32_t                     m_glitchFrameOf[kGlitchFrames] = {};
    GlitchGrid                   m_glitchGrids[kGlitchGrids];
    uint32_t                     m_glitchLevel = 0, m_glitchColumns = 0, m_glitchRows = 0;
    uint32_t                     m_glitchWidth = 0, m_glitchHeight = 0;
    bool                         m_glitchBgra = false;
    bool                         m_glitchOff = false;
    uint32_t                     m_glitchEvents = 0;
    uint32_t                     m_glitchLastEvent = 0;
    // A flicker seen at a present, reported once the next frames show it
    // was not a dark object moving across (that is seen frame after frame).
    struct GlitchCandidate {
      uint32_t glitch = 0, frames = 0, dark = 0, bright = 0, tiles = 0;
      uint32_t box[4] = {};
      uint32_t luma[3] = {};
    };
    GlitchCandidate              m_glitchPending;
    // The pending candidate's frame and the one before, copied aside when it
    // is seen (the kept frames roll on before it is reported).
    ComPtr<ID3D11Texture2D>      m_glitchAside[2];
    bool                         m_glitchAsideValid[2] = {};
    uint32_t                     m_glitchLastSeen = 0;     // the last glitch frame any candidate had
    uint32_t                     m_glitchSuppressed = 0;

  public:
    void detect(ID3D11Device* device, ID3D11Texture2D* backBuffer, uint32_t present, DrawHistory* history) {
      m_device = device;
      m_history = history;
      if (m_glitchOff || (!m_glitchMips && !prepareGlitchDetection(backBuffer)))
        return;
      D3D11_TEXTURE2D_DESC desc = {};
      backBuffer->GetDesc(&desc);
      if (desc.Width != m_glitchWidth || desc.Height != m_glitchHeight) {
        // A new size: start over.
        m_glitchAside[0].Reset();
        m_glitchAside[1].Reset();
        m_glitchAsideValid[0] = m_glitchAsideValid[1] = false;
        m_glitchPending = GlitchCandidate();
        m_glitchMips.Reset();
        m_glitchMipsView.Reset();
        for (auto& grid : m_glitchGrids)
          grid.present = 0;
        if (!prepareGlitchDetection(backBuffer))
          return;
      }
      ComPtr<ID3D11DeviceContext> context;
      m_device->GetImmediateContext(context.GetAddressOf());
      const uint32_t frameSlot = present % kGlitchFrames;
      if (m_glitchFrames[frameSlot]) {
        context->CopySubresourceRegion(m_glitchFrames[frameSlot].Get(), 0, 0, 0, 0, backBuffer, 0, nullptr);
        m_glitchFrameOf[frameSlot] = present;
      }
      context->CopySubresourceRegion(m_glitchMips.Get(), 0, 0, 0, 0, backBuffer, 0, nullptr);
      context->GenerateMips(m_glitchMipsView.Get());
      const uint32_t smallSlot = present % kGlitchSmall;
      context->CopySubresourceRegion(m_glitchSmall[smallSlot].Get(), 0, 0, 0, 0, m_glitchMips.Get(), m_glitchLevel, nullptr);
      m_glitchSmallOf[smallSlot] = present;
      // The grids of earlier presents the GPU is done with, oldest first.
      for (uint32_t back = kGlitchSmall - 1; back >= 1; back--) {
        const uint32_t q = present - back;
        const uint32_t slot = q % kGlitchSmall;
        if (q == 0 || q > present || m_glitchSmallOf[slot] != q)
          continue;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map(m_glitchSmall[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
          break;
        m_glitchSmallOf[slot] = 0;
        if (FAILED(hr))
          continue;
        GlitchGrid& grid = m_glitchGrids[q % kGlitchGrids];
        grid.present = q;
        grid.luma.resize(size_t(m_glitchColumns) * m_glitchRows);
        for (uint32_t y = 0; y < m_glitchRows; y++) {
          const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
          for (uint32_t x = 0; x < m_glitchColumns; x++) {
            const uint8_t* p = row + x * 4;
            const uint32_t r = m_glitchBgra ? p[2] : p[0], g = p[1], b = m_glitchBgra ? p[0] : p[2];
            grid.luma[size_t(y) * m_glitchColumns + x] = uint8_t((r * 77 + g * 150 + b * 29) >> 8);
          }
        }
        context->Unmap(m_glitchSmall[slot].Get(), 0);
        checkFlicker(q, present);
      }
    }

  private:
    bool prepareGlitchDetection(ID3D11Texture2D* backBuffer) {
      D3D11_TEXTURE2D_DESC desc = {};
      backBuffer->GetDesc(&desc);
      DXGI_FORMAT format = desc.Format;
      switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
          break;
        default:
          format = DXGI_FORMAT_UNKNOWN;
          break;
      }
      if (format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count > 1 || desc.Width < 64 || desc.Height < 32) {
        logMessage(LogLevel::Warn, "glitch detection: back buffer %ux%u format %d samples %u not supported",
          desc.Width, desc.Height, static_cast<int>(desc.Format), desc.SampleDesc.Count);
        m_glitchOff = true;
        return false;
      }
      m_glitchBgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
      m_glitchLevel = 0;
      while ((desc.Width >> m_glitchLevel) > 64 && m_glitchLevel < 12)
        m_glitchLevel++;
      m_glitchColumns = (std::max)(desc.Width >> m_glitchLevel, 1u);
      m_glitchRows = (std::max)(desc.Height >> m_glitchLevel, 1u);
      m_glitchWidth = desc.Width;
      m_glitchHeight = desc.Height;

      D3D11_TEXTURE2D_DESC mips = {};
      mips.Width = desc.Width;
      mips.Height = desc.Height;
      mips.MipLevels = m_glitchLevel + 1;
      mips.ArraySize = 1;
      mips.Format = format;
      mips.SampleDesc.Count = 1;
      mips.Usage = D3D11_USAGE_DEFAULT;
      mips.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      mips.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
      D3D11_TEXTURE2D_DESC gridDesc = {};
      gridDesc.Width = m_glitchColumns;
      gridDesc.Height = m_glitchRows;
      gridDesc.MipLevels = 1;
      gridDesc.ArraySize = 1;
      gridDesc.Format = format;
      gridDesc.SampleDesc.Count = 1;
      gridDesc.Usage = D3D11_USAGE_STAGING;
      gridDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(m_device->CreateTexture2D(&mips, nullptr, m_glitchMips.ReleaseAndGetAddressOf()))
       || FAILED(m_device->CreateShaderResourceView(m_glitchMips.Get(), nullptr, m_glitchMipsView.ReleaseAndGetAddressOf()))) {
        logMessage(LogLevel::Warn, "glitch detection: cannot create its textures");
        m_glitchMips.Reset();
        m_glitchOff = true;
        return false;
      }
      for (uint32_t i = 0; i < kGlitchSmall; i++) {
        m_glitchSmallOf[i] = 0;
        if (FAILED(m_device->CreateTexture2D(&gridDesc, nullptr, m_glitchSmall[i].ReleaseAndGetAddressOf()))) {
          m_glitchMips.Reset();
          m_glitchOff = true;
          return false;
        }
      }
      // The last frames in full, to save the glitch frame and the one before
      // (without them only the log tells).
      D3D11_TEXTURE2D_DESC frame = {};
      frame.Width = desc.Width;
      frame.Height = desc.Height;
      frame.MipLevels = 1;
      frame.ArraySize = 1;
      frame.Format = desc.Format;
      frame.SampleDesc.Count = 1;
      frame.Usage = D3D11_USAGE_DEFAULT;
      for (uint32_t i = 0; i < kGlitchFrames; i++) {
        m_glitchFrameOf[i] = 0;
        if (FAILED(m_device->CreateTexture2D(&frame, nullptr, m_glitchFrames[i].ReleaseAndGetAddressOf())))
          m_glitchFrames[i].Reset();
      }
      logMessage(LogLevel::Warn, "glitch detection: on, %ux%u tiles of %ux%u pixels", m_glitchColumns, m_glitchRows,
        desc.Width / m_glitchColumns, desc.Height / m_glitchRows);
      return true;
    }

    const GlitchGrid* glitchGrid(uint32_t present) const {
      const GlitchGrid& grid = m_glitchGrids[present % kGlitchGrids];
      return present && grid.present == present ? &grid : nullptr;
    }

    // With the grid of present q in: does present q-1 (or q-2 and q-1)
    // differ from the frames around it where those agree?
    void checkFlicker(uint32_t q, uint32_t present) {
      for (uint32_t frames = 1; frames <= 2; frames++) {
        if (q < frames + 2)
          return;
        const GlitchGrid* after = glitchGrid(q);
        const GlitchGrid* before = glitchGrid(q - frames - 1);
        const GlitchGrid* middle[2] = { glitchGrid(q - frames), frames == 2 ? glitchGrid(q - 1) : nullptr };
        if (!after || !before || !middle[0] || (frames == 2 && !middle[1]))
          continue;
        const size_t tiles = after->luma.size();
        // How much the picture moves: the mean difference of the frames
        // around (0 for a still camera).
        uint64_t motionSum = 0;
        for (size_t t = 0; t < tiles; t++)
          motionSum += uint32_t(std::abs(int(before->luma[t]) - int(after->luma[t])));
        const uint32_t motion = uint32_t(motionSum / (std::max)(tiles, size_t(1)));
        uint32_t dark = 0, bright = 0;
        uint32_t box[4] = { UINT32_MAX, UINT32_MAX, 0, 0 };
        uint32_t lumaBefore = 0, lumaGlitch = 0, lumaAfter = 0;
        for (size_t t = 0; t < tiles; t++) {
          const int a = before->luma[t], b = after->luma[t];
          // The frames around agree here.
          if (std::abs(a - b) > (std::max)(a, b) / 4 + 6)
            continue;
          const int low = (std::min)(a, b), high = (std::max)(a, b);
          bool isDark = low >= 32, isBright = true;
          for (uint32_t m = 0; m < frames; m++) {
            const int v = middle[m]->luma[t];
            isDark = isDark && v * 5 < low * 2;
            isBright = isBright && v > high * 2 + 40;
          }
          if (!isDark && !isBright)
            continue;
          (isDark ? dark : bright)++;
          const uint32_t x = uint32_t(t % m_glitchColumns), y = uint32_t(t / m_glitchColumns);
          box[0] = (std::min)(box[0], x);
          box[1] = (std::min)(box[1], y);
          box[2] = (std::max)(box[2], x);
          box[3] = (std::max)(box[3], y);
          lumaBefore += uint32_t(a);
          lumaGlitch += middle[0]->luma[t];
          lumaAfter += uint32_t(b);
        }
        const uint32_t count = dark + bright;
        // A patch, not tiles strewn over the picture (edges of a moving
        // camera): at least 6 tiles filling a fifth of their bounding box.
        const uint32_t area = count ? (box[2] - box[0] + 1) * (box[3] - box[1] + 1) : 0;
        if (count < 6 || count * 5 < area)
          continue;
        // With the camera moving, near objects cross the picture within a
        // frame: then only a larger patch (2 % of the picture) counts.
        if (motion > 4 && count * 50 < tiles) {
          m_glitchSuppressed++;
          continue;
        }
        const uint32_t glitch = q - frames;
        // One flicker is seen once (as one frame, not again as two).
        if (glitch <= m_glitchLastEvent)
          return;
        m_glitchLastEvent = q - 1;
        // A candidate right after another is a dark object moving across
        // the picture: neither counts.
        const bool afterOther = m_glitchLastSeen && glitch <= m_glitchLastSeen + 2;
        m_glitchLastSeen = q - 1;
        if (afterOther) {
          if (m_glitchPending.glitch)
            m_glitchSuppressed++;
          m_glitchPending = GlitchCandidate();
          m_glitchSuppressed++;
          return;
        }
        flushGlitchCandidate(present);
        keepGlitchFrames(glitch);
        GlitchCandidate& candidate = m_glitchPending;
        candidate.glitch = glitch;
        candidate.frames = frames;
        candidate.dark = dark;
        candidate.bright = bright;
        candidate.tiles = uint32_t(tiles);
        std::memcpy(candidate.box, box, sizeof(box));
        candidate.luma[0] = lumaBefore / count;
        candidate.luma[1] = lumaGlitch / count;
        candidate.luma[2] = lumaAfter / count;
        return;
      }
      // Two frames on without another candidate: report the pending one.
      if (m_glitchPending.glitch && q >= m_glitchPending.glitch + m_glitchPending.frames + 2)
        flushGlitchCandidate(present);
    }

    void keepGlitchFrames(uint32_t glitch) {
      ComPtr<ID3D11DeviceContext> context;
      m_device->GetImmediateContext(context.GetAddressOf());
      for (uint32_t which = 0; which < 2; which++) {
        m_glitchAsideValid[which] = false;
        const uint32_t frame = glitch - which;
        const uint32_t slot = frame % kGlitchFrames;
        if (!m_glitchFrames[slot] || m_glitchFrameOf[slot] != frame || m_glitchEvents >= 4)
          continue;
        if (!m_glitchAside[which]) {
          D3D11_TEXTURE2D_DESC desc = {};
          m_glitchFrames[slot]->GetDesc(&desc);
          if (FAILED(m_device->CreateTexture2D(&desc, nullptr, m_glitchAside[which].GetAddressOf())))
            continue;
        }
        context->CopyResource(m_glitchAside[which].Get(), m_glitchFrames[slot].Get());
        m_glitchAsideValid[which] = true;
      }
    }

    void flushGlitchCandidate(uint32_t present) {
      GlitchCandidate candidate = m_glitchPending;
      m_glitchPending = GlitchCandidate();
      if (!candidate.glitch)
        return;
      // Video and loading screens (a few calls a frame) change as they like.
      const size_t calls = m_history ? m_history->calls(present - candidate.glitch) : SIZE_MAX;
      if (calls < 10) {
        m_glitchSuppressed++;
        return;
      }
      reportGlitch(candidate.glitch, candidate.frames, candidate.dark, candidate.bright, candidate.tiles, candidate.box,
        candidate.luma[0], candidate.luma[1], candidate.luma[2], present);
    }

    void reportGlitch(uint32_t glitch, uint32_t frames, uint32_t dark, uint32_t bright, uint32_t tiles,
                      const uint32_t box[4], uint32_t lumaBefore, uint32_t lumaGlitch, uint32_t lumaAfter,
                      uint32_t present) {
      const uint32_t event = ++m_glitchEvents;
      if (event > 32) {
        if (event == 33)
          logMessage(LogLevel::Warn, "glitch: more than 32, the rest are not logged");
        return;
      }
      const uint32_t tileWidth = m_glitchWidth / m_glitchColumns, tileHeight = m_glitchHeight / m_glitchRows;
      logMessage(LogLevel::Warn,
        "glitch %u: present %u, %u frame%s: %u tiles darker, %u brighter of %u (%.1f%%), pixels %u,%u to %u,%u; brightness %u -> %u -> %u (%u moving objects and video frames skipped so far)",
        event, glitch, frames, frames > 1 ? "s" : "", dark, bright, tiles, 100.0 * (dark + bright) / tiles,
        box[0] * tileWidth, box[1] * tileHeight, (box[2] + 1) * tileWidth, (box[3] + 1) * tileHeight,
        lumaBefore, lumaGlitch, lumaAfter, m_glitchSuppressed);
      // The glitch frame and the one before, copied aside when the glitch
      // was seen (images of the first 4 glitches: 8 MB each at 1080p).
      if (event <= 4) {
        for (uint32_t which = 0; which < 2; which++) {
          if (!m_glitchAsideValid[which])
            continue;
          m_glitchAsideValid[which] = false;
          char name[64];
          sprintf_s(name, which ? "dxup_glitch_%u_before.bmp" : "dxup_glitch_%u.bmp", glitch);
          if (writeTextureBitmap(m_device, m_glitchAside[which].Get(), name))
            logMessage(LogLevel::Warn, "glitch %u: wrote %s", event, name);
        }
      }
      // The calls: the glitch frame is `present - glitch` presents back.
      if (m_history && event <= 8)
        m_history->log(present - glitch, glitch, event, event == 1);
    }
  };

}
