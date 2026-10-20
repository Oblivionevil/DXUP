#include "ddraw_objects.h"

#include "../legacy/legacy_formats.h"
#include "../util/com_ptr.h"
#include "../util/util_gpu_spoof.h"
#include "../util/util_types.h"

#include <dxgi.h>
#include <float.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace dxup::ddraw {

  using legacy::Format;

  void logUnsupported(const char* what) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::lock_guard<std::mutex> lock(mutex);
    if (reported.insert(what).second)
      logMessage(LogLevel::Warn, "ddraw: %s not supported", what);
  }

  namespace {
    std::string guidKey(REFGUID guid) {
      return std::string(reinterpret_cast<const char*>(&guid), sizeof(GUID));
    }

    template<typename T>
    void zeroSized(T* value) {
      const DWORD size = value->dwSize;
      std::memset(value, 0, size ? (std::min)(size_t(size), sizeof(T)) : sizeof(T));
      value->dwSize = size ? size : sizeof(T);
    }
  }

  // ============================================================
  // Pixel formats
  // ============================================================

  Format formatFromPixelFormat(const DDPIXELFORMAT& pf) {
    if (pf.dwFlags & DDPF_FOURCC) {
      switch (pf.dwFourCC) {
        case legacy::FormatDXT1: case legacy::FormatDXT2: case legacy::FormatDXT3:
        case legacy::FormatDXT4: case legacy::FormatDXT5:
          return Format(pf.dwFourCC);
        default:
          return legacy::FormatUnknown;
      }
    }
    if (pf.dwFlags & DDPF_ZBUFFER) {
      const bool stencil = (pf.dwFlags & DDPF_STENCILBUFFER) && pf.dwStencilBitDepth;
      switch (pf.dwZBufferBitDepth) {
        case 16: return stencil ? legacy::FormatD15S1 : legacy::FormatD16;
        case 24: return legacy::FormatD24X8;
        default: return stencil ? legacy::FormatD24S8 : (pf.dwZBitMask == 0xffffffffu ? legacy::FormatD32 : legacy::FormatD24X8);
      }
    }
    if (pf.dwFlags & DDPF_PALETTEINDEXED8)
      return legacy::FormatP8;
    if (pf.dwFlags & DDPF_BUMPDUDV)
      return legacy::FormatV8U8;
    if (pf.dwFlags & DDPF_LUMINANCE) {
      if (pf.dwLuminanceBitCount == 16) return legacy::FormatA8L8;
      return (pf.dwFlags & DDPF_ALPHAPIXELS) ? legacy::FormatA4L4 : legacy::FormatL8;
    }
    if ((pf.dwFlags & DDPF_ALPHA) && !(pf.dwFlags & DDPF_RGB))
      return legacy::FormatA8;
    if (pf.dwFlags & DDPF_RGB) {
      const bool alpha = (pf.dwFlags & DDPF_ALPHAPIXELS) && pf.dwRGBAlphaBitMask;
      switch (pf.dwRGBBitCount) {
        case 32:
          if (pf.dwRBitMask == 0xff)
            return alpha ? legacy::FormatA8B8G8R8 : legacy::FormatX8B8G8R8;
          return alpha ? legacy::FormatA8R8G8B8 : legacy::FormatX8R8G8B8;
        case 24: return legacy::FormatR8G8B8;
        case 16:
          if (pf.dwRBitMask == 0xf800) return legacy::FormatR5G6B5;
          if (pf.dwRBitMask == 0x7c00) return alpha ? legacy::FormatA1R5G5B5 : legacy::FormatX1R5G5B5;
          if (pf.dwRBitMask == 0x0f00) return alpha ? legacy::FormatA4R4G4B4 : legacy::FormatX4R4G4B4;
          return legacy::FormatR5G6B5;
        case 8:  return legacy::FormatR3G3B2;
        default: break;
      }
    }
    return legacy::FormatUnknown;
  }

  void pixelFormatFromFormat(Format format, DDPIXELFORMAT& pf) {
    std::memset(&pf, 0, sizeof(pf));
    pf.dwSize = sizeof(pf);
    auto rgb = [&](DWORD bits, DWORD r, DWORD g, DWORD b, DWORD a) {
      pf.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
      pf.dwRGBBitCount = bits;
      pf.dwRBitMask = r;
      pf.dwGBitMask = g;
      pf.dwBBitMask = b;
      pf.dwRGBAlphaBitMask = a;
    };
    switch (format) {
      case legacy::FormatA8R8G8B8: rgb(32, 0xff0000, 0xff00, 0xff, 0xff000000u); break;
      case legacy::FormatX8R8G8B8: rgb(32, 0xff0000, 0xff00, 0xff, 0); break;
      case legacy::FormatA8B8G8R8: rgb(32, 0xff, 0xff00, 0xff0000, 0xff000000u); break;
      case legacy::FormatX8B8G8R8: rgb(32, 0xff, 0xff00, 0xff0000, 0); break;
      case legacy::FormatR8G8B8:   rgb(24, 0xff0000, 0xff00, 0xff, 0); break;
      case legacy::FormatR5G6B5:   rgb(16, 0xf800, 0x07e0, 0x001f, 0); break;
      case legacy::FormatX1R5G5B5: rgb(16, 0x7c00, 0x03e0, 0x001f, 0); break;
      case legacy::FormatA1R5G5B5: rgb(16, 0x7c00, 0x03e0, 0x001f, 0x8000); break;
      case legacy::FormatA4R4G4B4: rgb(16, 0x0f00, 0x00f0, 0x000f, 0xf000); break;
      case legacy::FormatX4R4G4B4: rgb(16, 0x0f00, 0x00f0, 0x000f, 0); break;
      case legacy::FormatR3G3B2:   rgb(8, 0xe0, 0x1c, 0x03, 0); break;
      case legacy::FormatA8:
        pf.dwFlags = DDPF_ALPHA;
        pf.dwAlphaBitDepth = 8;
        break;
      case legacy::FormatP8:
        pf.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        pf.dwRGBBitCount = 8;
        break;
      case legacy::FormatL8:
        pf.dwFlags = DDPF_LUMINANCE;
        pf.dwLuminanceBitCount = 8;
        pf.dwLuminanceBitMask = 0xff;
        break;
      case legacy::FormatA8L8:
        pf.dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
        pf.dwLuminanceBitCount = 16;
        pf.dwLuminanceBitMask = 0xff;
        pf.dwLuminanceAlphaBitMask = 0xff00;
        break;
      case legacy::FormatA4L4:
        pf.dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
        pf.dwLuminanceBitCount = 8;
        pf.dwLuminanceBitMask = 0x0f;
        pf.dwLuminanceAlphaBitMask = 0xf0;
        break;
      case legacy::FormatV8U8:
        pf.dwFlags = DDPF_BUMPDUDV;
        pf.dwBumpBitCount = 16;
        pf.dwBumpDuBitMask = 0xff;
        pf.dwBumpDvBitMask = 0xff00;
        break;
      case legacy::FormatDXT1: case legacy::FormatDXT2: case legacy::FormatDXT3:
      case legacy::FormatDXT4: case legacy::FormatDXT5:
        pf.dwFlags = DDPF_FOURCC;
        pf.dwFourCC = format;
        break;
      case legacy::FormatD16:
      case legacy::FormatD16Lockable:
        pf.dwFlags = DDPF_ZBUFFER;
        pf.dwZBufferBitDepth = 16;
        pf.dwZBitMask = 0xffff;
        break;
      case legacy::FormatD15S1:
        pf.dwFlags = DDPF_ZBUFFER | DDPF_STENCILBUFFER;
        pf.dwZBufferBitDepth = 16;
        pf.dwStencilBitDepth = 1;
        pf.dwZBitMask = 0x7fff;
        pf.dwStencilBitMask = 0x8000;
        break;
      case legacy::FormatD24X8:
        pf.dwFlags = DDPF_ZBUFFER;
        pf.dwZBufferBitDepth = 32;
        pf.dwZBitMask = 0x00ffffff;
        break;
      case legacy::FormatD24S8:
      case legacy::FormatD24X4S4:
        pf.dwFlags = DDPF_ZBUFFER | DDPF_STENCILBUFFER;
        pf.dwZBufferBitDepth = 32;
        pf.dwStencilBitDepth = 8;
        pf.dwZBitMask = 0x00ffffff;
        pf.dwStencilBitMask = 0xff000000u;
        break;
      case legacy::FormatD32:
        pf.dwFlags = DDPF_ZBUFFER;
        pf.dwZBufferBitDepth = 32;
        pf.dwZBitMask = 0xffffffffu;
        break;
      default:
        break;
    }
  }

  // ============================================================
  // Clipper and palette
  // ============================================================

  HRESULT Clipper::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawClipper) {
      *ppv = static_cast<IDirectDrawClipper*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG Clipper::Release() {
    const ULONG ref = --m_ref;
    if (!ref) delete this;
    return ref;
  }

  HRESULT Clipper::GetClipList(LPRECT rect, LPRGNDATA data, LPDWORD size) {
    (void)rect;
    if (!size) return DDERR_INVALIDPARAMS;
    RECT client = {};
    if (m_window) {
      GetClientRect(m_window, &client);
      MapWindowPoints(m_window, nullptr, reinterpret_cast<POINT*>(&client), 2);
    }
    const DWORD needed = sizeof(RGNDATAHEADER) + sizeof(RECT);
    if (!data) { *size = needed; return DD_OK; }
    if (*size < needed) { *size = needed; return DDERR_REGIONTOOSMALL; }
    data->rdh.dwSize = sizeof(RGNDATAHEADER);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = 1;
    data->rdh.nRgnSize = sizeof(RECT);
    data->rdh.rcBound = client;
    std::memcpy(data->Buffer, &client, sizeof(RECT));
    return DD_OK;
  }

  HRESULT Clipper::GetHWnd(HWND* window) {
    if (!window) return DDERR_INVALIDPARAMS;
    *window = m_window;
    return DD_OK;
  }

  HRESULT Clipper::IsClipListChanged(BOOL* changed) {
    if (changed) *changed = FALSE;
    return DD_OK;
  }

  Palette::Palette(DWORD caps, const PALETTEENTRY* entries) : m_caps(caps) {
    if (entries)
      SetEntries(0, 0, (caps & DDPCAPS_8BIT) ? 256 : (caps & DDPCAPS_4BIT) ? 16 : 256, const_cast<LPPALETTEENTRY>(entries));
  }

  HRESULT Palette::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawPalette) {
      *ppv = static_cast<IDirectDrawPalette*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG Palette::Release() {
    const ULONG ref = --m_ref;
    if (!ref) delete this;
    return ref;
  }

  HRESULT Palette::GetCaps(LPDWORD caps) {
    if (!caps) return DDERR_INVALIDPARAMS;
    *caps = m_caps;
    return DD_OK;
  }

  HRESULT Palette::GetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY entries) {
    if (!entries || base + count > 256) return DDERR_INVALIDPARAMS;
    std::memcpy(entries, m_entries + base, count * sizeof(PALETTEENTRY));
    return DD_OK;
  }

  HRESULT Palette::SetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY entries) {
    if (!entries || base + count > 256) return DDERR_INVALIDPARAMS;
    std::memcpy(m_entries + base, entries, count * sizeof(PALETTEENTRY));
    const bool alpha = (m_caps & DDPCAPS_ALPHA) != 0;
    for (DWORD i = base; i < base + count; i++) {
      const PALETTEENTRY& e = m_entries[i];
      m_argb[i] = (uint32_t(alpha ? e.peFlags : 0xff) << 24) | (uint32_t(e.peRed) << 16) | (uint32_t(e.peGreen) << 8) | e.peBlue;
    }
    m_version++;
    return DD_OK;
  }

  // ============================================================
  // Surface
  // ============================================================
  void traceCall(const char* what, const DDSURFACEDESC2& desc) {
    static std::atomic<int> count{0};
    if (count.fetch_add(1) < 40)
      logMessage(LogLevel::Info, "ddraw: %s on %lux%lu caps 0x%lx", what, desc.dwWidth, desc.dwHeight, desc.ddsCaps.dwCaps);
  }


  Surface::Surface(DirectDraw* dd, const DDSURFACEDESC2& desc, std::shared_ptr<legacy::Texture> texture,
                   uint32_t subresource, Surface* owner)
  : m_dd(dd), m_desc(desc), m_texture(std::move(texture)), m_subresource(subresource), m_owner(owner) {
    s_vtable = *reinterpret_cast<void* const*>(static_cast<IDirectDrawSurface7*>(this));
    if (!m_owner) {
      m_dd->AddRef();
      m_ref = 1;
    }
    for (int i = 0; i < 256; i++) {
      const WORD value = WORD(i * 257);
      m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = value;
    }
  }

  Surface::~Surface() {
    if (m_dc) {
      SelectObject(m_dc, m_oldBitmap);
      DeleteDC(m_dc);
      DeleteObject(m_bitmap);
    }
    for (Surface* child : m_attached)
      delete child;
    for (Surface* attached : m_attachedRefs)
      attached->Release();
    if (m_clipper) m_clipper->Release();
    if (m_palette) m_palette->Release();
    if (m_foreignStage) m_foreignStage->Release();
    if (!m_owner)
      m_dd->Release();
  }

  Surface* Surface::fromInterface(IDirectDrawSurface7* surface) {
    if (!surface)
      return nullptr;
    if (*reinterpret_cast<void* const*>(surface) == s_vtable)
      return static_cast<Surface*>(surface);
    // Games hand a surface's older interfaces where DirectDraw 7 takes one,
    // as Windows' ddraw.dll accepts (its interfaces share one object).
    if (Surface1* old = Surface1::fromInterface(reinterpret_cast<IDirectDrawSurface*>(surface)))
      return old->owner();
    return nullptr;
  }

  namespace {
    uint32_t maskChannel(uint32_t pixel, uint32_t mask) {
      if (!mask)
        return 0;
      unsigned long shift = 0;
      _BitScanForward(&shift, mask);
      const uint32_t maximum = mask >> shift;
      return ((pixel & mask) >> shift) * 255u / maximum;
    }

    uint32_t yuvToXrgb(int y, int u, int v) {
      // BT.601, video range.
      const int c = (y - 16) * 298, d = u - 128, e = v - 128;
      auto clamp = [](int value) { return uint32_t(value < 0 ? 0 : value > 255 ? 255 : value); };
      const uint32_t r = clamp((c + 409 * e + 128) >> 8);
      const uint32_t g = clamp((c - 100 * d - 208 * e + 128) >> 8);
      const uint32_t b = clamp((c + 516 * d + 128) >> 8);
      return 0xff000000u | r << 16 | g << 8 | b;
    }

    // Rows of another DirectDraw's surface as X8R8G8B8: RGB of any masks,
    // YUY2 and UYVY (DirectShow's video samples).
    bool convertForeign(const DDPIXELFORMAT& pf, const uint8_t* src, LONG srcPitch, uint8_t* dst, LONG dstPitch,
                        uint32_t width, uint32_t height) {
      if (pf.dwFlags & DDPF_FOURCC) {
        const bool yuy2 = pf.dwFourCC == MAKEFOURCC('Y', 'U', 'Y', '2');
        if (!yuy2 && pf.dwFourCC != MAKEFOURCC('U', 'Y', 'V', 'Y'))
          return false;
        for (uint32_t row = 0; row < height; row++) {
          const uint8_t* s = src + ptrdiff_t(row) * srcPitch;
          auto* d = reinterpret_cast<uint32_t*>(dst + ptrdiff_t(row) * dstPitch);
          for (uint32_t x = 0; x < width; x += 2, s += 4) {
            const int y0 = yuy2 ? s[0] : s[1], y1 = yuy2 ? s[2] : s[3];
            const int u = yuy2 ? s[1] : s[0], v = yuy2 ? s[3] : s[2];
            d[x] = yuvToXrgb(y0, u, v);
            if (x + 1 < width)
              d[x + 1] = yuvToXrgb(y1, u, v);
          }
        }
        return true;
      }
      if (!(pf.dwFlags & DDPF_RGB))
        return false;
      const uint32_t bytes = pf.dwRGBBitCount / 8;
      if (bytes < 2 || bytes > 4)
        return false;
      const bool xrgb = bytes == 4 && pf.dwRBitMask == 0xff0000 && pf.dwGBitMask == 0xff00 && pf.dwBBitMask == 0xff;
      for (uint32_t row = 0; row < height; row++) {
        const uint8_t* s = src + ptrdiff_t(row) * srcPitch;
        auto* d = reinterpret_cast<uint32_t*>(dst + ptrdiff_t(row) * dstPitch);
        if (xrgb) {
          std::memcpy(d, s, size_t(width) * 4);
          continue;
        }
        for (uint32_t x = 0; x < width; x++, s += bytes) {
          uint32_t pixel = 0;
          std::memcpy(&pixel, s, bytes);
          d[x] = 0xff000000u | maskChannel(pixel, pf.dwRBitMask) << 16 | maskChannel(pixel, pf.dwGBitMask) << 8
               | maskChannel(pixel, pf.dwBBitMask);
        }
      }
      return true;
    }
  }

  // A surface of another DirectDraw: DirectShow's video streams make their
  // own with Windows' ddraw.dll when ours offers no DirectDraw 1, and games
  // blit their frames into ours. Its pixels go through a surface of ours.
  Surface* Surface::stageForeign(IDirectDrawSurface7* foreign) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    if (FAILED(foreign->GetSurfaceDesc(&desc)) || !desc.dwWidth || !desc.dwHeight)
      return nullptr;
    if (!m_foreignStage || m_foreignStage->m_desc.dwWidth != desc.dwWidth
     || m_foreignStage->m_desc.dwHeight != desc.dwHeight) {
      if (m_foreignStage)
        m_foreignStage->Release();
      m_foreignStage = nullptr;
      DDSURFACEDESC2 stage = {};
      stage.dwSize = sizeof(stage);
      stage.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
      stage.dwWidth = desc.dwWidth;
      stage.dwHeight = desc.dwHeight;
      stage.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
      stage.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
      stage.ddpfPixelFormat.dwFlags = DDPF_RGB;
      stage.ddpfPixelFormat.dwRGBBitCount = 32;
      stage.ddpfPixelFormat.dwRBitMask = 0xff0000;
      stage.ddpfPixelFormat.dwGBitMask = 0xff00;
      stage.ddpfPixelFormat.dwBBitMask = 0xff;
      IDirectDrawSurface7* created = nullptr;
      if (FAILED(m_dd->CreateSurface(&stage, &created, nullptr)) || !created)
        return nullptr;
      m_foreignStage = static_cast<Surface*>(created);
    }
    DDSURFACEDESC2 from = {}, to = {};
    from.dwSize = to.dwSize = sizeof(DDSURFACEDESC2);
    if (FAILED(foreign->Lock(nullptr, &from, DDLOCK_WAIT | DDLOCK_READONLY, nullptr)))
      return nullptr;
    bool converted = false;
    if (SUCCEEDED(m_foreignStage->Lock(nullptr, &to, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) {
      converted = convertForeign(desc.ddpfPixelFormat, static_cast<const uint8_t*>(from.lpSurface), from.lPitch,
        static_cast<uint8_t*>(to.lpSurface), to.lPitch, desc.dwWidth, desc.dwHeight);
      m_foreignStage->Unlock(nullptr);
    }
    foreign->Unlock(nullptr);
    if (!converted) {
      logMessage(LogLevel::Warn, "ddraw: blit from a foreign surface of format flags=0x%lx bits=%lu fourcc=0x%lx",
        desc.ddpfPixelFormat.dwFlags, desc.ddpfPixelFormat.dwRGBBitCount, desc.ddpfPixelFormat.dwFourCC);
      return nullptr;
    }
    return m_foreignStage;
  }

  HRESULT Surface::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface7) {
      *ppv = static_cast<IDirectDrawSurface7*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_IDirectDrawGammaControl) {
      *ppv = static_cast<IDirectDrawGammaControl*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_IDirectDrawSurface) {
      *ppv = &m_surface1;
      AddRef();
      return S_OK;
    }
    char text[64];
    snprintf(text, sizeof(text), "IDirectDrawSurface7::QueryInterface %08lX", riid.Data1);
    logUnsupported(text);
    return E_NOINTERFACE;
  }

  // Parts of a complex surface live as long as it does.
  ULONG Surface::AddRef() {
    if (m_owner)
      return m_owner->AddRef();
    return ++m_ref;
  }

  ULONG Surface::Release() {
    if (m_owner)
      return m_owner->Release();
    const ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  Surface* Surface::attachedDepth() const {
    for (Surface* s : m_attachedRefs)
      if (s->m_desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return s;
    for (Surface* s : m_attached)
      if (s->m_desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return s;
    return nullptr;
  }

  HRESULT Surface::AddAttachedSurface(LPDIRECTDRAWSURFACE7 surface) {
    auto* other = fromInterface(surface);
    if (!other) return DDERR_INVALIDPARAMS;
    if (std::find(m_attachedRefs.begin(), m_attachedRefs.end(), other) != m_attachedRefs.end())
      return DDERR_SURFACEALREADYATTACHED;
    other->AddRef();
    m_attachedRefs.push_back(other);
    return DD_OK;
  }

  HRESULT Surface::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 surface) {
    if (!surface) {
      for (Surface* attached : m_attachedRefs) attached->Release();
      m_attachedRefs.clear();
      return DD_OK;
    }
    auto it = std::find(m_attachedRefs.begin(), m_attachedRefs.end(), fromInterface(surface));
    if (it == m_attachedRefs.end())
      return DDERR_SURFACENOTATTACHED;
    (*it)->Release();
    m_attachedRefs.erase(it);
    return DD_OK;
  }

  HRESULT Surface::EnumAttachedSurfaces(LPVOID context, LPDDENUMSURFACESCALLBACK7 callback) {
    if (!callback) return DDERR_INVALIDPARAMS;
    std::vector<Surface*> all = m_attached;
    all.insert(all.end(), m_attachedRefs.begin(), m_attachedRefs.end());
    for (Surface* s : all) {
      s->AddRef();
      DDSURFACEDESC2 desc = {};
      desc.dwSize = sizeof(desc);
      s->GetSurfaceDesc(&desc);
      if (callback(s, &desc, context) == DDENUMRET_CANCEL)
        break;
    }
    return DD_OK;
  }

  HRESULT Surface::GetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* surface) {
    if (!caps || !surface) return DDERR_INVALIDPARAMS;
    *surface = nullptr;
    std::vector<Surface*> all = m_attached;
    all.insert(all.end(), m_attachedRefs.begin(), m_attachedRefs.end());
    for (Surface* s : all) {
      const DDSCAPS2& have = s->m_desc.ddsCaps;
      if ((have.dwCaps & caps->dwCaps) == caps->dwCaps && (have.dwCaps2 & caps->dwCaps2) == caps->dwCaps2) {
        s->AddRef();
        *surface = s;
        return DD_OK;
      }
    }
    return DDERR_NOTFOUND;
  }

  HRESULT Surface::GetCaps(LPDDSCAPS2 caps) {
    if (!caps) return DDERR_INVALIDPARAMS;
    *caps = m_desc.ddsCaps;
    return DD_OK;
  }

  HRESULT Surface::GetClipper(LPDIRECTDRAWCLIPPER* clipper) {
    if (!clipper) return DDERR_INVALIDPARAMS;
    *clipper = m_clipper;
    if (!m_clipper) return DDERR_NOCLIPPERATTACHED;
    m_clipper->AddRef();
    return DD_OK;
  }

  HRESULT Surface::SetClipper(LPDIRECTDRAWCLIPPER clipper) {
    auto* next = static_cast<Clipper*>(clipper);
    if (next) next->AddRef();
    if (m_clipper) m_clipper->Release();
    m_clipper = next;
    return DD_OK;
  }

  HRESULT Surface::GetColorKey(DWORD flags, LPDDCOLORKEY key) {
    if (!key) return DDERR_INVALIDPARAMS;
    if ((flags & DDCKEY_SRCBLT) && m_hasSrcBltKey) {
      *key = m_srcBltKey;
      return DD_OK;
    }
    return DDERR_NOCOLORKEY;
  }

  HRESULT Surface::SetColorKey(DWORD flags, LPDDCOLORKEY key) {
    if (flags & DDCKEY_SRCBLT) {
      m_hasSrcBltKey = key != nullptr;
      if (key) m_srcBltKey = *key;
      if (key) { m_desc.dwFlags |= DDSD_CKSRCBLT; m_desc.ddckCKSrcBlt = *key; }
      else m_desc.dwFlags &= ~DDSD_CKSRCBLT;
    }
    return DD_OK;
  }

  void Surface::applyColorKey(bool enabled) {
    if (!m_texture)
      return;
    if (!enabled || !m_hasSrcBltKey) {
      m_texture->setColorKey(false, 0);
      return;
    }
    uint8_t raw[4] = {};
    std::memcpy(raw, &m_srcBltKey.dwColorSpaceLowValue, 4);
    m_texture->setColorKey(true, legacy::toArgb(m_texture->desc().format, raw, nullptr));
  }

  HRESULT Surface::GetPalette(LPDIRECTDRAWPALETTE* palette) {
    if (!palette) return DDERR_INVALIDPARAMS;
    *palette = m_palette;
    if (!m_palette) return DDERR_NOPALETTEATTACHED;
    m_palette->AddRef();
    return DD_OK;
  }

  HRESULT Surface::SetPalette(LPDIRECTDRAWPALETTE palette) {
    auto* next = static_cast<Palette*>(palette);
    if (next) next->AddRef();
    if (m_palette) m_palette->Release();
    m_palette = next;
    m_paletteVersion = UINT32_MAX;
    updateTexturePalette();
    return DD_OK;
  }

  void Surface::updateTexturePalette() {
    if (!m_texture || !m_palette || m_paletteVersion == m_palette->version())
      return;
    m_paletteVersion = m_palette->version();
    m_texture->setPalette(m_palette->argb());
  }

  HRESULT Surface::GetPixelFormat(LPDDPIXELFORMAT format) {
    if (!format) return DDERR_INVALIDPARAMS;
    *format = m_desc.ddpfPixelFormat;
    return DD_OK;
  }

  HRESULT Surface::GetSurfaceDesc(LPDDSURFACEDESC2 desc) {
    if (!desc) return DDERR_INVALIDPARAMS;
    const DWORD size = desc->dwSize;
    if (size != sizeof(DDSURFACEDESC2) && size != sizeof(DDSURFACEDESC))
      return DDERR_INVALIDPARAMS;
    std::memcpy(desc, &m_desc, size);
    desc->dwSize = size;
    return DD_OK;
  }

  HRESULT Surface::Lock(LPRECT rect, LPDDSURFACEDESC2 desc, DWORD flags, HANDLE) {
    if (!desc || !m_texture) return DDERR_INVALIDPARAMS;
    traceCall("Lock", m_desc);
    void* data = nullptr;
    uint32_t pitch = 0;
    const legacy::Rect* lockRect = reinterpret_cast<const legacy::Rect*>(rect);
    if (!m_texture->lock(m_subresource, lockRect, (flags & DDLOCK_READONLY) != 0, &data, &pitch))
      return lockRect ? DDERR_INVALIDPARAMS : DDERR_GENERIC;   // rectangles outside the surface
    const DWORD size = desc->dwSize ? desc->dwSize : sizeof(DDSURFACEDESC2);
    std::memcpy(desc, &m_desc, (std::min)(size_t(size), sizeof(DDSURFACEDESC2)));
    desc->dwSize = size;
    desc->dwFlags |= DDSD_LPSURFACE | DDSD_PITCH;
    desc->lpSurface = data;
    desc->lPitch = LONG(pitch);
    return DD_OK;
  }

  HRESULT Surface::Unlock(LPRECT) {
    if (m_texture)
      m_texture->unlock(m_subresource);
    // The primary is the screen: what is written into it shows at once.
    if (isPrimary() && m_dd->fullscreen())
      presentTo(nullptr, this, nullptr, 0);
    return DD_OK;
  }


  HRESULT Surface::GetDC(HDC* dc) {
    if (!dc || !m_texture) return DDERR_INVALIDPARAMS;
    if (m_dc) return DDERR_DCALREADYCREATED;
    const uint32_t w = m_texture->width(m_subresource), h = m_texture->height(m_subresource);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LONG(w);
    info.bmiHeader.biHeight = -LONG(h);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    m_bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &m_dcBits, nullptr, 0);
    if (!m_bitmap) return DDERR_GENERIC;
    m_dc = CreateCompatibleDC(nullptr);
    m_oldBitmap = SelectObject(m_dc, m_bitmap);
    void* data = nullptr;
    uint32_t pitch = 0;
    if (m_texture->lock(m_subresource, nullptr, true, &data, &pitch)) {
      const Format format = m_texture->desc().format;
      const uint32_t bpp = legacy::formatInfo(format).bytesPerBlock;
      for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
          static_cast<uint32_t*>(m_dcBits)[size_t(y) * w + x] =
            legacy::toArgb(format, static_cast<const uint8_t*>(data) + size_t(y) * pitch + size_t(x) * bpp, nullptr);
      m_texture->unlock(m_subresource);
    }
    *dc = m_dc;
    return DD_OK;
  }

  HRESULT Surface::ReleaseDC(HDC dc) {
    if (!m_dc || dc != m_dc) return DDERR_INVALIDPARAMS;
    GdiFlush();
    const uint32_t w = m_texture->width(m_subresource), h = m_texture->height(m_subresource);
    void* data = nullptr;
    uint32_t pitch = 0;
    if (m_texture->lock(m_subresource, nullptr, false, &data, &pitch)) {
      const Format format = m_texture->desc().format;
      const uint32_t bpp = legacy::formatInfo(format).bytesPerBlock;
      for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
          legacy::fromArgb(format, static_cast<const uint32_t*>(m_dcBits)[size_t(y) * w + x] | 0xff000000u,
            static_cast<uint8_t*>(data) + size_t(y) * pitch + size_t(x) * bpp);
      m_texture->unlock(m_subresource);
    }
    SelectObject(m_dc, m_oldBitmap);
    DeleteDC(m_dc);
    DeleteObject(m_bitmap);
    m_dc = nullptr;
    m_bitmap = nullptr;
    m_dcBits = nullptr;
    if (isPrimary() && m_dd->fullscreen())
      presentTo(nullptr, this, nullptr, 0);
    return DD_OK;
  }

  HRESULT Surface::presentTo(const RECT* screenRect, Surface* source, const RECT* sourceRect, DWORD syncInterval) {
    (void)screenRect;
    legacy::Device* device = m_dd->legacyDevice();
    if (!device || !source || !source->m_texture)
      return DDERR_GENERIC;
    HWND window = m_dd->window();
    if (!m_dd->fullscreen() && m_clipper && m_clipper->window())
      window = m_clipper->window();
    if (!window)
      return DD_OK;
    const HRESULT hr = device->present(source->m_texture.get(), reinterpret_cast<const legacy::Rect*>(sourceRect),
      window, syncInterval, &m_gamma.red[0]);
    if (FAILED(hr))
      logMessage(LogLevel::Warn, "ddraw: present failed hr=0x%08lx", hr);
    return DD_OK;
  }

  HRESULT Surface::Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) {
    traceCall("Blt", m_desc);
    const HRESULT hr = blt(dstRect, src, srcRect, flags, fx);
    if (SUCCEEDED(hr) && isPrimary() && m_dd->fullscreen())
      presentTo(nullptr, this, nullptr, 0);
    return hr;
  }

  HRESULT Surface::blt(LPRECT dstRect, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) {
    legacy::Device* device = m_dd->legacyDevice();
    if (flags & DDBLT_COLORFILL) {
      if (!fx || !device || !m_texture) return DDERR_INVALIDPARAMS;
      device->fill(m_texture.get(), m_subresource, reinterpret_cast<const legacy::Rect*>(dstRect), fx->dwFillColor);
      return DD_OK;
    }
    if (flags & DDBLT_DEPTHFILL) {
      if (!fx || !device || !m_texture || !m_texture->dsv()) return DDERR_INVALIDPARAMS;
      const uint32_t bits = m_desc.ddpfPixelFormat.dwZBufferBitDepth;
      const double maximum = bits >= 32 ? 4294967295.0 : double((1ull << bits) - 1);
      device->context()->ClearDepthStencilView(m_texture->dsv(), D3D11_CLEAR_DEPTH, float(double(fx->dwFillDepth) / maximum), 0);
      return DD_OK;
    }
    if (!src) return DDERR_INVALIDPARAMS;
    Surface* source = fromInterface(src);
    if (!source && !(source = stageForeign(src)))
      return DDERR_GENERIC;
    if (isPrimary() && !m_dd->fullscreen())
      return presentTo(dstRect, source, srcRect, 1);
    if (!device || !m_texture || !source->m_texture) return DDERR_GENERIC;
    source->updateTexturePalette();
    bool colorKey = false;
    uint32_t key = 0;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) {
      colorKey = true;
      key = fx->ddckSrcColorkey.dwColorSpaceLowValue;
    } else if ((flags & DDBLT_KEYSRC) && source->m_hasSrcBltKey) {
      colorKey = true;
      key = source->m_srcBltKey.dwColorSpaceLowValue;
    }
    bool scaled = false;
    if (dstRect && srcRect)
      scaled = (dstRect->right - dstRect->left) != (srcRect->right - srcRect->left)
            || (dstRect->bottom - dstRect->top) != (srcRect->bottom - srcRect->top);
    device->blit(m_texture.get(), m_subresource, reinterpret_cast<const legacy::Rect*>(dstRect), source->m_texture.get(),
      source->m_subresource, reinterpret_cast<const legacy::Rect*>(srcRect), scaled, colorKey, key);
    return DD_OK;
  }

  HRESULT Surface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags) {
    if (!src) return DDERR_INVALIDPARAMS;
    RECT sr = {};
    if (srcRect) {
      sr = *srcRect;
    } else if (Surface* source = fromInterface(src)) {
      if (!source->m_texture) return DDERR_INVALIDPARAMS;
      sr = RECT{ 0, 0, LONG(source->m_texture->width(source->m_subresource)),
                 LONG(source->m_texture->height(source->m_subresource)) };
    } else {
      DDSURFACEDESC2 desc = {};
      desc.dwSize = sizeof(desc);
      if (FAILED(src->GetSurfaceDesc(&desc))) return DDERR_INVALIDPARAMS;
      sr = RECT{ 0, 0, LONG(desc.dwWidth), LONG(desc.dwHeight) };
    }
    RECT dr = { LONG(x), LONG(y), LONG(x) + (sr.right - sr.left), LONG(y) + (sr.bottom - sr.top) };
    traceCall("BltFast", m_desc);
    return Blt(&dr, src, &sr, (flags & DDBLTFAST_SRCCOLORKEY) ? DDBLT_KEYSRC : 0, nullptr);
  }

  HRESULT Surface::Flip(LPDIRECTDRAWSURFACE7 target, DWORD flags) {
    Surface* back = fromInterface(target);
    if (!back) {
      for (Surface* s : m_attached)
        if (s->m_desc.ddsCaps.dwCaps & DDSCAPS_BACKBUFFER) { back = s; break; }
    }
    if (!back)
      return DDERR_NOTFLIPPABLE;
    traceCall("Flip", m_desc);
    return presentTo(nullptr, back, nullptr, (flags & DDFLIP_NOVSYNC) ? 0 : 1);
  }

  HRESULT Surface::GetDDInterface(LPVOID* dd) {
    if (!dd) return DDERR_INVALIDPARAMS;
    m_dd->AddRef();
    *dd = static_cast<IDirectDraw7*>(m_dd);
    return DD_OK;
  }

  HRESULT Surface::SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) {
    logUnsupported("IDirectDrawSurface7::SetSurfaceDesc");
    return DDERR_UNSUPPORTED;
  }

  HRESULT Surface::SetPrivateData(REFGUID guid, LPVOID data, DWORD size, DWORD flags) {
    if (flags & DDSPD_IUNKNOWNPOINTER) {
      logUnsupported("IDirectDrawSurface7::SetPrivateData(IUnknown)");
      return DDERR_UNSUPPORTED;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    m_privateData[guidKey(guid)] = std::vector<uint8_t>(bytes, bytes + size);
    return DD_OK;
  }

  HRESULT Surface::GetPrivateData(REFGUID guid, LPVOID data, LPDWORD size) {
    if (!size) return DDERR_INVALIDPARAMS;
    auto it = m_privateData.find(guidKey(guid));
    if (it == m_privateData.end()) return DDERR_NOTFOUND;
    if (!data || *size < it->second.size()) {
      *size = DWORD(it->second.size());
      return DDERR_MOREDATA;
    }
    std::memcpy(data, it->second.data(), it->second.size());
    *size = DWORD(it->second.size());
    return DD_OK;
  }

  HRESULT Surface::FreePrivateData(REFGUID guid) {
    return m_privateData.erase(guidKey(guid)) ? DD_OK : DDERR_NOTFOUND;
  }

  HRESULT Surface::GetUniquenessValue(LPDWORD value) {
    if (!value) return DDERR_INVALIDPARAMS;
    *value = m_uniqueness;
    return DD_OK;
  }

  HRESULT Surface::GetPriority(LPDWORD priority) {
    if (!priority) return DDERR_INVALIDPARAMS;
    *priority = m_priority;
    return DD_OK;
  }

  HRESULT Surface::GetLOD(LPDWORD lod) {
    if (!lod) return DDERR_INVALIDPARAMS;
    *lod = m_lod;
    return DD_OK;
  }

  HRESULT Surface::GetGammaRamp(DWORD, LPDDGAMMARAMP ramp) {
    if (!ramp) return DDERR_INVALIDPARAMS;
    *ramp = m_gamma;
    return DD_OK;
  }

  HRESULT Surface::SetGammaRamp(DWORD, LPDDGAMMARAMP ramp) {
    if (!ramp) return DDERR_INVALIDPARAMS;
    m_gamma = *ramp;
    return DD_OK;
  }

  // ============================================================
  // Vertex buffer
  // ============================================================

  VertexBuffer7::VertexBuffer7(DirectDraw* dd, const D3DVERTEXBUFFERDESC& desc)
  : m_dd(dd), m_desc(desc), m_stride(legacy::fvfStride(desc.dwFVF)) {
    m_dd->AddRef();
    if (legacy::Device* device = m_dd->legacyDevice())
      m_buffer = std::make_unique<legacy::Buffer>(device, (std::max)(uint32_t(1), uint32_t(m_stride * desc.dwNumVertices)), false);
  }

  VertexBuffer7::~VertexBuffer7() {
    m_buffer.reset();
    m_dd->Release();
  }

  HRESULT VertexBuffer7::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDirect3DVertexBuffer7) {
      *ppv = static_cast<IDirect3DVertexBuffer7*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG VertexBuffer7::Release() {
    const ULONG ref = --m_ref;
    if (!ref) delete this;
    return ref;
  }

  HRESULT VertexBuffer7::Lock(DWORD flags, LPVOID* data, LPDWORD size) {
    if (!data || !m_buffer) return DDERR_INVALIDPARAMS;
    *data = m_buffer->data();
    if (size) *size = m_buffer->size();
    // Without no-overwrite the application may change what draws read.
    if (!(flags & DDLOCK_NOOVERWRITE) && !(flags & DDLOCK_READONLY))
      m_buffer->invalidate();
    return D3D_OK;
  }

  HRESULT VertexBuffer7::Unlock() {
    return D3D_OK;
  }

  HRESULT VertexBuffer7::ProcessVertices(DWORD op, DWORD dstIndex, DWORD count, LPDIRECT3DVERTEXBUFFER7 src, DWORD srcIndex,
                                         LPDIRECT3DDEVICE7 device, DWORD flags) {
    auto* source = static_cast<VertexBuffer7*>(src);
    (void)device;  // the one of this DirectDraw
    if (!source || !m_buffer || !source->m_buffer || !(op & D3DVOP_TRANSFORM))
      return DDERR_INVALIDPARAMS;
    if ((m_desc.dwFVF & legacy::FvfPositionMask) != legacy::FvfXyzRhw
     || (source->m_desc.dwFVF & legacy::FvfPositionMask) == legacy::FvfXyzRhw)
      return DDERR_INVALIDPARAMS;
    if (srcIndex + count > source->m_desc.dwNumVertices || dstIndex + count > m_desc.dwNumVertices)
      return DDERR_INVALIDPARAMS;
    legacy::Device* legacy = m_dd->legacyDevice();
    if (!legacy)
      return DDERR_GENERIC;
    legacy->processVertices(source->m_buffer->data() + size_t(srcIndex) * source->m_stride, source->m_desc.dwFVF, source->m_stride,
      m_buffer->data() + size_t(dstIndex) * m_stride, m_desc.dwFVF, m_stride, count, (op & D3DVOP_LIGHT) != 0,
      !(flags & D3DPV_DONOTCOPYDATA));
    m_buffer->invalidate(dstIndex * m_stride, count * m_stride);
    return D3D_OK;
  }

  HRESULT VertexBuffer7::ProcessVerticesStrided(DWORD, DWORD, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD,
                                                LPDIRECT3DDEVICE7, DWORD) {
    logUnsupported("IDirect3DVertexBuffer7::ProcessVerticesStrided");
    return DDERR_UNSUPPORTED;
  }

  HRESULT VertexBuffer7::GetVertexBufferDesc(LPD3DVERTEXBUFFERDESC desc) {
    if (!desc) return DDERR_INVALIDPARAMS;
    const DWORD size = desc->dwSize;
    *desc = m_desc;
    if (size) desc->dwSize = size;
    return D3D_OK;
  }

  // ============================================================
  // Device caps
  // ============================================================

  namespace {
    void fillPrimCaps(D3DPRIMCAPS& caps) {
      std::memset(&caps, 0, sizeof(caps));
      caps.dwSize = sizeof(caps);
      caps.dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
      caps.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX
        | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS | D3DPRASTERCAPS_ANISOTROPY
        | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_WBUFFER;
      caps.dwZCmpCaps = caps.dwAlphaCmpCaps = 0xff;
      caps.dwSrcBlendCaps = caps.dwDestBlendCaps = 0x1fff;
      caps.dwShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND
        | D3DPSHADECAPS_FOGGOURAUD | D3DPSHADECAPS_COLORFLATRGB;
      caps.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY
        | D3DPTEXTURECAPS_BORDER | D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_COLORKEYBLEND
        | D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
      caps.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST
        | D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR
        | D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC
        | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR
        | D3DPTFILTERCAPS_MAGFANISOTROPIC;
      caps.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA
        | D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
      caps.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP
        | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    }

    void fillDeviceDesc(D3DDEVICEDESC7& desc, const GUID& guid) {
      std::memset(&desc, 0, sizeof(desc));
      desc.dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY
        | D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY
        | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM
        | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_CANBLTSYSTONONLOCAL;
      if (guid == IID_IDirect3DTnLHalDevice)
        desc.dwDevCaps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
      fillPrimCaps(desc.dpcLineCaps);
      fillPrimCaps(desc.dpcTriCaps);
      desc.dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
      desc.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
      desc.dwMinTextureWidth = desc.dwMinTextureHeight = 1;
      desc.dwMaxTextureWidth = desc.dwMaxTextureHeight = 8192;
      desc.dwMaxTextureRepeat = 8192;
      desc.dwMaxTextureAspectRatio = 8192;
      desc.dwMaxAnisotropy = 16;
      desc.dvGuardBandLeft = desc.dvGuardBandTop = -16384.0f;
      desc.dvGuardBandRight = desc.dvGuardBandBottom = 16384.0f;
      desc.dwStencilCaps = 0xff;
      desc.dwFVFCaps = 8;
      desc.dwTextureOpCaps = 0x03ffffff;
      desc.wMaxTextureBlendStages = 8;
      desc.wMaxSimultaneousTextures = 8;
      desc.dwMaxActiveLights = 8;
      desc.dvMaxVertexW = 1e10f;
      desc.deviceGUID = guid;
      desc.wMaxUserClipPlanes = 6;
      desc.wMaxVertexBlendMatrices = 4;
      desc.dwVertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_VERTEXFOG
        | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    }

    const Format kTextureFormats[] = {
      legacy::FormatA8R8G8B8, legacy::FormatX8R8G8B8, legacy::FormatR5G6B5, legacy::FormatX1R5G5B5,
      legacy::FormatA1R5G5B5, legacy::FormatA4R4G4B4, legacy::FormatDXT1, legacy::FormatDXT2, legacy::FormatDXT3,
      legacy::FormatDXT4, legacy::FormatDXT5, legacy::FormatL8, legacy::FormatA8L8, legacy::FormatV8U8, legacy::FormatP8,
    };
  }

  // ============================================================
  // Device
  // ============================================================

  struct Device7::StateBlock {
    std::unordered_map<uint32_t, uint32_t> renderStates;
    std::unordered_map<uint32_t, uint32_t> stageStates;       // stage << 16 | type
    std::unordered_map<uint32_t, ComPtr<Surface>> textures;   // held: the application may release them
    std::unordered_map<uint32_t, D3DMATRIX> transforms;
    bool hasViewport = false;
    D3DVIEWPORT7 viewport = {};
    bool hasMaterial = false;
    D3DMATERIAL7 material = {};
    std::unordered_map<uint32_t, D3DLIGHT7> lights;
    std::unordered_map<uint32_t, BOOL> lightEnables;
  };

  Device7::Device7(DirectDraw* dd, Surface* target, const GUID& type)
  : m_dd(dd), m_device(dd->legacyDevice()), m_target(target), m_type(type) {
    m_dd->AddRef();
    m_target->AddRef();
    // Direct3D 7 starts with depth testing when a z-buffer is attached.
    m_device->setRenderState(legacy::RsZEnable, m_target->attachedDepth() ? 1 : 0);
    bindTargets();
  }

  Device7::~Device7() {
    for (Surface*& texture : m_textures) {
      if (texture) texture->Release();
      texture = nullptr;
      }
    for (uint32_t stage = 0; stage < 8; stage++)
      m_device->setTexture(stage, nullptr);
    m_device->setRenderTarget(nullptr, 0);
    m_device->setDepthStencil(nullptr);
    m_target->Release();
    m_dd->Release();
  }

  HRESULT Device7::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice7) {
      *ppv = static_cast<IDirect3DDevice7*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG Device7::Release() {
    const ULONG ref = --m_ref;
    if (!ref) delete this;
    return ref;
  }

  void Device7::bindTargets() {
    m_device->setRenderTarget(m_target->texture(), m_target->subresource());
    Surface* depth = m_target->attachedDepth();
    m_device->setDepthStencil(depth ? depth->texture() : nullptr);
  }

  HRESULT Device7::GetCaps(LPD3DDEVICEDESC7 desc) {
    if (!desc) return DDERR_INVALIDPARAMS;
    fillDeviceDesc(*desc, m_type);
    return D3D_OK;
  }

  HRESULT Device7::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) {
    if (!callback) return DDERR_INVALIDPARAMS;
    for (Format format : kTextureFormats) {
      DDPIXELFORMAT pf;
      pixelFormatFromFormat(format, pf);
      if (callback(&pf, context) == D3DENUMRET_CANCEL)
        break;
    }
    return D3D_OK;
  }

  HRESULT Device7::GetDirect3D(LPDIRECT3D7* d3d) {
    if (!d3d) return DDERR_INVALIDPARAMS;
    *d3d = m_dd->direct3d();
    (*d3d)->AddRef();
    return D3D_OK;
  }

  HRESULT Device7::SetRenderTarget(LPDIRECTDRAWSURFACE7 surface, DWORD) {
    auto* next = Surface::fromInterface(surface);
    if (!next) return DDERR_INVALIDPARAMS;
    next->AddRef();
    m_target->Release();
    m_target = next;
    bindTargets();
    return D3D_OK;
  }

  HRESULT Device7::GetRenderTarget(LPDIRECTDRAWSURFACE7* surface) {
    if (!surface) return DDERR_INVALIDPARAMS;
    m_target->AddRef();
    *surface = m_target;
    return D3D_OK;
  }

  HRESULT Device7::Clear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) {
    if (count && !rects)
      return DDERR_INVALIDPARAMS;
    bindTargets();
    m_device->clear(count, reinterpret_cast<const legacy::Rect*>(rects), flags, color, z, stencil);
    return D3D_OK;
  }

  namespace {
    uint32_t transformState(D3DTRANSFORMSTATETYPE state) {
      switch (state) {
        case D3DTRANSFORMSTATE_WORLD:  return legacy::TransformWorld;
        case D3DTRANSFORMSTATE_WORLD1: return legacy::TransformWorld + 1;
        case D3DTRANSFORMSTATE_WORLD2: return legacy::TransformWorld + 2;
        case D3DTRANSFORMSTATE_WORLD3: return legacy::TransformWorld + 3;
        default: return uint32_t(state);
      }
    }
  }

  HRESULT Device7::SetTransform(D3DTRANSFORMSTATETYPE state, LPD3DMATRIX matrix) {
    if (!matrix) return DDERR_INVALIDPARAMS;
    if (m_recording) { m_recording->transforms[uint32_t(state)] = *matrix; return D3D_OK; }
    m_device->setTransform(transformState(state), &matrix->_11);
    return D3D_OK;
  }

  HRESULT Device7::GetTransform(D3DTRANSFORMSTATETYPE state, LPD3DMATRIX matrix) {
    if (!matrix) return DDERR_INVALIDPARAMS;
    m_device->getTransform(transformState(state), &matrix->_11);
    return D3D_OK;
  }

  HRESULT Device7::MultiplyTransform(D3DTRANSFORMSTATETYPE state, LPD3DMATRIX matrix) {
    if (!matrix) return DDERR_INVALIDPARAMS;
    float current[16], result[16];
    const float* m = &matrix->_11;
    m_device->getTransform(transformState(state), current);
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        result[r * 4 + c] = m[r * 4] * current[c] + m[r * 4 + 1] * current[4 + c]
                          + m[r * 4 + 2] * current[8 + c] + m[r * 4 + 3] * current[12 + c];
    m_device->setTransform(transformState(state), result);
    return D3D_OK;
  }

  HRESULT Device7::SetViewport(LPD3DVIEWPORT7 viewport) {
    if (!viewport) return DDERR_INVALIDPARAMS;
    if (m_recording) { m_recording->hasViewport = true; m_recording->viewport = *viewport; return D3D_OK; }
    m_device->setViewport({ viewport->dwX, viewport->dwY, viewport->dwWidth, viewport->dwHeight, viewport->dvMinZ,
                            viewport->dvMaxZ });
    return D3D_OK;
  }

  HRESULT Device7::GetViewport(LPD3DVIEWPORT7 viewport) {
    if (!viewport) return DDERR_INVALIDPARAMS;
    const legacy::Viewport& v = m_device->viewport();
    *viewport = { v.x, v.y, v.width, v.height, v.minZ, v.maxZ };
    return D3D_OK;
  }

  HRESULT Device7::SetMaterial(LPD3DMATERIAL7 material) {
    static_assert(sizeof(D3DMATERIAL7) == sizeof(legacy::Material), "material layouts differ");
    if (!material) return DDERR_INVALIDPARAMS;
    if (m_recording) { m_recording->hasMaterial = true; m_recording->material = *material; return D3D_OK; }
    m_device->setMaterial(*reinterpret_cast<const legacy::Material*>(material));
    return D3D_OK;
  }

  HRESULT Device7::GetMaterial(LPD3DMATERIAL7 material) {
    if (!material) return DDERR_INVALIDPARAMS;
    std::memcpy(material, &m_device->material(), sizeof(*material));
    return D3D_OK;
  }

  HRESULT Device7::SetLight(DWORD index, LPD3DLIGHT7 light) {
    static_assert(sizeof(D3DLIGHT7) == sizeof(legacy::Light), "light layouts differ");
    if (!light) return DDERR_INVALIDPARAMS;
    if (m_recording) { m_recording->lights[index] = *light; return D3D_OK; }
    return m_device->setLight(index, *reinterpret_cast<const legacy::Light*>(light)) ? D3D_OK : DDERR_OUTOFMEMORY;
  }

  HRESULT Device7::GetLight(DWORD index, LPD3DLIGHT7 light) {
    if (!light) return DDERR_INVALIDPARAMS;
    return m_device->getLight(index, reinterpret_cast<legacy::Light*>(light)) ? D3D_OK : DDERR_INVALIDPARAMS;
  }

  HRESULT Device7::LightEnable(DWORD index, BOOL enable) {
    if (m_recording) { m_recording->lightEnables[index] = enable; return D3D_OK; }
    return m_device->lightEnable(index, enable != FALSE) ? D3D_OK : DDERR_OUTOFMEMORY;
  }

  HRESULT Device7::GetLightEnable(DWORD index, BOOL* enable) {
    if (!enable) return DDERR_INVALIDPARAMS;
    *enable = m_device->lightEnabled(index);
    return D3D_OK;
  }

  HRESULT Device7::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    const uint32_t s = uint32_t(state);
    if (m_recording) { m_recording->renderStates[s] = value; return D3D_OK; }
    switch (s) {
      case D3DRENDERSTATE_COLORKEYENABLE:
        m_colorKeyEnable = value != 0;
        return D3D_OK;
      case 7: case 8: case 9: case 14: case 15: case 16: case 19: case 20: case 22: case 23: case 24: case 25:
      case 26: case 27: case 28: case 29: case 34: case 35: case 36: case 37: case 38: case 47: case 48:
      case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 60:
      case 136: case 137: case 139: case 140: case 141: case 142: case 143:
      case 145: case 146: case 147: case 148: case 151: case 152:
        m_device->setRenderState(s, value);
        return D3D_OK;
      case 2: case 4: case 10: case 30: case 31: case 32: case 33: case 39: case 40: case 49: case 50:
      case 51: case 138: case 144:
        return D3D_OK;
      default:
        if (s >= 128 && s <= 135)  // WRAP0-7
          return D3D_OK;
        {
          char text[64];
          snprintf(text, sizeof(text), "D3D7 render state %u", s);
          logUnsupported(text);
        }
        return D3D_OK;
    }
  }

  HRESULT Device7::GetRenderState(D3DRENDERSTATETYPE state, LPDWORD value) {
    if (!value) return DDERR_INVALIDPARAMS;
    *value = uint32_t(state) == D3DRENDERSTATE_COLORKEYENABLE ? m_colorKeyEnable : m_device->renderState(uint32_t(state));
    return D3D_OK;
  }

  HRESULT Device7::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (stage >= 8) return DDERR_INVALIDPARAMS;
    const uint32_t t = uint32_t(type);
    if (m_recording) { m_recording->stageStates[(stage << 16) | t] = value; return D3D_OK; }
    switch (t) {
      case D3DTSS_ADDRESS:
        m_device->setSamplerState(stage, legacy::SampAddressU, value);
        m_device->setSamplerState(stage, legacy::SampAddressV, value);
        return D3D_OK;
      case D3DTSS_ADDRESSU:      m_device->setSamplerState(stage, legacy::SampAddressU, value); return D3D_OK;
      case D3DTSS_ADDRESSV:      m_device->setSamplerState(stage, legacy::SampAddressV, value); return D3D_OK;
      case D3DTSS_BORDERCOLOR:   m_device->setSamplerState(stage, legacy::SampBorderColor, value); return D3D_OK;
      case D3DTSS_MAGFILTER:     // D3DTFG: point 1, linear 2, cubic 3/4, anisotropic 5
        m_device->setSamplerState(stage, legacy::SampMagFilter, value == 5 ? 3 : value >= 2 ? 2 : 1);
        return D3D_OK;
      case D3DTSS_MINFILTER:     // D3DTFN: point 1, linear 2, anisotropic 3
        m_device->setSamplerState(stage, legacy::SampMinFilter, value >= 1 && value <= 3 ? value : 1);
        return D3D_OK;
      case D3DTSS_MIPFILTER:     // D3DTFP: none 1, point 2, linear 3
        m_device->setSamplerState(stage, legacy::SampMipFilter, value >= 2 && value <= 3 ? value - 1 : 0);
        return D3D_OK;
      case D3DTSS_MIPMAPLODBIAS: m_device->setSamplerState(stage, legacy::SampMipMapLodBias, value); return D3D_OK;
      case D3DTSS_MAXMIPLEVEL:   m_device->setSamplerState(stage, legacy::SampMaxMipLevel, value); return D3D_OK;
      case D3DTSS_MAXANISOTROPY: m_device->setSamplerState(stage, legacy::SampMaxAnisotropy, value); return D3D_OK;
      default:
        m_device->setTextureStageState(stage, t, value);
        return D3D_OK;
    }
  }

  HRESULT Device7::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, LPDWORD value) {
    if (stage >= 8 || !value) return DDERR_INVALIDPARAMS;
    switch (uint32_t(type)) {
      case D3DTSS_ADDRESS:
      case D3DTSS_ADDRESSU:      *value = m_device->samplerState(stage, legacy::SampAddressU); break;
      case D3DTSS_ADDRESSV:      *value = m_device->samplerState(stage, legacy::SampAddressV); break;
      case D3DTSS_BORDERCOLOR:   *value = m_device->samplerState(stage, legacy::SampBorderColor); break;
      case D3DTSS_MAGFILTER: {
        const uint32_t f = m_device->samplerState(stage, legacy::SampMagFilter);
        *value = f == 3 ? 5 : f == 2 ? 2 : 1;
        break;
      }
      case D3DTSS_MINFILTER:     *value = (std::max)(1u, m_device->samplerState(stage, legacy::SampMinFilter)); break;
      case D3DTSS_MIPFILTER:     *value = m_device->samplerState(stage, legacy::SampMipFilter) + 1; break;
      case D3DTSS_MIPMAPLODBIAS: *value = m_device->samplerState(stage, legacy::SampMipMapLodBias); break;
      case D3DTSS_MAXMIPLEVEL:   *value = m_device->samplerState(stage, legacy::SampMaxMipLevel); break;
      case D3DTSS_MAXANISOTROPY: *value = m_device->samplerState(stage, legacy::SampMaxAnisotropy); break;
      default:                   *value = m_device->textureStageState(stage, uint32_t(type)); break;
    }
    return D3D_OK;
  }

  HRESULT Device7::SetTexture(DWORD stage, LPDIRECTDRAWSURFACE7 surface) {
    if (stage >= 8) return DDERR_INVALIDPARAMS;
    auto* next = Surface::fromInterface(surface);
    if (surface && !next) return DDERR_INVALIDPARAMS;
    if (m_recording) { m_recording->textures[stage] = next; return D3D_OK; }
    if (next) next->AddRef();
    if (m_textures[stage]) m_textures[stage]->Release();
    m_textures[stage] = next;
    m_device->setTexture(stage, next ? next->texture() : nullptr);
    return D3D_OK;
  }

  HRESULT Device7::GetTexture(DWORD stage, LPDIRECTDRAWSURFACE7* surface) {
    if (stage >= 8 || !surface) return DDERR_INVALIDPARAMS;
    *surface = m_textures[stage];
    if (*surface) (*surface)->AddRef();
    return D3D_OK;
  }

  void Device7::prepareDraw() {
    // Color-keyed textures: transparent texels, dropped like an alpha test.
    bool keyed = false;
    for (uint32_t stage = 0; stage < 8; stage++) {
      if (!m_textures[stage])
        continue;
      m_textures[stage]->applyColorKey(m_colorKeyEnable);
      DDCOLORKEY key;
      keyed = keyed || (m_colorKeyEnable && SUCCEEDED(m_textures[stage]->GetColorKey(DDCKEY_SRCBLT, &key)));
    }
    if (keyed && !m_device->renderState(legacy::RsAlphaTestEnable)) {
      m_device->setRenderState(legacy::RsAlphaTestEnable, 1);
      m_device->setRenderState(legacy::RsAlphaFunc, 5);   // greater
      m_device->setRenderState(legacy::RsAlphaRef, 0);
    }
  }

  namespace {
    legacy::PrimitiveType primitive(D3DPRIMITIVETYPE type) { return legacy::PrimitiveType(type); }
  }

  HRESULT Device7::DrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID vertices, DWORD count, DWORD) {
    prepareDraw();
    m_device->drawUP(primitive(type), fvf, vertices, legacy::fvfStride(fvf), count, nullptr, false, 0);
    return D3D_OK;
  }

  HRESULT Device7::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID vertices, DWORD vertexCount,
                                        LPWORD indices, DWORD indexCount, DWORD) {
    prepareDraw();
    m_device->drawUP(primitive(type), fvf, vertices, legacy::fvfStride(fvf), vertexCount, indices, false, indexCount);
    return D3D_OK;
  }

  namespace {
    // Interleaves strided vertex data into the flexible vertex format.
    std::vector<uint8_t> interleave(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA* data, DWORD count) {
      const uint32_t stride = legacy::fvfStride(fvf);
      std::vector<uint8_t> out(size_t(stride) * count);
      for (DWORD v = 0; v < count; v++) {
        uint8_t* dst = out.data() + size_t(v) * stride;
        auto copy = [&](const D3DDP_PTRSTRIDE& src, uint32_t size) {
          if (src.lpvData)
            std::memcpy(dst, static_cast<const uint8_t*>(src.lpvData) + size_t(v) * src.dwStride, size);
          dst += size;
        };
        const uint32_t position = fvf & D3DFVF_POSITION_MASK;
        copy(data->position, position == D3DFVF_XYZRHW ? 16 : position == D3DFVF_XYZ ? 12
          : 12 + 4 * ((position - D3DFVF_XYZB1) / 2 + 1));
        if (fvf & D3DFVF_NORMAL) copy(data->normal, 12);
        if (fvf & D3DFVF_DIFFUSE) copy(data->diffuse, 4);
        if (fvf & D3DFVF_SPECULAR) copy(data->specular, 4);
        const DWORD texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        for (DWORD t = 0; t < texCount && t < 8; t++) {
          static const uint32_t sizes[4] = { 2, 3, 4, 1 };
          copy(data->textureCoords[t], 4 * sizes[(fvf >> (16 + t * 2)) & 3]);
        }
      }
      return out;
    }
  }

  HRESULT Device7::DrawPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
                                        DWORD count, DWORD) {
    if (!data) return DDERR_INVALIDPARAMS;
    prepareDraw();
    const auto vertices = interleave(fvf, data, count);
    m_device->drawUP(primitive(type), fvf, vertices.data(), legacy::fvfStride(fvf), count, nullptr, false, 0);
    return D3D_OK;
  }

  HRESULT Device7::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
                                               DWORD vertexCount, LPWORD indices, DWORD indexCount, DWORD) {
    if (!data) return DDERR_INVALIDPARAMS;
    prepareDraw();
    const auto vertices = interleave(fvf, data, vertexCount);
    m_device->drawUP(primitive(type), fvf, vertices.data(), legacy::fvfStride(fvf), vertexCount, indices, false, indexCount);
    return D3D_OK;
  }

  HRESULT Device7::DrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count, DWORD) {
    auto* buffer = static_cast<VertexBuffer7*>(vb);
    if (!buffer || !buffer->buffer()) return DDERR_INVALIDPARAMS;
    prepareDraw();
    m_device->draw(primitive(type), buffer->fvf(), buffer->buffer(), buffer->stride(), start, count, nullptr, false, 0, 0, 0);
    return D3D_OK;
  }

  HRESULT Device7::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count,
                                          LPWORD indices, DWORD indexCount, DWORD) {
    auto* buffer = static_cast<VertexBuffer7*>(vb);
    if (!buffer || !buffer->buffer()) return DDERR_INVALIDPARAMS;
    prepareDraw();
    m_device->drawIndexedVB(primitive(type), buffer->fvf(), buffer->buffer(), buffer->stride(), int32_t(start), count,
      indices, false, indexCount);
    return D3D_OK;
  }

  HRESULT Device7::SetClipStatus(LPD3DCLIPSTATUS status) {
    if (status) m_clipStatus = *status;
    return D3D_OK;
  }

  HRESULT Device7::GetClipStatus(LPD3DCLIPSTATUS status) {
    if (!status) return DDERR_INVALIDPARAMS;
    *status = m_clipStatus;
    return D3D_OK;
  }

  HRESULT Device7::ComputeSphereVisibility(LPD3DVECTOR, LPD3DVALUE, DWORD count, DWORD, LPDWORD results) {
    if (results)
      for (DWORD i = 0; i < count; i++) results[i] = 0;
    return D3D_OK;
  }

  HRESULT Device7::ValidateDevice(LPDWORD passes) {
    if (passes) *passes = 1;
    return D3D_OK;
  }

  Device7::StateBlock Device7::capture() const {
    StateBlock block;
    for (uint32_t s = 1; s < legacy::RsCount; s++)
      block.renderStates[s] = m_device->renderState(s);
    block.renderStates[D3DRENDERSTATE_COLORKEYENABLE] = m_colorKeyEnable;
    for (uint32_t stage = 0; stage < 8; stage++) {
      for (uint32_t t = 1; t <= 24; t++) {
        DWORD value = 0;
        const_cast<Device7*>(this)->GetTextureStageState(stage, D3DTEXTURESTAGESTATETYPE(t), &value);
        block.stageStates[(stage << 16) | t] = value;
      }
      block.textures[stage] = m_textures[stage];
    }
    for (uint32_t t : { 1u, 2u, 3u, 4u, 5u, 6u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u }) {
      D3DMATRIX m;
      const_cast<Device7*>(this)->GetTransform(D3DTRANSFORMSTATETYPE(t), &m);
      block.transforms[t] = m;
    }
    block.hasViewport = true;
    const_cast<Device7*>(this)->GetViewport(&block.viewport);
    block.hasMaterial = true;
    const_cast<Device7*>(this)->GetMaterial(&block.material);
    for (uint32_t i = 0; i < 8; i++) {
      D3DLIGHT7 light;
      if (m_device->getLight(i, reinterpret_cast<legacy::Light*>(&light))) {
        block.lights[i] = light;
        block.lightEnables[i] = m_device->lightEnabled(i);
      }
    }
    return block;
  }

  void Device7::apply(const StateBlock& block) {
    for (const auto& [state, value] : block.renderStates) SetRenderState(D3DRENDERSTATETYPE(state), value);
    for (const auto& [key, value] : block.stageStates) SetTextureStageState(key >> 16, D3DTEXTURESTAGESTATETYPE(key & 0xffff), value);
    for (const auto& [stage, texture] : block.textures) SetTexture(stage, texture.Get());
    for (const auto& [state, matrix] : block.transforms) { D3DMATRIX m = matrix; SetTransform(D3DTRANSFORMSTATETYPE(state), &m); }
    if (block.hasViewport) { D3DVIEWPORT7 v = block.viewport; SetViewport(&v); }
    if (block.hasMaterial) { D3DMATERIAL7 m = block.material; SetMaterial(&m); }
    for (const auto& [index, light] : block.lights) { D3DLIGHT7 l = light; SetLight(index, &l); }
    for (const auto& [index, enable] : block.lightEnables) LightEnable(index, enable);
  }

  HRESULT Device7::BeginStateBlock() {
    if (m_recording) return D3DERR_INBEGINSTATEBLOCK;
    m_recording = std::make_unique<StateBlock>();
    return D3D_OK;
  }

  HRESULT Device7::EndStateBlock(LPDWORD handle) {
    if (!handle) return DDERR_INVALIDPARAMS;
    if (!m_recording) return D3DERR_NOTINBEGINSTATEBLOCK;
    m_stateBlocks.push_back(std::move(m_recording));
    *handle = DWORD(m_stateBlocks.size());
    return D3D_OK;
  }

  HRESULT Device7::CreateStateBlock(D3DSTATEBLOCKTYPE, LPDWORD handle) {
    if (!handle) return DDERR_INVALIDPARAMS;
    m_stateBlocks.push_back(std::make_unique<StateBlock>(capture()));
    *handle = DWORD(m_stateBlocks.size());
    return D3D_OK;
  }

  HRESULT Device7::ApplyStateBlock(DWORD handle) {
    if (!handle || handle > m_stateBlocks.size() || !m_stateBlocks[handle - 1]) return D3DERR_INVALIDSTATEBLOCK;
    apply(*m_stateBlocks[handle - 1]);
    return D3D_OK;
  }

  HRESULT Device7::CaptureStateBlock(DWORD handle) {
    if (!handle || handle > m_stateBlocks.size() || !m_stateBlocks[handle - 1]) return D3DERR_INVALIDSTATEBLOCK;
    StateBlock& block = *m_stateBlocks[handle - 1];
    const StateBlock current = capture();
    for (auto& [state, value] : block.renderStates) value = current.renderStates.at(state);
    for (auto& [key, value] : block.stageStates) { auto it = current.stageStates.find(key); if (it != current.stageStates.end()) value = it->second; }
    for (auto& [stage, texture] : block.textures) texture = m_textures[stage];
    for (auto& [state, matrix] : block.transforms) { auto it = current.transforms.find(state); if (it != current.transforms.end()) matrix = it->second; }
    if (block.hasViewport) block.viewport = current.viewport;
    if (block.hasMaterial) block.material = current.material;
    return D3D_OK;
  }

  HRESULT Device7::DeleteStateBlock(DWORD handle) {
    if (!handle || handle > m_stateBlocks.size() || !m_stateBlocks[handle - 1]) return D3DERR_INVALIDSTATEBLOCK;
    m_stateBlocks[handle - 1].reset();
    return D3D_OK;
  }

  HRESULT Device7::Load(LPDIRECTDRAWSURFACE7 dst, LPPOINT dstPoint, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD) {
    auto* d = Surface::fromInterface(dst);
    auto* s = Surface::fromInterface(src);
    if (!d || !s) return DDERR_INVALIDPARAMS;
    // The surface and its mip levels.
    while (d && s) {
      RECT sr = srcRect ? *srcRect : RECT{ 0, 0, LONG(s->texture()->width(s->subresource())), LONG(s->texture()->height(s->subresource())) };
      RECT dr = { dstPoint ? dstPoint->x : 0, dstPoint ? dstPoint->y : 0, 0, 0 };
      dr.right = dr.left + (sr.right - sr.left);
      dr.bottom = dr.top + (sr.bottom - sr.top);
      d->Blt(&dr, s, &sr, 0, nullptr);
      if (srcRect || dstPoint)
        break;
      DDSCAPS2 caps = {};
      caps.dwCaps = DDSCAPS_MIPMAP;
      LPDIRECTDRAWSURFACE7 nextD = nullptr, nextS = nullptr;
      if (FAILED(d->GetAttachedSurface(&caps, &nextD)) || FAILED(s->GetAttachedSurface(&caps, &nextS))) {
        if (nextD) nextD->Release();
        if (nextS) nextS->Release();
        break;
      }
      nextD->Release();
      nextS->Release();
      d = static_cast<Surface*>(nextD);
      s = static_cast<Surface*>(nextS);
    }
    return D3D_OK;
  }

  HRESULT Device7::SetClipPlane(DWORD index, D3DVALUE* plane) {
    if (index >= 6 || !plane) return DDERR_INVALIDPARAMS;
    std::memcpy(m_clipPlanes[index], plane, 4 * sizeof(float));
    logUnsupported("IDirect3DDevice7::SetClipPlane (kept, not applied)");
    return D3D_OK;
  }

  HRESULT Device7::GetClipPlane(DWORD index, D3DVALUE* plane) {
    if (index >= 6 || !plane) return DDERR_INVALIDPARAMS;
    std::memcpy(plane, m_clipPlanes[index], 4 * sizeof(float));
    return D3D_OK;
  }

  // ============================================================
  // Direct3D7
  // ============================================================

  HRESULT Direct3D7::QueryInterface(REFIID riid, void** ppv) { return m_dd->QueryInterface(riid, ppv); }
  ULONG Direct3D7::AddRef() { return m_dd->AddRef(); }
  ULONG Direct3D7::Release() { return m_dd->Release(); }

  HRESULT Direct3D7::EnumDevices(LPD3DENUMDEVICESCALLBACK7 callback, LPVOID context) {
    if (!callback) return DDERR_INVALIDPARAMS;
    D3DDEVICEDESC7 desc;
    fillDeviceDesc(desc, IID_IDirect3DHALDevice);
    char description[] = "Direct3D HAL (DXUP)";
    char name[] = "Direct3D HAL";
    if (callback(description, name, &desc, context) == D3DENUMRET_CANCEL)
      return D3D_OK;
    fillDeviceDesc(desc, IID_IDirect3DTnLHalDevice);
    char tnlDescription[] = "Direct3D T&L HAL (DXUP)";
    char tnlName[] = "Direct3D T&L HAL";
    callback(tnlDescription, tnlName, &desc, context);
    return D3D_OK;
  }

  HRESULT Direct3D7::CreateDevice(REFCLSID type, LPDIRECTDRAWSURFACE7 surface, LPDIRECT3DDEVICE7* device) {
    if (!surface || !device) return DDERR_INVALIDPARAMS;
    *device = nullptr;
    auto* target = Surface::fromInterface(surface);
    if (!target) return DDERR_INVALIDPARAMS;
    if (!m_dd->legacyDevice() || !target->texture() || !target->texture()->rtv(target->subresource()))
      return DDERR_INVALIDCAPS;
#if defined(_M_IX86)
    // Direct3D 7 runs the x87 unit at single precision unless the
    // cooperative level asked to keep the application's setting.
    if (!(m_dd->cooperativeFlags() & DDSCL_FPUPRESERVE)) {
      unsigned int control = 0;
      _controlfp_s(&control, _PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC);
    }
#endif
    *device = new Device7(m_dd, target, type);
    return D3D_OK;
  }

  HRESULT Direct3D7::CreateVertexBuffer(LPD3DVERTEXBUFFERDESC desc, LPDIRECT3DVERTEXBUFFER7* vb, DWORD) {
    if (!desc || !vb) return DDERR_INVALIDPARAMS;
    auto* buffer = new VertexBuffer7(m_dd, *desc);
    if (!buffer->buffer()) {
      buffer->Release();
      return DDERR_OUTOFVIDEOMEMORY;
    }
    *vb = buffer;
    return D3D_OK;
  }

  HRESULT Direct3D7::EnumZBufferFormats(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) {
    if (!callback) return DDERR_INVALIDPARAMS;
    for (Format format : { legacy::FormatD16, legacy::FormatD24X8, legacy::FormatD24S8, legacy::FormatD32 }) {
      DDPIXELFORMAT pf;
      pixelFormatFromFormat(format, pf);
      if (callback(&pf, context) == D3DENUMRET_CANCEL)
        break;
    }
    return D3D_OK;
  }

  // ============================================================
  // DirectDraw
  // ============================================================

  DirectDraw::DirectDraw() = default;

  DirectDraw::~DirectDraw() {
    RestoreDisplayMode();
  }

  HRESULT DirectDraw::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDirectDraw7) {
      *ppv = static_cast<IDirectDraw7*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_IDirect3D7) {
      *ppv = static_cast<IDirect3D7*>(&m_d3d);
      AddRef();
      return S_OK;
    }
    if (riid == IID_IDirectDraw) {
      *ppv = static_cast<IDirectDraw*>(&m_dd1);
      AddRef();
      return S_OK;
    }
    char text[80];
    snprintf(text, sizeof(text), "IDirectDraw7::QueryInterface {%08lX-%04X-%04X}", riid.Data1, riid.Data2, riid.Data3);
    logUnsupported(text);
    return E_NOINTERFACE;
  }

  // ============================================================
  // DirectDraw 1 interfaces
  // ============================================================

  namespace {
    // The DirectDraw 7 surface behind a DirectDraw 1 one: ours directly,
    // another DirectDraw's through its own interface, held for the call.
    class Surface7Of {
    public:
      explicit Surface7Of(IDirectDrawSurface* surface) {
        if (!surface)
          return;
        if (Surface1* ours = Surface1::fromInterface(surface))
          m_surface = ours->owner();
        else if (SUCCEEDED(surface->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&m_surface))))
          m_held = true;
      }
      ~Surface7Of() {
        if (m_held)
          m_surface->Release();
      }
      Surface7Of(const Surface7Of&) = delete;
      Surface7Of& operator=(const Surface7Of&) = delete;
      IDirectDrawSurface7* get() const { return m_surface; }
      // A surface was given that offers no DirectDraw 7 interface.
      bool missing(IDirectDrawSurface* given) const { return given && !m_surface; }
    private:
      IDirectDrawSurface7* m_surface = nullptr;
      bool m_held = false;
    };

    // The same surface (and reference) under its DirectDraw 1 interface.
    IDirectDrawSurface* asSurface1(IDirectDrawSurface7* surface) {
      Surface* ours = Surface::fromInterface(surface);
      return ours ? ours->surface1() : nullptr;
    }

    // DDSURFACEDESC is DDSURFACEDESC2 up to the first word of its caps.
    void toDesc2(const DDSURFACEDESC& from, DDSURFACEDESC2& to) {
      to = {};
      std::memcpy(&to, &from, sizeof(DDSURFACEDESC));
      to.dwSize = sizeof(DDSURFACEDESC2);
    }

    void toDesc1(const DDSURFACEDESC2& from, DDSURFACEDESC& to) {
      std::memcpy(&to, &from, sizeof(DDSURFACEDESC));
      to.dwSize = sizeof(DDSURFACEDESC);
    }
  }

  Surface1* Surface1::fromInterface(IDirectDrawSurface* surface) {
    if (!surface || *reinterpret_cast<void* const*>(surface) != s_vtable)
      return nullptr;
    return static_cast<Surface1*>(surface);
  }

  HRESULT Surface1::QueryInterface(REFIID riid, void** ppv) { return m_owner->QueryInterface(riid, ppv); }

  ULONG Surface1::AddRef() { return m_owner->AddRef(); }
  ULONG Surface1::Release() { return m_owner->Release(); }

  HRESULT Surface1::AddAttachedSurface(LPDIRECTDRAWSURFACE surface) {
    Surface7Of other(surface);
    if (!other.get()) return DDERR_INVALIDPARAMS;
    return m_owner->AddAttachedSurface(other.get());
  }

  HRESULT Surface1::Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) {
    Surface7Of source(src);
    if (source.missing(src)) return DDERR_INVALIDPARAMS;
    return m_owner->Blt(dstRect, source.get(), srcRect, flags, fx);
  }

  HRESULT Surface1::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags) {
    Surface7Of source(src);
    if (!source.get()) return DDERR_INVALIDPARAMS;
    return m_owner->BltFast(x, y, source.get(), srcRect, flags);
  }

  HRESULT Surface1::DeleteAttachedSurface(DWORD flags, LPDIRECTDRAWSURFACE surface) {
    Surface7Of other(surface);
    if (other.missing(surface)) return DDERR_INVALIDPARAMS;
    return m_owner->DeleteAttachedSurface(flags, other.get());
  }

  HRESULT Surface1::EnumAttachedSurfaces(LPVOID context, LPDDENUMSURFACESCALLBACK callback) {
    if (!callback) return DDERR_INVALIDPARAMS;
    struct Forward { LPVOID context; LPDDENUMSURFACESCALLBACK callback; } forward = { context, callback };
    return m_owner->EnumAttachedSurfaces(&forward, [](LPDIRECTDRAWSURFACE7 surface, LPDDSURFACEDESC2 desc, LPVOID data) -> HRESULT {
      auto* f = static_cast<Forward*>(data);
      DDSURFACEDESC desc1 = {};
      toDesc1(*desc, desc1);
      return f->callback(asSurface1(surface), &desc1, f->context);
    });
  }

  HRESULT Surface1::Flip(LPDIRECTDRAWSURFACE target, DWORD flags) {
    Surface7Of next(target);
    if (next.missing(target)) return DDERR_INVALIDPARAMS;
    return m_owner->Flip(next.get(), flags);
  }

  HRESULT Surface1::GetAttachedSurface(LPDDSCAPS caps, LPDIRECTDRAWSURFACE* surface) {
    if (!caps || !surface) return DDERR_INVALIDPARAMS;
    DDSCAPS2 caps2 = {};
    caps2.dwCaps = caps->dwCaps;
    IDirectDrawSurface7* found = nullptr;
    const HRESULT hr = m_owner->GetAttachedSurface(&caps2, &found);
    *surface = SUCCEEDED(hr) ? asSurface1(found) : nullptr;
    return hr;
  }

  HRESULT Surface1::GetBltStatus(DWORD flags) { return m_owner->GetBltStatus(flags); }

  HRESULT Surface1::GetCaps(LPDDSCAPS caps) {
    if (!caps) return DDERR_INVALIDPARAMS;
    DDSCAPS2 caps2 = {};
    const HRESULT hr = m_owner->GetCaps(&caps2);
    caps->dwCaps = caps2.dwCaps;
    return hr;
  }

  HRESULT Surface1::GetClipper(LPDIRECTDRAWCLIPPER* clipper) { return m_owner->GetClipper(clipper); }
  HRESULT Surface1::GetColorKey(DWORD flags, LPDDCOLORKEY key) { return m_owner->GetColorKey(flags, key); }
  HRESULT Surface1::GetDC(HDC* dc) { return m_owner->GetDC(dc); }
  HRESULT Surface1::GetFlipStatus(DWORD flags) { return m_owner->GetFlipStatus(flags); }
  HRESULT Surface1::GetPalette(LPDIRECTDRAWPALETTE* palette) { return m_owner->GetPalette(palette); }
  HRESULT Surface1::GetPixelFormat(LPDDPIXELFORMAT format) { return m_owner->GetPixelFormat(format); }

  HRESULT Surface1::GetSurfaceDesc(LPDDSURFACEDESC desc) {
    if (!desc || desc->dwSize != sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    return m_owner->GetSurfaceDesc(reinterpret_cast<LPDDSURFACEDESC2>(desc));
  }

  HRESULT Surface1::Lock(LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE event) {
    if (!desc) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 desc2 = {};
    desc2.dwSize = sizeof(desc2);
    const HRESULT hr = m_owner->Lock(rect, &desc2, flags, event);
    if (SUCCEEDED(hr))
      toDesc1(desc2, *desc);
    return hr;
  }

  HRESULT Surface1::ReleaseDC(HDC dc) { return m_owner->ReleaseDC(dc); }
  HRESULT Surface1::SetClipper(LPDIRECTDRAWCLIPPER clipper) { return m_owner->SetClipper(clipper); }
  HRESULT Surface1::SetColorKey(DWORD flags, LPDDCOLORKEY key) { return m_owner->SetColorKey(flags, key); }
  HRESULT Surface1::SetPalette(LPDIRECTDRAWPALETTE palette) { return m_owner->SetPalette(palette); }
  HRESULT Surface1::Unlock(LPVOID) { return m_owner->Unlock(nullptr); }

  HRESULT DirectDraw1::QueryInterface(REFIID riid, void** ppv) { return m_owner->QueryInterface(riid, ppv); }
  ULONG DirectDraw1::AddRef() { return m_owner->AddRef(); }
  ULONG DirectDraw1::Release() { return m_owner->Release(); }

  HRESULT DirectDraw1::CreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* clipper, IUnknown* outer) {
    return m_owner->CreateClipper(flags, clipper, outer);
  }

  HRESULT DirectDraw1::CreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* palette, IUnknown* outer) {
    return m_owner->CreatePalette(flags, entries, palette, outer);
  }

  HRESULT DirectDraw1::CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* surface, IUnknown* outer) {
    if (!desc || !surface || desc->dwSize != sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 desc2;
    toDesc2(*desc, desc2);
    IDirectDrawSurface7* created = nullptr;
    const HRESULT hr = m_owner->CreateSurface(&desc2, &created, outer);
    *surface = SUCCEEDED(hr) ? asSurface1(created) : nullptr;
    return hr;
  }

  HRESULT DirectDraw1::EnumDisplayModes(DWORD flags, LPDDSURFACEDESC desc, LPVOID context, LPDDENUMMODESCALLBACK callback) {
    if (!callback) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 filter;
    if (desc)
      toDesc2(*desc, filter);
    struct Forward { LPVOID context; LPDDENUMMODESCALLBACK callback; } forward = { context, callback };
    return m_owner->EnumDisplayModes(flags, desc ? &filter : nullptr, &forward, [](LPDDSURFACEDESC2 mode, LPVOID data) -> HRESULT {
      auto* f = static_cast<Forward*>(data);
      DDSURFACEDESC mode1 = {};
      toDesc1(*mode, mode1);
      return f->callback(&mode1, f->context);
    });
  }

  HRESULT DirectDraw1::GetCaps(LPDDCAPS driverCaps, LPDDCAPS helCaps) { return m_owner->GetCaps(driverCaps, helCaps); }

  HRESULT DirectDraw1::GetDisplayMode(LPDDSURFACEDESC desc) {
    if (!desc) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 desc2 = {};
    desc2.dwSize = sizeof(desc2);
    const HRESULT hr = m_owner->GetDisplayMode(&desc2);
    if (SUCCEEDED(hr))
      toDesc1(desc2, *desc);
    return hr;
  }

  HRESULT DirectDraw1::GetFourCCCodes(LPDWORD count, LPDWORD codes) { return m_owner->GetFourCCCodes(count, codes); }
  HRESULT DirectDraw1::GetMonitorFrequency(LPDWORD frequency) { return m_owner->GetMonitorFrequency(frequency); }
  HRESULT DirectDraw1::GetScanLine(LPDWORD line) { return m_owner->GetScanLine(line); }
  HRESULT DirectDraw1::GetVerticalBlankStatus(LPBOOL status) { return m_owner->GetVerticalBlankStatus(status); }
  HRESULT DirectDraw1::RestoreDisplayMode() { return m_owner->RestoreDisplayMode(); }
  HRESULT DirectDraw1::SetCooperativeLevel(HWND window, DWORD flags) { return m_owner->SetCooperativeLevel(window, flags); }
  HRESULT DirectDraw1::SetDisplayMode(DWORD width, DWORD height, DWORD bpp) { return m_owner->SetDisplayMode(width, height, bpp, 0, 0); }
  HRESULT DirectDraw1::WaitForVerticalBlank(DWORD flags, HANDLE event) { return m_owner->WaitForVerticalBlank(flags, event); }

  ULONG DirectDraw::Release() {
    const ULONG ref = --m_ref;
    if (!ref) delete this;
    return ref;
  }

  legacy::Device* DirectDraw::legacyDevice() {
    if (!m_legacy) {
      std::string error;
      m_legacy = legacy::Device::create(m_window, error);
      if (!m_legacy)
        logMessage(LogLevel::Error, "ddraw: legacy device creation failed: %s", error.c_str());
      else
        logMessage(LogLevel::Info, "ddraw: legacy device created");
    }
    return m_legacy.get();
  }

  uint32_t DirectDraw::modeWidth() const {
    return m_modeWidth ? m_modeWidth : uint32_t(GetSystemMetrics(SM_CXSCREEN));
  }

  uint32_t DirectDraw::modeHeight() const {
    return m_modeHeight ? m_modeHeight : uint32_t(GetSystemMetrics(SM_CYSCREEN));
  }

  HRESULT DirectDraw::CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* clipper, IUnknown*) {
    if (!clipper) return DDERR_INVALIDPARAMS;
    *clipper = new Clipper();
    return DD_OK;
  }

  HRESULT DirectDraw::CreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* palette, IUnknown*) {
    if (!palette) return DDERR_INVALIDPARAMS;
    *palette = new Palette(flags, entries);
    return DD_OK;
  }

  HRESULT DirectDraw::CreateSurface(LPDDSURFACEDESC2 descIn, LPDIRECTDRAWSURFACE7* surface, IUnknown*) {
    if (!descIn || !surface) return DDERR_INVALIDPARAMS;
    *surface = nullptr;
    DDSURFACEDESC2 desc = {};
    std::memcpy(&desc, descIn, (std::min)(size_t(descIn->dwSize ? descIn->dwSize : sizeof(desc)), sizeof(desc)));
    desc.dwSize = sizeof(desc);
    DWORD& caps = desc.ddsCaps.dwCaps;
    const bool primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    const bool zbuffer = (caps & DDSCAPS_ZBUFFER) != 0;
    const bool texture = (caps & DDSCAPS_TEXTURE) != 0;
    const bool systemMemory = (caps & DDSCAPS_SYSTEMMEMORY) != 0;
    const bool cube = (desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) != 0;

    if (primary) {
      desc.dwWidth = modeWidth();
      desc.dwHeight = modeHeight();
      desc.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
    } else if (!(desc.dwFlags & DDSD_WIDTH) || !(desc.dwFlags & DDSD_HEIGHT) || !desc.dwWidth || !desc.dwHeight) {
      return DDERR_INVALIDPARAMS;
    }

    Format format = legacy::FormatUnknown;
    if (desc.dwFlags & DDSD_PIXELFORMAT)
      format = formatFromPixelFormat(desc.ddpfPixelFormat);
    else if (zbuffer)
      format = legacy::FormatD24X8;
    else
      format = displayFormat();
    if (format == legacy::FormatUnknown) {
      logMessage(LogLevel::Warn, "ddraw: CreateSurface with unknown pixel format flags=0x%lx bits=%lu fourcc=0x%lx",
        desc.ddpfPixelFormat.dwFlags, desc.ddpfPixelFormat.dwRGBBitCount, desc.ddpfPixelFormat.dwFourCC);
      return DDERR_INVALIDPIXELFORMAT;
    }
    pixelFormatFromFormat(format, desc.ddpfPixelFormat);
    desc.dwFlags |= DDSD_PIXELFORMAT | DDSD_CAPS | DDSD_PITCH;

    uint32_t levels = 1;
    if (texture && (caps & DDSCAPS_MIPMAP)) {
      if (desc.dwFlags & DDSD_MIPMAPCOUNT) {
        levels = (std::max)(1ul, desc.dwMipMapCount);
      } else {
        for (uint32_t size = (std::max)(desc.dwWidth, desc.dwHeight); size > 1; size >>= 1)
          levels++;
      }
      desc.dwMipMapCount = levels;
      desc.dwFlags |= DDSD_MIPMAPCOUNT;
    }

    legacy::Device* device = systemMemory && !primary ? nullptr : legacyDevice();
    legacy::TextureDesc td;
    td.width = desc.dwWidth;
    td.height = desc.dwHeight;
    td.levels = levels;
    td.format = format;
    td.cube = cube;
    td.depthStencil = zbuffer;
    // Windowed primaries are the desktop: memory only, for locks.
    td.systemMemory = systemMemory || (primary && !m_fullscreen) || !device;
    td.renderTarget = !td.systemMemory && !zbuffer && !legacy::formatInfo(format).block
      && (!texture || (caps & DDSCAPS_3DDEVICE));
    if (!td.systemMemory && !texture && !zbuffer)
      td.renderTarget = true;
    // The full screen primary and its back buffers are the swap chain; a
    // windowed application draws into surfaces of the window's size.
    td.backBuffer = primary && !td.systemMemory;
    if (primary && td.systemMemory && device && m_window) {
      RECT client = {};
      if (GetClientRect(m_window, &client) && client.right > client.left && client.bottom > client.top)
        device->noteBackBufferSize(uint32_t(client.right - client.left), uint32_t(client.bottom - client.top));
    }
    auto shared = std::make_shared<legacy::Texture>(legacyDevice(), td);
    if (!shared->valid()) {
      logMessage(LogLevel::Warn, "ddraw: surface %lux%lu format %u caps 0x%lx failed", desc.dwWidth, desc.dwHeight,
        uint32_t(format), caps);
      return DDERR_OUTOFVIDEOMEMORY;
    }
    if (td.systemMemory)
      caps |= DDSCAPS_SYSTEMMEMORY;
    else
      caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    desc.lPitch = LONG(legacy::legacyPitch(format, desc.dwWidth));
    if (legacy::formatInfo(format).block) {
      desc.dwFlags = (desc.dwFlags & ~DDSD_PITCH) | DDSD_LINEARSIZE;
      desc.dwLinearSize = legacy::legacyPitch(format, desc.dwWidth) * legacy::legacyRows(format, desc.dwHeight);
    }

    auto* root = new Surface(this, desc, shared, 0, nullptr);
    // Mip levels: a chain of attached surfaces.
    Surface* previous = root;
    for (uint32_t level = 1; level < levels && !cube; level++) {
      DDSURFACEDESC2 ld = desc;
      ld.dwWidth = (std::max)(1ul, desc.dwWidth >> level);
      ld.dwHeight = (std::max)(1ul, desc.dwHeight >> level);
      ld.dwMipMapCount = levels - level;
      ld.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
      ld.lPitch = LONG(legacy::legacyPitch(format, ld.dwWidth));
      auto* child = new Surface(this, ld, shared, level, root);
      previous->addChild(child);
      previous = child;
    }
    if (cube) {
      static const DWORD faces[6] = { DDSCAPS2_CUBEMAP_POSITIVEX, DDSCAPS2_CUBEMAP_NEGATIVEX, DDSCAPS2_CUBEMAP_POSITIVEY,
                                      DDSCAPS2_CUBEMAP_NEGATIVEY, DDSCAPS2_CUBEMAP_POSITIVEZ, DDSCAPS2_CUBEMAP_NEGATIVEZ };
      for (uint32_t face = 1; face < 6; face++) {
        DDSURFACEDESC2 fd = desc;
        fd.ddsCaps.dwCaps2 = (fd.ddsCaps.dwCaps2 & ~0xfc00u) | faces[face] | DDSCAPS2_CUBEMAP;
        root->addChild(new Surface(this, fd, shared, face * levels, root));
      }
    }
    // Flipping chains: back buffers.
    if ((caps & DDSCAPS_FLIP) && (desc.dwFlags & DDSD_BACKBUFFERCOUNT) && desc.dwBackBufferCount) {
      Surface* previousBuffer = root;
      for (DWORD i = 0; i < desc.dwBackBufferCount; i++) {
        legacy::TextureDesc bd = td;
        bd.systemMemory = false;
        bd.renderTarget = true;
        auto backTexture = std::make_shared<legacy::Texture>(legacyDevice(), bd);
        if (!backTexture->valid()) {
          delete root;
          return DDERR_OUTOFVIDEOMEMORY;
        }
        DDSURFACEDESC2 bdesc = desc;
        bdesc.ddsCaps.dwCaps &= ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_FRONTBUFFER);
        bdesc.ddsCaps.dwCaps |= DDSCAPS_FLIP | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | (i == 0 ? DDSCAPS_BACKBUFFER : 0);
        bdesc.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
        bdesc.dwBackBufferCount = 0;
        auto* back = new Surface(this, bdesc, backTexture, 0, root);
        previousBuffer->addChild(back);
        previousBuffer = back;
      }
    }
    if (primary)
      root->addCaps(DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE);
    *surface = root;
    return DD_OK;
  }

  HRESULT DirectDraw::EnumDisplayModes(DWORD, LPDDSURFACEDESC2 filter, LPVOID context, LPDDENUMMODESCALLBACK2 callback) {
    if (!callback) return DDERR_INVALIDPARAMS;
    std::set<std::pair<DWORD, DWORD>> sizes;
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); i++)
      if (mode.dmPelsWidth >= 640 && mode.dmPelsHeight >= 480)
        sizes.insert({ mode.dmPelsWidth, mode.dmPelsHeight });
    for (DWORD bpp : { 16ul, 32ul }) {
      for (const auto& [w, h] : sizes) {
        if (filter && (filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != w) continue;
        if (filter && (filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != h) continue;
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        desc.dwWidth = w;
        desc.dwHeight = h;
        desc.dwRefreshRate = 60;
        pixelFormatFromFormat(bpp == 16 ? legacy::FormatR5G6B5 : legacy::FormatX8R8G8B8, desc.ddpfPixelFormat);
        desc.lPitch = LONG(w * bpp / 8);
        if (filter && (filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount
         && filter->ddpfPixelFormat.dwRGBBitCount != bpp) continue;
        if (callback(&desc, context) == DDENUMRET_CANCEL)
          return DD_OK;
      }
    }
    return DD_OK;
  }

  HRESULT DirectDraw::GetCaps(LPDDCAPS driverCaps, LPDDCAPS helCaps) {
    auto fill = [](LPDDCAPS out) {
      if (!out) return;
      // Older DirectDraws' DDCAPS are shorter (DirectX 3: 316 bytes, 5: 364):
      // filled in full here, copied as far as the caller's goes.
      const DWORD size = out->dwSize ? (std::min)(out->dwSize, DWORD(sizeof(DDCAPS))) : DWORD(sizeof(DDCAPS));
      DDCAPS full = {};
      DDCAPS* caps = &full;
      caps->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH
        | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM | DDCAPS_ZBLTS | DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_ALIGNBOUNDARYDEST;
      caps->dwCaps2 = DDCAPS2_FLIPNOVSYNC | DDCAPS2_FLIPINTERVAL | DDCAPS2_CANRENDERWINDOWED | DDCAPS2_WIDESURFACES
        | DDCAPS2_PRIMARYGAMMA | DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_CANMANAGETEXTURE;
      caps->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
      caps->dwVidMemTotal = caps->dwVidMemFree = 0x40000000;
      caps->ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER
        | DDSCAPS_MIPMAP | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY
        | DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER | DDSCAPS_LOCALVIDMEM;
      caps->ddsOldCaps.dwCaps = caps->ddsCaps.dwCaps;
      const DWORD given = out->dwSize ? out->dwSize : DWORD(sizeof(DDCAPS));
      std::memset(out, 0, given);
      std::memcpy(out, caps, size);
      out->dwSize = given;
    };
    fill(driverCaps);
    fill(helCaps);
    return DD_OK;
  }

  HRESULT DirectDraw::GetDisplayMode(LPDDSURFACEDESC2 desc) {
    if (!desc) return DDERR_INVALIDPARAMS;
    const DWORD size = desc->dwSize ? desc->dwSize : sizeof(DDSURFACEDESC2);
    std::memset(desc, 0, size);
    desc->dwSize = size;
    desc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    desc->dwWidth = modeWidth();
    desc->dwHeight = modeHeight();
    desc->dwRefreshRate = 60;
    pixelFormatFromFormat(displayFormat(), desc->ddpfPixelFormat);
    desc->lPitch = LONG(desc->dwWidth * modeBpp() / 8);
    return DD_OK;
  }

  HRESULT DirectDraw::GetFourCCCodes(LPDWORD count, LPDWORD codes) {
    static const DWORD kCodes[] = { legacy::FormatDXT1, legacy::FormatDXT2, legacy::FormatDXT3, legacy::FormatDXT4, legacy::FormatDXT5 };
    if (!count) return DDERR_INVALIDPARAMS;
    if (codes)
      std::memcpy(codes, kCodes, (std::min)(*count, DWORD(5)) * sizeof(DWORD));
    *count = 5;
    return DD_OK;
  }

  HRESULT DirectDraw::GetMonitorFrequency(LPDWORD frequency) {
    if (!frequency) return DDERR_INVALIDPARAMS;
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    *frequency = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) ? mode.dmDisplayFrequency : 60;
    return DD_OK;
  }

  HRESULT DirectDraw::SetCooperativeLevel(HWND window, DWORD flags) {
    if (window)
      m_window = window;
    m_cooperativeFlags = flags;
    m_fullscreen = (flags & DDSCL_FULLSCREEN) != 0;
    logMessage(LogLevel::Info, "ddraw: SetCooperativeLevel(%p, 0x%lx)", static_cast<void*>(window), flags);
    if (m_fullscreen && window)
      coverMonitor();
    return DD_OK;
  }

  // Display modes are not changed: the game's window covers the monitor and
  // the picture scales to it.
  HRESULT DirectDraw::SetDisplayMode(DWORD width, DWORD height, DWORD bpp, DWORD, DWORD) {
    logMessage(LogLevel::Info, "ddraw: SetDisplayMode(%lu, %lu, %lu)", width, height, bpp);
    m_modeWidth = width;
    m_modeHeight = height;
    m_modeBpp = bpp;
    coverMonitor();
    return DD_OK;
  }

  // Exclusive full screen: the window covers its monitor, without a frame.
  void DirectDraw::coverMonitor() {
    if (m_window && m_fullscreen) {
      // Before the window's place is kept: in the coordinates it then has.
      dpiAwareForFullScreen(MonitorFromWindow(m_window, MONITOR_DEFAULTTOPRIMARY));
      if (!m_savedStyle) {
        m_savedStyle = GetWindowLongW(m_window, GWL_STYLE);
        GetWindowRect(m_window, &m_savedRect);
      }
      HMONITOR monitor = MonitorFromWindow(m_window, MONITOR_DEFAULTTOPRIMARY);
      MONITORINFO info = { sizeof(info) };
      GetMonitorInfoW(monitor, &info);
      SetWindowLongW(m_window, GWL_STYLE, (m_savedStyle & ~(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE);
      SetWindowPos(m_window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
        info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
      SetForegroundWindow(m_window);
    }
  }

  HRESULT DirectDraw::RestoreDisplayMode() {
    if (m_window && m_savedStyle && IsWindow(m_window)) {
      SetWindowLongW(m_window, GWL_STYLE, m_savedStyle);
      SetWindowPos(m_window, nullptr, m_savedRect.left, m_savedRect.top, m_savedRect.right - m_savedRect.left,
        m_savedRect.bottom - m_savedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    m_savedStyle = 0;
    return DD_OK;
  }

  HRESULT DirectDraw::GetAvailableVidMem(LPDDSCAPS2, LPDWORD total, LPDWORD free) {
    if (total) *total = 0x40000000;
    if (free) *free = 0x40000000;
    return DD_OK;
  }

  HRESULT DirectDraw::GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 identifier, DWORD) {
    if (!identifier) return DDERR_INVALIDPARAMS;
    std::memset(identifier, 0, sizeof(*identifier));
    strcpy_s(identifier->szDriver, "dxup");
    strcpy_s(identifier->szDescription, "DXUP Direct3D 7 on Direct3D 12");
    if (legacy::Device* device = legacyDevice()) {
      IDXGIDevice* dxgiDevice = nullptr;
      if (SUCCEEDED(device->d3d11()->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
          spoofAdapterDesc(desc);
          identifier->dwVendorId = desc.VendorId;
          identifier->dwDeviceId = desc.DeviceId;
          identifier->dwSubSysId = desc.SubSysId;
          identifier->dwRevision = desc.Revision;
          WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, identifier->szDescription,
            sizeof(identifier->szDescription), nullptr, nullptr);
        }
        if (adapter) adapter->Release();
        dxgiDevice->Release();
      }
    }
    identifier->liDriverVersion.QuadPart = 0x0007000000000000ll;
    return DD_OK;
  }

}
