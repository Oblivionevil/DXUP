// The Direct3D 9 device and swap chains (see d3d9_objects.h).

#include "d3d9_objects.h"

#include "../legacy/legacy_formats.h"
#include "../util/util_config.h"
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

    // body() under the lock, out of line: the calls games make thousands of
    // times a frame run body() directly on devices without a lock, with no
    // lock object, so 32-bit code registers no unwinding frame for them.
    template<typename Body>
    __declspec(noinline) HRESULT withLock(Device* device, Body& body) {
      DeviceLock lock(device);
      return body();
    }

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

    // Registers start .. start + count within limit (without the sum
    // wrapping around).
    bool inRange(UINT start, UINT count, UINT limit) {
      return start <= limit && count <= limit - start;
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
    createBuffers(params);
  }

  void SwapChain::reset(const D3DPRESENT_PARAMETERS& params, std::vector<Surface*>& retired) {
    for (Surface* surface : m_backBuffers) {
      if (surface->heldByApplication())
        retired.push_back(surface);
      else
        surface->destroyContained();
    }
    m_backBuffers.clear();
    m_resolved.reset();
    createBuffers(params);
  }

  void SwapChain::createBuffers(const D3DPRESENT_PARAMETERS& params) {
    m_params = params;
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
      auto texture = m_device->makeTexture(desc);
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
      m_resolved = m_device->makeTexture(desc);
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
    legacy::Texture* source = m_backBuffers[0]->texture();
    legacy::Texture* resolved = m_resolved && m_resolved->valid() ? m_resolved.get() : nullptr;
    const bool withRect = sourceRect != nullptr;
    const legacy::Rect rect = sourceRect ? toRect(*sourceRect) : legacy::Rect{};
    HWND target = window ? window : (m_params.hDeviceWindow ? m_params.hDeviceWindow : nullptr);
    UINT interval = syncInterval(m_params.PresentationInterval);
    if (flags & D3DPRESENT_FORCEIMMEDIATE)
      interval = 0;
    m_device->runData(&m_gamma, sizeof(m_gamma),
      [source, resolved, withRect, rect, target, interval](legacy::Device& core, const uint8_t* gamma) {
        legacy::Texture* shown = source;
        if (resolved) {
          core.context()->ResolveSubresource(resolved->resource(), 0, source->resource(), 0, source->dxgiFormat());
          shown = resolved;
        }
        const HRESULT hr = core.present(shown, withRect ? &rect : nullptr, target, interval,
                                        reinterpret_cast<const uint16_t*>(gamma));
        if (FAILED(hr))
          logMessage(LogLevel::Warn, "D3D9 Present failed: 0x%08lx", hr);
      });
    m_presentCount++;
    m_device->endFrame();
    // A removed device: Direct3D 9Ex games make a new one (Direct3D 9 has
    // no way back from it but Reset, which cannot bring it back).
    if (m_device->removed())
      return m_device->ex() ? D3DERR_DEVICEREMOVED : D3D_OK;
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
    // What is recorded is made first; from here on the core is called directly.
    m_recorder = nullptr;
    m_worker.reset();
    releaseHeld(true);
    for (auto& entry : m_fvfDeclarations)
      entry.second->releasePrivate();
    m_fvfDeclarations.clear();
    if (m_autoDepthStencil)
      delete m_autoDepthStencil;
    for (Surface* surface : m_retiredSurfaces)
      delete surface;
    delete m_swapChain;
    restoreWindow();
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
      m_textureObjects[i] = nullptr;
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
    // A full screen window gets its frame and place back with the last
    // reference of the application (the device may live on privately).
    restoreWindow();
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
    if (UserOverrides::get().d3d9WorkerThread) {
      m_worker = std::make_unique<Worker>(m_core.get());
      m_recorder = m_worker.get();
      logMessage(LogLevel::Info, "D3D9: calls on the core made by a worker thread (d3d9.workerThread)");
    }
    return D3D_OK;
  }

  std::shared_ptr<legacy::Texture> Device::makeTexture(const legacy::TextureDesc& desc) {
    return std::shared_ptr<legacy::Texture>(new legacy::Texture(m_core.get(), desc),
                                            [this](legacy::Texture* texture) { retire(texture); });
  }

  Device::Direct::Direct(Device* device, const char* why)
  : m_device(device), m_recorder(nullptr), m_locked(device->multithreaded()) {
    if (m_locked)
      device->m_mutex.lock();
    m_recorder = device->m_recorder;
    if (m_recorder) {
      m_recorder->synchronize();
      device->m_recorder = nullptr;
      device->noteDirect(why);
    }
  }

  Device::Direct::~Direct() {
    if (m_recorder)
      m_device->m_recorder = m_recorder;
    if (m_locked)
      m_device->m_mutex.unlock();
  }

  void Device::noteDirect(const char* why) {
    for (auto& entry : m_directWaits) {
      if (entry.first == why) {
        entry.second++;
        return;
      }
    }
    m_directWaits.emplace_back(why, 1u);
  }

  void Device::endFrame() {
    Worker* worker = m_recorder;
    if (!worker)
      return;
    worker->endFrame();
    // What made the application's thread wait for the worker (it gives up
    // the worker's gain): logged per frame now and then.
    if (++m_framesSinceWaits < 2000 || m_waitReports >= 4)
      return;
    m_waitReports++;
    std::sort(m_directWaits.begin(), m_directWaits.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (const auto& entry : m_directWaits) {
      char part[96];
      snprintf(part, sizeof(part), "%s%s %.2f", text.empty() ? "" : ", ", entry.first, double(entry.second) / m_framesSinceWaits);
      text += part;
    }
    logMessage(LogLevel::Info, "D3D9 worker: waits per frame: %s", text.empty() ? "none" : text.c_str());
    m_directWaits.clear();
    m_framesSinceWaits = 0;
  }

  void Device::applyFullscreen(const D3DPRESENT_PARAMETERS& params) {
    HWND window = params.hDeviceWindow ? params.hDeviceWindow : m_focusWindow;
    // Windowed (again), or another window: the one changed comes back.
    if (params.Windowed || window != m_savedWindow)
      restoreWindow();
    if (params.Windowed || !window)
      return;
    // No display mode change: the window covers its monitor, and the swap
    // chain scales the back buffer to it.
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    dpiAwareForFullScreen(monitor);
    MONITORINFO info = { sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info))
      return;
    // Kept once, before the first change.
    if (!m_savedWindow) {
      m_savedWindow = window;
      m_savedStyle = GetWindowLongW(window, GWL_STYLE);
      m_savedExStyle = GetWindowLongW(window, GWL_EXSTYLE);
    }
    LONG style = GetWindowLongW(window, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_POPUP;
    SetWindowLongW(window, GWL_STYLE, style);
    // Edges would take pixels of the monitor from the client area.
    const LONG exStyle = GetWindowLongW(window, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE);
    SetWindowLongW(window, GWL_EXSTYLE, exStyle);
    m_fullscreenStyle = style;
    m_fullscreenExStyle = exStyle;
    SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
  }

  // The style comes back as Windows' D3D9 brings it back (the window stays
  // where it is: games place it after Reset) - unless the game changed it
  // meanwhile (some do before a Reset to windowed). Visibility stays.
  void Device::restoreWindow() {
    HWND window = m_savedWindow;
    m_savedWindow = nullptr;
    if (!window || !IsWindow(window))
      return;
    const LONG style = GetWindowLongW(window, GWL_STYLE);
    if ((style & ~WS_VISIBLE) != (m_fullscreenStyle & ~WS_VISIBLE) || GetWindowLongW(window, GWL_EXSTYLE) != m_fullscreenExStyle)
      return;
    SetWindowLongW(window, GWL_STYLE, (m_savedStyle & ~WS_VISIBLE) | (style & WS_VISIBLE));
    SetWindowLongW(window, GWL_EXSTYLE, m_savedExStyle);
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    D3DPRESENT_PARAMETERS p = *params;
    if (!p.hDeviceWindow)
      p.hDeviceWindow = m_focusWindow;
    applyFullscreen(p);
    // The implicit swap chain stays the same object (as with Windows'
    // D3D9): its back buffers are made anew.
    if (m_swapChain) {
      m_swapChain->reset(p, m_retiredSurfaces);
      if (!m_swapChain->valid())
        return D3DERR_NOTAVAILABLE;
    } else {
      m_swapChain = new SwapChain(this, p, true);
      if (!m_swapChain->valid()) {
        delete m_swapChain;
        m_swapChain = nullptr;
        return D3DERR_NOTAVAILABLE;
      }
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
      auto texture = makeTexture(desc);
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
      m_textureObjects[i] = nullptr;
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
    m_lightSet.clear();
    m_core->resetState();
  }

  void Device::releaseHeld(bool all) {
    if (!all) {
      // Bound now: kept.
      for (RefCounted* texture : m_textureObjects)
        if (texture) texture->heldEpoch = m_heldEpoch;
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
    if (!swapChain)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (index != 0 || !m_swapChain)
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
    Direct direct(this, "Reset");
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
    DeviceLock lock(this);
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
      desc.dropCopies = sizeof(void*) == 4 && pool == D3DPOOL_MANAGED && !(usage & D3DUSAGE_DYNAMIC);
      auto shared_ = makeTexture(desc);
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
      auto shared_ = makeTexture(desc);
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
      desc.dropCopies = sizeof(void*) == 4 && pool == D3DPOOL_MANAGED && !(usage & D3DUSAGE_DYNAMIC);
      auto shared_ = makeTexture(desc);
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
      // Managed vertex buffers in a 32-bit process: the copy goes once on the GPU.
      auto core = std::make_unique<legacy::Buffer>(m_core.get(), length, false, (usage & D3DUSAGE_DYNAMIC) != 0,
                                                   sizeof(void*) == 4 && pool == D3DPOOL_MANAGED);
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
      auto texture = makeTexture(desc);
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
    Direct direct(this, "UpdateTexture");
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
    if (s->nullTarget() || d->nullTarget())
      return D3D_OK;
    // Depth: copied whole, as Direct3D 9 allows.
    if (legacy::formatInfo(legacy::Format(src->format())).depth
     && (sr.right - sr.left != dr.right - dr.left || sr.bottom - sr.top != dr.bottom - dr.top
      || sr.left || sr.top || UINT(sr.right) != src->width() || UINT(sr.bottom) != src->height()
      || dr.left || dr.top || UINT(dr.right) != dst->width() || UINT(dr.bottom) != dst->height()
      || src->format() != dst->format() || !s->d3dResource() || !d->d3dResource()))
      return D3DERR_INVALIDCALL;
    // Video surfaces stretch filtered whatever the filter (Windows' Direct3D
    // 9 converts them through the video processor).
    const bool linear = filter != D3DTEXF_POINT || src->format() == D3DFMT_YUY2 || src->format() == D3DFMT_UYVY;
    const uint32_t srcSub = src->subresource(), dstSub = dst->subresource();
    run([this, s, srcSub, d, dstSub, sr, dr, linear](legacy::Device& core) { stretch(core, s, srcSub, d, dstSub, sr, dr, linear); });
    return D3D_OK;
  }

  void Device::stretch(legacy::Device& core, legacy::Texture* s, uint32_t srcSub, legacy::Texture* d, uint32_t dstSub,
                       const RECT& sr, const RECT& dr, bool linear) {
    const bool sameSize = sr.right - sr.left == dr.right - dr.left && sr.bottom - sr.top == dr.bottom - dr.top;
    const bool wholeSource = sr.left == 0 && sr.top == 0 && UINT(sr.right) == s->width(srcSub) && UINT(sr.bottom) == s->height(srcSub);
    const bool wholeDest = dr.left == 0 && dr.top == 0 && UINT(dr.right) == d->width(dstSub) && UINT(dr.bottom) == d->height(dstSub);
    ID3D11DeviceContext* context = core.context();
    s->flush();
    d->flush();
    if (legacy::formatInfo(s->desc().format).depth) {
      if (s->desc().samples == d->desc().samples)
        context->CopySubresourceRegion(d->d3dResource(), dstSub, 0, 0, 0, s->d3dResource(), srcSub, nullptr);
      return;
    }
    if (s->desc().samples > 1) {
      // Multisampled: resolved, straight into the destination when it fits.
      if (sameSize && wholeSource && wholeDest && s->dxgiFormat() == d->dxgiFormat() && d->desc().samples == 1) {
        context->ResolveSubresource(d->resource(), dstSub, s->resource(), srcSub, s->dxgiFormat());
        d->markGpuWritten();
        return;
      }
      legacy::TextureDesc desc = s->desc();
      desc.samples = 1;
      const legacy::TextureDesc* kept = m_resolveTarget ? &m_resolveTarget->desc() : nullptr;
      if (!kept || kept->width != desc.width || kept->height != desc.height || kept->format != desc.format
       || kept->levels != desc.levels || kept->cube != desc.cube || kept->srgb != desc.srgb
       || kept->renderTarget != desc.renderTarget) {
        m_resolveTarget = makeTexture(desc);
        if (!m_resolveTarget->valid()) {
          m_resolveTarget.reset();
          logFailedCall(D3DERR_INVALIDCALL, "StretchRect: no resolve target %ux%u, format %s", desc.width, desc.height,
                        formatName(D3DFORMAT(desc.format)).c_str());
          return;
        }
      }
      legacy::Texture* resolved = m_resolveTarget.get();
      context->ResolveSubresource(resolved->resource(), 0, s->resource(), srcSub, s->dxgiFormat());
      const legacy::Rect a = toRect(sr), b = toRect(dr);
      core.blit(d, dstSub, &b, resolved, 0, &a, linear, false, 0);
      return;
    }
    if (sameSize && s->dxgiFormat() == d->dxgiFormat() && s->desc().samples == d->desc().samples && s->d3dResource()
     && d->d3dResource() && !s->desc().systemMemory && !d->desc().systemMemory) {
      D3D11_BOX box = { UINT(sr.left), UINT(sr.top), 0, UINT(sr.right), UINT(sr.bottom), 1 };
      context->CopySubresourceRegion(d->d3dResource(), dstSub, UINT(dr.left), UINT(dr.top), 0, s->d3dResource(), srcSub, &box);
      d->markGpuWritten();
      return;
    }
    const legacy::Rect a = toRect(sr), b = toRect(dr);
    core.blit(d, dstSub, &b, s, srcSub, &a, linear, false, 0);
  }

  HRESULT Device::ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    Surface* s = asSurface(surface);
    const legacy::Rect r = rect ? toRect(*rect) : legacy::Rect{};
    const bool whole = !rect;
    legacy::Texture* texture = s->texture();
    const uint32_t subresource = s->subresource();
    run([texture, subresource, r, whole, color](legacy::Device& core) { core.fillArgb(texture, subresource, whole ? nullptr : &r, color); });
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
    legacy::Texture* texture = s ? s->texture() : nullptr;
    const uint32_t subresource = s ? s->subresource() : 0;
    // Direct3D 9 sets viewport and scissor to the first target, also when it
    // is bound already (UE3 games set their scene target again after a
    // smaller viewport: The Vanishing of Ethan Carter drew its world into a
    // 512x512 corner). Direct3D 7 keeps its viewport.
    run([index, texture, subresource](legacy::Device& core) {
      core.setRenderTarget(index, texture, subresource);
      if (index == 0)
        core.resetViewport();
    });
    return D3D_OK;
  }

  // Getters of bindings lock too: the reference they give is taken while no
  // other thread unbinds (and releases) the object.
  HRESULT Device::GetRenderTarget(DWORD index, IDirect3DSurface9** surface) {
    if (!surface || index >= legacy::kMaxRenderTargets)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
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
    legacy::Texture* texture = s ? s->texture() : nullptr;
    run([texture](legacy::Device& core) { core.setDepthStencil(texture); });
    return D3D_OK;
  }

  HRESULT Device::GetDepthStencilSurface(IDirect3DSurface9** surface) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
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
    auto fill = [&](legacy::Rect* list) {
      for (DWORD i = 0; i < count; i++)
        list[i] = { rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2 };
    };
    if (Worker* worker = m_recorder) {
      worker->emitFilled(size_t(count) * sizeof(legacy::Rect), [&](uint8_t* to) { fill(reinterpret_cast<legacy::Rect*>(to)); },
        [count, flags, color, z, stencil](legacy::Device& core, const uint8_t* data) {
          core.clear(count, reinterpret_cast<const legacy::Rect*>(data), flags, color, z, stencil);
        });
      return D3D_OK;
    }
    std::vector<legacy::Rect> list(count);
    fill(list.data());
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
    runData(matrix, sizeof(D3DMATRIX), [state = uint32_t(state)](legacy::Device& core, const uint8_t* data) {
      core.setTransform(state, reinterpret_cast<const float*>(data));
    });
    return D3D_OK;
  }

  HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) {
    if (!matrix)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetTransform");
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
    const legacy::Viewport v = { viewport->X, viewport->Y, viewport->Width, viewport->Height, viewport->MinZ, viewport->MaxZ };
    run([v](legacy::Device& core) { core.setViewport(v); });
    return D3D_OK;
  }

  HRESULT Device::GetViewport(D3DVIEWPORT9* viewport) {
    if (!viewport)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetViewport");
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
    run([m](legacy::Device& core) { core.setMaterial(m); });
    return D3D_OK;
  }

  HRESULT Device::GetMaterial(D3DMATERIAL9* material) {
    if (!material)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetMaterial");
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
    if (index >= legacy::Device::kMaxLights)
      return E_OUTOFMEMORY;
    run([index, l](legacy::Device& core) { core.setLight(index, l); });
    markLight(index);
    return D3D_OK;
  }

  void Device::markLight(DWORD index) {
    if (index >= m_lightSet.size())
      m_lightSet.resize(index + 1, false);
    m_lightSet[index] = true;
  }

  // Lights never set nor enabled do not exist (the core keeps those below
  // the largest index): Windows' D3D9 fails for them.
  HRESULT Device::GetLight(DWORD index, D3DLIGHT9* light) {
    if (!light)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetLight");
    legacy::Light l;
    if (!lightExists(index) || !m_core->getLight(index, &l))
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
    if (index >= legacy::Device::kMaxLights)
      return E_OUTOFMEMORY;
    run([index, enable](legacy::Device& core) { core.lightEnable(index, enable != FALSE); });
    markLight(index);
    return D3D_OK;
  }

  HRESULT Device::GetLightEnable(DWORD index, BOOL* enable) {
    if (!enable)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetLightEnable");
    if (!lightExists(index))
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
    const std::array<float, 4> p = m_clipPlanes[index];
    run([index, p](legacy::Device& core) { core.setClipPlane(index, p.data()); });
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
    // The usual call (thousands a frame) written out: a lambda's captures
    // went through the stack even inlined.
    if (!multithreaded() && !m_recording) {
      if (Worker* worker = m_recorder)
        worker->emit([s = uint32_t(state), value](legacy::Device& core) { core.setRenderState(s, value); });
      else
        m_core->setRenderState(uint32_t(state), value);
      return D3D_OK;
    }
    auto body = [&]() -> HRESULT {
      if (StateSet* rec = recording()) {
        rec->renderStates[state] = value;
        rec->renderStatesSet.set(state);
        return D3D_OK;
      }
      run([s = uint32_t(state), value](legacy::Device& core) { core.setRenderState(s, value); });
      return D3D_OK;
    };
    return withLock(this, body);
  }

  HRESULT Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) {
    if (!value)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetRenderState");
    *value = m_core->renderState(uint32_t(state));
    return D3D_OK;
  }

  HRESULT Device::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) {
    const int index = samplerIndex(stage);
    if (index < 0)
      return stage == D3DDMAPSAMPLER ? D3D_OK : D3DERR_INVALIDCALL;
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      if (StateSet* rec = recording()) {
        setHeld(rec->textures[index], texture);
        rec->texturesSet.set(index);
        return D3D_OK;
      }
      if (m_textures[index] == texture)
        return D3D_OK;
      RefCounted* next = textureObjectOf(texture);
      if (next) hold(next);
      m_textures[index] = texture;
      m_textureObjects[index] = next;
      legacy::Texture* core = next ? static_cast<legacy::Texture*>(next->boundCore) : nullptr;
      if (Worker* worker = m_recorder)
        worker->emit([index, core](legacy::Device& c) { c.setTexture(uint32_t(index), core); });
      else
        m_core->setTexture(uint32_t(index), core);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetTexture(DWORD stage, IDirect3DBaseTexture9** texture) {
    if (!texture)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    const int index = samplerIndex(stage);
    *texture = index >= 0 ? m_textures[index] : nullptr;
    if (*texture)
      (*texture)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (stage >= 8 || uint32_t(type) >= 33)
      return D3DERR_INVALIDCALL;
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      if (StateSet* rec = recording()) {
        rec->stageStates[stage][type] = value;
        rec->stageStatesSet[stage].set(type);
        return D3D_OK;
      }
      if (Worker* worker = m_recorder)
        worker->emit([stage, t = uint32_t(type), value](legacy::Device& core) { core.setTextureStageState(stage, t, value); });
      else
        m_core->setTextureStageState(stage, uint32_t(type), value);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) {
    if (!value || stage >= 8)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetTextureStageState");
    *value = m_core->textureStageState(stage, uint32_t(type));
    return D3D_OK;
  }

  HRESULT Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    const int index = samplerIndex(sampler);
    if (uint32_t(type) >= 14)
      return D3DERR_INVALIDCALL;
    if (index < 0)
      return sampler == D3DDMAPSAMPLER ? D3D_OK : D3DERR_INVALIDCALL;
    if (!multithreaded() && !m_recording) {
      if (Worker* worker = m_recorder)
        worker->emit([i = uint32_t(index), t = uint32_t(type), value](legacy::Device& core) { core.setSamplerState(i, t, value); });
      else
        m_core->setSamplerState(uint32_t(index), uint32_t(type), value);
      return D3D_OK;
    }
    auto body = [&]() -> HRESULT {
      if (StateSet* rec = recording()) {
        rec->samplerStates[index][type] = value;
        rec->samplerStatesSet[index].set(type);
        return D3D_OK;
      }
      run([i = uint32_t(index), t = uint32_t(type), value](legacy::Device& core) { core.setSamplerState(i, t, value); });
      return D3D_OK;
    };
    return withLock(this, body);
  }

  HRESULT Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) {
    if (!value)
      return D3DERR_INVALIDCALL;
    const int index = samplerIndex(sampler);
    Direct direct(this, "GetSamplerState");
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
    const legacy::Rect r = toRect(*rect);
    run([r](legacy::Device& core) { core.setScissor(r); });
    return D3D_OK;
  }

  HRESULT Device::GetScissorRect(RECT* rect) {
    if (!rect)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetScissorRect");
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
    Direct direct(this, "state block Capture");
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
    // Lights the device does not have (recorded ones) keep their values.
    for (auto& entry : s.lights)
      GetLight(entry.first, &entry.second);
    for (auto& entry : s.lightEnables)
      if (lightExists(entry.first))
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
        run([state, value = s.renderStates[state]](legacy::Device& core) { core.setRenderState(state, value); });
    for (uint32_t stage = 0; stage < 8; stage++)
      for (uint32_t type = 0; type < 33; type++)
        if (s.stageStatesSet[stage].test(type))
          run([stage, type, value = s.stageStates[stage][type]](legacy::Device& core) { core.setTextureStageState(stage, type, value); });
    for (uint32_t sampler = 0; sampler < legacy::kSamplerCount; sampler++)
      for (uint32_t type = 0; type < 14; type++)
        if (s.samplerStatesSet[sampler].test(type))
          run([sampler, type, value = s.samplerStates[sampler][type]](legacy::Device& core) {
            core.setSamplerState(sampler, type, value);
          });
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
      else
        SetVertexDeclaration(nullptr);   // captured without one
    }
    if (s.vertexShaderSet)
      SetVertexShader(s.vertexShader);
    if (s.pixelShaderSet)
      SetPixelShader(s.pixelShader);
    for (uint32_t i = 0; i < 256; i++) {
      for (const bool pixel : { false, true }) {
        if ((pixel ? s.psFloatsSet : s.vsFloatsSet).test(i))
          run([pixel, i, v = (pixel ? s.psFloats : s.vsFloats)[i]](legacy::Device& core) { core.setShaderConstantF(pixel, i, v.data(), 1); });
      }
    }
    for (uint32_t i = 0; i < 16; i++) {
      for (const bool pixel : { false, true }) {
        if ((pixel ? s.psIntsSet : s.vsIntsSet).test(i))
          run([pixel, i, v = (pixel ? s.psInts : s.vsInts)[i]](legacy::Device& core) {
            core.setShaderConstantI(pixel, i, reinterpret_cast<const int32_t*>(v.data()), 1);
          });
        if ((pixel ? s.psBoolsSet : s.vsBoolsSet).test(i))
          run([pixel, i, v = uint32_t((pixel ? s.psBools : s.vsBools)[i])](legacy::Device& core) {
            core.setShaderConstantB(pixel, i, &v, 1);
          });
      }
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
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      auto* decl = static_cast<VertexDeclaration*>(declaration);
      if (StateSet* rec = recording()) {
        setHeld(rec->declaration, static_cast<IDirect3DVertexDeclaration9*>(decl));
        rec->fvf = 0;
        rec->declarationSet = true;
        return D3D_OK;
      }
      if (m_declaration.set(decl) && decl)
        hold(decl);
      m_fvf = decl ? decl->fvf() : 0;
      legacy::VertexDecl* core = decl ? decl->decl() : nullptr;
      run([core](legacy::Device& c) { c.setVertexDeclaration(core); });
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** declaration) {
    if (!declaration)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    *declaration = m_declaration.get();
    if (*declaration)
      (*declaration)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::SetFVF(DWORD fvf) {
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      if (!fvf)
        return D3D_OK;   // not an FVF: nothing to record
      setHeld(rec->declaration, static_cast<IDirect3DVertexDeclaration9*>(nullptr));
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
    legacy::VertexDecl* core = decl->decl();
    run([core](legacy::Device& c) { c.setVertexDeclaration(core); });
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
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      auto* vb = static_cast<VertexBuffer*>(buffer);
      if (StateSet* rec = recording()) {
        setHeld(rec->streams[stream].buffer, static_cast<IDirect3DVertexBuffer9*>(vb));
        rec->streams[stream].offset = offset;
        rec->streams[stream].stride = stride;
        rec->streamsSet.set(stream);
        return D3D_OK;
      }
      if (m_streams[stream].buffer.set(vb) && vb)
        hold(vb);
      m_streams[stream].offset = offset;
      m_streams[stream].stride = stride;
      legacy::Buffer* core = vb ? vb->buffer() : nullptr;
      if (Worker* worker = m_recorder)
        worker->emit([stream, core, offset, stride](legacy::Device& c) { c.setStreamSource(stream, core, offset, stride); });
      else
        m_core->setStreamSource(stream, core, offset, stride);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9** buffer, UINT* offset, UINT* stride) {
    if (stream >= 16 || !buffer || !offset || !stride)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
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
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      if (StateSet* rec = recording()) {
        rec->streams[stream].frequency = setting;
        rec->frequenciesSet.set(stream);
        return D3D_OK;
      }
      m_streams[stream].frequency = setting;
      run([stream, setting](legacy::Device& core) { core.setStreamFrequency(stream, setting); });
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetStreamSourceFreq(UINT stream, UINT* setting) {
    if (stream >= 16 || !setting)
      return D3DERR_INVALIDCALL;
    *setting = m_streams[stream].frequency;
    return D3D_OK;
  }

  HRESULT Device::SetIndices(IDirect3DIndexBuffer9* indices) {
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      auto* ib = static_cast<IndexBuffer*>(indices);
      if (StateSet* rec = recording()) {
        setHeld(rec->indices, static_cast<IDirect3DIndexBuffer9*>(ib));
        rec->indicesSet = true;
        return D3D_OK;
      }
      if (m_indices.set(ib) && ib)
        hold(ib);
      legacy::Buffer* core = ib ? ib->buffer() : nullptr;
      const bool index32 = ib && ib->index32();
      if (Worker* worker = m_recorder)
        worker->emit([core, index32](legacy::Device& c) { c.setIndices(core, index32); });
      else
        m_core->setIndices(core, index32);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetIndices(IDirect3DIndexBuffer9** indices) {
    if (!indices)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
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
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      auto* vs = static_cast<VertexShader*>(shader);
      if (StateSet* rec = recording()) {
        setHeld(rec->vertexShader, static_cast<IDirect3DVertexShader9*>(vs));
        rec->vertexShaderSet = true;
        return D3D_OK;
      }
      if (m_vertexShader.set(vs) && vs)
        hold(vs);
      legacy::Shader* core = vs ? vs->shader() : nullptr;
      run([core](legacy::Device& c) { c.setVertexShader(core); });
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetVertexShader(IDirect3DVertexShader9** shader) {
    if (!shader)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    *shader = m_vertexShader.get();
    if (*shader)
      (*shader)->AddRef();
    return D3D_OK;
  }

  HRESULT Device::SetPixelShader(IDirect3DPixelShader9* shader) {
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      auto* ps = static_cast<PixelShader*>(shader);
      if (StateSet* rec = recording()) {
        setHeld(rec->pixelShader, static_cast<IDirect3DPixelShader9*>(ps));
        rec->pixelShaderSet = true;
        return D3D_OK;
      }
      if (m_pixelShader.set(ps) && ps)
        hold(ps);
      legacy::Shader* core = ps ? ps->shader() : nullptr;
      run([core](legacy::Device& c) { c.setPixelShader(core); });
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::GetPixelShader(IDirect3DPixelShader9** shader) {
    if (!shader)
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
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
    if (!data || !inRange(start, count, 256))
      return D3DERR_INVALIDCALL;
    if (!multithreaded() && !m_recording) {
      if (Worker* worker = m_recorder)
        worker->emitData(data, size_t(count) * 16, [start, count](legacy::Device& core, const uint8_t* values) {
          core.setShaderConstantF(false, start, reinterpret_cast<const float*>(values), count);
        });
      else
        m_core->setShaderConstantF(false, start, data, count);
      return D3D_OK;
    }
    auto body = [&]() -> HRESULT {
      if (StateSet* rec = recording()) {
        recordConstants(rec->vsFloats, rec->vsFloatsSet, start, data, count);
        return D3D_OK;
      }
      runData(data, size_t(count) * 16, [start, count](legacy::Device& core, const uint8_t* values) {
        core.setShaderConstantF(false, start, reinterpret_cast<const float*>(values), count);
      });
      return D3D_OK;
    };
    return withLock(this, body);
  }

  HRESULT Device::GetVertexShaderConstantF(UINT start, float* data, UINT count) {
    if (!data || !inRange(start, count, 256))
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetShaderConstantF");
    m_core->getShaderConstantF(false, start, data, count);
    return D3D_OK;
  }

  HRESULT Device::SetVertexShaderConstantI(UINT start, const int* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      recordConstants(rec->vsInts, rec->vsIntsSet, start, data, count);
      return D3D_OK;
    }
    runData(data, size_t(count) * 16, [start, count](legacy::Device& core, const uint8_t* values) {
      core.setShaderConstantI(false, start, reinterpret_cast<const int32_t*>(values), count);
    });
    return D3D_OK;
  }

  HRESULT Device::GetVertexShaderConstantI(UINT start, int* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetShaderConstantI");
    m_core->getShaderConstantI(false, start, reinterpret_cast<int32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::SetVertexShaderConstantB(UINT start, const BOOL* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      for (UINT i = 0; i < count; i++) {
        rec->vsBools[start + i] = data[i];
        rec->vsBoolsSet.set(start + i);
      }
      return D3D_OK;
    }
    runData(data, size_t(count) * 4, [start, count](legacy::Device& core, const uint8_t* values) {
      core.setShaderConstantB(false, start, reinterpret_cast<const uint32_t*>(values), count);
    });
    return D3D_OK;
  }

  HRESULT Device::GetVertexShaderConstantB(UINT start, BOOL* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetShaderConstantB");
    m_core->getShaderConstantB(false, start, reinterpret_cast<uint32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::SetPixelShaderConstantF(UINT start, const float* data, UINT count) {
    if (!data || !inRange(start, count, 224))
      return D3DERR_INVALIDCALL;
    if (!multithreaded() && !m_recording) {
      if (Worker* worker = m_recorder)
        worker->emitData(data, size_t(count) * 16, [start, count](legacy::Device& core, const uint8_t* values) {
          core.setShaderConstantF(true, start, reinterpret_cast<const float*>(values), count);
        });
      else
        m_core->setShaderConstantF(true, start, data, count);
      return D3D_OK;
    }
    auto body = [&]() -> HRESULT {
      if (StateSet* rec = recording()) {
        recordConstants(rec->psFloats, rec->psFloatsSet, start, data, count);
        return D3D_OK;
      }
      runData(data, size_t(count) * 16, [start, count](legacy::Device& core, const uint8_t* values) {
        core.setShaderConstantF(true, start, reinterpret_cast<const float*>(values), count);
      });
      return D3D_OK;
    };
    return withLock(this, body);
  }

  HRESULT Device::GetPixelShaderConstantF(UINT start, float* data, UINT count) {
    if (!data || !inRange(start, count, 224))
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetShaderConstantF");
    m_core->getShaderConstantF(true, start, data, count);
    return D3D_OK;
  }

  HRESULT Device::SetPixelShaderConstantI(UINT start, const int* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      recordConstants(rec->psInts, rec->psIntsSet, start, data, count);
      return D3D_OK;
    }
    runData(data, size_t(count) * 16, [start, count](legacy::Device& core, const uint8_t* values) {
      core.setShaderConstantI(true, start, reinterpret_cast<const int32_t*>(values), count);
    });
    return D3D_OK;
  }

  HRESULT Device::GetPixelShaderConstantI(UINT start, int* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetShaderConstantI");
    m_core->getShaderConstantI(true, start, reinterpret_cast<int32_t*>(data), count);
    return D3D_OK;
  }

  HRESULT Device::SetPixelShaderConstantB(UINT start, const BOOL* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    DeviceLock lock(this);
    if (StateSet* rec = recording()) {
      for (UINT i = 0; i < count; i++) {
        rec->psBools[start + i] = data[i];
        rec->psBoolsSet.set(start + i);
      }
      return D3D_OK;
    }
    runData(data, size_t(count) * 4, [start, count](legacy::Device& core, const uint8_t* values) {
      core.setShaderConstantB(true, start, reinterpret_cast<const uint32_t*>(values), count);
    });
    return D3D_OK;
  }

  HRESULT Device::GetPixelShaderConstantB(UINT start, BOOL* data, UINT count) {
    if (!data || !inRange(start, count, 16))
      return D3DERR_INVALIDCALL;
    Direct direct(this, "GetShaderConstantB");
    m_core->getShaderConstantB(true, start, reinterpret_cast<uint32_t*>(data), count);
    return D3D_OK;
  }

  // ---- Drawing ----

  HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) {
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      const UINT vertices = vertexCountOf(type, primitiveCount);
      if (!vertices)
        return D3D_OK;
      if (Worker* worker = m_recorder)
        worker->emit([t = primitive(type), startVertex, vertices](legacy::Device& core) { core.drawPrimitive(t, startVertex, vertices); });
      else
        m_core->drawPrimitive(primitive(type), startVertex, vertices);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices,
                                       UINT startIndex, UINT primitiveCount) {
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      const UINT indices = vertexCountOf(type, primitiveCount);
      if (!indices)
        return D3D_OK;
      if (Worker* worker = m_recorder)
        worker->emit([t = primitive(type), baseVertex, minIndex, numVertices, startIndex, indices](legacy::Device& core) {
          core.drawIndexedPrimitive(t, baseVertex, minIndex, numVertices, startIndex, indices);
        });
      else
        m_core->drawIndexedPrimitive(primitive(type), baseVertex, minIndex, numVertices, startIndex, indices);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) {
    if (!data || !stride)
      return D3DERR_INVALIDCALL;
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      const UINT vertices = vertexCountOf(type, primitiveCount);
      // As Direct3D 9: stream 0 is unset afterwards.
      m_streams[0].buffer.reset();
      m_streams[0].offset = m_streams[0].stride = 0;
      if (Worker* worker = m_recorder) {
        worker->emitData(data, size_t(vertices) * stride, [t = primitive(type), vertices, stride](legacy::Device& core, const uint8_t* copy) {
          if (vertices)
            core.drawPrimitiveUP(t, vertices, copy, stride);
          core.setStreamSource(0, nullptr, 0, 0);
        });
        return D3D_OK;
      }
      if (vertices)
        m_core->drawPrimitiveUP(primitive(type), vertices, data, stride);
      m_core->setStreamSource(0, nullptr, 0, 0);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primitiveCount,
                                         const void* indices, D3DFORMAT indexFormat, const void* vertices, UINT stride) {
    if (!indices || !vertices || !stride)
      return D3DERR_INVALIDCALL;
    auto body = [&]() [[msvc::forceinline]] -> HRESULT {
      const UINT count = vertexCountOf(type, primitiveCount);
      const bool index32 = indexFormat == D3DFMT_INDEX32;
      // As Direct3D 9: stream 0 and the indices are unset afterwards.
      m_streams[0].buffer.reset();
      m_streams[0].offset = m_streams[0].stride = 0;
      m_indices.reset();
      if (Worker* worker = m_recorder) {
        // The vertices the draw names (Direct3D 9 copies those) after the
        // indices; the core finds vertex minIndex where it was.
        const size_t indexBytes = (size_t(count) * (index32 ? 4 : 2) + 15) & ~size_t(15);
        const size_t vertexBytes = size_t(numVertices) * stride;
        worker->emitFilled(indexBytes + vertexBytes, [&](uint8_t* to) {
          std::memcpy(to, indices, size_t(count) * (index32 ? 4 : 2));
          std::memcpy(to + indexBytes, static_cast<const uint8_t*>(vertices) + size_t(minIndex) * stride, vertexBytes);
        }, [t = primitive(type), minIndex, numVertices, count, index32, stride, indexBytes](legacy::Device& core, const uint8_t* copy) {
          if (count)
            core.drawIndexedPrimitiveUP(t, minIndex, numVertices, count, copy, index32,
              reinterpret_cast<const uint8_t*>(uintptr_t(copy + indexBytes) - uintptr_t(minIndex) * stride), stride);
          core.setStreamSource(0, nullptr, 0, 0);
          core.setIndices(nullptr, false);
        });
        return D3D_OK;
      }
      if (count)
        m_core->drawIndexedPrimitiveUP(primitive(type), minIndex, numVertices, count, indices, index32, vertices, stride);
      m_core->setStreamSource(0, nullptr, 0, 0);
      m_core->setIndices(nullptr, false);
      return D3D_OK;
    };
    return multithreaded() ? withLock(this, body) : body();
  }

  // Vertices of stream 0 through the fixed-function transform (and
  // lighting) into the destination buffer's FVF, as Direct3D 7's.
  HRESULT Device::ProcessVertices(UINT srcStart, UINT destIndex, UINT count, IDirect3DVertexBuffer9* destination,
                                  IDirect3DVertexDeclaration9*, DWORD flags) {
    if (!destination)
      return D3DERR_INVALIDCALL;
    Direct direct(this, "ProcessVertices");
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
    uint8_t* source = from->data();
    uint8_t* target = to->data();
    if (!source || !target)
      return D3DERR_OUTOFVIDEOMEMORY;
    m_core->processVertices(source + stream.offset + size_t(srcStart) * srcStride, m_fvf, srcStride,
      target + size_t(destIndex) * dstStride, desc.FVF, dstStride, count, m_core->renderState(D3DRS_LIGHTING) != 0,
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
