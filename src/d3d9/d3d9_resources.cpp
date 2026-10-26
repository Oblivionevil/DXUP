// Direct3D 9 objects besides the device and the interface (see d3d9_objects.h).

#include "d3d9_objects.h"

#include "../legacy/legacy_formats.h"
#include "../util/util_types.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>

namespace dxup::d3d9 {

  void logUnsupported(const char* what) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::lock_guard<std::mutex> lock(mutex);
    if (reported.insert(what).second)
      logMessage(LogLevel::Warn, "D3D9: unsupported: %s", what);
  }

  void logFailedCall(HRESULT hr, const char* format, ...) {
    char text[384];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::lock_guard<std::mutex> lock(mutex);
    if (reported.size() >= 64 || !reported.insert(text).second)
      return;
    const char* name = hr == D3DERR_INVALIDCALL ? "D3DERR_INVALIDCALL" : hr == D3DERR_NOTAVAILABLE ? "D3DERR_NOTAVAILABLE"
                     : hr == D3DERR_OUTOFVIDEOMEMORY ? "D3DERR_OUTOFVIDEOMEMORY" : hr == E_OUTOFMEMORY ? "E_OUTOFMEMORY"
                     : hr == D3DERR_WASSTILLDRAWING ? "D3DERR_WASSTILLDRAWING" : nullptr;
    char code[16];
    if (!name) {
      snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(hr));
      name = code;
    }
    logMessage(LogLevel::Warn, "glitch: the game's call failed with %s: %s%s", name, text,
      reported.size() == 64 ? " (no more such lines)" : "");
  }

  std::string formatName(D3DFORMAT format) {
    switch (format) {
#define DXUP_FORMAT(name) case D3DFMT_##name: return #name;
      DXUP_FORMAT(UNKNOWN) DXUP_FORMAT(R8G8B8) DXUP_FORMAT(A8R8G8B8) DXUP_FORMAT(X8R8G8B8) DXUP_FORMAT(R5G6B5)
      DXUP_FORMAT(X1R5G5B5) DXUP_FORMAT(A1R5G5B5) DXUP_FORMAT(A4R4G4B4) DXUP_FORMAT(R3G3B2) DXUP_FORMAT(A8)
      DXUP_FORMAT(A8R3G3B2) DXUP_FORMAT(X4R4G4B4) DXUP_FORMAT(A2B10G10R10) DXUP_FORMAT(A8B8G8R8) DXUP_FORMAT(X8B8G8R8)
      DXUP_FORMAT(G16R16) DXUP_FORMAT(A2R10G10B10) DXUP_FORMAT(A16B16G16R16) DXUP_FORMAT(A8P8) DXUP_FORMAT(P8)
      DXUP_FORMAT(L8) DXUP_FORMAT(A8L8) DXUP_FORMAT(A4L4) DXUP_FORMAT(V8U8) DXUP_FORMAT(L6V5U5) DXUP_FORMAT(X8L8V8U8)
      DXUP_FORMAT(Q8W8V8U8) DXUP_FORMAT(V16U16) DXUP_FORMAT(A2W10V10U10) DXUP_FORMAT(D16_LOCKABLE) DXUP_FORMAT(D32)
      DXUP_FORMAT(D15S1) DXUP_FORMAT(D24S8) DXUP_FORMAT(D24X8) DXUP_FORMAT(D24X4S4) DXUP_FORMAT(D16)
      DXUP_FORMAT(D32F_LOCKABLE) DXUP_FORMAT(D24FS8) DXUP_FORMAT(L16) DXUP_FORMAT(INDEX16) DXUP_FORMAT(INDEX32)
      DXUP_FORMAT(Q16W16V16U16) DXUP_FORMAT(R16F) DXUP_FORMAT(G16R16F) DXUP_FORMAT(A16B16G16R16F) DXUP_FORMAT(R32F)
      DXUP_FORMAT(G32R32F) DXUP_FORMAT(A32B32G32R32F) DXUP_FORMAT(CxV8U8) DXUP_FORMAT(VERTEXDATA)
#undef DXUP_FORMAT
      default: break;
    }
    const uint32_t value = uint32_t(format);
    char text[16];
    if (value > 0xffff) {
      const char c[5] = { char(value & 0xff), char((value >> 8) & 0xff), char((value >> 16) & 0xff), char(value >> 24), 0 };
      bool printable = true;
      for (int i = 0; i < 4; i++)
        printable = printable && c[i] >= 32 && c[i] < 127;
      if (printable)
        return std::string("'") + c + "'";
    }
    snprintf(text, sizeof(text), "%u", value);
    return text;
  }

  // ============================================================
  // Private data
  // ============================================================

  HRESULT PrivateData::set(REFGUID guid, const void* data, DWORD size, DWORD flags) {
    free(guid);
    if (!data && size)
      return D3DERR_INVALIDCALL;
    Entry entry;
    if (flags & D3DSPD_IUNKNOWN) {
      if (size != sizeof(IUnknown*))
        return D3DERR_INVALIDCALL;
      entry.object = *static_cast<IUnknown* const*>(data);
      if (entry.object)
        entry.object->AddRef();
    } else {
      entry.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    }
    m_entries.emplace(guid, std::move(entry));
    return D3D_OK;
  }

  HRESULT PrivateData::get(REFGUID guid, void* data, DWORD* size) {
    if (!size)
      return D3DERR_INVALIDCALL;
    auto it = m_entries.find(guid);
    if (it == m_entries.end())
      return D3DERR_NOTFOUND;
    const DWORD needed = it->second.object ? DWORD(sizeof(IUnknown*)) : DWORD(it->second.data.size());
    if (!data) {
      *size = needed;
      return D3D_OK;
    }
    if (*size < needed) {
      *size = needed;
      return D3DERR_MOREDATA;
    }
    *size = needed;
    if (it->second.object) {
      it->second.object->AddRef();
      *static_cast<IUnknown**>(data) = it->second.object;
    } else if (needed) {
      std::memcpy(data, it->second.data.data(), needed);
    }
    return D3D_OK;
  }

  HRESULT PrivateData::free(REFGUID guid) {
    auto it = m_entries.find(guid);
    if (it == m_entries.end())
      return D3DERR_NOTFOUND;
    if (it->second.object)
      it->second.object->Release();
    m_entries.erase(it);
    return D3D_OK;
  }

  void PrivateData::clear() {
    for (auto& entry : m_entries)
      if (entry.second.object)
        entry.second.object->Release();
    m_entries.clear();
  }

  // ============================================================
  // Resource
  // ============================================================

  template<typename Base>
  Resource<Base>::Resource(Device* device, D3DPOOL pool, bool owned)
  : m_device(device), m_pool(pool), m_owned(owned) {
    if (!m_owned)
      m_device->addRefPrivate();
  }

  template<typename Base>
  Resource<Base>::~Resource() {
    if (!m_owned)
      m_device->releasePrivate();
  }

  template<typename Base>
  HRESULT Resource<Base>::GetDevice(IDirect3DDevice9** device) {
    if (!device)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
  }

  template class Resource<IDirect3DSurface9>;
  template class Resource<IDirect3DVolume9>;
  template class Resource<IDirect3DTexture9>;
  template class Resource<IDirect3DCubeTexture9>;
  template class Resource<IDirect3DVolumeTexture9>;
  template class Resource<IDirect3DVertexBuffer9>;
  template class Resource<IDirect3DIndexBuffer9>;

  // ============================================================
  // Surface
  // ============================================================

  Surface::Surface(Device* device, D3DPOOL pool, std::shared_ptr<legacy::Texture> texture, uint32_t subresource,
                   IUnknown* container, RefCounted* containerRef, DWORD usage, D3DMULTISAMPLE_TYPE multisample,
                   DWORD quality, bool owned)
  : Resource(device, pool, owned || container != nullptr), m_texture(std::move(texture)), m_subresource(subresource),
    m_container(container), m_containerRef(containerRef), m_usage(usage), m_multisample(multisample), m_quality(quality) {}

  HRESULT Surface::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DSurface9)) {
      AddRef();
      *object = static_cast<IDirect3DSurface9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG Surface::AddRef() {
    if (m_container) {
      ++m_containedRefs;
      return m_container->AddRef();
    }
    return addRefPublic();
  }

  ULONG Surface::Release() {
    if (m_container) {
      --m_containedRefs;
      return m_container->Release();
    }
    return releasePublic();
  }

  HRESULT Surface::GetContainer(REFIID riid, void** container) {
    if (!container)
      return D3DERR_INVALIDCALL;
    if (m_container)
      return m_container->QueryInterface(riid, container);
    return m_device->QueryInterface(riid, container);
  }

  HRESULT Surface::GetDesc(D3DSURFACE_DESC* desc) {
    if (!desc)
      return D3DERR_INVALIDCALL;
    desc->Format = format();
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = m_usage;
    desc->Pool = m_pool;
    desc->MultiSampleType = m_multisample;
    desc->MultiSampleQuality = m_quality;
    desc->Width = width();
    desc->Height = height();
    return D3D_OK;
  }

  HRESULT Surface::LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    const HRESULT hr = [&]() -> HRESULT {
      if (!locked)
        return D3DERR_INVALIDCALL;
      std::unique_lock<std::recursive_mutex> guard(m_device->mutex(), std::defer_lock);
      if (m_device->multithreaded()) guard.lock();
      if (m_locked)
        return D3DERR_INVALIDCALL;
      legacy::Rect r = {};
      if (rect)
        r = { rect->left, rect->top, rect->right, rect->bottom };
      void* data = nullptr;
      uint32_t pitch = 0;
      if (!m_texture->lock(m_subresource, rect ? &r : nullptr, (flags & D3DLOCK_READONLY) != 0, &data, &pitch,
                           (flags & D3DLOCK_DISCARD) != 0)) {
        locked->pBits = nullptr;
        locked->Pitch = 0;
        return D3DERR_INVALIDCALL;
      }
      locked->pBits = data;
      locked->Pitch = INT(pitch);
      m_locked = true;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "LockRect of a %ux%u %s surface, flags 0x%lx%s", m_texture->width(m_subresource), m_texture->height(m_subresource), formatName(D3DFORMAT(m_texture->desc().format)).c_str(), flags, m_locked ? " (locked already)" : "");
    return hr;
  }

  HRESULT Surface::UnlockRect() {
    std::unique_lock<std::recursive_mutex> guard(m_device->mutex(), std::defer_lock);
    if (m_device->multithreaded()) guard.lock();
    if (!m_locked)
      return D3DERR_INVALIDCALL;
    m_locked = false;
    m_texture->unlock(m_subresource);
    return D3D_OK;
  }

  HRESULT Surface::GetDC(HDC* dc) {
    if (!dc)
      return D3DERR_INVALIDCALL;
    const D3DFORMAT f = format();
    if (f != D3DFMT_R5G6B5 && f != D3DFMT_X1R5G5B5 && f != D3DFMT_R8G8B8 && f != D3DFMT_X8R8G8B8 && f != D3DFMT_A8R8G8B8)
      return D3DERR_INVALIDCALL;
    if (m_dc)
      return D3DERR_INVALIDCALL;
    const UINT w = width(), h = height();
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LONG(w);
    info.bmiHeader.biHeight = -LONG(h);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    m_bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    if (!m_bitmap)
      return D3DERR_INVALIDCALL;
    m_dc = CreateCompatibleDC(nullptr);
    m_oldBitmap = SelectObject(m_dc, m_bitmap);
    D3DLOCKED_RECT locked = {};
    if (SUCCEEDED(LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      const legacy::FormatInfo fi = legacy::formatInfo(legacy::Format(f));
      for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++)
          static_cast<uint32_t*>(m_bits)[y * w + x] = legacy::toArgb(legacy::Format(f),
            static_cast<const uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch + size_t(x) * fi.bytesPerBlock, nullptr);
      UnlockRect();
    }
    m_dcOriginal.assign(static_cast<const uint32_t*>(m_bits), static_cast<const uint32_t*>(m_bits) + size_t(w) * h);
    *dc = m_dc;
    return D3D_OK;
  }

  HRESULT Surface::ReleaseDC(HDC dc) {
    if (!m_dc || dc != m_dc)
      return D3DERR_INVALIDCALL;
    GdiFlush();
    const D3DFORMAT f = format();
    const UINT w = width(), h = height();
    // Only the pixels GDI drew go back, as GDI left them (A8R8G8B8: alpha
    // 0, as with Windows' D3D9, whose DC draws into the surface): the
    // others keep their texels, alpha included.
    const auto* bits = static_cast<const uint32_t*>(m_bits);
    bool changed = false;
    for (size_t i = 0; i < m_dcOriginal.size() && !changed; i++)
      changed = bits[i] != m_dcOriginal[i];
    D3DLOCKED_RECT locked = {};
    if (changed && SUCCEEDED(LockRect(&locked, nullptr, 0))) {
      const legacy::FormatInfo fi = legacy::formatInfo(legacy::Format(f));
      for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) {
          const size_t i = size_t(y) * w + x;
          if (bits[i] != m_dcOriginal[i])
            legacy::fromArgb(legacy::Format(f), bits[i],
              static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch + size_t(x) * fi.bytesPerBlock);
        }
      UnlockRect();
    }
    m_dcOriginal.clear();
    m_dcOriginal.shrink_to_fit();
    SelectObject(m_dc, m_oldBitmap);
    DeleteDC(m_dc);
    DeleteObject(m_bitmap);
    m_dc = nullptr;
    m_bitmap = nullptr;
    m_bits = nullptr;
    return D3D_OK;
  }

  // ============================================================
  // Volume
  // ============================================================

  Volume::Volume(Device* device, std::shared_ptr<legacy::Texture> texture, uint32_t level, IUnknown* container,
                 RefCounted* containerRef)
  : Resource(device, D3DPOOL_DEFAULT, true), m_texture(std::move(texture)), m_level(level), m_container(container),
    m_containerRef(containerRef) {}

  HRESULT Volume::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVolume9)) {
      AddRef();
      *object = static_cast<IDirect3DVolume9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT Volume::GetContainer(REFIID riid, void** container) {
    if (!container)
      return D3DERR_INVALIDCALL;
    return m_container->QueryInterface(riid, container);
  }

  HRESULT Volume::GetDesc(D3DVOLUME_DESC* desc) {
    if (!desc)
      return D3DERR_INVALIDCALL;
    IDirect3DVolumeTexture9* texture = nullptr;
    D3DVOLUME_DESC level = {};
    if (SUCCEEDED(m_container->QueryInterface(__uuidof(IDirect3DVolumeTexture9), reinterpret_cast<void**>(&texture)))) {
      texture->GetLevelDesc(m_level, &level);
      texture->Release();
    }
    *desc = level;
    desc->Type = D3DRTYPE_VOLUME;
    return D3D_OK;
  }

  HRESULT Volume::LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
    if (!locked)
      return D3DERR_INVALIDCALL;
    std::unique_lock<std::recursive_mutex> guard(m_device->mutex(), std::defer_lock);
    if (m_device->multithreaded()) guard.lock();
    legacy::Box b = {};
    if (box)
      b = { box->Left, box->Top, box->Front, box->Right, box->Bottom, box->Back };
    void* data = nullptr;
    uint32_t rowPitch = 0, slicePitch = 0;
    if (!m_texture->lockBox(m_level, box ? &b : nullptr, (flags & D3DLOCK_READONLY) != 0, &data, &rowPitch, &slicePitch,
                            (flags & D3DLOCK_DISCARD) != 0))
      return D3DERR_INVALIDCALL;
    locked->pBits = data;
    locked->RowPitch = INT(rowPitch);
    locked->SlicePitch = INT(slicePitch);
    return D3D_OK;
  }

  HRESULT Volume::UnlockBox() {
    std::unique_lock<std::recursive_mutex> guard(m_device->mutex(), std::defer_lock);
    if (m_device->multithreaded()) guard.lock();
    m_texture->unlock(m_level);
    return D3D_OK;
  }

  // ============================================================
  // Textures
  // ============================================================

  template<typename Base>
  DWORD TextureCommon<Base>::GetLevelCount() {
    // Mips generated by the device count as one level.
    return (m_usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : m_texture->desc().levels;
  }

  template<typename Base>
  void TextureCommon<Base>::GenerateMipSubLevels() {
    if (m_usage & D3DUSAGE_AUTOGENMIPMAP)
      m_texture->markGpuWritten();
  }

  template class TextureCommon<IDirect3DTexture9>;
  template class TextureCommon<IDirect3DCubeTexture9>;
  template class TextureCommon<IDirect3DVolumeTexture9>;

  namespace {
    void fillSurfaceDesc(const legacy::Texture& texture, uint32_t subresource, DWORD usage, D3DPOOL pool, D3DSURFACE_DESC* desc) {
      desc->Format = D3DFORMAT(texture.desc().format);
      desc->Type = D3DRTYPE_SURFACE;
      desc->Usage = usage;
      desc->Pool = pool;
      desc->MultiSampleType = D3DMULTISAMPLE_NONE;
      desc->MultiSampleQuality = 0;
      desc->Width = texture.width(subresource);
      desc->Height = texture.height(subresource);
    }
  }

  Texture2D::Texture2D(Device* device, std::shared_ptr<legacy::Texture> texture, D3DPOOL pool, DWORD usage)
  : TextureCommon(device, pool) {
    m_texture = std::move(texture);
    this->boundCore = m_texture.get();
    m_usage = usage;
    const uint32_t levels = GetLevelCount();
    for (uint32_t level = 0; level < levels; level++)
      m_surfaces.push_back(new Surface(device, pool, m_texture, level, static_cast<IDirect3DTexture9*>(this), this, usage,
                                       D3DMULTISAMPLE_NONE, 0));
  }

  Texture2D::~Texture2D() {
    for (Surface* surface : m_surfaces)
      surface->destroyContained();
  }

  HRESULT Texture2D::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9)
     || riid == __uuidof(IDirect3DTexture9)) {
      AddRef();
      *object = static_cast<IDirect3DTexture9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT Texture2D::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) {
    if (!desc || level >= m_surfaces.size())
      return D3DERR_INVALIDCALL;
    fillSurfaceDesc(*m_texture, level, m_usage, m_pool, desc);
    return D3D_OK;
  }

  HRESULT Texture2D::GetSurfaceLevel(UINT level, IDirect3DSurface9** surface) {
    if (!surface || level >= m_surfaces.size())
      return D3DERR_INVALIDCALL;
    m_surfaces[level]->AddRef();
    *surface = m_surfaces[level];
    return D3D_OK;
  }

  HRESULT Texture2D::LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    if (level >= m_surfaces.size())
      return D3DERR_INVALIDCALL;
    return m_surfaces[level]->LockRect(locked, rect, flags);
  }

  HRESULT Texture2D::UnlockRect(UINT level) {
    if (level >= m_surfaces.size())
      return D3DERR_INVALIDCALL;
    return m_surfaces[level]->UnlockRect();
  }

  CubeTexture::CubeTexture(Device* device, std::shared_ptr<legacy::Texture> texture, D3DPOOL pool, DWORD usage)
  : TextureCommon(device, pool) {
    m_texture = std::move(texture);
    this->boundCore = m_texture.get();
    m_usage = usage;
    const uint32_t levels = m_texture->desc().levels;
    for (uint32_t face = 0; face < 6; face++)
      for (uint32_t level = 0; level < levels; level++)
        m_surfaces.push_back(new Surface(device, pool, m_texture, face * levels + level,
                                         static_cast<IDirect3DCubeTexture9*>(this), this, usage, D3DMULTISAMPLE_NONE, 0));
  }

  CubeTexture::~CubeTexture() {
    for (Surface* surface : m_surfaces)
      surface->destroyContained();
  }

  HRESULT CubeTexture::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9)
     || riid == __uuidof(IDirect3DCubeTexture9)) {
      AddRef();
      *object = static_cast<IDirect3DCubeTexture9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) {
    if (!desc || level >= GetLevelCount())
      return D3DERR_INVALIDCALL;
    fillSurfaceDesc(*m_texture, level, m_usage, m_pool, desc);
    return D3D_OK;
  }

  HRESULT CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** surface) {
    const uint32_t levels = m_texture->desc().levels;
    if (!surface || uint32_t(face) >= 6 || level >= levels)
      return D3DERR_INVALIDCALL;
    Surface* s = m_surfaces[uint32_t(face) * levels + level];
    s->AddRef();
    *surface = s;
    return D3D_OK;
  }

  HRESULT CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    const uint32_t levels = m_texture->desc().levels;
    if (uint32_t(face) >= 6 || level >= levels)
      return D3DERR_INVALIDCALL;
    return m_surfaces[uint32_t(face) * levels + level]->LockRect(locked, rect, flags);
  }

  HRESULT CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level) {
    const uint32_t levels = m_texture->desc().levels;
    if (uint32_t(face) >= 6 || level >= levels)
      return D3DERR_INVALIDCALL;
    return m_surfaces[uint32_t(face) * levels + level]->UnlockRect();
  }

  VolumeTexture::VolumeTexture(Device* device, std::shared_ptr<legacy::Texture> texture, D3DPOOL pool, DWORD usage)
  : TextureCommon(device, pool) {
    m_texture = std::move(texture);
    this->boundCore = m_texture.get();
    m_usage = usage;
    for (uint32_t level = 0; level < m_texture->desc().levels; level++)
      m_volumes.push_back(new Volume(device, m_texture, level, static_cast<IDirect3DVolumeTexture9*>(this), this));
  }

  VolumeTexture::~VolumeTexture() {
    for (Volume* volume : m_volumes)
      delete volume;
  }

  HRESULT VolumeTexture::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9)
     || riid == __uuidof(IDirect3DVolumeTexture9)) {
      AddRef();
      *object = static_cast<IDirect3DVolumeTexture9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT VolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC* desc) {
    if (!desc || level >= m_volumes.size())
      return D3DERR_INVALIDCALL;
    desc->Format = D3DFORMAT(m_texture->desc().format);
    desc->Type = D3DRTYPE_VOLUMETEXTURE;
    desc->Usage = m_usage;
    desc->Pool = m_pool;
    desc->Width = m_texture->width(level);
    desc->Height = m_texture->height(level);
    desc->Depth = m_texture->depth(level);
    return D3D_OK;
  }

  HRESULT VolumeTexture::GetVolumeLevel(UINT level, IDirect3DVolume9** volume) {
    if (!volume || level >= m_volumes.size())
      return D3DERR_INVALIDCALL;
    m_volumes[level]->AddRef();
    *volume = m_volumes[level];
    return D3D_OK;
  }

  HRESULT VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
    if (level >= m_volumes.size())
      return D3DERR_INVALIDCALL;
    return m_volumes[level]->LockBox(locked, box, flags);
  }

  HRESULT VolumeTexture::UnlockBox(UINT level) {
    if (level >= m_volumes.size())
      return D3DERR_INVALIDCALL;
    return m_volumes[level]->UnlockBox();
  }

  BaseTexture* baseTextureOf(IDirect3DBaseTexture9* texture) {
    if (!texture)
      return nullptr;
    switch (texture->GetType()) {
      case D3DRTYPE_TEXTURE:       return static_cast<Texture2D*>(static_cast<IDirect3DTexture9*>(texture));
      case D3DRTYPE_CUBETEXTURE:   return static_cast<CubeTexture*>(static_cast<IDirect3DCubeTexture9*>(texture));
      case D3DRTYPE_VOLUMETEXTURE: return static_cast<VolumeTexture*>(static_cast<IDirect3DVolumeTexture9*>(texture));
      default:                     return nullptr;
    }
  }

  RefCounted* textureObjectOf(IDirect3DBaseTexture9* texture) {
    if (!texture)
      return nullptr;
    switch (texture->GetType()) {
      case D3DRTYPE_TEXTURE:       return static_cast<Texture2D*>(static_cast<IDirect3DTexture9*>(texture));
      case D3DRTYPE_CUBETEXTURE:   return static_cast<CubeTexture*>(static_cast<IDirect3DCubeTexture9*>(texture));
      case D3DRTYPE_VOLUMETEXTURE: return static_cast<VolumeTexture*>(static_cast<IDirect3DVolumeTexture9*>(texture));
      default:                     return nullptr;
    }
  }

  // ============================================================
  // Buffers
  // ============================================================

  template<typename Base, typename Desc>
  BufferCommon<Base, Desc>::BufferCommon(Device* device, std::unique_ptr<legacy::Buffer> buffer, const Desc& desc)
  : Resource<Base>(device, desc.Pool), m_buffer(std::move(buffer)), m_desc(desc) {}

  template<typename Base, typename Desc>
  HRESULT BufferCommon<Base, Desc>::Lock(UINT offset, UINT size, void** data, DWORD flags) {
    if (!data)
      return D3DERR_INVALIDCALL;
    // Draws read the buffer's clean ranges on other threads.
    std::unique_lock<std::recursive_mutex> guard(this->m_device->mutex(), std::defer_lock);
    if (this->m_device->multithreaded()) guard.lock();
    const UINT total = m_buffer->size();
    if (offset > total) {
      *data = nullptr;
      return D3DERR_INVALIDCALL;
    }
    if (!size || uint64_t(offset) + size > total)
      size = total - offset;
    // The copy first: one that cannot come back from the GPU fails the
    // lock with the buffer as it was.
    uint8_t* copy = m_buffer->data();
    if (!copy) {
      *data = nullptr;
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    if (flags & D3DLOCK_DISCARD)
      m_buffer->invalidate(0, 0);
    else if (!(flags & D3DLOCK_READONLY))
      m_buffer->invalidate(offset, size);
    *data = copy + offset;
    return D3D_OK;
  }

  template class BufferCommon<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>;
  template class BufferCommon<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>;

  HRESULT VertexBuffer::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DVertexBuffer9)) {
      AddRef();
      *object = static_cast<IDirect3DVertexBuffer9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT IndexBuffer::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DIndexBuffer9)) {
      AddRef();
      *object = static_cast<IDirect3DIndexBuffer9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  // ============================================================
  // Device children
  // ============================================================

  template<typename Base>
  DeviceChild<Base>::DeviceChild(Device* device, bool keptByDevice) : m_device(device), m_keptByDevice(keptByDevice) {
    if (!m_keptByDevice)
      m_device->addRefPrivate();
  }

  template<typename Base>
  DeviceChild<Base>::~DeviceChild() {
    if (!m_keptByDevice)
      m_device->releasePrivate();
  }

  template<typename Base>
  void DeviceChild<Base>::onRevived() {
    if (m_keptByDevice)
      m_device->addRefPrivate();
  }

  template<typename Base>
  void DeviceChild<Base>::onPublicReleased() {
    if (m_keptByDevice)
      m_device->releasePrivate();
  }

  template<typename Base>
  HRESULT DeviceChild<Base>::GetDevice(IDirect3DDevice9** device) {
    if (!device)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
  }

  template class DeviceChild<IDirect3DVertexDeclaration9>;
  template class DeviceChild<IDirect3DVertexShader9>;
  template class DeviceChild<IDirect3DPixelShader9>;
  template class DeviceChild<IDirect3DQuery9>;
  template class DeviceChild<IDirect3DStateBlock9>;

  namespace {
    uint32_t elementCount(const D3DVERTEXELEMENT9* elements) {
      uint32_t count = 0;
      while (elements && elements[count].Stream != 0xff && count < 64)
        count++;
      return count;
    }
  }

  VertexDeclaration::VertexDeclaration(Device* device, const D3DVERTEXELEMENT9* elements, uint32_t fvf, bool keptByDevice)
  : DeviceChild(device, keptByDevice), m_elements(elements, elements + elementCount(elements)),
    m_decl(reinterpret_cast<const legacy::VertexElement*>(elements), elementCount(elements), fvf), m_fvf(fvf) {
    m_elements.push_back(D3DDECL_END());
  }

  HRESULT VertexDeclaration::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexDeclaration9)) {
      AddRef();
      *object = static_cast<IDirect3DVertexDeclaration9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9* elements, UINT* count) {
    if (!count)
      return D3DERR_INVALIDCALL;
    *count = UINT(m_elements.size());
    if (elements)
      std::memcpy(elements, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
    return D3D_OK;
  }

  template<typename Base>
  HRESULT ShaderObject<Base>::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(Base)) {
      this->AddRef();
      *object = static_cast<Base*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  template<typename Base>
  HRESULT ShaderObject<Base>::GetFunction(void* data, UINT* size) {
    if (!size)
      return D3DERR_INVALIDCALL;
    const auto& tokens = m_shader->tokens();
    const UINT bytes = UINT(tokens.size() * 4);
    if (!data) {
      *size = bytes;
      return D3D_OK;
    }
    if (*size < bytes)
      return D3DERR_INVALIDCALL;
    std::memcpy(data, tokens.data(), bytes);
    *size = bytes;
    return D3D_OK;
  }

  template class ShaderObject<IDirect3DVertexShader9>;
  template class ShaderObject<IDirect3DPixelShader9>;

  // ============================================================
  // Query
  // ============================================================

  Query::Query(Device* device, D3DQUERYTYPE type)
  : DeviceChild(device), m_type(type) {
    ID3D11Device* d3d11 = device->core()->d3d11();
    D3D11_QUERY_DESC desc = {};
    switch (type) {
      case D3DQUERYTYPE_EVENT:             desc.Query = D3D11_QUERY_EVENT; break;
      case D3DQUERYTYPE_OCCLUSION:         desc.Query = D3D11_QUERY_OCCLUSION; break;
      case D3DQUERYTYPE_TIMESTAMP:         desc.Query = D3D11_QUERY_TIMESTAMP; break;
      case D3DQUERYTYPE_TIMESTAMPDISJOINT:
      case D3DQUERYTYPE_TIMESTAMPFREQ:     desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; break;
      case D3DQUERYTYPE_VCACHE:            m_valid = true; return;
      default:                             return;
    }
    m_valid = SUCCEEDED(d3d11->CreateQuery(&desc, m_query.put()));
  }

  Query::~Query() = default;

  HRESULT Query::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9)) {
      AddRef();
      *object = static_cast<IDirect3DQuery9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  DWORD Query::GetDataSize() {
    switch (m_type) {
      case D3DQUERYTYPE_EVENT:             return sizeof(BOOL);
      case D3DQUERYTYPE_OCCLUSION:         return sizeof(DWORD);
      case D3DQUERYTYPE_TIMESTAMP:         return sizeof(UINT64);
      case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
      case D3DQUERYTYPE_TIMESTAMPFREQ:     return sizeof(UINT64);
      case D3DQUERYTYPE_VCACHE:            return sizeof(D3DDEVINFO_VCACHE);
      default:                             return 0;
    }
  }

  HRESULT Query::Issue(DWORD flags) {
    if (!m_query)
      return D3D_OK;
    std::unique_lock<std::recursive_mutex> guard(m_device->mutex(), std::defer_lock);
    if (m_device->multithreaded()) guard.lock();
    ID3D11DeviceContext* context = m_device->core()->context();
    const bool beginEnd = m_type == D3DQUERYTYPE_OCCLUSION || m_type == D3DQUERYTYPE_TIMESTAMPDISJOINT
      || m_type == D3DQUERYTYPE_TIMESTAMPFREQ;
    if (flags & D3DISSUE_BEGIN) {
      if (beginEnd) {
        context->Begin(m_query.get());
        m_building = true;
      }
    }
    if (m_device->core()->tracing9()) {
      logMessage(LogLevel::Warn, "trace9   query %p type %u issue%s%s", static_cast<void*>(this), unsigned(m_type),
        (flags & D3DISSUE_BEGIN) ? " begin" : "", (flags & D3DISSUE_END) ? " end" : "");
      if (m_type == D3DQUERYTYPE_OCCLUSION && (flags & D3DISSUE_BEGIN))
        m_device->core()->traceDepth9("at the first occlusion query");
    }
    if (flags & D3DISSUE_END) {
      // An end without a begin: an empty query (occlusion: nothing drawn).
      if (beginEnd && !m_building && (m_type == D3DQUERYTYPE_OCCLUSION || !m_issued))
        context->Begin(m_query.get());
      context->End(m_query.get());
      m_issued = true;
      m_building = false;
      m_resultNoted = false;
    }
    return D3D_OK;
  }

  HRESULT Query::GetData(void* data, DWORD size, DWORD flags) {
    if (m_type == D3DQUERYTYPE_VCACHE) {
      if (data && size >= sizeof(D3DDEVINFO_VCACHE)) {
        D3DDEVINFO_VCACHE info = { MAKEFOURCC('C', 'A', 'C', 'H'), 1, 16, 7 };
        std::memcpy(data, &info, sizeof(info));
      }
      return D3D_OK;
    }
    if (!m_query)
      return D3DERR_INVALIDCALL;
    std::unique_lock<std::recursive_mutex> guard(m_device->mutex(), std::defer_lock);
    if (m_device->multithreaded()) guard.lock();
    // Begun and not ended: no result yet.
    if (m_building)
      return S_FALSE;
    if (!m_issued) {
      // Never issued: done, nothing counted.
      if (data && size) std::memset(data, 0, size);
      if (m_type == D3DQUERYTYPE_EVENT && data && size >= sizeof(BOOL))
        *static_cast<BOOL*>(data) = TRUE;
      return D3D_OK;
    }
    ID3D11DeviceContext* context = m_device->core()->context();
    const UINT getFlags = (flags & D3DGETDATA_FLUSH) ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
    HRESULT hr = S_FALSE;
    switch (m_type) {
      case D3DQUERYTYPE_EVENT: {
        BOOL done = FALSE;
        hr = context->GetData(m_query.get(), &done, sizeof(done), getFlags);
        if (hr == S_OK && data && size >= sizeof(BOOL))
          *static_cast<BOOL*>(data) = TRUE;
        break;
      }
      case D3DQUERYTYPE_OCCLUSION: {
        UINT64 samples = 0;
        hr = context->GetData(m_query.get(), &samples, sizeof(samples), getFlags);
        if (hr == S_OK && data && size >= sizeof(DWORD))
          *static_cast<DWORD*>(data) = DWORD((std::min)(samples, UINT64(0xffffffffu)));
        if (hr == S_OK && !m_resultNoted) {
          m_resultNoted = true;
          m_device->core()->noteOcclusionResult(samples);
        }
        if (m_device->core()->tracing9())
          logMessage(LogLevel::Warn, "trace9   query %p getdata %s %llu", static_cast<void*>(this),
            hr == S_OK ? "done" : "pending", static_cast<unsigned long long>(samples));
        break;
      }
      case D3DQUERYTYPE_TIMESTAMP: {
        UINT64 time = 0;
        hr = context->GetData(m_query.get(), &time, sizeof(time), getFlags);
        if (hr == S_OK && data && size >= sizeof(UINT64))
          *static_cast<UINT64*>(data) = time;
        break;
      }
      case D3DQUERYTYPE_TIMESTAMPDISJOINT:
      case D3DQUERYTYPE_TIMESTAMPFREQ: {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        hr = context->GetData(m_query.get(), &disjoint, sizeof(disjoint), getFlags);
        if (hr == S_OK && data) {
          if (m_type == D3DQUERYTYPE_TIMESTAMPDISJOINT && size >= sizeof(BOOL))
            *static_cast<BOOL*>(data) = disjoint.Disjoint;
          else if (m_type == D3DQUERYTYPE_TIMESTAMPFREQ && size >= sizeof(UINT64))
            *static_cast<UINT64*>(data) = disjoint.Frequency;
        }
        break;
      }
      default:
        break;
    }
    // A failing poll is a lost device only when the device is gone.
    if (FAILED(hr))
      return FAILED(m_device->core()->d3d11()->GetDeviceRemovedReason()) ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
    return hr == S_OK ? D3D_OK : S_FALSE;
  }

  // ============================================================
  // State blocks
  // ============================================================

  namespace {
    const DWORD kPixelRenderStates[] = {
      D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_LASTPIXEL,
      D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DITHERENABLE, D3DRS_FOGSTART,
      D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_ALPHABLENDENABLE, D3DRS_DEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL,
      D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK,
      D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR, D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4,
      D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_WRAP8, D3DRS_WRAP9, D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12,
      D3DRS_WRAP13, D3DRS_WRAP14, D3DRS_WRAP15, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE,
      D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL,
      D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1,
      D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR, D3DRS_SRGBWRITEENABLE,
      D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA,
    };
    const DWORD kVertexRenderStates[] = {
      D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
      D3DRS_RANGEFOGENABLE, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE, D3DRS_CLIPPING, D3DRS_LIGHTING,
      D3DRS_LOCALVIEWER, D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_DIFFUSEMATERIALSOURCE,
      D3DRS_SPECULARMATERIALSOURCE, D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE, D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN,
      D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C,
      D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK, D3DRS_PATCHEDGESTYLE, D3DRS_POINTSIZE_MAX,
      D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR, D3DRS_POSITIONDEGREE, D3DRS_NORMALDEGREE,
      D3DRS_MINTESSELLATIONLEVEL, D3DRS_MAXTESSELLATIONLEVEL, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y,
      D3DRS_ADAPTIVETESS_Z, D3DRS_ADAPTIVETESS_W, D3DRS_ENABLEADAPTIVETESSELLATION, D3DRS_NORMALIZENORMALS,
      D3DRS_SPECULARENABLE, D3DRS_SHADEMODE,
    };

    // Texture stage states of pixel blocks; vertex blocks hold TEXCOORDINDEX
    // and TEXTURETRANSFORMFLAGS (also in pixel blocks), as Windows' D3D9
    // captures them (D3DTSS_CONSTANT in neither).
    const DWORD kPixelStageStates[] = {
      D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
      D3DTSS_BUMPENVMAT00, D3DTSS_BUMPENVMAT01, D3DTSS_BUMPENVMAT10, D3DTSS_BUMPENVMAT11, D3DTSS_TEXCOORDINDEX,
      D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_COLORARG0, D3DTSS_ALPHAARG0,
      D3DTSS_RESULTARG,
    };
    const DWORD kVertexStageStates[] = { D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
  }

  StateBlock::StateBlock(Device* device, D3DSTATEBLOCKTYPE type)
  : DeviceChild(device) {
    StateSet& s = m_set;
    const bool all = type == D3DSBT_ALL;
    const bool pixel = all || type == D3DSBT_PIXELSTATE;
    const bool vertex = all || type == D3DSBT_VERTEXSTATE;
    if (all) {
      for (DWORD state = D3DRS_ZENABLE; state <= D3DRS_BLENDOPALPHA; state++)
        s.renderStatesSet.set(state);
    } else {
      if (pixel) for (DWORD state : kPixelRenderStates) s.renderStatesSet.set(state);
      if (vertex) for (DWORD state : kVertexRenderStates) s.renderStatesSet.set(state);
    }
    for (auto& stage : s.stageStatesSet) {
      if (pixel) for (DWORD t : kPixelStageStates) stage.set(t);
      if (vertex) for (DWORD t : kVertexStageStates) stage.set(t);
    }
    if (pixel) {
      // Sampler states but the displacement map's offset (ALL blocks only).
      for (uint32_t sampler = 0; sampler < legacy::kSamplerCount; sampler++)
        for (DWORD t = D3DSAMP_ADDRESSU; t <= D3DSAMP_ELEMENTINDEX; t++)
          s.samplerStatesSet[sampler].set(t);
      s.pixelShaderSet = true;
      s.psFloatsSet.set();
      s.psIntsSet.set();
      s.psBoolsSet.set();
    }
    if (vertex) {
      s.vertexShaderSet = true;
      s.vsFloatsSet.set();
      s.vsIntsSet.set();
      s.vsBoolsSet.set();
      s.declarationSet = true;
      s.frequenciesSet.set();
      // The lights that exist, any index (lights made later stay out).
      const std::vector<bool>& lights = device->lightsSet();
      for (uint32_t light = 0; light < lights.size(); light++) {
        if (!lights[light])
          continue;
        s.lights[light] = {};
        s.lightEnables[light] = FALSE;
      }
    }
    if (all) {
      for (uint32_t sampler = 0; sampler < legacy::kSamplerCount; sampler++)
        s.samplerStatesSet[sampler].set(D3DSAMP_DMAPOFFSET);
      s.texturesSet.set();
      s.streamsSet.set();
      s.indicesSet = true;
      s.materialSet = true;
      s.viewportSet = true;
      s.scissorSet = true;
      s.clipPlanesSet.set();
      for (uint32_t t : { 2u, 3u }) s.transforms[t] = {};
      for (uint32_t t = 16; t < 24; t++) s.transforms[t] = {};
      for (uint32_t t = 256; t < 260; t++) s.transforms[t] = {};
    }
    if (type)
      Capture();
  }

  StateBlock::~StateBlock() {
    releaseObjects();
  }

  namespace {
    template<typename T>
    void retainObject(T* object) {
      if (RefCounted* counted = object ? refCountedOf(object) : nullptr)
        counted->addRefPrivate();
    }

    template<typename T>
    void releaseObject(T*& object) {
      T* released = object;
      object = nullptr;
      if (RefCounted* counted = released ? refCountedOf(released) : nullptr)
        counted->releasePrivate();
    }
  }

  // Private references, as the device's bindings: the application's counts
  // stay what Windows' D3D9 shows.
  void StateBlock::retain() {
    for (auto* texture : m_set.textures) retainObject(texture);
    for (auto& stream : m_set.streams) retainObject(stream.buffer);
    retainObject(m_set.indices);
    retainObject(m_set.declaration);
    retainObject(m_set.vertexShader);
    retainObject(m_set.pixelShader);
  }

  void StateBlock::releaseObjects() {
    for (auto*& texture : m_set.textures) releaseObject(texture);
    for (auto& stream : m_set.streams) releaseObject(stream.buffer);
    releaseObject(m_set.indices);
    releaseObject(m_set.declaration);
    releaseObject(m_set.vertexShader);
    releaseObject(m_set.pixelShader);
  }

  HRESULT StateBlock::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DStateBlock9)) {
      AddRef();
      *object = static_cast<IDirect3DStateBlock9*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT StateBlock::Capture() {
    std::unique_lock<std::recursive_mutex> guard(m_device->mutex(), std::defer_lock);
    if (m_device->multithreaded()) guard.lock();
    // What the device binds it holds itself: released first, still there.
    releaseObjects();
    m_device->captureState(m_set);
    retain();
    return D3D_OK;
  }

  HRESULT StateBlock::Apply() {
    m_device->applyState(m_set);
    return D3D_OK;
  }

}
