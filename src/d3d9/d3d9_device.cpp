// The Direct3D 9 device and swap chains (see d3d9_objects.h).

#include "d3d9_objects.h"

#include "../legacy/legacy_formats.h"
#include "../util/util_types.h"

#include <algorithm>
#include <cmath>

namespace dxup::d3d9 {

  namespace {
    // Locks the device for applications that create it multithreaded.
    class DeviceLock {
    public:
      explicit DeviceLock(Device* device) : m_mutex(device->multithreaded() ? &device->mutex() : nullptr) {
        if (m_mutex) m_mutex->lock();
      }
      ~DeviceLock() { if (m_mutex) m_mutex->unlock(); }
    private:
      std::recursive_mutex* m_mutex;
    };

    // Samplers of the interface: 0-15, D3DVERTEXTEXTURESAMPLER0-3; else -1.
    int samplerIndex(DWORD sampler) {
      if (sampler < 16)
        return int(sampler);
      if (sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler <= D3DVERTEXTEXTURESAMPLER3)
        return int(legacy::kVertexSamplerBase + (sampler - D3DVERTEXTEXTURESAMPLER0));
      return -1;
    }

    UINT vertexCountOf(D3DPRIMITIVETYPE type, UINT primitives) {
      switch (type) {
        case D3DPT_POINTLIST:     return primitives;
        case D3DPT_LINELIST:      return primitives * 2;
        case D3DPT_LINESTRIP:     return primitives + 1;
        case D3DPT_TRIANGLELIST:  return primitives * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:   return primitives + 2;
        default:                  return 0;
      }
    }

    uint32_t samplesOf(D3DMULTISAMPLE_TYPE type, DWORD quality) {
      if (type == D3DMULTISAMPLE_NONMASKABLE)
        return 2u << (std::min)(quality, DWORD(2));
      if (type >= D3DMULTISAMPLE_2_SAMPLES && type <= D3DMULTISAMPLE_16_SAMPLES) {
        uint32_t samples = uint32_t(type);
        // D3D11 guarantees 1, 2, 4 and 8.
        if (samples > 8) samples = 8;
        while (samples & (samples - 1)) samples--;
        return samples;
      }
      return 1;
    }

    UINT syncInterval(UINT presentationInterval) {
      switch (presentationInterval) {
        case D3DPRESENT_INTERVAL_IMMEDIATE: return 0;
        case D3DPRESENT_INTERVAL_TWO:       return 2;
        case D3DPRESENT_INTERVAL_THREE:     return 3;
        case D3DPRESENT_INTERVAL_FOUR:      return 4;
        default:                            return 1;
      }
    }

    legacy::Rect toRect(const RECT& r) {
      return { r.left, r.top, r.right, r.bottom };
    }

    Surface* asSurface(IDirect3DSurface9* surface) { return static_cast<Surface*>(surface); }
  }

  // ============================================================
  // Swap chain
  // ============================================================

  SwapChain::SwapChain(Device* device, const D3DPRESENT_PARAMETERS& params, bool implicit)
  : m_device(device), m_implicit(implicit), m_params(params) {
    if (!m_implicit)
      m_device->addRefPrivate();
    for (UINT i = 0; i < 256; i++)
      m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = WORD(i * 257);
    if (m_params.BackBufferFormat == D3DFMT_UNKNOWN)
      m_params.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!m_params.BackBufferCount)
      m_params.BackBufferCount = 1;
    if (!m_params.BackBufferWidth || !m_params.BackBufferHeight) {
      RECT client = {};
      GetClientRect(m_params.hDeviceWindow, &client);
      if (!m_params.BackBufferWidth) m_params.BackBufferWidth = (std::max)(1L, client.right - client.left);
      if (!m_params.BackBufferHeight) m_params.BackBufferHeight = (std::max)(1L, client.bottom - client.top);
    }
    legacy::TextureDesc desc;
    desc.width = m_params.BackBufferWidth;
    desc.height = m_params.BackBufferHeight;
    desc.format = legacy::Format(m_params.BackBufferFormat);
    desc.renderTarget = true;
    desc.srgb = true;
    desc.samples = samplesOf(m_params.MultiSampleType, m_params.MultiSampleQuality);
    desc.backBuffer = true;
    for (UINT i = 0; i < m_params.BackBufferCount; i++) {
      auto texture = std::make_shared<legacy::Texture>(m_device->core(), desc);
      if (!texture->valid()) {
        destroyBuffers();
        return;
      }
      m_backBuffers.push_back(new Surface(m_device, D3DPOOL_DEFAULT, texture, 0, static_cast<IDirect3DSwapChain9Ex*>(this),
                                          this, D3DUSAGE_RENDERTARGET, m_params.MultiSampleType,
                                          m_params.MultiSampleQuality));
    }
    if (desc.samples > 1) {
      desc.samples = 1;
      m_resolved = std::make_shared<legacy::Texture>(m_device->core(), desc);
    }
  }

  SwapChain::~SwapChain() {
    destroyBuffers();
    if (!m_implicit)
      m_device->releasePrivate();
  }

  void SwapChain::destroyBuffers() {
    for (Surface* surface : m_backBuffers)
      surface->destroyContained();
    m_backBuffers.clear();
  }

  void SwapChain::addRefPrivate() {
    if (m_implicit) m_device->addRefPrivate();
    else RefCounted::addRefPrivate();
  }

  void SwapChain::releasePrivate() {
    if (m_implicit) m_device->releasePrivate();
    else RefCounted::releasePrivate();
  }

  ULONG SwapChain::AddRef() {
    if (m_implicit) {
      ++m_appRefs;
      return m_device->addRefInternal();
    }
    return addRefPublic();
  }

  ULONG SwapChain::Release() {
    if (m_implicit) {
      --m_appRefs;
      return m_device->releaseInternal();
    }
    return releasePublic();
  }

  HRESULT SwapChain::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DSwapChain9) || riid == __uuidof(IDirect3DSwapChain9Ex)) {
      AddRef();
      *object = static_cast<IDirect3DSwapChain9Ex*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT SwapChain::Present(const RECT* sourceRect, const RECT*, HWND window, const RGNDATA*, DWORD flags) {
    if (m_backBuffers.empty())
      return D3DERR_INVALIDCALL;
    DeviceLock lock(m_device);
    // What the frame drew with and no longer binds goes.
    m_device->releaseHeld(false);
    legacy::Device* core = m_device->core();
    legacy::Texture* source = m_backBuffers[0]->texture();
    if (m_resolved && m_resolved->valid()) {
      core->context()->ResolveSubresource(m_resolved->resource(), 0, source->resource(), 0, source->dxgiFormat());
      source = m_resolved.get();
    }
    legacy::Rect rect = {};
    if (sourceRect)
      rect = toRect(*sourceRect);
    HWND target = window ? window : (m_params.hDeviceWindow ? m_params.hDeviceWindow : nullptr);
    UINT interval = syncInterval(m_params.PresentationInterval);
    if (flags & D3DPRESENT_FORCEIMMEDIATE)
      interval = 0;
    const HRESULT hr = core->present(source, sourceRect ? &rect : nullptr, target, interval, &m_gamma.red[0]);
    m_presentCount++;
    if (FAILED(hr))
      logMessage(LogLevel::Warn, "D3D9 Present failed: 0x%08lx", hr);
    // The application drew; flush its work so the next frame starts fresh.
    return D3D_OK;
  }

  // The screen as shown, in an A8R8G8B8 surface (Windows' D3D9 copies the
  // monitor's picture, also around a window): read through GDI.
  HRESULT SwapChain::GetFrontBufferData(IDirect3DSurface9* destination) {
    D3DSURFACE_DESC desc = {};
    if (!destination || FAILED(destination->GetDesc(&desc)) || desc.Format != D3DFMT_A8R8G8B8)
      return D3DERR_INVALIDCALL;
    MONITORINFO info = { sizeof(info) };
    if (!GetMonitorInfoW(MonitorFromWindow(window(), MONITOR_DEFAULTTOPRIMARY), &info))
      return D3DERR_INVALIDCALL;
    const int width = (std::min)(int(desc.Width), int(info.rcMonitor.right - info.rcMonitor.left));
    const int height = (std::min)(int(desc.Height), int(info.rcMonitor.bottom - info.rcMonitor.top));
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO bitmap = {};
    bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP section = memory ? CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    HRESULT hr = D3DERR_INVALIDCALL;
    D3DLOCKED_RECT locked = {};
    if (section && width > 0 && height > 0) {
      HGDIOBJ old = SelectObject(memory, section);
      BitBlt(memory, 0, 0, width, height, screen, info.rcMonitor.left, info.rcMonitor.top, SRCCOPY);
      GdiFlush();
      SelectObject(memory, old);
      if (SUCCEEDED(destination->LockRect(&locked, nullptr, 0))) {
        for (int y = 0; y < height; y++) {
          const auto* from = static_cast<const uint32_t*>(bits) + size_t(y) * width;
          auto* to = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch);
          for (int x = 0; x < width; x++)
            to[x] = from[x] | 0xff000000u;
        }
        destination->UnlockRect();
        hr = D3D_OK;
      }
    }
    if (section) DeleteObject(section);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return hr;
  }

  HRESULT SwapChain::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface9** buffer) {
    if (!buffer || index >= m_backBuffers.size())
      return D3DERR_INVALIDCALL;
    m_backBuffers[index]->AddRef();
    *buffer = m_backBuffers[index];
    return D3D_OK;
  }

  HRESULT SwapChain::GetRasterStatus(D3DRASTER_STATUS* status) {
    if (!status)
      return D3DERR_INVALIDCALL;
    // Without the hardware's scanline: an estimate from the time.
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double period = 1.0 / 60.0;
    const double phase = std::fmod(double(now.QuadPart) / double(frequency.QuadPart), period) / period;
    const UINT height = m_params.BackBufferHeight ? m_params.BackBufferHeight : 1080;
    const UINT total = height + height / 20;
    const UINT line = UINT(phase * total);
    status->InVBlank = line >= height;
    status->ScanLine = status->InVBlank ? 0 : line;
    return D3D_OK;
  }

  HRESULT SwapChain::GetDisplayMode(D3DDISPLAYMODE* mode) {
    return m_device->GetDisplayMode(0, mode);
  }

  HRESULT SwapChain::GetDevice(IDirect3DDevice9** device) {
    if (!device)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
  }

  HRESULT SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS* params) {
    if (!params)
      return D3DERR_INVALIDCALL;
    *params = m_params;
    return D3D_OK;
  }

  HRESULT SwapChain::GetLastPresentCount(UINT* count) {
    if (!count)
      return D3DERR_INVALIDCALL;
    *count = m_presentCount;
    return D3D_OK;
  }

  HRESULT SwapChain::GetPresentStats(D3DPRESENTSTATS* stats) {
    if (!stats)
      return D3DERR_INVALIDCALL;
    *stats = {};
    stats->PresentCount = m_presentCount;
    return D3D_OK;
  }

  HRESULT SwapChain::GetDisplayModeEx(D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) {
    return m_device->GetDisplayModeEx(0, mode, rotation);
  }

  // ============================================================
  // Device
  // ============================================================

  Device::Device(Interface* parent, UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior, bool ex)
  : m_parent(parent), m_adapter(adapter), m_deviceType(type), m_focusWindow(focusWindow), m_behavior(behavior), m_ex(ex) {
    m_parent->AddRef();
    m_clipStatus.ClipUnion = 0;
    m_clipStatus.ClipIntersection = 0xffffffffu;
  }

  Device::~Device() {
    releaseHeld(true);
    for (auto& entry : m_fvfDeclarations)
      entry.second->releasePrivate();
    m_fvfDeclarations.clear();
    if (m_autoDepthStencil)
      delete m_autoDepthStencil;
    delete m_swapChain;
    for (Surface* surface : m_retiredSurfaces)
      delete surface;
    for (SwapChain* chain : m_retiredSwapChains)
      delete chain;
    if (m_cursor)
      DestroyCursor(m_cursor);
    m_resolveTarget.reset();
    m_core.reset();
    m_parent->Release();
  }

  void Device::onPublicReleased() {
    // The application let go: nothing stays bound.
    for (auto& target : m_renderTargets) target.reset();
    m_depthStencil.reset();
    for (uint32_t i = 0; i < m_textures.size(); i++) {
      m_textures[i] = nullptr;
      m_textureBases[i] = nullptr;
    }
    for (auto& stream : m_streams) stream.buffer.reset();
    m_indices.reset();
    m_declaration.reset();
    m_vertexShader.reset();
    m_pixelShader.reset();
    releaseHeld(true);
    if (m_recording) {
      m_recording->Release();
      m_recording = nullptr;
    }
  }

  HRESULT Device::init(D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX*) {
    if (!params)
      return D3DERR_INVALIDCALL;
    std::string error;
    HWND window = params->hDeviceWindow ? params->hDeviceWindow : m_focusWindow;
    m_core = legacy::Device::create(window, error);
    if (!m_core) {
      logMessage(LogLevel::Error, "D3D9: device creation failed: %s", error.c_str());
      return D3DERR_NOTAVAILABLE;
    }
    const HRESULT hr = resetSwapChain(params);
    if (FAILED(hr))
      return hr;
    logMessage(LogLevel::Info, "D3D9 device: %ux%u format %u, %u back buffers, %s, multisample %u, interval 0x%x",
      params->BackBufferWidth, params->BackBufferHeight, params->BackBufferFormat, params->BackBufferCount,
      params->Windowed ? "windowed" : "fullscreen", params->MultiSampleType, params->PresentationInterval);
    return D3D_OK;
  }

  void Device::applyFullscreen(const D3DPRESENT_PARAMETERS& params) {
    if (params.Windowed)
      return;
    // No display mode change: the window covers its monitor, and the swap
    // chain scales the back buffer to it.
    HWND window = params.hDeviceWindow ? params.hDeviceWindow : m_focusWindow;
    if (!window)
      return;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    dpiAwareForFullScreen(monitor);
    MONITORINFO info = { sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info))
      return;
    LONG style = GetWindowLongW(window, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_POPUP;
    SetWindowLongW(window, GWL_STYLE, style);
    SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
  }

  HRESULT Device::resetSwapChain(D3DPRESENT_PARAMETERS* params) {
    // Unbind what the old swap chain gave.
    for (auto& target : m_renderTargets) target.reset();
    m_depthStencil.reset();
    m_core->setDepthStencil(nullptr);
    for (uint32_t i = 0; i < legacy::kMaxRenderTargets; i++)
      m_core->setRenderTarget(i, nullptr, 0);
    // What the application still holds (Direct3D 9Ex) lives on until the
    // device goes.
    if (m_autoDepthStencil) {
      if (m_autoDepthStencil->heldByApplication())
        m_retiredSurfaces.push_back(m_autoDepthStencil);
      else
        delete m_autoDepthStencil;
      m_autoDepthStencil = nullptr;
    }
    if (m_swapChain && m_swapChain->heldByApplication())
      m_retiredSwapChains.push_back(m_swapChain);
    else
      delete m_swapChain;
    m_swapChain = nullptr;

    D3DPRESENT_PARAMETERS p = *params;
    if (!p.hDeviceWindow)
      p.hDeviceWindow = m_focusWindow;
    applyFullscreen(p);
    m_swapChain = new SwapChain(this, p, true);
    if (!m_swapChain->valid()) {
      delete m_swapChain;
      m_swapChain = nullptr;
      return D3DERR_NOTAVAILABLE;
    }
    // The application sees what was filled in.
    const D3DPRESENT_PARAMETERS& actual = m_swapChain->params();
    params->BackBufferWidth = actual.BackBufferWidth;
    params->BackBufferHeight = actual.BackBufferHeight;
    params->BackBufferFormat = actual.BackBufferFormat;
    params->BackBufferCount = actual.BackBufferCount;

    if (p.EnableAutoDepthStencil) {
      legacy::TextureDesc desc;
      desc.width = actual.BackBufferWidth;
      desc.height = actual.BackBufferHeight;
      desc.format = legacy::Format(p.AutoDepthStencilFormat);
      desc.samples = samplesOf(p.MultiSampleType, p.MultiSampleQuality);
      auto texture = std::make_shared<legacy::Texture>(m_core.get(), desc);
      if (!texture->valid())
        return D3DERR_NOTAVAILABLE;
      // Counted on the device (as the back buffers): the device lives as
      // long as the application holds it.
      m_autoDepthStencil = new Surface(this, D3DPOOL_DEFAULT, texture, 0, static_cast<IDirect3DDevice9Ex*>(this), this,
                                       D3DUSAGE_DEPTHSTENCIL, p.MultiSampleType, p.MultiSampleQuality, true);
      m_depthStencil.set(m_autoDepthStencil);
      m_core->setDepthStencil(texture.get());
    }
    Surface* back = m_swapChain->backBuffer(0);
    m_renderTargets[0].set(back);
    m_core->setRenderTarget(0, back->texture(), 0);
    m_core->setRenderState(D3DRS_ZENABLE, p.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE);
    return D3D_OK;
  }

  void Device::resetState() {
    for (uint32_t i = 0; i < m_textures.size(); i++) {
      m_textures[i] = nullptr;
      m_textureBases[i] = nullptr;
    }
    for (auto& stream : m_streams) {
      stream.buffer.reset();
      stream.offset = stream.stride = 0;
      stream.frequency = 1;
    }
    m_indices.reset();
    m_declaration.reset();
    m_fvf = 0;
    m_vertexShader.reset();
    m_pixelShader.reset();
    releaseHeld(true);
    m_core->resetState();
  }

  void Device::releaseHeld(bool all) {
    if (!all) {
      // Bound now: kept.
      for (BaseTexture* texture : m_textureBases)
        if (texture) texture->refCounted().heldEpoch = m_heldEpoch;
      for (auto& stream : m_streams)
        if (stream.buffer) stream.buffer->heldEpoch = m_heldEpoch;
      if (m_indices) m_indices->heldEpoch = m_heldEpoch;
      if (m_declaration) m_declaration->heldEpoch = m_heldEpoch;
      if (m_vertexShader) m_vertexShader->heldEpoch = m_heldEpoch;
      if (m_pixelShader) m_pixelShader->heldEpoch = m_heldEpoch;
    }
    size_t kept = 0;
    for (RefCounted* object : m_held) {
      if (!all && object->heldEpoch == m_heldEpoch)
        m_held[kept++] = object;
      else
        m_heldReleasing.push_back(object);
    }
    m_held.resize(kept);
    if (++m_heldEpoch == 0)
      m_heldEpoch = 1;
    // Releasing may destroy the object (and what it holds).
    for (RefCounted* object : m_heldReleasing) {
      object->heldEpoch = 0;
      object->releasePrivate();
    }
    m_heldReleasing.clear();
  }

  HRESULT Device::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9) || (m_ex && riid == __uuidof(IDirect3DDevice9Ex))) {
      AddRef();
      *object = static_cast<IDirect3DDevice9Ex*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT Device::TestCooperativeLevel() {
    return D3D_OK;
  }

  UINT Device::GetAvailableTextureMem() {
    // Old games keep this in a signed 32-bit integer.
    return 0x7ff00000u;
  }

  HRESULT Device::GetDirect3D(IDirect3D9** d3d) {
    if (!d3d)
      return D3DERR_INVALIDCALL;
    m_parent->AddRef();
    *d3d = m_parent;
    return D3D_OK;
  }

  D3DCAPS9 Device::caps() const {
    return Interface::makeCaps(m_adapter);
  }

  HRESULT Device::GetDeviceCaps(D3DCAPS9* caps) {
    if (!caps)
      return D3DERR_INVALIDCALL;
    *caps = this->caps();
    return D3D_OK;
  }

  HRESULT Device::GetDisplayMode(UINT, D3DDISPLAYMODE* mode) {
    return m_parent->GetAdapterDisplayMode(m_adapter, mode);
  }

  HRESULT Device::GetDisplayModeEx(UINT, D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) {
    return m_parent->GetAdapterDisplayModeEx(m_adapter, mode, rotation);
  }

  HRESULT Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) {
    if (!params)
      return D3DERR_INVALIDCALL;
    params->AdapterOrdinal = m_adapter;
    params->DeviceType = m_deviceType;
    params->hFocusWindow = m_focusWindow;
    params->BehaviorFlags = m_behavior;
    return D3D_OK;
  }

  HRESULT Device::SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* bitmap) {
    if (!bitmap)
      return D3DERR_INVALIDCALL;
    Surface* surface = asSurface(bitmap);
    const UINT w = surface->width(), h = surface->height();
    if (surface->format() != D3DFMT_A8R8G8B8 || w > 256 || h > 256)
      return D3DERR_INVALIDCALL;
    D3DLOCKED_RECT locked = {};
    if (FAILED(surface->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
      return D3DERR_INVALIDCALL;
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = LONG(w);
    header.bV5Height = -LONG(h);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HDC dc = ::GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (color && bits)
      for (UINT row = 0; row < h; row++)
        std::memcpy(static_cast<uint8_t*>(bits) + size_t(row) * w * 4,
                    static_cast<const uint8_t*>(locked.pBits) + size_t(row) * locked.Pitch, size_t(w) * 4);
    surface->UnlockRect();
    HBITMAP mask = CreateBitmap(int(w), int(h), 1, 1, nullptr);
    ICONINFO info = {};
    info.fIcon = FALSE;
    info.xHotspot = x;
    info.yHotspot = y;
    info.hbmMask = mask;
    info.hbmColor = color;
    HCURSOR cursor = CreateIconIndirect(&info);
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    if (!cursor)
      return D3DERR_INVALIDCALL;
    if (m_cursor)
      DestroyCursor(m_cursor);
    m_cursor = cursor;
    if (m_cursorShown)
      SetCursor(m_cursor);
    return D3D_OK;
  }

  void Device::SetCursorPosition(int x, int y, DWORD) {
    SetCursorPos(x, y);
  }

  BOOL Device::ShowCursor(BOOL show) {
    const BOOL previous = m_cursorShown;
    m_cursorShown = show != FALSE;
    if (m_cursorShown && m_cursor)
      SetCursor(m_cursor);
    return previous;
  }

  HRESULT Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* params, IDirect3DSwapChain9** swapChain) {
    if (!params || !swapChain)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    D3DPRESENT_PARAMETERS p = *params;
    if (!p.hDeviceWindow)
      p.hDeviceWindow = m_focusWindow;
    auto* chain = new SwapChain(this, p, false);
    if (!chain->valid()) {
      delete chain;
      return D3DERR_NOTAVAILABLE;
    }
    chain->AddRef();
    *swapChain = chain;
    return D3D_OK;
  }

  HRESULT Device::GetSwapChain(UINT index, IDirect3DSwapChain9** swapChain) {
    if (!swapChain || index != 0 || !m_swapChain)
      return D3DERR_INVALIDCALL;
    m_swapChain->AddRef();
    *swapChain = m_swapChain;
    return D3D_OK;
  }

  HRESULT Device::Reset(D3DPRESENT_PARAMETERS* params) {
    if (!params)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    // Direct3D 9 refuses while the application holds the back buffers or
    // the automatic depth buffer (default pool resources).
    if (!m_ex && ((m_autoDepthStencil && m_autoDepthStencil->heldByApplication())
                  || (m_swapChain && m_swapChain->buffersHeld()))) {
      logFailedCall(D3DERR_INVALIDCALL, "Reset while the application holds a back buffer or the automatic depth buffer");
      return D3DERR_INVALIDCALL;
    }
    if (m_recording) {
      m_recording->Release();
      m_recording = nullptr;
    }
    for (uint32_t i = 1; i < legacy::kMaxRenderTargets; i++)
      m_renderTargets[i].reset();
    resetState();
    const HRESULT hr = resetSwapChain(params);
    logMessage(LogLevel::Info, "D3D9 Reset: %ux%u %s, interval 0x%x -> 0x%08lx", params->BackBufferWidth, params->BackBufferHeight,
      params->Windowed ? "windowed" : "fullscreen", params->PresentationInterval, hr);
    return hr;
  }

  HRESULT Device::ResetEx(D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX*) {
    return Reset(params);
  }

  HRESULT Device::Present(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty) {
    if (!m_swapChain)
      return D3DERR_INVALIDCALL;
    return m_swapChain->Present(sourceRect, destRect, window, dirty, 0);
  }

  HRESULT Device::PresentEx(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty, DWORD flags) {
    if (!m_swapChain)
      return D3DERR_INVALIDCALL;
    return m_swapChain->Present(sourceRect, destRect, window, dirty, flags);
  }

  HRESULT Device::GetBackBuffer(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** buffer) {
    if (swapChain != 0 || !m_swapChain)
      return D3DERR_INVALIDCALL;
    return m_swapChain->GetBackBuffer(index, type, buffer);
  }

  HRESULT Device::GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* status) {
    if (swapChain != 0 || !m_swapChain)
      return D3DERR_INVALIDCALL;
    return m_swapChain->GetRasterStatus(status);
  }

  void Device::SetGammaRamp(UINT swapChain, DWORD, const D3DGAMMARAMP* ramp) {
    if (swapChain == 0 && ramp && m_swapChain)
      m_swapChain->setGammaRamp(*ramp);
  }

  void Device::GetGammaRamp(UINT swapChain, D3DGAMMARAMP* ramp) {
    if (swapChain == 0 && ramp && m_swapChain)
      *ramp = m_swapChain->gammaRamp();
  }

  // ---- Resources ----

  HRESULT Device::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                IDirect3DTexture9** texture, HANDLE* shared) {
    const HRESULT hr = [&]() -> HRESULT {
      if (!texture || !width || !height || shared)
        return D3DERR_INVALIDCALL;
      *texture = nullptr;
      DeviceLock lock(this);
      const UINT fullChain = UINT(std::floor(std::log2(double((std::max)(width, height))))) + 1;
      if (!levels || levels > fullChain)
        levels = fullChain;
      if ((usage & D3DUSAGE_AUTOGENMIPMAP) && pool == D3DPOOL_SYSTEMMEM)
        return D3DERR_INVALIDCALL;
      legacy::TextureDesc desc;
      desc.width = width;
      desc.height = height;
      desc.levels = levels;
      desc.format = legacy::Format(format);
      desc.renderTarget = (usage & D3DUSAGE_RENDERTARGET) != 0;
      desc.depthStencil = (usage & D3DUSAGE_DEPTHSTENCIL) != 0;
      desc.sampledDepth = desc.depthStencil;
      desc.systemMemory = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
      desc.srgb = true;
      desc.autoMips = (usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
      auto shared_ = std::make_shared<legacy::Texture>(m_core.get(), desc);
      if (!shared_->valid())
        return D3DERR_INVALIDCALL;
      auto* object = new Texture2D(this, std::move(shared_), pool, usage);
      object->AddRef();
      *texture = object;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "CreateTexture %ux%u, %u levels, usage 0x%lx, format %s, pool %u", width, height, levels, usage, formatName(format).c_str(), unsigned(pool));
    return hr;
  }

  HRESULT Device::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                                      D3DPOOL pool, IDirect3DVolumeTexture9** texture, HANDLE* shared) {
    const HRESULT hr = [&]() -> HRESULT {
      if (!texture || !width || !height || !depth || shared)
        return D3DERR_INVALIDCALL;
      *texture = nullptr;
      DeviceLock lock(this);
      const UINT fullChain = UINT(std::floor(std::log2(double((std::max)((std::max)(width, height), depth))))) + 1;
      if (!levels || levels > fullChain)
        levels = fullChain;
      legacy::TextureDesc desc;
      desc.width = width;
      desc.height = height;
      desc.depth = depth;
      desc.volume = true;
      desc.levels = levels;
      desc.format = legacy::Format(format);
      desc.systemMemory = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
      auto shared_ = std::make_shared<legacy::Texture>(m_core.get(), desc);
      if (!shared_->valid())
        return D3DERR_INVALIDCALL;
      auto* object = new VolumeTexture(this, std::move(shared_), pool, usage);
      object->AddRef();
      *texture = object;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "CreateVolumeTexture %ux%ux%u, %u levels, usage 0x%lx, format %s, pool %u", width, height, depth, levels, usage, formatName(format).c_str(), unsigned(pool));
    return hr;
  }

  HRESULT Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                    IDirect3DCubeTexture9** texture, HANDLE* shared) {
    const HRESULT hr = [&]() -> HRESULT {
      if (!texture || !edge || shared)
        return D3DERR_INVALIDCALL;
      *texture = nullptr;
      DeviceLock lock(this);
      const UINT fullChain = UINT(std::floor(std::log2(double(edge)))) + 1;
      if (!levels || levels > fullChain)
        levels = fullChain;
      legacy::TextureDesc desc;
      desc.width = desc.height = edge;
      desc.levels = levels;
      desc.cube = true;
      desc.format = legacy::Format(format);
      desc.renderTarget = (usage & D3DUSAGE_RENDERTARGET) != 0;
      desc.depthStencil = (usage & D3DUSAGE_DEPTHSTENCIL) != 0;
      desc.sampledDepth = desc.depthStencil;
      desc.systemMemory = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
      desc.srgb = true;
      desc.autoMips = (usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
      auto shared_ = std::make_shared<legacy::Texture>(m_core.get(), desc);
      if (!shared_->valid())
        return D3DERR_INVALIDCALL;
      auto* object = new CubeTexture(this, std::move(shared_), pool, usage);
      object->AddRef();
      *texture = object;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "CreateCubeTexture %u, %u levels, usage 0x%lx, format %s, pool %u", edge, levels, usage, formatName(format).c_str(), unsigned(pool));
    return hr;
  }

  HRESULT Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** buffer,
                                     HANDLE* shared) {
    const HRESULT hr = [&]() -> HRESULT {
      if (!buffer || !length || shared)
        return D3DERR_INVALIDCALL;
      *buffer = nullptr;
      DeviceLock lock(this);
      auto core = std::make_unique<legacy::Buffer>(m_core.get(), length, false, (usage & D3DUSAGE_DYNAMIC) != 0);
      if (!core->valid())
        return D3DERR_OUTOFVIDEOMEMORY;
      D3DVERTEXBUFFER_DESC desc = { D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, usage, pool, length, fvf };
      auto* object = new VertexBuffer(this, std::move(core), desc);
      object->AddRef();
      *buffer = object;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "CreateVertexBuffer %u bytes, usage 0x%lx, FVF 0x%lx, pool %u", length, usage, fvf, unsigned(pool));
    return hr;
  }

  HRESULT Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** buffer,
                                    HANDLE* shared) {
    const HRESULT hr = [&]() -> HRESULT {
      if (!buffer || !length || shared || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32))
        return D3DERR_INVALIDCALL;
      *buffer = nullptr;
      DeviceLock lock(this);
      auto core = std::make_unique<legacy::Buffer>(m_core.get(), length, true, (usage & D3DUSAGE_DYNAMIC) != 0);
      if (!core->valid())
        return D3DERR_OUTOFVIDEOMEMORY;
      D3DINDEXBUFFER_DESC desc = { format, D3DRTYPE_INDEXBUFFER, usage, pool, length };
      auto* object = new IndexBuffer(this, std::move(core), desc);
      object->AddRef();
      *buffer = object;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "CreateIndexBuffer %u bytes, usage 0x%lx, format %s, pool %u", length, usage, formatName(format).c_str(), unsigned(pool));
    return hr;
  }

  HRESULT Device::createSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, DWORD usage,
                                D3DMULTISAMPLE_TYPE multisample, DWORD quality, IDirect3DSurface9** surface) {
    const HRESULT hr = [&]() -> HRESULT {
      if (!surface || !width || !height)
        return D3DERR_INVALIDCALL;
      *surface = nullptr;
      DeviceLock lock(this);
      legacy::TextureDesc desc;
      desc.width = width;
      desc.height = height;
      desc.format = legacy::Format(format);
      desc.renderTarget = (usage & D3DUSAGE_RENDERTARGET) != 0;
      desc.depthStencil = (usage & D3DUSAGE_DEPTHSTENCIL) != 0;
      desc.systemMemory = pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
      desc.srgb = desc.renderTarget;
      desc.samples = samplesOf(multisample, quality);
      auto texture = std::make_shared<legacy::Texture>(m_core.get(), desc);
      if (!texture->valid())
        return D3DERR_INVALIDCALL;
      auto* object = new Surface(this, pool, std::move(texture), 0, nullptr, nullptr, usage, multisample, quality);
      object->AddRef();
      *surface = object;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "surface %ux%u, format %s, pool %u, usage 0x%lx, multisample %u/%lu", width, height, formatName(format).c_str(), unsigned(pool), usage, unsigned(multisample), quality);
    return hr;
  }

  HRESULT Device::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, DWORD quality,
                                     BOOL, IDirect3DSurface9** surface, HANDLE* shared) {
    if (shared)
      return D3DERR_INVALIDCALL;
    return createSurface(width, height, format, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, multisample, quality, surface);
  }

  HRESULT Device::CreateRenderTargetEx(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                       DWORD quality, BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared, DWORD) {
    return CreateRenderTarget(width, height, format, multisample, quality, lockable, surface, shared);
  }

  HRESULT Device::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                            DWORD quality, BOOL, IDirect3DSurface9** surface, HANDLE* shared) {
    if (shared)
      return D3DERR_INVALIDCALL;
    return createSurface(width, height, format, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL, multisample, quality, surface);
  }

  HRESULT Device::CreateDepthStencilSurfaceEx(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                              DWORD quality, BOOL discard, IDirect3DSurface9** surface, HANDLE* shared, DWORD) {
    return CreateDepthStencilSurface(width, height, format, multisample, quality, discard, surface, shared);
  }

  HRESULT Device::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                              IDirect3DSurface9** surface, HANDLE* shared) {
    if (shared || pool == D3DPOOL_MANAGED)
      return D3DERR_INVALIDCALL;
    return createSurface(width, height, format, pool, 0, D3DMULTISAMPLE_NONE, 0, surface);
  }

  HRESULT Device::CreateOffscreenPlainSurfaceEx(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DSurface9** surface, HANDLE* shared, DWORD) {
    return CreateOffscreenPlainSurface(width, height, format, pool, surface, shared);
  }

  // ---- Copies ----

  namespace {
    // Rows of a rectangle from one surface into another of its format.
    HRESULT copyRect(Surface* source, const RECT& sourceRect, Surface* destination, POINT destPoint) {
      if (source->format() != destination->format())
        return D3DERR_INVALIDCALL;
      const legacy::FormatInfo info = legacy::formatInfo(legacy::Format(source->format()));
      const RECT destRect = { destPoint.x, destPoint.y, destPoint.x + (sourceRect.right - sourceRect.left),
                              destPoint.y + (sourceRect.bottom - sourceRect.top) };
      D3DLOCKED_RECT src = {}, dst = {};
      if (FAILED(source->LockRect(&src, &sourceRect, D3DLOCK_READONLY)))
        return D3DERR_INVALIDCALL;
      const bool whole = destRect.left == 0 && destRect.top == 0 && UINT(destRect.right) == destination->width()
        && UINT(destRect.bottom) == destination->height();
      if (FAILED(destination->LockRect(&dst, &destRect, whole ? D3DLOCK_DISCARD : 0))) {
        source->UnlockRect();
        return D3DERR_INVALIDCALL;
      }
      const LONG width = sourceRect.right - sourceRect.left, height = sourceRect.bottom - sourceRect.top;
      const UINT rows = info.block ? UINT(height + 3) / 4 : UINT(height);
      const size_t bytes = info.block ? size_t((width + 3) / 4) * info.bytesPerBlock : size_t(width) * info.bytesPerBlock;
      for (UINT row = 0; row < rows; row++)
        std::memcpy(static_cast<uint8_t*>(dst.pBits) + size_t(row) * dst.Pitch,
                    static_cast<const uint8_t*>(src.pBits) + size_t(row) * src.Pitch, bytes);
      destination->UnlockRect();
      source->UnlockRect();
      return D3D_OK;
    }
  }

  HRESULT Device::UpdateSurface(IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* destination,
                                const POINT* destPoint) {
    if (!source || !destination)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    Surface* src = asSurface(source);
    const RECT rect = sourceRect ? *sourceRect : RECT{ 0, 0, LONG(src->width()), LONG(src->height()) };
    const POINT point = destPoint ? *destPoint : POINT{ 0, 0 };
    return copyRect(src, rect, asSurface(destination), point);
  }

  HRESULT Device::UpdateTexture(IDirect3DBaseTexture9* source, IDirect3DBaseTexture9* destination) {
    if (!source || !destination || source->GetType() != destination->GetType())
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    BaseTexture* src = baseTextureOf(source);
    BaseTexture* dst = baseTextureOf(destination);
    if (!src || !dst)
      return D3DERR_INVALIDCALL;
    legacy::Texture* s = src->texture();
    legacy::Texture* d = dst->texture();
    if (s->desc().format != d->desc().format)
      return D3DERR_INVALIDCALL;
    // Levels of the same size: from the first source level the destination's top matches.
    uint32_t offset = 0;
    while (offset < s->desc().levels && s->width(offset) > d->width(0))
      offset++;
    const uint32_t faces = s->desc().cube ? 6 : 1;
    const uint32_t levels = (std::min)(d->desc().levels, s->desc().levels - offset);
    for (uint32_t face = 0; face < faces; face++) {
      for (uint32_t level = 0; level < levels; level++) {
        const uint32_t srcSub = face * s->desc().levels + level + offset;
        const uint32_t dstSub = face * d->desc().levels + level;
        void* srcData = nullptr;
        void* dstData = nullptr;
        uint32_t srcRow = 0, dstRow = 0, srcSlice = 0, dstSlice = 0;
        if (!s->lockBox(srcSub, nullptr, true, &srcData, &srcRow, &srcSlice))
          continue;
        if (!d->lockBox(dstSub, nullptr, false, &dstData, &dstRow, &dstSlice, true)) {
          s->unlock(srcSub);
          continue;
        }
        const size_t bytes = size_t((std::min)(srcSlice, dstSlice)) * (std::min)(s->depth(srcSub), d->depth(dstSub));
        std::memcpy(dstData, srcData, bytes);
        d->unlock(dstSub);
        s->unlock(srcSub);
      }
    }
    if (d->desc().autoMips)
      d->markGpuWritten();
    return D3D_OK;
  }

  HRESULT Device::GetRenderTargetData(IDirect3DSurface9* renderTarget, IDirect3DSurface9* destination) {
    if (!renderTarget || !destination)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    Surface* src = asSurface(renderTarget);
    Surface* dst = asSurface(destination);
    if (src->width() != dst->width() || src->height() != dst->height())
      return D3DERR_INVALIDCALL;
    const RECT rect = { 0, 0, LONG(src->width()), LONG(src->height()) };
    return copyRect(src, rect, dst, POINT{ 0, 0 });
  }

  HRESULT Device::GetFrontBufferData(UINT swapChain, IDirect3DSurface9* destination) {
    if (swapChain != 0 || !m_swapChain)
      return D3DERR_INVALIDCALL;
    return m_swapChain->GetFrontBufferData(destination);
  }

  HRESULT Device::StretchRect(IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* destination,
                              const RECT* destRect, D3DTEXTUREFILTERTYPE filter) {
    if (!source || !destination)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    Surface* src = asSurface(source);
    Surface* dst = asSurface(destination);
    legacy::Texture* s = src->texture();
    legacy::Texture* d = dst->texture();
    const RECT sr = sourceRect ? *sourceRect : RECT{ 0, 0, LONG(src->width()), LONG(src->height()) };
    const RECT dr = destRect ? *destRect : RECT{ 0, 0, LONG(dst->width()), LONG(dst->height()) };
    const bool sameSize = sr.right - sr.left == dr.right - dr.left && sr.bottom - sr.top == dr.bottom - dr.top;
    const bool wholeSource = sr.left == 0 && sr.top == 0 && UINT(sr.right) == src->width() && UINT(sr.bottom) == src->height();
    const bool wholeDest = dr.left == 0 && dr.top == 0 && UINT(dr.right) == dst->width() && UINT(dr.bottom) == dst->height();
    ID3D11DeviceContext* context = m_core->context();
    const bool depth = legacy::formatInfo(legacy::Format(src->format())).depth;
    if (s->nullTarget() || d->nullTarget())
      return D3D_OK;
    s->flush();
    d->flush();
    if (depth) {
      // Depth: copied whole, as Direct3D 9 allows.
      if (!sameSize || !wholeSource || !wholeDest || src->format() != dst->format() || !s->d3dResource() || !d->d3dResource())
        return D3DERR_INVALIDCALL;
      if (s->desc().samples == d->desc().samples)
        context->CopySubresourceRegion(d->d3dResource(), dst->subresource(), 0, 0, 0, s->d3dResource(), src->subresource(), nullptr);
      return D3D_OK;
    }
    if (s->desc().samples > 1) {
      // Multisampled: resolved, straight into the destination when it fits.
      if (sameSize && wholeSource && wholeDest && s->dxgiFormat() == d->dxgiFormat() && d->desc().samples == 1) {
        context->ResolveSubresource(d->resource(), dst->subresource(), s->resource(), src->subresource(), s->dxgiFormat());
        d->markGpuWritten();
        return D3D_OK;
      }
      legacy::TextureDesc desc = s->desc();
      desc.samples = 1;
      const legacy::TextureDesc* kept = m_resolveTarget ? &m_resolveTarget->desc() : nullptr;
      if (!kept || kept->width != desc.width || kept->height != desc.height || kept->format != desc.format
       || kept->levels != desc.levels || kept->cube != desc.cube || kept->srgb != desc.srgb
       || kept->renderTarget != desc.renderTarget) {
        m_resolveTarget = std::make_shared<legacy::Texture>(m_core.get(), desc);
        if (!m_resolveTarget->valid()) {
          m_resolveTarget.reset();
          return D3DERR_INVALIDCALL;
        }
      }
      legacy::Texture* resolved = m_resolveTarget.get();
      context->ResolveSubresource(resolved->resource(), 0, s->resource(), src->subresource(), s->dxgiFormat());
      const legacy::Rect a = toRect(sr), b = toRect(dr);
      m_core->blit(d, dst->subresource(), &b, resolved, 0, &a, filter != D3DTEXF_POINT, false, 0);
      return D3D_OK;
    }
    if (sameSize && s->dxgiFormat() == d->dxgiFormat() && s->desc().samples == d->desc().samples && s->d3dResource()
     && d->d3dResource() && !s->desc().systemMemory && !d->desc().systemMemory) {
      D3D11_BOX box = { UINT(sr.left), UINT(sr.top), 0, UINT(sr.right), UINT(sr.bottom), 1 };
      context->CopySubresourceRegion(d->d3dResource(), dst->subresource(), UINT(dr.left), UINT(dr.top), 0, s->d3dResource(),
                                     src->subresource(), &box);
      d->markGpuWritten();
      return D3D_OK;
    }
    // Video surfaces stretch filtered whatever the filter (Windows' Direct3D
    // 9 converts them through the video processor).
    const bool video = src->format() == D3DFMT_YUY2 || src->format() == D3DFMT_UYVY;
    const legacy::Rect a = toRect(sr), b = toRect(dr);
    m_core->blit(d, dst->subresource(), &b, s, src->subresource(), &a, filter != D3DTEXF_POINT || video, false, 0);
    return D3D_OK;
  }

  HRESULT Device::ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    Surface* s = asSurface(surface);
    legacy::Rect r = {};
    if (rect)
      r = toRect(*rect);
    m_core->fillArgb(s->texture(), s->subresource(), rect ? &r : nullptr, color);
    return D3D_OK;
  }

  // ---- Targets ----

  HRESULT Device::SetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
    if (index >= legacy::kMaxRenderTargets || (index == 0 && !surface))
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    Surface* s = asSurface(surface);
    if (s && !(s->usage() & D3DUSAGE_RENDERTARGET))
      return D3DERR_INVALIDCALL;
    m_renderTargets[index].set(s);
    m_core->setRenderTarget(index, s ? s->texture() : nullptr, s ? s->subresource() : 0);
    // Direct3D 9 sets viewport and scissor to the first target, also when it
    // is bound already (UE3 games set their scene target again after a
    // smaller viewport: The Vanishing of Ethan Carter drew its world into a
    // 512x512 corner). Direct3D 7 keeps its viewport.
    if (index == 0)
      m_core->resetViewport();
    return D3D_OK;
  }

  HRESULT Device::GetRenderTarget(DWORD index, IDirect3DSurface9** surface) {
    if (!surface || index >= legacy::kMaxRenderTargets)
      return D3DERR_INVALIDCALL;
    *surface = m_renderTargets[index].get();
    if (!*surface)
      return D3DERR_NOTFOUND;
    (*surface)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::SetDepthStencilSurface(IDirect3DSurface9* surface) {
    DeviceLock lock(this);
    Surface* s = asSurface(surface);
    m_depthStencil.set(s);
    m_core->setDepthStencil(s ? s->texture() : nullptr);
    return D3D_OK;
  }

  HRESULT Device::GetDepthStencilSurface(IDirect3DSurface9** surface) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    *surface = m_depthStencil.get();
    if (!*surface)
      return D3DERR_NOTFOUND;
    (*surface)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::BeginScene() {
    if (m_inScene)
      return D3DERR_INVALIDCALL;
    m_inScene = true;
    return D3D_OK;
  }

  HRESULT Device::EndScene() {
    if (!m_inScene)
      return D3DERR_INVALIDCALL;
    m_inScene = false;
    return D3D_OK;
  }

  HRESULT Device::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    // Rectangles without a count clear nothing (as Windows' D3D9, DXVK).
    if (!count && rects)
      return D3D_OK;
    DeviceLock lock(this);
    if (count && !rects)
      count = 0;
    std::vector<legacy::Rect> list(count);
    for (DWORD i = 0; i < count; i++)
      list[i] = { rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2 };
    m_core->clear(count, list.data(), flags, color, z, stencil);
    return D3D_OK;
  }

  // ---- Fixed-function state ----

  HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    if (!matrix)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->transforms[uint32_t(state)] = *matrix;
      return D3D_OK;
    }
    m_core->setTransform(uint32_t(state), &matrix->_11);
    return D3D_OK;
  }

  HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) {
    if (!matrix)
      return D3DERR_INVALIDCALL;
    m_core->getTransform(uint32_t(state), &matrix->_11);
    return D3D_OK;
  }

  HRESULT Device::MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    if (!matrix)
      return D3DERR_INVALIDCALL;
    // The given matrix times the current one (as Direct3D 7 and Wine).
    D3DMATRIX current, result;
    GetTransform(state, &current);
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        result.m[r][c] = matrix->m[r][0] * current.m[0][c] + matrix->m[r][1] * current.m[1][c]
                       + matrix->m[r][2] * current.m[2][c] + matrix->m[r][3] * current.m[3][c];
    return SetTransform(state, &result);
  }

  HRESULT Device::SetViewport(const D3DVIEWPORT9* viewport) {
    if (!viewport)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->viewport = *viewport;
      rec->viewportSet = true;
      return D3D_OK;
    }
    m_core->setViewport({ viewport->X, viewport->Y, viewport->Width, viewport->Height, viewport->MinZ, viewport->MaxZ });
    return D3D_OK;
  }

  HRESULT Device::GetViewport(D3DVIEWPORT9* viewport) {
    if (!viewport)
      return D3DERR_INVALIDCALL;
    const legacy::Viewport& v = m_core->viewport();
    *viewport = { v.x, v.y, v.width, v.height, v.minZ, v.maxZ };
    return D3D_OK;
  }

  HRESULT Device::SetMaterial(const D3DMATERIAL9* material) {
    if (!material)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->material = *material;
      rec->materialSet = true;
      return D3D_OK;
    }
    static_assert(sizeof(legacy::Material) == sizeof(D3DMATERIAL9), "material layouts");
    legacy::Material m;
    std::memcpy(&m, material, sizeof(m));
    m_core->setMaterial(m);
    return D3D_OK;
  }

  HRESULT Device::GetMaterial(D3DMATERIAL9* material) {
    if (!material)
      return D3DERR_INVALIDCALL;
    std::memcpy(material, &m_core->material(), sizeof(*material));
    return D3D_OK;
  }

  HRESULT Device::SetLight(DWORD index, const D3DLIGHT9* light) {
    if (!light)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->lights[index] = *light;
      return D3D_OK;
    }
    static_assert(sizeof(legacy::Light) == sizeof(D3DLIGHT9), "light layouts");
    legacy::Light l;
    std::memcpy(&l, light, sizeof(l));
    return m_core->setLight(index, l) ? D3D_OK : E_OUTOFMEMORY;
  }

  HRESULT Device::GetLight(DWORD index, D3DLIGHT9* light) {
    if (!light)
      return D3DERR_INVALIDCALL;
    legacy::Light l;
    if (!m_core->getLight(index, &l))
      return D3DERR_INVALIDCALL;
    std::memcpy(light, &l, sizeof(*light));
    return D3D_OK;
  }

  HRESULT Device::LightEnable(DWORD index, BOOL enable) {
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->lightEnables[index] = enable;
      return D3D_OK;
    }
    return m_core->lightEnable(index, enable != FALSE) ? D3D_OK : E_OUTOFMEMORY;
  }

  HRESULT Device::GetLightEnable(DWORD index, BOOL* enable) {
    if (!enable)
      return D3DERR_INVALIDCALL;
    legacy::Light l;
    if (!m_core->getLight(index, &l))
      return D3DERR_INVALIDCALL;
    *enable = m_core->lightEnabled(index) ? 128 : 0;
    return D3D_OK;
  }

  HRESULT Device::SetClipPlane(DWORD index, const float* plane) {
    if (!plane || index >= 6)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      std::memcpy(rec->clipPlanes[index].data(), plane, 16);
      rec->clipPlanesSet.set(index);
      return D3D_OK;
    }
    m_clipPlanes[index] = { plane[0], plane[1], plane[2], plane[3] };
    m_core->setClipPlane(index, plane);
    return D3D_OK;
  }

  HRESULT Device::GetClipPlane(DWORD index, float* plane) {
    if (!plane || index >= 6)
      return D3DERR_INVALIDCALL;
    std::memcpy(plane, m_clipPlanes[index].data(), 16);
    return D3D_OK;
  }

  HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    if (uint32_t(state) >= 256)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->renderStates[state] = value;
      rec->renderStatesSet.set(state);
      return D3D_OK;
    }
    m_core->setRenderState(uint32_t(state), value);
    return D3D_OK;
  }

  HRESULT Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) {
    if (!value)
      return D3DERR_INVALIDCALL;
    *value = m_core->renderState(uint32_t(state));
    return D3D_OK;
  }

  HRESULT Device::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) {
    const int index = samplerIndex(stage);
    if (index < 0)
      return stage == D3DDMAPSAMPLER ? D3D_OK : D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      if (texture) texture->AddRef();
      if (rec->textures[index]) rec->textures[index]->Release();
      rec->textures[index] = texture;
      rec->texturesSet.set(index);
      return D3D_OK;
    }
    if (m_textures[index] == texture)
      return D3D_OK;
    BaseTexture* next = baseTextureOf(texture);
    if (next) hold(&next->refCounted());
    m_textures[index] = texture;
    m_textureBases[index] = next;
    m_core->setTexture(uint32_t(index), next ? next->texture() : nullptr);
    return D3D_OK;
  }

  HRESULT Device::GetTexture(DWORD stage, IDirect3DBaseTexture9** texture) {
    if (!texture)
      return D3DERR_INVALIDCALL;
    const int index = samplerIndex(stage);
    *texture = index >= 0 ? m_textures[index] : nullptr;
    if (*texture)
      (*texture)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (stage >= 8 || uint32_t(type) >= 33)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->stageStates[stage][type] = value;
      rec->stageStatesSet[stage].set(type);
      return D3D_OK;
    }
    m_core->setTextureStageState(stage, uint32_t(type), value);
    return D3D_OK;
  }

  HRESULT Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) {
    if (!value || stage >= 8)
      return D3DERR_INVALIDCALL;
    *value = m_core->textureStageState(stage, uint32_t(type));
    return D3D_OK;
  }

  HRESULT Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    const int index = samplerIndex(sampler);
    if (uint32_t(type) >= 14)
      return D3DERR_INVALIDCALL;
    if (index < 0)
      return sampler == D3DDMAPSAMPLER ? D3D_OK : D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->samplerStates[index][type] = value;
      rec->samplerStatesSet[index].set(type);
      return D3D_OK;
    }
    m_core->setSamplerState(uint32_t(index), uint32_t(type), value);
    return D3D_OK;
  }

  HRESULT Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) {
    if (!value)
      return D3DERR_INVALIDCALL;
    const int index = samplerIndex(sampler);
    *value = index >= 0 ? m_core->samplerState(uint32_t(index), uint32_t(type)) : 0;
    return D3D_OK;
  }

  HRESULT Device::ValidateDevice(DWORD* passes) {
    if (passes)
      *passes = 1;
    return D3D_OK;
  }

  HRESULT Device::SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) {
    if (!entries)
      return D3DERR_INVALIDCALL;
    std::memcpy(m_palettes[palette].data(), entries, sizeof(PALETTEENTRY) * 256);
    return D3D_OK;
  }

  HRESULT Device::GetPaletteEntries(UINT palette, PALETTEENTRY* entries) {
    if (!entries)
      return D3DERR_INVALIDCALL;
    auto it = m_palettes.find(palette);
    if (it == m_palettes.end())
      return D3DERR_INVALIDCALL;
    std::memcpy(entries, it->second.data(), sizeof(PALETTEENTRY) * 256);
    return D3D_OK;
  }

  HRESULT Device::GetCurrentTexturePalette(UINT* palette) {
    if (!palette)
      return D3DERR_INVALIDCALL;
    *palette = m_currentPalette;
    return D3D_OK;
  }

  HRESULT Device::SetScissorRect(const RECT* rect) {
    if (!rect)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->scissor = *rect;
      rec->scissorSet = true;
      return D3D_OK;
    }
    m_core->setScissor(toRect(*rect));
    return D3D_OK;
  }

  HRESULT Device::GetScissorRect(RECT* rect) {
    if (!rect)
      return D3DERR_INVALIDCALL;
    const legacy::Rect& r = m_core->scissor();
    *rect = { r.left, r.top, r.right, r.bottom };
    return D3D_OK;
  }

  HRESULT Device::SetClipStatus(const D3DCLIPSTATUS9* status) {
    if (!status)
      return D3DERR_INVALIDCALL;
    m_clipStatus = *status;
    return D3D_OK;
  }

  HRESULT Device::GetClipStatus(D3DCLIPSTATUS9* status) {
    if (!status)
      return D3DERR_INVALIDCALL;
    *status = m_clipStatus;
    return D3D_OK;
  }

  // ---- State blocks ----

  HRESULT Device::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** block) {
    if (!block || (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE))
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    auto* object = new StateBlock(this, type);
    object->AddRef();
    *block = object;
    return D3D_OK;
  }

  HRESULT Device::BeginStateBlock() {
    DeviceLock lock(this);
    if (m_recording)
      return D3DERR_INVALIDCALL;
    m_recording = new StateBlock(this, D3DSTATEBLOCKTYPE(0));
    m_recording->AddRef();
    return D3D_OK;
  }

  HRESULT Device::EndStateBlock(IDirect3DStateBlock9** block) {
    DeviceLock lock(this);
    if (!block || !m_recording)
      return D3DERR_INVALIDCALL;
    *block = m_recording;
    m_recording = nullptr;
    return D3D_OK;
  }

  void Device::captureState(StateSet& s) {
    for (uint32_t state = 0; state < 256; state++)
      if (s.renderStatesSet.test(state))
        s.renderStates[state] = m_core->renderState(state);
    for (uint32_t stage = 0; stage < 8; stage++)
      for (uint32_t type = 0; type < 33; type++)
        if (s.stageStatesSet[stage].test(type))
          s.stageStates[stage][type] = m_core->textureStageState(stage, type);
    for (uint32_t sampler = 0; sampler < legacy::kSamplerCount; sampler++)
      for (uint32_t type = 0; type < 14; type++)
        if (s.samplerStatesSet[sampler].test(type))
          s.samplerStates[sampler][type] = m_core->samplerState(sampler, type);
    for (uint32_t i = 0; i < legacy::kSamplerCount; i++)
      if (s.texturesSet.test(i))
        s.textures[i] = m_textures[i];
    for (uint32_t i = 0; i < 16; i++) {
      if (s.streamsSet.test(i)) {
        s.streams[i].buffer = m_streams[i].buffer.get();
        s.streams[i].offset = m_streams[i].offset;
        s.streams[i].stride = m_streams[i].stride;
      }
      if (s.frequenciesSet.test(i))
        s.streams[i].frequency = m_streams[i].frequency;
    }
    if (s.indicesSet)
      s.indices = m_indices.get();
    if (s.declarationSet) {
      s.declaration = m_declaration.get();
      s.fvf = m_fvf;
    }
    if (s.vertexShaderSet)
      s.vertexShader = m_vertexShader.get();
    if (s.pixelShaderSet)
      s.pixelShader = m_pixelShader.get();
    for (uint32_t i = 0; i < 256; i++) {
      if (s.vsFloatsSet.test(i)) m_core->getShaderConstantF(false, i, s.vsFloats[i].data(), 1);
      if (s.psFloatsSet.test(i)) m_core->getShaderConstantF(true, i, s.psFloats[i].data(), 1);
    }
    for (uint32_t i = 0; i < 16; i++) {
      if (s.vsIntsSet.test(i)) m_core->getShaderConstantI(false, i, s.vsInts[i].data(), 1);
      if (s.psIntsSet.test(i)) m_core->getShaderConstantI(true, i, s.psInts[i].data(), 1);
      uint32_t value = 0;
      if (s.vsBoolsSet.test(i)) { m_core->getShaderConstantB(false, i, &value, 1); s.vsBools[i] = value; }
      if (s.psBoolsSet.test(i)) { m_core->getShaderConstantB(true, i, &value, 1); s.psBools[i] = value; }
    }
    for (auto& entry : s.transforms)
      GetTransform(D3DTRANSFORMSTATETYPE(entry.first), &entry.second);
    if (s.materialSet)
      GetMaterial(&s.material);
    for (auto& entry : s.lights) {
      if (FAILED(GetLight(entry.first, &entry.second)))
        entry.second = {};
    }
    for (auto& entry : s.lightEnables)
      entry.second = m_core->lightEnabled(entry.first);
    if (s.viewportSet)
      GetViewport(&s.viewport);
    if (s.scissorSet)
      GetScissorRect(&s.scissor);
    for (uint32_t i = 0; i < 6; i++)
      if (s.clipPlanesSet.test(i))
        std::memcpy(s.clipPlanes[i].data(), m_clipPlanes[i].data(), 16);
  }

  void Device::applyState(const StateSet& s) {
    DeviceLock lock(this);
    StateBlock* recording = m_recording;
    m_recording = nullptr;
    for (uint32_t state = 0; state < 256; state++)
      if (s.renderStatesSet.test(state))
        m_core->setRenderState(state, s.renderStates[state]);
    for (uint32_t stage = 0; stage < 8; stage++)
      for (uint32_t type = 0; type < 33; type++)
        if (s.stageStatesSet[stage].test(type))
          m_core->setTextureStageState(stage, type, s.stageStates[stage][type]);
    for (uint32_t sampler = 0; sampler < legacy::kSamplerCount; sampler++)
      for (uint32_t type = 0; type < 14; type++)
        if (s.samplerStatesSet[sampler].test(type))
          m_core->setSamplerState(sampler, type, s.samplerStates[sampler][type]);
    for (uint32_t i = 0; i < legacy::kSamplerCount; i++)
      if (s.texturesSet.test(i))
        SetTexture(i < 16 ? i : D3DVERTEXTEXTURESAMPLER0 + (i - 16), s.textures[i]);
    for (uint32_t i = 0; i < 16; i++) {
      if (s.streamsSet.test(i))
        SetStreamSource(i, s.streams[i].buffer, s.streams[i].offset, s.streams[i].stride);
      if (s.frequenciesSet.test(i))
        SetStreamSourceFreq(i, s.streams[i].frequency);
    }
    if (s.indicesSet)
      SetIndices(s.indices);
    if (s.declarationSet) {
      if (s.declaration)
        SetVertexDeclaration(s.declaration);
      else if (s.fvf)
        SetFVF(s.fvf);
    }
    if (s.vertexShaderSet)
      SetVertexShader(s.vertexShader);
    if (s.pixelShaderSet)
      SetPixelShader(s.pixelShader);
    for (uint32_t i = 0; i < 256; i++) {
      if (s.vsFloatsSet.test(i)) m_core->setShaderConstantF(false, i, s.vsFloats[i].data(), 1);
      if (s.psFloatsSet.test(i)) m_core->setShaderConstantF(true, i, s.psFloats[i].data(), 1);
    }
    for (uint32_t i = 0; i < 16; i++) {
      if (s.vsIntsSet.test(i)) m_core->setShaderConstantI(false, i, s.vsInts[i].data(), 1);
      if (s.psIntsSet.test(i)) m_core->setShaderConstantI(true, i, s.psInts[i].data(), 1);
      const uint32_t vsBool = s.vsBools[i], psBool = s.psBools[i];
      if (s.vsBoolsSet.test(i)) m_core->setShaderConstantB(false, i, &vsBool, 1);
      if (s.psBoolsSet.test(i)) m_core->setShaderConstantB(true, i, &psBool, 1);
    }
    for (const auto& entry : s.transforms)
      SetTransform(D3DTRANSFORMSTATETYPE(entry.first), &entry.second);
    if (s.materialSet)
      SetMaterial(&s.material);
    for (const auto& entry : s.lights)
      SetLight(entry.first, &entry.second);
    for (const auto& entry : s.lightEnables)
      LightEnable(entry.first, entry.second);
    if (s.viewportSet)
      SetViewport(&s.viewport);
    if (s.scissorSet)
      SetScissorRect(&s.scissor);
    for (uint32_t i = 0; i < 6; i++)
      if (s.clipPlanesSet.test(i))
        SetClipPlane(i, s.clipPlanes[i].data());
    m_recording = recording;
  }

  // ---- Vertex input ----

  HRESULT Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** declaration) {
    if (!elements || !declaration)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    auto* object = new VertexDeclaration(this, elements);
    object->AddRef();
    *declaration = object;
    return D3D_OK;
  }

  HRESULT Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* declaration) {
    DeviceLock lock(this);
    auto* decl = static_cast<VertexDeclaration*>(declaration);
    if (StateSet* rec = recording()) {
      if (decl) decl->AddRef();
      if (rec->declaration) rec->declaration->Release();
      rec->declaration = decl;
      rec->fvf = 0;
      rec->declarationSet = true;
      return D3D_OK;
    }
    if (m_declaration.set(decl) && decl)
      hold(decl);
    m_fvf = decl ? decl->fvf() : 0;
    m_core->setVertexDeclaration(decl ? decl->decl() : nullptr);
    return D3D_OK;
  }

  HRESULT Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** declaration) {
    if (!declaration)
      return D3DERR_INVALIDCALL;
    *declaration = m_declaration.get();
    if (*declaration)
      (*declaration)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::SetFVF(DWORD fvf) {
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      if (rec->declaration) rec->declaration->Release();
      rec->declaration = nullptr;
      rec->fvf = fvf;
      rec->declarationSet = true;
      return D3D_OK;
    }
    if (!fvf) {
      m_fvf = 0;
      return D3D_OK;
    }
    VertexDeclaration*& decl = m_fvfDeclarations[fvf];
    if (!decl) {
      const auto elements = legacy::VertexDecl::elementsOf(fvf);
      std::vector<D3DVERTEXELEMENT9> d3dElements(elements.size() + 1);
      std::memcpy(d3dElements.data(), elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9));
      d3dElements.back() = D3DDECL_END();
      decl = new VertexDeclaration(this, d3dElements.data(), fvf, true);
      decl->addRefPrivate();
    }
    if (m_declaration.set(decl) && decl)
      hold(decl);
    m_fvf = fvf;
    m_core->setVertexDeclaration(decl->decl());
    return D3D_OK;
  }

  HRESULT Device::GetFVF(DWORD* fvf) {
    if (!fvf)
      return D3DERR_INVALIDCALL;
    *fvf = m_fvf;
    return D3D_OK;
  }

  HRESULT Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) {
    if (stream >= 16)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    auto* vb = static_cast<VertexBuffer*>(buffer);
    if (StateSet* rec = recording()) {
      if (vb) vb->AddRef();
      if (rec->streams[stream].buffer) rec->streams[stream].buffer->Release();
      rec->streams[stream].buffer = vb;
      rec->streams[stream].offset = offset;
      rec->streams[stream].stride = stride;
      rec->streamsSet.set(stream);
      return D3D_OK;
    }
    if (m_streams[stream].buffer.set(vb) && vb)
      hold(vb);
    m_streams[stream].offset = offset;
    m_streams[stream].stride = stride;
    m_core->setStreamSource(stream, vb ? vb->buffer() : nullptr, offset, stride);
    return D3D_OK;
  }

  HRESULT Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9** buffer, UINT* offset, UINT* stride) {
    if (stream >= 16 || !buffer || !offset || !stride)
      return D3DERR_INVALIDCALL;
    *buffer = m_streams[stream].buffer.get();
    if (*buffer)
      (*buffer)->AddRef();
    *offset = m_streams[stream].offset;
    *stride = m_streams[stream].stride;
    return D3D_OK;
  }

  HRESULT Device::SetStreamSourceFreq(UINT stream, UINT setting) {
    if (stream >= 16)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      rec->streams[stream].frequency = setting;
      rec->frequenciesSet.set(stream);
      return D3D_OK;
    }
    m_streams[stream].frequency = setting;
    m_core->setStreamFrequency(stream, setting);
    return D3D_OK;
  }

  HRESULT Device::GetStreamSourceFreq(UINT stream, UINT* setting) {
    if (stream >= 16 || !setting)
      return D3DERR_INVALIDCALL;
    *setting = m_streams[stream].frequency;
    return D3D_OK;
  }

  HRESULT Device::SetIndices(IDirect3DIndexBuffer9* indices) {
    DeviceLock lock(this);
    auto* ib = static_cast<IndexBuffer*>(indices);
    if (StateSet* rec = recording()) {
      if (ib) ib->AddRef();
      if (rec->indices) rec->indices->Release();
      rec->indices = ib;
      rec->indicesSet = true;
      return D3D_OK;
    }
    if (m_indices.set(ib) && ib)
      hold(ib);
    m_core->setIndices(ib ? ib->buffer() : nullptr, ib && ib->index32());
    return D3D_OK;
  }

  HRESULT Device::GetIndices(IDirect3DIndexBuffer9** indices) {
    if (!indices)
      return D3DERR_INVALIDCALL;
    *indices = m_indices.get();
    if (*indices)
      (*indices)->AddRef();
    return D3D_OK;
  }

  // ---- Shaders ----

  HRESULT Device::CreateVertexShader(const DWORD* function, IDirect3DVertexShader9** shader) {
    if (!function || !shader)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if ((function[0] >> 16) != 0xfffe)
      return D3DERR_INVALIDCALL;
    std::string error;
    auto core = legacy::Shader::create(m_core.get(), reinterpret_cast<const uint32_t*>(function), 1u << 20, error);
    if (!core) {
      logMessage(LogLevel::Warn, "D3D9 vertex shader: %s", error.c_str());
      return D3DERR_INVALIDCALL;
    }
    auto* object = new VertexShader(this, std::move(core));
    object->AddRef();
    *shader = object;
    return D3D_OK;
  }

  HRESULT Device::CreatePixelShader(const DWORD* function, IDirect3DPixelShader9** shader) {
    if (!function || !shader)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if ((function[0] >> 16) != 0xffff)
      return D3DERR_INVALIDCALL;
    std::string error;
    auto core = legacy::Shader::create(m_core.get(), reinterpret_cast<const uint32_t*>(function), 1u << 20, error);
    if (!core) {
      logMessage(LogLevel::Warn, "D3D9 pixel shader: %s", error.c_str());
      return D3DERR_INVALIDCALL;
    }
    auto* object = new PixelShader(this, std::move(core));
    object->AddRef();
    *shader = object;
    return D3D_OK;
  }

  HRESULT Device::SetVertexShader(IDirect3DVertexShader9* shader) {
    DeviceLock lock(this);
    auto* vs = static_cast<VertexShader*>(shader);
    if (StateSet* rec = recording()) {
      if (vs) vs->AddRef();
      if (rec->vertexShader) rec->vertexShader->Release();
      rec->vertexShader = vs;
      rec->vertexShaderSet = true;
      return D3D_OK;
    }
    if (m_vertexShader.set(vs) && vs)
      hold(vs);
    m_core->setVertexShader(vs ? vs->shader() : nullptr);
    return D3D_OK;
  }

  HRESULT Device::GetVertexShader(IDirect3DVertexShader9** shader) {
    if (!shader)
      return D3DERR_INVALIDCALL;
    *shader = m_vertexShader.get();
    if (*shader)
      (*shader)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::SetPixelShader(IDirect3DPixelShader9* shader) {
    DeviceLock lock(this);
    auto* ps = static_cast<PixelShader*>(shader);
    if (StateSet* rec = recording()) {
      if (ps) ps->AddRef();
      if (rec->pixelShader) rec->pixelShader->Release();
      rec->pixelShader = ps;
      rec->pixelShaderSet = true;
      return D3D_OK;
    }
    if (m_pixelShader.set(ps) && ps)
      hold(ps);
    m_core->setPixelShader(ps ? ps->shader() : nullptr);
    return D3D_OK;
  }

  HRESULT Device::GetPixelShader(IDirect3DPixelShader9** shader) {
    if (!shader)
      return D3DERR_INVALIDCALL;
    *shader = m_pixelShader.get();
    if (*shader)
      (*shader)->AddRef();
    return D3D_OK;
  }

  namespace {
    template<typename T, size_t N>
    void recordConstants(std::array<std::array<T, 4>, N>& values, std::bitset<N>& set, UINT start, const T* data, UINT count) {
      for (UINT i = 0; i < count && start + i < N; i++) {
        std::memcpy(values[start + i].data(), data + size_t(i) * 4, sizeof(T) * 4);
        set.set(start + i);
      }
    }
  }

  HRESULT Device::SetVertexShaderConstantF(UINT start, const float* data, UINT count) {
    if (!data || start + count > 256)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      recordConstants(rec->vsFloats, rec->vsFloatsSet, start, data, count);
      return D3D_OK;
    }
    m_core->setShaderConstantF(false, start, data, count);
    return D3D_OK;
  }

  HRESULT Device::GetVertexShaderConstantF(UINT start, float* data, UINT count) {
    if (!data || start + count > 256)
      return D3DERR_INVALIDCALL;
    m_core->getShaderConstantF(false, start, data, count);
    return D3D_OK;
  }

  HRESULT Device::SetVertexShaderConstantI(UINT start, const int* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      recordConstants(rec->vsInts, rec->vsIntsSet, start, data, count);
      return D3D_OK;
    }
    m_core->setShaderConstantI(false, start, reinterpret_cast<const int32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::GetVertexShaderConstantI(UINT start, int* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    m_core->getShaderConstantI(false, start, reinterpret_cast<int32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::SetVertexShaderConstantB(UINT start, const BOOL* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      for (UINT i = 0; i < count; i++) {
        rec->vsBools[start + i] = data[i];
        rec->vsBoolsSet.set(start + i);
      }
      return D3D_OK;
    }
    m_core->setShaderConstantB(false, start, reinterpret_cast<const uint32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::GetVertexShaderConstantB(UINT start, BOOL* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    m_core->getShaderConstantB(false, start, reinterpret_cast<uint32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::SetPixelShaderConstantF(UINT start, const float* data, UINT count) {
    if (!data || start + count > 224)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      recordConstants(rec->psFloats, rec->psFloatsSet, start, data, count);
      return D3D_OK;
    }
    m_core->setShaderConstantF(true, start, data, count);
    return D3D_OK;
  }

  HRESULT Device::GetPixelShaderConstantF(UINT start, float* data, UINT count) {
    if (!data || start + count > 224)
      return D3DERR_INVALIDCALL;
    m_core->getShaderConstantF(true, start, data, count);
    return D3D_OK;
  }

  HRESULT Device::SetPixelShaderConstantI(UINT start, const int* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      recordConstants(rec->psInts, rec->psIntsSet, start, data, count);
      return D3D_OK;
    }
    m_core->setShaderConstantI(true, start, reinterpret_cast<const int32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::GetPixelShaderConstantI(UINT start, int* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    m_core->getShaderConstantI(true, start, reinterpret_cast<int32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::SetPixelShaderConstantB(UINT start, const BOOL* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      for (UINT i = 0; i < count; i++) {
        rec->psBools[start + i] = data[i];
        rec->psBoolsSet.set(start + i);
      }
      return D3D_OK;
    }
    m_core->setShaderConstantB(true, start, reinterpret_cast<const uint32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::GetPixelShaderConstantB(UINT start, BOOL* data, UINT count) {
    if (!data || start + count > 16)
      return D3DERR_INVALIDCALL;
    m_core->getShaderConstantB(true, start, reinterpret_cast<uint32_t*>(data), count);
    return D3D_OK;
  }

  // ---- Drawing ----

  HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) {
    DeviceLock lock(this);
    const UINT vertices = vertexCountOf(type, primitiveCount);
    if (!vertices)
      return D3D_OK;
    m_core->drawPrimitive(primitive(type), startVertex, vertices);
    return D3D_OK;
  }

  HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices,
                                       UINT startIndex, UINT primitiveCount) {
    DeviceLock lock(this);
    const UINT indices = vertexCountOf(type, primitiveCount);
    if (!indices)
      return D3D_OK;
    m_core->drawIndexedPrimitive(primitive(type), baseVertex, minIndex, numVertices, startIndex, indices);
    return D3D_OK;
  }

  HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) {
    if (!data || !stride)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    const UINT vertices = vertexCountOf(type, primitiveCount);
    if (vertices)
      m_core->drawPrimitiveUP(primitive(type), vertices, data, stride);
    // As Direct3D 9: stream 0 is unset afterwards.
    m_streams[0].buffer.reset();
    m_streams[0].offset = m_streams[0].stride = 0;
    m_core->setStreamSource(0, nullptr, 0, 0);
    return D3D_OK;
  }

  HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primitiveCount,
                                         const void* indices, D3DFORMAT indexFormat, const void* vertices, UINT stride) {
    if (!indices || !vertices || !stride)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    const UINT count = vertexCountOf(type, primitiveCount);
    if (count)
      m_core->drawIndexedPrimitiveUP(primitive(type), minIndex, numVertices, count, indices, indexFormat == D3DFMT_INDEX32,
                                     vertices, stride);
    // As Direct3D 9: stream 0 and the indices are unset afterwards.
    m_streams[0].buffer.reset();
    m_streams[0].offset = m_streams[0].stride = 0;
    m_core->setStreamSource(0, nullptr, 0, 0);
    m_indices.reset();
    m_core->setIndices(nullptr, false);
    return D3D_OK;
  }

  // Vertices of stream 0 through the fixed-function transform (and
  // lighting) into the destination buffer's FVF, as Direct3D 7's.
  HRESULT Device::ProcessVertices(UINT srcStart, UINT destIndex, UINT count, IDirect3DVertexBuffer9* destination,
                                  IDirect3DVertexDeclaration9*, DWORD flags) {
    if (!destination)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    auto* dst = static_cast<VertexBuffer*>(destination);
    D3DVERTEXBUFFER_DESC desc = {};
    dst->GetDesc(&desc);
    const Stream& stream = m_streams[0];
    if (!m_fvf || m_vertexShader || !stream.buffer || !desc.FVF
     || (desc.FVF & legacy::FvfPositionMask) != legacy::FvfXyzRhw) {
      logUnsupported("ProcessVertices without FVFs, or with a vertex shader");
      return D3DERR_INVALIDCALL;
    }
    legacy::Buffer* from = stream.buffer->buffer();
    legacy::Buffer* to = dst->buffer();
    const uint32_t srcStride = stream.stride, dstStride = legacy::fvfStride(desc.FVF);
    if (!from || !to || !srcStride
     || stream.offset + (uint64_t(srcStart) + count) * srcStride > from->size()
     || (uint64_t(destIndex) + count) * dstStride > to->size())
      return D3DERR_INVALIDCALL;
    m_core->processVertices(from->data() + stream.offset + size_t(srcStart) * srcStride, m_fvf, srcStride,
      to->data() + size_t(destIndex) * dstStride, desc.FVF, dstStride, count, m_core->renderState(D3DRS_LIGHTING) != 0,
      !(flags & D3DPV_DONOTCOPYDATA));
    to->invalidate(destIndex * dstStride, count * dstStride);
    return D3D_OK;
  }

  HRESULT Device::DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) {
    logUnsupported("DrawRectPatch");
    return D3DERR_INVALIDCALL;
  }

  HRESULT Device::DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) {
    logUnsupported("DrawTriPatch");
    return D3DERR_INVALIDCALL;
  }

  HRESULT Device::DeletePatch(UINT) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9** query) {
    const HRESULT hr = [&]() -> HRESULT {
      DeviceLock lock(this);
      auto* object = new Query(this, type);
      if (!object->valid()) {
        object->AddRef();
        object->Release();
        return D3DERR_NOTAVAILABLE;
      }
      // A null pointer asks whether the type is supported.
      if (!query) {
        object->AddRef();
        object->Release();
        return D3D_OK;
      }
      object->AddRef();
      *query = object;
      return D3D_OK;
    }();
    if (FAILED(hr))
      logFailedCall(hr, "CreateQuery type %u", unsigned(type));
    return hr;
  }

  // ---- Direct3D 9Ex ----

  HRESULT Device::ComposeRects(IDirect3DSurface9*, IDirect3DSurface9*, IDirect3DVertexBuffer9*, UINT, IDirect3DVertexBuffer9*,
                               D3DCOMPOSERECTSOP, int, int) {
    logUnsupported("ComposeRects");
    return D3DERR_INVALIDCALL;
  }

  HRESULT Device::GetGPUThreadPriority(INT* priority) {
    if (!priority)
      return D3DERR_INVALIDCALL;
    *priority = m_gpuThreadPriority;
    return D3D_OK;
  }

  HRESULT Device::SetGPUThreadPriority(INT priority) {
    if (priority < -7 || priority > 7)
      return D3DERR_INVALIDCALL;
    m_gpuThreadPriority = priority;
    return D3D_OK;
  }

  HRESULT Device::WaitForVBlank(UINT) {
    return D3D_OK;
  }

  HRESULT Device::SetMaximumFrameLatency(UINT latency) {
    m_maximumFrameLatency = latency ? (std::min)(latency, 20u) : 3u;
    return D3D_OK;
  }

  HRESULT Device::GetMaximumFrameLatency(UINT* latency) {
    if (!latency)
      return D3DERR_INVALIDCALL;
    *latency = m_maximumFrameLatency;
    return D3D_OK;
  }

}
