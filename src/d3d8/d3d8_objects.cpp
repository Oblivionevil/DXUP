// Direct3D 8 on DXUP's Direct3D 9 (see d3d8_objects.h).

#include "d3d8_objects.h"

#include "../legacy/d3d9_shader.h"
#include "../legacy/legacy_formats.h"
#include "../util/util_types.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>

namespace dxup::d3d8 {

  // How far a shader's end token is looked for (as the Direct3D 9 layer).
  constexpr size_t kMaxShaderTokens = size_t(1) << 20;

  void logUnsupported(const char* what) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::lock_guard<std::mutex> lock(mutex);
    if (reported.insert(what).second)
      logMessage(LogLevel::Warn, "D3D8: unsupported: %s", what);
  }

  UINT surfaceSize(D3DFORMAT format, UINT width, UINT height, UINT depth) {
    const legacy::Format f = legacy::Format(format);
    return legacy::legacyPitch(f, width) * legacy::legacyRows(f, height) * depth;
  }

  D3DPRESENT_PARAMETERS toParams9(const D3DPRESENT_PARAMETERS8& p) {
    D3DPRESENT_PARAMETERS q = {};
    q.BackBufferWidth = p.BackBufferWidth;
    q.BackBufferHeight = p.BackBufferHeight;
    q.BackBufferFormat = p.BackBufferFormat;
    q.BackBufferCount = p.BackBufferCount;
    q.MultiSampleType = p.MultiSampleType;
    q.MultiSampleQuality = 0;
    q.SwapEffect = p.SwapEffect == D3DSWAPEFFECT_COPY_VSYNC8 ? D3DSWAPEFFECT_COPY : p.SwapEffect;
    q.hDeviceWindow = p.hDeviceWindow;
    q.Windowed = p.Windowed;
    q.EnableAutoDepthStencil = p.EnableAutoDepthStencil;
    q.AutoDepthStencilFormat = p.AutoDepthStencilFormat;
    q.Flags = p.Flags & ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER ? p.Flags : p.Flags;
    q.FullScreen_RefreshRateInHz = p.Windowed ? 0 : p.FullScreen_RefreshRateInHz;
    // Windows of Direct3D 8 present at once, but for COPY_VSYNC.
    if (p.Windowed)
      q.PresentationInterval = p.SwapEffect == D3DSWAPEFFECT_COPY_VSYNC8 ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    else
      q.PresentationInterval = p.FullScreen_PresentationInterval;
    return q;
  }

  void fromParams9(const D3DPRESENT_PARAMETERS& q, D3DPRESENT_PARAMETERS8& p) {
    p.BackBufferWidth = q.BackBufferWidth;
    p.BackBufferHeight = q.BackBufferHeight;
    p.BackBufferFormat = q.BackBufferFormat;
    p.BackBufferCount = q.BackBufferCount;
  }

  // ============================================================
  // Wrappers
  // ============================================================

  template<typename Iface8, typename Iface9>
  Wrapper<Iface8, Iface9>::Wrapper(Device8* device, Iface9* object) : m_device(device), m_object(object) {
    m_device->addRefInternal();
  }

  template<typename Iface8, typename Iface9>
  Wrapper<Iface8, Iface9>::~Wrapper() {
    m_device->forget(m_object, reinterpret_cast<IUnknown*>(this));
    m_object->Release();
    m_device->releaseInternal();
  }

  template<typename Iface8, typename Iface9>
  HRESULT ResourceWrapper<Iface8, Iface9>::GetDevice(IDirect3DDevice8** device) {
    if (!device)
      return D3DERR_INVALIDCALL;
    this->m_device->AddRef();
    *device = this->m_device;
    return D3D_OK;
  }

  template<typename W, typename Iface9>
  W* Device8::wrap(Iface9* object) {
    if (!object)
      return nullptr;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_wrappers.find(object);
    if (it != m_wrappers.end()) {
      // A wrapper released to 0 on another thread is going away: a new one.
      auto* wrapper = static_cast<W*>(reinterpret_cast<typename W::Wrapper*>(it->second));
      if (wrapper->tryAddRef()) {
        object->Release();
        return wrapper;
      }
      m_wrappers.erase(it);
    }
    auto* wrapper = new W(this, object);
    m_wrappers[object] = reinterpret_cast<IUnknown*>(static_cast<typename W::Wrapper*>(wrapper));
    return wrapper;
  }

  void Device8::forget(IUnknown* object, IUnknown* wrapper) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_wrappers.find(object);
    if (it != m_wrappers.end() && it->second == wrapper)
      m_wrappers.erase(it);
  }

  namespace {
    template<typename T>
    HRESULT queryWrapper(T* self, REFIID riid, void** object, std::initializer_list<GUID> ids) {
      if (!object)
        return E_POINTER;
      *object = nullptr;
      for (const GUID& id : ids) {
        if (riid == id) {
          self->AddRef();
          *object = self;
          return S_OK;
        }
      }
      return E_NOINTERFACE;
    }

    void toSurfaceDesc8(const D3DSURFACE_DESC& d, D3DSURFACE_DESC8* out) {
      out->Format = d.Format;
      out->Type = d.Type;
      out->Usage = d.Usage;
      out->Pool = d.Pool;
      out->Size = surfaceSize(d.Format, d.Width, d.Height);
      out->MultiSampleType = d.MultiSampleType;
      out->Width = d.Width;
      out->Height = d.Height;
    }

    void toVolumeDesc8(const D3DVOLUME_DESC& d, D3DVOLUME_DESC8* out) {
      out->Format = d.Format;
      out->Type = d.Type;
      out->Usage = d.Usage;
      out->Pool = d.Pool;
      out->Size = surfaceSize(d.Format, d.Width, d.Height, d.Depth);
      out->Width = d.Width;
      out->Height = d.Height;
      out->Depth = d.Depth;
    }

    IDirect3DSurface9* unwrap(IDirect3DSurface8* surface) {
      return surface ? static_cast<Surface8*>(surface)->d3d9() : nullptr;
    }
  }

  // ---- Surfaces and volumes ----

  HRESULT Surface8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DSurface8*>(this), riid, object, { __uuidof(IUnknown), __uuidof(IDirect3DSurface8) });
  }

  HRESULT Surface8::GetDevice(IDirect3DDevice8** device) {
    if (!device)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
  }

  HRESULT Surface8::GetContainer(REFIID riid, void** container) {
    if (!container)
      return D3DERR_INVALIDCALL;
    *container = nullptr;
    // The container's wrapper: a texture, else the device.
    IDirect3DBaseTexture9* texture = nullptr;
    if (SUCCEEDED(m_object->GetContainer(__uuidof(IDirect3DBaseTexture9), reinterpret_cast<void**>(&texture)))) {
      IUnknown* wrapper = nullptr;
      switch (texture->GetType()) {
        case D3DRTYPE_TEXTURE: wrapper = m_device->wrap<Texture8>(static_cast<IDirect3DTexture9*>(texture)); break;
        case D3DRTYPE_CUBETEXTURE: wrapper = m_device->wrap<CubeTexture8>(static_cast<IDirect3DCubeTexture9*>(texture)); break;
        default: texture->Release(); break;
      }
      if (wrapper) {
        const HRESULT hr = wrapper->QueryInterface(riid, container);
        wrapper->Release();
        return hr;
      }
    }
    return m_device->QueryInterface(riid, container);
  }

  HRESULT Surface8::GetDesc(D3DSURFACE_DESC8* desc) {
    if (!desc)
      return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d = {};
    const HRESULT hr = m_object->GetDesc(&d);
    if (SUCCEEDED(hr))
      toSurfaceDesc8(d, desc);
    return hr;
  }

  HRESULT Volume8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DVolume8*>(this), riid, object, { __uuidof(IUnknown), __uuidof(IDirect3DVolume8) });
  }

  HRESULT Volume8::GetDevice(IDirect3DDevice8** device) {
    if (!device)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
  }

  HRESULT Volume8::GetContainer(REFIID riid, void** container) {
    if (!container)
      return D3DERR_INVALIDCALL;
    IDirect3DVolumeTexture9* texture = nullptr;
    if (FAILED(m_object->GetContainer(__uuidof(IDirect3DVolumeTexture9), reinterpret_cast<void**>(&texture))))
      return D3DERR_INVALIDCALL;
    auto* wrapper = m_device->wrap<VolumeTexture8>(texture);
    const HRESULT hr = wrapper->QueryInterface(riid, container);
    wrapper->Release();
    return hr;
  }

  HRESULT Volume8::GetDesc(D3DVOLUME_DESC8* desc) {
    if (!desc)
      return D3DERR_INVALIDCALL;
    D3DVOLUME_DESC d = {};
    const HRESULT hr = m_object->GetDesc(&d);
    if (SUCCEEDED(hr))
      toVolumeDesc8(d, desc);
    return hr;
  }

  // ---- Textures ----

  HRESULT Texture8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DTexture8*>(this), riid, object, { __uuidof(IUnknown), __uuidof(IDirect3DResource8),
      __uuidof(IDirect3DBaseTexture8), __uuidof(IDirect3DTexture8) });
  }

  HRESULT Texture8::GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) {
    if (!desc)
      return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d = {};
    const HRESULT hr = m_object->GetLevelDesc(level, &d);
    if (SUCCEEDED(hr))
      toSurfaceDesc8(d, desc);
    return hr;
  }

  HRESULT Texture8::GetSurfaceLevel(UINT level, IDirect3DSurface8** surface) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_object->GetSurfaceLevel(level, &s);
    *surface = SUCCEEDED(hr) ? m_device->wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT CubeTexture8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DCubeTexture8*>(this), riid, object, { __uuidof(IUnknown),
      __uuidof(IDirect3DResource8), __uuidof(IDirect3DBaseTexture8), __uuidof(IDirect3DCubeTexture8) });
  }

  HRESULT CubeTexture8::GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) {
    if (!desc)
      return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d = {};
    const HRESULT hr = m_object->GetLevelDesc(level, &d);
    if (SUCCEEDED(hr))
      toSurfaceDesc8(d, desc);
    return hr;
  }

  HRESULT CubeTexture8::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** surface) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_object->GetCubeMapSurface(face, level, &s);
    *surface = SUCCEEDED(hr) ? m_device->wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT VolumeTexture8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DVolumeTexture8*>(this), riid, object, { __uuidof(IUnknown),
      __uuidof(IDirect3DResource8), __uuidof(IDirect3DBaseTexture8), __uuidof(IDirect3DVolumeTexture8) });
  }

  HRESULT VolumeTexture8::GetLevelDesc(UINT level, D3DVOLUME_DESC8* desc) {
    if (!desc)
      return D3DERR_INVALIDCALL;
    D3DVOLUME_DESC d = {};
    const HRESULT hr = m_object->GetLevelDesc(level, &d);
    if (SUCCEEDED(hr))
      toVolumeDesc8(d, desc);
    return hr;
  }

  HRESULT VolumeTexture8::GetVolumeLevel(UINT level, IDirect3DVolume8** volume) {
    if (!volume)
      return D3DERR_INVALIDCALL;
    IDirect3DVolume9* v = nullptr;
    const HRESULT hr = m_object->GetVolumeLevel(level, &v);
    *volume = SUCCEEDED(hr) ? m_device->wrap<Volume8>(v) : nullptr;
    return hr;
  }

  HRESULT VertexBuffer8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DVertexBuffer8*>(this), riid, object, { __uuidof(IUnknown),
      __uuidof(IDirect3DResource8), __uuidof(IDirect3DVertexBuffer8) });
  }

  HRESULT IndexBuffer8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DIndexBuffer8*>(this), riid, object, { __uuidof(IUnknown),
      __uuidof(IDirect3DResource8), __uuidof(IDirect3DIndexBuffer8) });
  }

  HRESULT SwapChain8::QueryInterface(REFIID riid, void** object) {
    return queryWrapper(static_cast<IDirect3DSwapChain8*>(this), riid, object, { __uuidof(IUnknown), __uuidof(IDirect3DSwapChain8) });
  }

  HRESULT SwapChain8::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** buffer) {
    if (!buffer)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_object->GetBackBuffer(index, type, &s);
    *buffer = SUCCEEDED(hr) ? m_device->wrap<Surface8>(s) : nullptr;
    return hr;
  }

  // ============================================================
  // Device
  // ============================================================

  Device8::Device8(Interface8* parent, IDirect3DDevice9* device) : m_parent(parent), m_device(device) {
    m_parent->AddRef();
  }

  Device8::~Device8() {
    for (auto* block : m_stateBlocks)
      if (block) block->Release();
    for (auto& shader : m_vertexShaders) {
      if (!shader) continue;
      if (shader->decl) shader->decl->Release();
      if (shader->shader) shader->shader->Release();
    }
    for (auto& shader : m_pixelShaders)
      if (shader && shader->shader) shader->shader->Release();
    m_device->Release();
    m_parent->Release();
  }

  void Device8::releaseInternal() {
    if (--m_internal == 0 && m_ref == 0)
      delete this;
  }

  ULONG Device8::Release() {
    const ULONG count = --m_ref;
    if (!count) {
      // Unbind what the application set, so its wrappers can go.
      ++m_internal;
      m_device->SetVertexShader(nullptr);
      m_device->SetPixelShader(nullptr);
      for (DWORD i = 0; i < 8; i++)
        m_device->SetTexture(i, nullptr);
      for (UINT i = 0; i < 16; i++)
        m_device->SetStreamSource(i, nullptr, 0, 0);
      m_device->SetIndices(nullptr);
      releaseInternal();
    }
    return count;
  }

  HRESULT Device8::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice8)) {
      AddRef();
      *object = static_cast<IDirect3DDevice8*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT Device8::GetDirect3D(IDirect3D8** d3d) {
    if (!d3d)
      return D3DERR_INVALIDCALL;
    m_parent->AddRef();
    *d3d = m_parent;
    return D3D_OK;
  }

  HRESULT Device8::GetDeviceCaps(D3DCAPS8* caps) {
    if (!caps)
      return D3DERR_INVALIDCALL;
    D3DCAPS9 caps9 = {};
    const HRESULT hr = m_device->GetDeviceCaps(&caps9);
    if (SUCCEEDED(hr))
      Interface8::toCaps8(caps9, *caps);
    return hr;
  }

  HRESULT Device8::SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* bitmap) {
    return m_device->SetCursorProperties(x, y, unwrap(bitmap));
  }

  HRESULT Device8::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* params, IDirect3DSwapChain8** swapChain) {
    if (!params || !swapChain)
      return D3DERR_INVALIDCALL;
    D3DPRESENT_PARAMETERS p = toParams9(*params);
    IDirect3DSwapChain9* chain = nullptr;
    const HRESULT hr = m_device->CreateAdditionalSwapChain(&p, &chain);
    if (FAILED(hr))
      return hr;
    fromParams9(p, *params);
    *swapChain = wrap<SwapChain8>(chain);
    return D3D_OK;
  }

  HRESULT Device8::Reset(D3DPRESENT_PARAMETERS8* params) {
    if (!params)
      return D3DERR_INVALIDCALL;
    D3DPRESENT_PARAMETERS p = toParams9(*params);
    const HRESULT hr = m_device->Reset(&p);
    if (SUCCEEDED(hr))
      fromParams9(p, *params);
    m_vertexShaderHandle = m_pixelShaderHandle = 0;
    m_baseVertexIndex = 0;
    return hr;
  }

  HRESULT Device8::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** buffer) {
    if (!buffer)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_device->GetBackBuffer(0, index, type, &s);
    *buffer = SUCCEEDED(hr) ? wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                 IDirect3DTexture8** texture) {
    if (!texture)
      return D3DERR_INVALIDCALL;
    IDirect3DTexture9* t = nullptr;
    const HRESULT hr = m_device->CreateTexture(width, height, levels, usage, format, pool, &t, nullptr);
    *texture = SUCCEEDED(hr) ? wrap<Texture8>(t) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                                       D3DPOOL pool, IDirect3DVolumeTexture8** texture) {
    if (!texture)
      return D3DERR_INVALIDCALL;
    IDirect3DVolumeTexture9* t = nullptr;
    const HRESULT hr = m_device->CreateVolumeTexture(width, height, depth, levels, usage, format, pool, &t, nullptr);
    *texture = SUCCEEDED(hr) ? wrap<VolumeTexture8>(t) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                     IDirect3DCubeTexture8** texture) {
    if (!texture)
      return D3DERR_INVALIDCALL;
    IDirect3DCubeTexture9* t = nullptr;
    const HRESULT hr = m_device->CreateCubeTexture(edge, levels, usage, format, pool, &t, nullptr);
    *texture = SUCCEEDED(hr) ? wrap<CubeTexture8>(t) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** buffer) {
    if (!buffer)
      return D3DERR_INVALIDCALL;
    IDirect3DVertexBuffer9* b = nullptr;
    const HRESULT hr = m_device->CreateVertexBuffer(length, usage, fvf, pool, &b, nullptr);
    *buffer = SUCCEEDED(hr) ? wrap<VertexBuffer8>(b) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** buffer) {
    if (!buffer)
      return D3DERR_INVALIDCALL;
    IDirect3DIndexBuffer9* b = nullptr;
    const HRESULT hr = m_device->CreateIndexBuffer(length, usage, format, pool, &b, nullptr);
    *buffer = SUCCEEDED(hr) ? wrap<IndexBuffer8>(b) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, BOOL lockable,
                                      IDirect3DSurface8** surface) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_device->CreateRenderTarget(width, height, format, multisample, 0, lockable, &s, nullptr);
    *surface = SUCCEEDED(hr) ? wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                             IDirect3DSurface8** surface) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_device->CreateDepthStencilSurface(width, height, format, multisample, 0, FALSE, &s, nullptr);
    *surface = SUCCEEDED(hr) ? wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT Device8::CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface) {
    if (!surface)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_device->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &s, nullptr);
    *surface = SUCCEEDED(hr) ? wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT Device8::CopyRects(IDirect3DSurface8* source, const RECT* sourceRects, UINT count, IDirect3DSurface8* destination,
                             const POINT* destPoints) {
    if (!source || !destination || source == destination)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* src = unwrap(source);
    IDirect3DSurface9* dst = unwrap(destination);
    if (!count)
      return m_device->UpdateSurface(src, nullptr, dst, nullptr);
    for (UINT i = 0; i < count; i++) {
      // Without points the rectangles keep their places.
      const POINT samePlace = sourceRects ? POINT{ sourceRects[i].left, sourceRects[i].top } : POINT{ 0, 0 };
      const HRESULT hr = m_device->UpdateSurface(src, sourceRects ? &sourceRects[i] : nullptr, dst,
                                                 destPoints ? &destPoints[i] : &samePlace);
      if (FAILED(hr))
        return hr;
    }
    return D3D_OK;
  }

  HRESULT Device8::UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination) {
    if (!source || !destination)
      return D3DERR_INVALIDCALL;
    auto unwrapTexture = [](IDirect3DBaseTexture8* t) -> IDirect3DBaseTexture9* {
      switch (t->GetType()) {
        case D3DRTYPE_TEXTURE: return static_cast<Texture8*>(t)->d3d9();
        case D3DRTYPE_CUBETEXTURE: return static_cast<CubeTexture8*>(t)->d3d9();
        case D3DRTYPE_VOLUMETEXTURE: return static_cast<VolumeTexture8*>(t)->d3d9();
        default: return nullptr;
      }
    };
    return m_device->UpdateTexture(unwrapTexture(source), unwrapTexture(destination));
  }

  HRESULT Device8::GetFrontBuffer(IDirect3DSurface8* destination) {
    return m_device->GetFrontBufferData(0, unwrap(destination));
  }

  HRESULT Device8::SetRenderTarget(IDirect3DSurface8* renderTarget, IDirect3DSurface8* depthStencil) {
    // A null target keeps the current one; a null depth buffer unbinds.
    if (renderTarget) {
      const HRESULT hr = m_device->SetRenderTarget(0, unwrap(renderTarget));
      if (FAILED(hr))
        return hr;
    }
    return m_device->SetDepthStencilSurface(unwrap(depthStencil));
  }

  HRESULT Device8::GetRenderTarget(IDirect3DSurface8** renderTarget) {
    if (!renderTarget)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_device->GetRenderTarget(0, &s);
    *renderTarget = SUCCEEDED(hr) ? wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT Device8::GetDepthStencilSurface(IDirect3DSurface8** depthStencil) {
    if (!depthStencil)
      return D3DERR_INVALIDCALL;
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = m_device->GetDepthStencilSurface(&s);
    *depthStencil = SUCCEEDED(hr) ? wrap<Surface8>(s) : nullptr;
    return hr;
  }

  HRESULT Device8::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    switch (DWORD(state)) {
      case D3DRS8_LINEPATTERN:
      case D3DRS8_ZVISIBLE:
      case D3DRS8_EDGEANTIALIAS:
      case D3DRS8_PATCHSEGMENTS:
        m_droppedRenderStates[DWORD(state)] = value;
        return D3D_OK;
      case D3DRS8_SOFTWAREVERTEXPROCESSING:
        m_droppedRenderStates[DWORD(state)] = value;
        return m_device->SetSoftwareVertexProcessing(value);
      default:
        // D3DRS_ZBIAS (47) is the legacy core's own state.
        return m_device->SetRenderState(state, value);
    }
  }

  HRESULT Device8::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) {
    if (!value)
      return D3DERR_INVALIDCALL;
    auto it = m_droppedRenderStates.find(DWORD(state));
    if (it != m_droppedRenderStates.end()) {
      *value = it->second;
      return D3D_OK;
    }
    return m_device->GetRenderState(state, value);
  }

  HRESULT Device8::EndStateBlock(DWORD* token) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!token)
      return D3DERR_INVALIDCALL;
    IDirect3DStateBlock9* block = nullptr;
    const HRESULT hr = m_device->EndStateBlock(&block);
    if (FAILED(hr))
      return hr;
    m_stateBlocks.push_back(block);
    *token = DWORD(m_stateBlocks.size());
    return D3D_OK;
  }

  HRESULT Device8::ApplyStateBlock(DWORD token) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!token || token > m_stateBlocks.size() || !m_stateBlocks[token - 1])
      return D3DERR_INVALIDCALL;
    return m_stateBlocks[token - 1]->Apply();
  }

  HRESULT Device8::CaptureStateBlock(DWORD token) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!token || token > m_stateBlocks.size() || !m_stateBlocks[token - 1])
      return D3DERR_INVALIDCALL;
    return m_stateBlocks[token - 1]->Capture();
  }

  HRESULT Device8::DeleteStateBlock(DWORD token) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!token || token > m_stateBlocks.size() || !m_stateBlocks[token - 1])
      return D3DERR_INVALIDCALL;
    m_stateBlocks[token - 1]->Release();
    m_stateBlocks[token - 1] = nullptr;
    return D3D_OK;
  }

  HRESULT Device8::CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!token)
      return D3DERR_INVALIDCALL;
    IDirect3DStateBlock9* block = nullptr;
    const HRESULT hr = m_device->CreateStateBlock(type, &block);
    if (FAILED(hr))
      return hr;
    m_stateBlocks.push_back(block);
    *token = DWORD(m_stateBlocks.size());
    return D3D_OK;
  }

  IDirect3DBaseTexture8* Device8::wrapTexture(IDirect3DBaseTexture9* texture) {
    if (!texture)
      return nullptr;
    switch (texture->GetType()) {
      case D3DRTYPE_TEXTURE: return wrap<Texture8>(static_cast<IDirect3DTexture9*>(texture));
      case D3DRTYPE_CUBETEXTURE: return wrap<CubeTexture8>(static_cast<IDirect3DCubeTexture9*>(texture));
      case D3DRTYPE_VOLUMETEXTURE: return wrap<VolumeTexture8>(static_cast<IDirect3DVolumeTexture9*>(texture));
      default: texture->Release(); return nullptr;
    }
  }

  HRESULT Device8::GetTexture(DWORD stage, IDirect3DBaseTexture8** texture) {
    if (!texture)
      return D3DERR_INVALIDCALL;
    IDirect3DBaseTexture9* t = nullptr;
    const HRESULT hr = m_device->GetTexture(stage, &t);
    *texture = SUCCEEDED(hr) ? wrapTexture(t) : nullptr;
    return hr;
  }

  HRESULT Device8::SetTexture(DWORD stage, IDirect3DBaseTexture8* texture) {
    IDirect3DBaseTexture9* t = nullptr;
    if (texture) {
      switch (texture->GetType()) {
        case D3DRTYPE_TEXTURE: t = static_cast<Texture8*>(texture)->d3d9(); break;
        case D3DRTYPE_CUBETEXTURE: t = static_cast<CubeTexture8*>(texture)->d3d9(); break;
        case D3DRTYPE_VOLUMETEXTURE: t = static_cast<VolumeTexture8*>(texture)->d3d9(); break;
        default: return D3DERR_INVALIDCALL;
      }
    }
    return m_device->SetTexture(stage, t);
  }

  namespace {
    // Texture stage states of Direct3D 8 that are sampler states now.
    D3DSAMPLERSTATETYPE samplerStateOf(DWORD type) {
      switch (type) {
        case D3DTSS8_ADDRESSU:      return D3DSAMP_ADDRESSU;
        case D3DTSS8_ADDRESSV:      return D3DSAMP_ADDRESSV;
        case D3DTSS8_ADDRESSW:      return D3DSAMP_ADDRESSW;
        case D3DTSS8_BORDERCOLOR:   return D3DSAMP_BORDERCOLOR;
        case D3DTSS8_MAGFILTER:     return D3DSAMP_MAGFILTER;
        case D3DTSS8_MINFILTER:     return D3DSAMP_MINFILTER;
        case D3DTSS8_MIPFILTER:     return D3DSAMP_MIPFILTER;
        case D3DTSS8_MIPMAPLODBIAS: return D3DSAMP_MIPMAPLODBIAS;
        case D3DTSS8_MAXMIPLEVEL:   return D3DSAMP_MAXMIPLEVEL;
        case D3DTSS8_MAXANISOTROPY: return D3DSAMP_MAXANISOTROPY;
        default:                    return D3DSAMPLERSTATETYPE(0);
      }
    }
  }

  HRESULT Device8::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) {
    if (const D3DSAMPLERSTATETYPE sampler = samplerStateOf(DWORD(type)))
      return m_device->GetSamplerState(stage, sampler, value);
    return m_device->GetTextureStageState(stage, type, value);
  }

  HRESULT Device8::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (const D3DSAMPLERSTATETYPE sampler = samplerStateOf(DWORD(type))) {
      // The cubic filters of Direct3D 8 are linear ones.
      if ((sampler == D3DSAMP_MAGFILTER || sampler == D3DSAMP_MINFILTER) && value > D3DTEXF_ANISOTROPIC)
        value = D3DTEXF_LINEAR;
      return m_device->SetSamplerState(stage, sampler, value);
    }
    return m_device->SetTextureStageState(stage, type, value);
  }

  HRESULT Device8::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) {
    const HRESULT hr = m_device->DrawPrimitiveUP(type, primitiveCount, data, stride);
    m_streamStrides[0] = 0;
    return hr;
  }

  HRESULT Device8::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primitiveCount,
                                          const void* indices, D3DFORMAT indexFormat, const void* vertices, UINT stride) {
    const HRESULT hr = m_device->DrawIndexedPrimitiveUP(type, minIndex, numVertices, primitiveCount, indices, indexFormat,
                                                        vertices, stride);
    m_streamStrides[0] = 0;
    m_baseVertexIndex = 0;
    return hr;
  }

  HRESULT Device8::ProcessVertices(UINT sourceStart, UINT destIndex, UINT count, IDirect3DVertexBuffer8* destination,
                                   DWORD flags) {
    return m_device->ProcessVertices(sourceStart, destIndex, count,
      destination ? static_cast<VertexBuffer8*>(destination)->d3d9() : nullptr, nullptr, flags);
  }

  // ---- Vertex shaders ----

  namespace {
    // The usage a Direct3D 8 input register stands for (D3DVSDE_*).
    void usageOfRegister(DWORD reg, BYTE& usage, BYTE& index) {
      switch (reg) {
        case 0:  usage = D3DDECLUSAGE_POSITION; index = 0; return;
        case 1:  usage = D3DDECLUSAGE_BLENDWEIGHT; index = 0; return;
        case 2:  usage = D3DDECLUSAGE_BLENDINDICES; index = 0; return;
        case 3:  usage = D3DDECLUSAGE_NORMAL; index = 0; return;
        case 4:  usage = D3DDECLUSAGE_PSIZE; index = 0; return;
        case 5:  usage = D3DDECLUSAGE_COLOR; index = 0; return;
        case 6:  usage = D3DDECLUSAGE_COLOR; index = 1; return;
        case 15: usage = D3DDECLUSAGE_POSITION; index = 1; return;
        case 16: usage = D3DDECLUSAGE_NORMAL; index = 1; return;
        default:
          usage = D3DDECLUSAGE_TEXCOORD;
          index = BYTE(reg >= 7 && reg <= 14 ? reg - 7 : reg);
          return;
      }
    }

    BYTE typeSize(DWORD type) {
      switch (type) {
        case D3DDECLTYPE_FLOAT1: return 4;
        case D3DDECLTYPE_FLOAT2: return 8;
        case D3DDECLTYPE_FLOAT3: return 12;
        case D3DDECLTYPE_FLOAT4: return 16;
        case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_SHORT2: return 4;
        case D3DDECLTYPE_SHORT4: return 8;
        default: return 4;
      }
    }
  }

  HRESULT Device8::CreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle, DWORD) {
    if (!declaration || !handle)
      return D3DERR_INVALIDCALL;
    auto shader = std::make_unique<VertexShader>();
    // The declaration: streams of registers, skips, constants.
    std::vector<D3DVERTEXELEMENT9> elements;
    std::vector<std::pair<BYTE, BYTE>> registers;   // register, usage index pairs for dcl
    WORD stream = 0, offset = 0;
    const DWORD* token = declaration;
    for (; *token != 0xffffffffu; token++) {
      const DWORD type = (*token >> 29) & 7;
      if (type == 1) {           // D3DVSD_TOKEN_STREAM
        stream = WORD(*token & 0xf);
        offset = 0;
      } else if (type == 2) {    // D3DVSD_TOKEN_STREAMDATA
        if (*token & (1u << 28)) {
          offset = WORD(offset + 4 * ((*token >> 16) & 0xf));   // skip dwords
        } else {
          const DWORD reg = *token & 0x1f;
          const DWORD dataType = (*token >> 16) & 0xf;
          BYTE usage, index;
          usageOfRegister(reg, usage, index);
          elements.push_back({ stream, offset, BYTE(dataType), D3DDECLMETHOD_DEFAULT, usage, index });
          registers.push_back({ BYTE(reg), 0 });
          offset = WORD(offset + typeSize(dataType));
        }
      } else if (type == 4) {    // D3DVSD_TOKEN_CONSTMEM
        const DWORD count = (*token >> 25) & 0xf;
        DWORD address = *token & 0x7f;
        for (DWORD i = 0; i < count; i++, address++) {
          std::array<float, 4> value;
          std::memcpy(value.data(), token + 1 + i * 4, 16);
          shader->constants.push_back({ address, value });
        }
        token += count * 4;
      } else if (type == 5) {    // D3DVSD_TOKEN_EXT
        token += (*token >> 24) & 0x1f;
      }
    }
    shader->declaration.assign(declaration, token + 1);
    elements.push_back(D3DDECL_END());
    HRESULT hr = m_device->CreateVertexDeclaration(elements.data(), &shader->decl);
    if (FAILED(hr))
      return hr;
    if (function) {
      // vs_1_1 of Direct3D 9 declares its inputs: dcl for each register.
      const size_t length = legacy::shaderTokenCount9(reinterpret_cast<const uint32_t*>(function), kMaxShaderTokens);
      if (!length)
        return D3DERR_INVALIDCALL;
      shader->function.assign(function, function + length);
      std::vector<DWORD> tokens;
      tokens.push_back(function[0]);
      for (const auto& reg : registers) {
        BYTE usage, index;
        usageOfRegister(reg.first, usage, index);
        tokens.push_back(D3DSIO_DCL);
        tokens.push_back(0x80000000u | usage | (DWORD(index) << 16));
        tokens.push_back(0x80000000u | (DWORD(D3DSPR_INPUT) << 28) | 0x000f0000u | reg.first);
      }
      tokens.insert(tokens.end(), function + 1, function + length);
      hr = m_device->CreateVertexShader(tokens.data(), &shader->shader);
      if (FAILED(hr)) {
        shader->decl->Release();
        return hr;
      }
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_vertexShaders.push_back(std::move(shader));
    // Odd: flexible vertex formats are even.
    *handle = (DWORD(m_vertexShaders.size()) << 1) | 1u;
    return D3D_OK;
  }

  HRESULT Device8::SetVertexShader(DWORD handle) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!(handle & 1)) {
      m_vertexShaderHandle = handle;
      m_device->SetVertexShader(nullptr);
      return m_device->SetFVF(handle);
    }
    const DWORD index = (handle >> 1) - 1;
    if (index >= m_vertexShaders.size() || !m_vertexShaders[index])
      return D3DERR_INVALIDCALL;
    VertexShader& shader = *m_vertexShaders[index];
    m_vertexShaderHandle = handle;
    m_device->SetVertexDeclaration(shader.decl);
    m_device->SetVertexShader(shader.shader);
    for (const auto& constant : shader.constants)
      m_device->SetVertexShaderConstantF(constant.first, constant.second.data(), 1);
    return D3D_OK;
  }

  HRESULT Device8::GetVertexShader(DWORD* handle) {
    if (!handle)
      return D3DERR_INVALIDCALL;
    *handle = m_vertexShaderHandle;
    return D3D_OK;
  }

  HRESULT Device8::DeleteVertexShader(DWORD handle) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const DWORD index = (handle >> 1) - 1;
    if (!(handle & 1) || index >= m_vertexShaders.size() || !m_vertexShaders[index])
      return D3DERR_INVALIDCALL;
    if (m_vertexShaderHandle == handle) {
      m_device->SetVertexShader(nullptr);
      m_device->SetVertexDeclaration(nullptr);
      m_vertexShaderHandle = 0;
    }
    VertexShader& shader = *m_vertexShaders[index];
    if (shader.decl) shader.decl->Release();
    if (shader.shader) shader.shader->Release();
    m_vertexShaders[index].reset();
    return D3D_OK;
  }

  HRESULT Device8::GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const DWORD index = (handle >> 1) - 1;
    if (!size || !(handle & 1) || index >= m_vertexShaders.size() || !m_vertexShaders[index])
      return D3DERR_INVALIDCALL;
    const auto& tokens = m_vertexShaders[index]->declaration;
    const DWORD bytes = DWORD(tokens.size() * 4);
    if (data) {
      if (*size < bytes) return D3DERR_MOREDATA;
      std::memcpy(data, tokens.data(), bytes);
    }
    *size = bytes;
    return D3D_OK;
  }

  HRESULT Device8::GetVertexShaderFunction(DWORD handle, void* data, DWORD* size) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const DWORD index = (handle >> 1) - 1;
    if (!size || !(handle & 1) || index >= m_vertexShaders.size() || !m_vertexShaders[index])
      return D3DERR_INVALIDCALL;
    const auto& tokens = m_vertexShaders[index]->function;
    const DWORD bytes = DWORD(tokens.size() * 4);
    if (data) {
      if (*size < bytes) return D3DERR_MOREDATA;
      std::memcpy(data, tokens.data(), bytes);
    }
    *size = bytes;
    return D3D_OK;
  }

  HRESULT Device8::SetStreamSource(UINT stream, IDirect3DVertexBuffer8* buffer, UINT stride) {
    if (stream >= 16)
      return D3DERR_INVALIDCALL;
    m_streamStrides[stream] = stride;
    return m_device->SetStreamSource(stream, buffer ? static_cast<VertexBuffer8*>(buffer)->d3d9() : nullptr, 0, stride);
  }

  HRESULT Device8::GetStreamSource(UINT stream, IDirect3DVertexBuffer8** buffer, UINT* stride) {
    if (!buffer || !stride)
      return D3DERR_INVALIDCALL;
    IDirect3DVertexBuffer9* b = nullptr;
    UINT offset = 0;
    const HRESULT hr = m_device->GetStreamSource(stream, &b, &offset, stride);
    *buffer = SUCCEEDED(hr) && b ? wrap<VertexBuffer8>(b) : nullptr;
    return hr;
  }

  HRESULT Device8::SetIndices(IDirect3DIndexBuffer8* indices, UINT baseVertexIndex) {
    m_baseVertexIndex = baseVertexIndex;
    return m_device->SetIndices(indices ? static_cast<IndexBuffer8*>(indices)->d3d9() : nullptr);
  }

  HRESULT Device8::GetIndices(IDirect3DIndexBuffer8** indices, UINT* baseVertexIndex) {
    if (!indices || !baseVertexIndex)
      return D3DERR_INVALIDCALL;
    IDirect3DIndexBuffer9* b = nullptr;
    const HRESULT hr = m_device->GetIndices(&b);
    *indices = SUCCEEDED(hr) && b ? wrap<IndexBuffer8>(b) : nullptr;
    *baseVertexIndex = m_baseVertexIndex;
    return hr;
  }

  // ---- Pixel shaders ----

  HRESULT Device8::CreatePixelShader(const DWORD* function, DWORD* handle) {
    if (!function || !handle)
      return D3DERR_INVALIDCALL;
    auto shader = std::make_unique<PixelShader>();
    const size_t length = legacy::shaderTokenCount9(reinterpret_cast<const uint32_t*>(function), kMaxShaderTokens);
    if (!length)
      return D3DERR_INVALIDCALL;
    shader->function.assign(function, function + length);
    // ps.1.0 is ps_1_1 for Direct3D 9.
    std::vector<DWORD> tokens = shader->function;
    if (tokens[0] == 0xffff0100u)
      tokens[0] = 0xffff0101u;
    const HRESULT hr = m_device->CreatePixelShader(tokens.data(), &shader->shader);
    if (FAILED(hr))
      return hr;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_pixelShaders.push_back(std::move(shader));
    *handle = DWORD(m_pixelShaders.size());
    return D3D_OK;
  }

  HRESULT Device8::SetPixelShader(DWORD handle) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!handle) {
      m_pixelShaderHandle = 0;
      return m_device->SetPixelShader(nullptr);
    }
    if (handle > m_pixelShaders.size() || !m_pixelShaders[handle - 1])
      return D3DERR_INVALIDCALL;
    m_pixelShaderHandle = handle;
    return m_device->SetPixelShader(m_pixelShaders[handle - 1]->shader);
  }

  HRESULT Device8::GetPixelShader(DWORD* handle) {
    if (!handle)
      return D3DERR_INVALIDCALL;
    *handle = m_pixelShaderHandle;
    return D3D_OK;
  }

  HRESULT Device8::DeletePixelShader(DWORD handle) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!handle || handle > m_pixelShaders.size() || !m_pixelShaders[handle - 1])
      return D3DERR_INVALIDCALL;
    if (m_pixelShaderHandle == handle) {
      m_device->SetPixelShader(nullptr);
      m_pixelShaderHandle = 0;
    }
    if (m_pixelShaders[handle - 1]->shader)
      m_pixelShaders[handle - 1]->shader->Release();
    m_pixelShaders[handle - 1].reset();
    return D3D_OK;
  }

  HRESULT Device8::GetPixelShaderFunction(DWORD handle, void* data, DWORD* size) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!size || !handle || handle > m_pixelShaders.size() || !m_pixelShaders[handle - 1])
      return D3DERR_INVALIDCALL;
    const auto& tokens = m_pixelShaders[handle - 1]->function;
    const DWORD bytes = DWORD(tokens.size() * 4);
    if (data) {
      if (*size < bytes) return D3DERR_MOREDATA;
      std::memcpy(data, tokens.data(), bytes);
    }
    *size = bytes;
    return D3D_OK;
  }

  HRESULT Device8::DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) {
    logUnsupported("DrawRectPatch");
    return D3DERR_INVALIDCALL;
  }

  HRESULT Device8::DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) {
    logUnsupported("DrawTriPatch");
    return D3DERR_INVALIDCALL;
  }

  // ============================================================
  // Interface
  // ============================================================

  Interface8::Interface8(IDirect3D9* d3d9) : m_d3d9(d3d9) {}

  Interface8::~Interface8() {
    m_d3d9->Release();
  }

  HRESULT Interface8::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D8)) {
      AddRef();
      *object = static_cast<IDirect3D8*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG Interface8::Release() {
    const ULONG count = --m_ref;
    if (!count)
      delete this;
    return count;
  }

  HRESULT Interface8::GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier) {
    if (!identifier)
      return D3DERR_INVALIDCALL;
    D3DADAPTER_IDENTIFIER9 id = {};
    const HRESULT hr = m_d3d9->GetAdapterIdentifier(adapter, flags, &id);
    if (FAILED(hr))
      return hr;
    std::memcpy(identifier->Driver, id.Driver, sizeof(identifier->Driver));
    std::memcpy(identifier->Description, id.Description, sizeof(identifier->Description));
    identifier->DriverVersion = id.DriverVersion;
    identifier->VendorId = id.VendorId;
    identifier->DeviceId = id.DeviceId;
    identifier->SubSysId = id.SubSysId;
    identifier->Revision = id.Revision;
    identifier->DeviceIdentifier = id.DeviceIdentifier;
    identifier->WHQLLevel = id.WHQLLevel;
    return D3D_OK;
  }

  // Direct3D 8 lists the modes of all display formats together.
  UINT Interface8::GetAdapterModeCount(UINT adapter) {
    return m_d3d9->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8) + m_d3d9->GetAdapterModeCount(adapter, D3DFMT_R5G6B5);
  }

  HRESULT Interface8::EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* displayMode) {
    const UINT modes32 = m_d3d9->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8);
    if (mode < modes32)
      return m_d3d9->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, mode, displayMode);
    return m_d3d9->EnumAdapterModes(adapter, D3DFMT_R5G6B5, mode - modes32, displayMode);
  }

  void Interface8::toCaps8(const D3DCAPS9& caps9, D3DCAPS8& caps) {
    static_assert(sizeof(D3DCAPS8) <= sizeof(D3DCAPS9), "caps layouts");
    std::memcpy(&caps, &caps9, sizeof(D3DCAPS8));
    // Shader models of Direct3D 8.
    caps.VertexShaderVersion = D3DVS_VERSION(1, 1);
    caps.PixelShaderVersion = D3DPS_VERSION(1, 4);
    caps.MaxVertexShaderConst = 96;
    caps.MaxPixelShaderValue = 1.0f;
    caps.Caps2 &= ~DWORD(D3DCAPS2_CANAUTOGENMIPMAP);
  }

  HRESULT Interface8::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* caps) {
    if (!caps)
      return D3DERR_INVALIDCALL;
    D3DCAPS9 caps9 = {};
    const HRESULT hr = m_d3d9->GetDeviceCaps(adapter, type, &caps9);
    if (SUCCEEDED(hr))
      toCaps8(caps9, *caps);
    return hr;
  }

  HRESULT Interface8::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
                                   D3DPRESENT_PARAMETERS8* params, IDirect3DDevice8** device) {
    if (!params || !device)
      return D3DERR_INVALIDCALL;
    *device = nullptr;
    D3DPRESENT_PARAMETERS p = toParams9(*params);
    IDirect3DDevice9* device9 = nullptr;
    const HRESULT hr = m_d3d9->CreateDevice(adapter, type, focusWindow, behavior, &p, &device9);
    if (FAILED(hr))
      return hr;
    fromParams9(p, *params);
    *device = new Device8(this, device9);
    return D3D_OK;
  }

}
