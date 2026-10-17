#pragma once

// Direct3D 9 (and 9Ex) on the legacy core (src/legacy): the interface,
// the device and its objects. The device keeps the state the core does
// not (bindings as COM objects, state blocks, swap chains); drawing,
// resources and shaders are the core's.
//
// Objects count references as Direct3D 9 does: the application's count,
// and the device's own while it holds them bound (never visible to the
// application). Surfaces and volumes of textures, and back buffers of
// swap chains, count on their container.

#include <windows.h>
#include <d3d9.h>

#include "../legacy/legacy_device.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dxup::d3d9 {

  class Interface;
  class Device;
  class SwapChain;

  // Reports a method the layer does not implement, once per name.
  void logUnsupported(const char* what);

  // ---- Reference counting ----

  // Objects alive while the application or the device holds them.
  class RefCounted {
  public:
    virtual ~RefCounted() = default;
    virtual void addRefPrivate() { ++m_private; }
    virtual void releasePrivate() {
      if (--m_private == 0 && m_public == 0)
        destroy();
    }
  protected:
    ULONG addRefPublic() {
      const ULONG count = ++m_public;
      if (count == 1)
        onRevived();
      return count;
    }
    ULONG releasePublic() {
      const ULONG count = --m_public;
      if (count == 0) {
        // Held while letting go of what it holds, which may release it.
        ++m_private;
        onPublicReleased();
        if (--m_private == 0 && m_public == 0)
          destroy();
      }
      return count;
    }
    virtual void onRevived() {}
    virtual void onPublicReleased() {}
    virtual void destroy() { delete this; }
    std::atomic<ULONG> m_public{0};
    std::atomic<ULONG> m_private{0};
  };

  // Holds a device-side reference to an object (bindings).
  template<typename T>
  class Bound {
  public:
    Bound() = default;
    Bound(const Bound&) = delete;
    Bound& operator=(const Bound&) = delete;
    ~Bound() { reset(); }
    void set(T* object) {
      if (object == m_object)
        return;
      if (object) object->addRefPrivate();
      if (m_object) m_object->releasePrivate();
      m_object = object;
    }
    void reset() { set(nullptr); }
    T* get() const { return m_object; }
    T* operator->() const { return m_object; }
    explicit operator bool() const { return m_object != nullptr; }
  private:
    T* m_object = nullptr;
  };

  // SetPrivateData storage.
  class PrivateData {
  public:
    ~PrivateData() { clear(); }
    HRESULT set(REFGUID guid, const void* data, DWORD size, DWORD flags);
    HRESULT get(REFGUID guid, void* data, DWORD* size);
    HRESULT free(REFGUID guid);
  private:
    void clear();
    struct Entry {
      std::vector<uint8_t> data;
      IUnknown* object = nullptr;
    };
    std::map<GUID, Entry, bool(*)(const GUID&, const GUID&)> m_entries{ [](const GUID& a, const GUID& b) {
      return std::memcmp(&a, &b, sizeof(GUID)) < 0; } };
  };

  // ---- Resources ----

  template<typename Base>
  class Resource : public Base, public RefCounted {
  public:
    // owned: a container's child or the device's own, holding no reference
    // to the device (which would keep it alive forever).
    explicit Resource(Device* device, D3DPOOL pool = D3DPOOL_DEFAULT, bool owned = false);
    ~Resource() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** device) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
      return m_privateData.set(guid, data, size, flags);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
      return m_privateData.get(guid, data, size);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override { return m_privateData.free(guid); }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) { const DWORD old = m_priority; m_priority = priority; return old; }
    DWORD STDMETHODCALLTYPE GetPriority() { return m_priority; }
    void STDMETHODCALLTYPE PreLoad() {}
    D3DPOOL pool() const { return m_pool; }
    Device* device() const { return m_device; }
  protected:
    Device*     m_device;
    D3DPOOL     m_pool;
    bool        m_owned;
    DWORD       m_priority = 0;
    PrivateData m_privateData;
  };

  class BaseTexture;

  // A surface: a subresource of a texture, a back buffer, or alone.
  class Surface final : public Resource<IDirect3DSurface9> {
  public:
    // container: the texture or swap chain counting for it, or null.
    Surface(Device* device, D3DPOOL pool, std::shared_ptr<legacy::Texture> texture, uint32_t subresource,
            IUnknown* container, RefCounted* containerRef, DWORD usage, D3DMULTISAMPLE_TYPE multisample, DWORD quality,
            bool owned = false);
    void addRefPrivate() override { if (m_containerRef) m_containerRef->addRefPrivate(); else RefCounted::addRefPrivate(); }
    void releasePrivate() override { if (m_containerRef) m_containerRef->releasePrivate(); else RefCounted::releasePrivate(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* dc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;

    legacy::Texture* texture() const { return m_texture.get(); }
    const std::shared_ptr<legacy::Texture>& sharedTexture() const { return m_texture; }
    uint32_t subresource() const { return m_subresource; }
    D3DFORMAT format() const { return D3DFORMAT(m_texture->desc().format); }
    UINT width() const { return m_texture->width(m_subresource); }
    UINT height() const { return m_texture->height(m_subresource); }
    D3DMULTISAMPLE_TYPE multisample() const { return m_multisample; }
    DWORD usage() const { return m_usage; }
    bool isContained() const { return m_container != nullptr; }
    // The container's surfaces live as long as it does.
    void destroyContained() { delete this; }

  private:
    // The device's own surfaces (its depth buffer) are deleted by it.
    void destroy() override { if (!m_owned || m_container) delete this; }
    std::shared_ptr<legacy::Texture> m_texture;
    uint32_t            m_subresource;
    IUnknown*           m_container;
    RefCounted*         m_containerRef;
    DWORD               m_usage;
    D3DMULTISAMPLE_TYPE m_multisample;
    DWORD               m_quality;
    bool                m_locked = false;
    // GetDC: a DIB with the surface's pixels, written back on ReleaseDC.
    HDC                 m_dc = nullptr;
    HBITMAP             m_bitmap = nullptr;
    HGDIOBJ             m_oldBitmap = nullptr;
    void*               m_bits = nullptr;
  };

  class Volume final : public Resource<IDirect3DVolume9> {
  public:
    Volume(Device* device, std::shared_ptr<legacy::Texture> texture, uint32_t level, IUnknown* container,
           RefCounted* containerRef);
    void addRefPrivate() override { m_containerRef->addRefPrivate(); }
    void releasePrivate() override { m_containerRef->releasePrivate(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return m_container->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_container->Release(); }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox() override;
  private:
    std::shared_ptr<legacy::Texture> m_texture;
    uint32_t  m_level;
    IUnknown* m_container;
    RefCounted* m_containerRef;
  };

  // What 2D, cube and volume textures share.
  class BaseTexture {
  public:
    virtual ~BaseTexture() = default;
    legacy::Texture* texture() const { return m_texture.get(); }
    const std::shared_ptr<legacy::Texture>& sharedTexture() const { return m_texture; }
    virtual void addRefPrivateTexture() = 0;
    virtual void releasePrivateTexture() = 0;
  protected:
    std::shared_ptr<legacy::Texture> m_texture;
    DWORD m_usage = 0;
    DWORD m_lod = 0;
    D3DTEXTUREFILTERTYPE m_autoGenFilter = D3DTEXF_LINEAR;
  };

  template<typename Base>
  class TextureCommon : public Resource<Base>, public BaseTexture {
  public:
    TextureCommon(Device* device, D3DPOOL pool) : Resource<Base>(device, pool) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return this->addRefPublic(); }
    ULONG STDMETHODCALLTYPE Release() override { return this->releasePublic(); }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override { const DWORD old = m_lod; m_lod = lod; return old; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return m_lod; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override;
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE type) override { m_autoGenFilter = type; return D3D_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return m_autoGenFilter; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override;
    void addRefPrivateTexture() override { this->addRefPrivate(); }
    void releasePrivateTexture() override { this->releasePrivate(); }
  };

  class Texture2D final : public TextureCommon<IDirect3DTexture9> {
  public:
    Texture2D(Device* device, std::shared_ptr<legacy::Texture> texture, D3DPOOL pool, DWORD usage);
    ~Texture2D() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface9** surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return D3D_OK; }
    Surface* surface(UINT level) const { return level < m_surfaces.size() ? m_surfaces[level] : nullptr; }
  private:
    std::vector<Surface*> m_surfaces;
  };

  class CubeTexture final : public TextureCommon<IDirect3DCubeTexture9> {
  public:
    CubeTexture(Device* device, std::shared_ptr<legacy::Texture> texture, D3DPOOL pool, DWORD usage);
    ~CubeTexture() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT*) override { return D3D_OK; }
  private:
    std::vector<Surface*> m_surfaces;   // face * levels + level
  };

  class VolumeTexture final : public TextureCommon<IDirect3DVolumeTexture9> {
  public:
    VolumeTexture(Device* device, std::shared_ptr<legacy::Texture> texture, D3DPOOL pool, DWORD usage);
    ~VolumeTexture() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DVOLUME_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, IDirect3DVolume9** volume) override;
    HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX*) override { return D3D_OK; }
  private:
    std::vector<Volume*> m_volumes;
  };

  // The legacy texture behind any texture interface.
  BaseTexture* baseTextureOf(IDirect3DBaseTexture9* texture);

  template<typename Base, typename Desc>
  class BufferCommon : public Resource<Base> {
  public:
    BufferCommon(Device* device, std::unique_ptr<legacy::Buffer> buffer, const Desc& desc);
    ULONG STDMETHODCALLTYPE AddRef() override { return this->addRefPublic(); }
    ULONG STDMETHODCALLTYPE Release() override { return this->releasePublic(); }
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void** data, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(Desc* desc) override { if (!desc) return D3DERR_INVALIDCALL; *desc = m_desc; return D3D_OK; }
    legacy::Buffer* buffer() const { return m_buffer.get(); }
  protected:
    std::unique_ptr<legacy::Buffer> m_buffer;
    Desc m_desc;
  };

  class VertexBuffer final : public BufferCommon<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC> {
  public:
    using BufferCommon::BufferCommon;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
  };

  class IndexBuffer final : public BufferCommon<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC> {
  public:
    using BufferCommon::BufferCommon;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
    bool index32() const { return m_desc.Format == D3DFMT_INDEX32; }
  };

  // ---- Device children that are not resources ----

  template<typename Base>
  class DeviceChild : public Base, public RefCounted {
  public:
    // Objects the device keeps for itself (declarations of FVFs) hold it only
    // while the application does: a reference both ways would keep both alive.
    explicit DeviceChild(Device* device, bool keptByDevice = false);
    ~DeviceChild() override;
    ULONG STDMETHODCALLTYPE AddRef() override { return addRefPublic(); }
    ULONG STDMETHODCALLTYPE Release() override { return releasePublic(); }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** device) override;
  protected:
    void onRevived() override;
    void onPublicReleased() override;
    Device* m_device;
    bool    m_keptByDevice;
  };

  class VertexDeclaration final : public DeviceChild<IDirect3DVertexDeclaration9> {
  public:
    VertexDeclaration(Device* device, const D3DVERTEXELEMENT9* elements, uint32_t fvf = 0, bool keptByDevice = false);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* elements, UINT* count) override;
    legacy::VertexDecl* decl() { return &m_decl; }
    uint32_t fvf() const { return m_fvf; }
  private:
    std::vector<D3DVERTEXELEMENT9> m_elements;   // with the end element
    legacy::VertexDecl m_decl;
    uint32_t m_fvf;
  };

  template<typename Base>
  class ShaderObject final : public DeviceChild<Base> {
  public:
    ShaderObject(Device* device, std::shared_ptr<legacy::Shader> shader) : DeviceChild<Base>(device), m_shader(std::move(shader)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void* data, UINT* size) override;
    legacy::Shader* shader() const { return m_shader.get(); }
  private:
    std::shared_ptr<legacy::Shader> m_shader;
  };
  using VertexShader = ShaderObject<IDirect3DVertexShader9>;
  using PixelShader = ShaderObject<IDirect3DPixelShader9>;

  class Query final : public DeviceChild<IDirect3DQuery9> {
  public:
    Query(Device* device, D3DQUERYTYPE type);
    ~Query() override;
    bool valid() const { return m_valid; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return m_type; }
    DWORD STDMETHODCALLTYPE GetDataSize() override;
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetData(void* data, DWORD size, DWORD flags) override;
  private:
    D3DQUERYTYPE m_type;
    bool m_valid = false;
    bool m_issued = false;
    legacy::Ref<ID3D11Query> m_query;
    legacy::Ref<ID3D11Query> m_disjoint;
  };

  // Everything a state block can hold, and which parts it does.
  struct StateSet {
    std::array<DWORD, 256>  renderStates = {};
    std::bitset<256>        renderStatesSet;
    std::array<std::array<DWORD, 33>, 8> stageStates = {};
    std::array<std::bitset<33>, 8> stageStatesSet;
    std::array<std::array<DWORD, 14>, legacy::kSamplerCount> samplerStates = {};
    std::array<std::bitset<14>, legacy::kSamplerCount> samplerStatesSet;
    std::array<IDirect3DBaseTexture9*, legacy::kSamplerCount> textures = {};
    std::bitset<legacy::kSamplerCount> texturesSet;
    struct Stream { IDirect3DVertexBuffer9* buffer = nullptr; UINT offset = 0, stride = 0, frequency = 1; };
    std::array<Stream, 16> streams = {};
    std::bitset<16> streamsSet;
    std::bitset<16> frequenciesSet;
    IDirect3DIndexBuffer9* indices = nullptr;
    bool indicesSet = false;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    DWORD fvf = 0;
    bool declarationSet = false;   // declaration or FVF
    IDirect3DVertexShader9* vertexShader = nullptr;
    bool vertexShaderSet = false;
    IDirect3DPixelShader9* pixelShader = nullptr;
    bool pixelShaderSet = false;
    std::array<std::array<float, 4>, 256> vsFloats = {}, psFloats = {};
    std::bitset<256> vsFloatsSet, psFloatsSet;
    std::array<std::array<int, 4>, 16> vsInts = {}, psInts = {};
    std::bitset<16> vsIntsSet, psIntsSet;
    std::array<BOOL, 16> vsBools = {}, psBools = {};
    std::bitset<16> vsBoolsSet, psBoolsSet;
    std::map<uint32_t, D3DMATRIX> transforms;
    D3DMATERIAL9 material = {};
    bool materialSet = false;
    std::map<uint32_t, D3DLIGHT9> lights;
    std::map<uint32_t, BOOL> lightEnables;
    D3DVIEWPORT9 viewport = {};
    bool viewportSet = false;
    RECT scissor = {};
    bool scissorSet = false;
    std::array<std::array<float, 4>, 6> clipPlanes = {};
    std::bitset<6> clipPlanesSet;
  };

  class StateBlock final : public DeviceChild<IDirect3DStateBlock9> {
  public:
    // type: D3DSBT_ALL, PIXELSTATE, VERTEXSTATE; 0 for recorded blocks.
    StateBlock(Device* device, D3DSTATEBLOCKTYPE type);
    ~StateBlock() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE Capture() override;
    HRESULT STDMETHODCALLTYPE Apply() override;
    StateSet& set() { return m_set; }
    // Holds the objects it refers to (privately).
    void retain();
    void releaseObjects();
  private:
    StateSet m_set;
  };

  // ---- Swap chains ----

  class SwapChain final : public IDirect3DSwapChain9Ex, public RefCounted {
  public:
    SwapChain(Device* device, const D3DPRESENT_PARAMETERS& params, bool implicit);
    ~SwapChain() override;
    // The implicit swap chain counts on the device.
    void addRefPrivate() override;
    void releasePrivate() override;
    bool valid() const { return !m_backBuffers.empty(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Present(const RECT* sourceRect, const RECT* destRect, HWND window,
                                      const RGNDATA* dirty, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9* destination) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** buffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* status) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* mode) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** device) override;
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* params) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;
    HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS* stats) override;
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) override;

    Surface* backBuffer(UINT index = 0) const { return index < m_backBuffers.size() ? m_backBuffers[index] : nullptr; }
    const D3DPRESENT_PARAMETERS& params() const { return m_params; }
    HWND window() const { return m_params.hDeviceWindow; }
    void setGammaRamp(const D3DGAMMARAMP& ramp) { m_gamma = ramp; }
    const D3DGAMMARAMP& gammaRamp() const { return m_gamma; }
  private:
    void destroyBuffers();
    Device* m_device;
    bool m_implicit;
    D3DPRESENT_PARAMETERS m_params;
    std::vector<Surface*> m_backBuffers;
    std::shared_ptr<legacy::Texture> m_resolved;   // multisampled back buffers before presenting
    D3DGAMMARAMP m_gamma = {};
    UINT m_presentCount = 0;
  };

  // ---- The device ----

  class Device final : public IDirect3DDevice9Ex, public RefCounted {
  public:
    Device(Interface* parent, UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior, bool ex);
    ~Device() override;
    HRESULT init(D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* fullscreenMode);

    legacy::Device* core() const { return m_core.get(); }
    std::recursive_mutex& mutex() { return m_mutex; }
    bool multithreaded() const { return (m_behavior & D3DCREATE_MULTITHREADED) != 0; }
    ULONG addRefInternal() { return addRefPublic(); }
    ULONG releaseInternal() { return releasePublic(); }
    // State blocks being recorded take what is set.
    StateSet* recording() { return m_recording ? &m_recording->set() : nullptr; }
    D3DCAPS9 caps() const;
    void applyState(const StateSet& set);
    void captureState(StateSet& set);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return addRefPublic(); }
    ULONG STDMETHODCALLTYPE Release() override { return releasePublic(); }

    // IDirect3DDevice9
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override;
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** d3d) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* caps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT swapChain, D3DDISPLAYMODE* mode) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) override;
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* bitmap) override;
    void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) override;
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override;
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* params, IDirect3DSwapChain9** swapChain) override;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index, IDirect3DSwapChain9** swapChain) override;
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* params) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** buffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* status) override;
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) override { return D3D_OK; }
    void STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) override;
    void STDMETHODCALLTYPE GetGammaRamp(UINT swapChain, D3DGAMMARAMP* ramp) override;
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                            IDirect3DTexture9** texture, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, IDirect3DVolumeTexture9** texture, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DCubeTexture9** texture, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                 IDirect3DVertexBuffer9** buffer, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DIndexBuffer9** buffer, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                 DWORD quality, BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                        DWORD quality, BOOL discard, IDirect3DSurface9** surface, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* destination,
                                            const POINT* destPoint) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* source, IDirect3DBaseTexture9* destination) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* renderTarget, IDirect3DSurface9* destination) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT swapChain, IDirect3DSurface9* destination) override;
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* destination,
                                          const RECT* destRect, D3DTEXTUREFILTERTYPE filter) override;
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                          IDirect3DSurface9** surface, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index, IDirect3DSurface9* surface) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index, IDirect3DSurface9** surface) override;
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* surface) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** surface) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* viewport) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* viewport) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* material) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* material) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* light) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3DLIGHT9* light) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, BOOL* enable) override;
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float* plane) override;
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, float* plane) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** block) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** block) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* status) override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* status) override;
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture9** texture) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override;
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) override;
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* entries) override;
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) override { m_currentPalette = palette; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* palette) override;
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* rect) override;
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* rect) override;
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL software) override { m_softwareVertexProcessing = software; return D3D_OK; }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return m_softwareVertexProcessing; }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) override { m_nPatchSegments = segments; return D3D_OK; }
    float STDMETHODCALLTYPE GetNPatchMode() override { return m_nPatchSegments; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices,
                                                   UINT startIndex, UINT primitiveCount) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primitiveCount,
                                                     const void* indices, D3DFORMAT indexFormat, const void* vertices,
                                                     UINT stride) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT srcStart, UINT destIndex, UINT count, IDirect3DVertexBuffer9* destination,
                                              IDirect3DVertexDeclaration9* declaration, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** declaration) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* declaration) override;
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** declaration) override;
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override;
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* fvf) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* function, IDirect3DVertexShader9** shader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* shader) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** shader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, IDirect3DVertexBuffer9** buffer, UINT* offset, UINT* stride) override;
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT setting) override;
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* setting) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* indices) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** indices) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* function, IDirect3DPixelShader9** shader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* shader) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** shader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) override;
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) override;
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9** query) override;

    // IDirect3DDevice9Ex
    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT, UINT, float*, float*) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9*, IDirect3DSurface9*, IDirect3DVertexBuffer9*, UINT,
                                           IDirect3DVertexBuffer9*, D3DCOMPOSERECTSOP, int, int) override;
    HRESULT STDMETHODCALLTYPE PresentEx(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty,
                                        DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* priority) override;
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override;
    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT swapChain) override;
    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**, UINT32) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                   DWORD quality, BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared,
                                                   DWORD usage) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                            IDirect3DSurface9** surface, HANDLE* shared, DWORD usage) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT width, UINT height, D3DFORMAT format,
                                                          D3DMULTISAMPLE_TYPE multisample, DWORD quality, BOOL discard,
                                                          IDirect3DSurface9** surface, HANDLE* shared, DWORD usage) override;
    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* fullscreenMode) override;
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT swapChain, D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) override;

  private:
    void onPublicReleased() override;
    HRESULT createSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, DWORD usage, D3DMULTISAMPLE_TYPE multisample,
                          DWORD quality, IDirect3DSurface9** surface);
    HRESULT resetSwapChain(D3DPRESENT_PARAMETERS* params);
    void resetState();
    void applyFullscreen(const D3DPRESENT_PARAMETERS& params);
    legacy::PrimitiveType primitive(D3DPRIMITIVETYPE type) const { return legacy::PrimitiveType(type); }

    Interface*                      m_parent;
    UINT                            m_adapter;
    D3DDEVTYPE                      m_deviceType;
    HWND                            m_focusWindow;
    DWORD                           m_behavior;
    bool                            m_ex;
    std::recursive_mutex            m_mutex;
    std::unique_ptr<legacy::Device> m_core;
    SwapChain*                      m_swapChain = nullptr;   // implicit
    Surface*                        m_autoDepthStencil = nullptr;

    // Bindings
    std::array<Bound<Surface>, legacy::kMaxRenderTargets> m_renderTargets;
    Bound<Surface>                  m_depthStencil;
    std::array<IDirect3DBaseTexture9*, legacy::kSamplerCount> m_textures = {};
    struct Stream { Bound<VertexBuffer> buffer; UINT offset = 0, stride = 0, frequency = 1; };
    std::array<Stream, 16>          m_streams;
    Bound<IndexBuffer>              m_indices;
    Bound<VertexDeclaration>        m_declaration;
    DWORD                           m_fvf = 0;
    std::map<DWORD, VertexDeclaration*> m_fvfDeclarations;
    Bound<VertexShader>             m_vertexShader;
    Bound<PixelShader>              m_pixelShader;
    StateBlock*                     m_recording = nullptr;
    D3DCLIPSTATUS9                  m_clipStatus = {};
    UINT                            m_currentPalette = 0;
    std::map<UINT, std::array<PALETTEENTRY, 256>> m_palettes;
    BOOL                            m_softwareVertexProcessing = FALSE;
    float                           m_nPatchSegments = 0.0f;
    INT                             m_gpuThreadPriority = 0;
    UINT                            m_maximumFrameLatency = 3;
    bool                            m_inScene = false;
    std::array<std::array<float, 4>, 6> m_clipPlanes = {};
    // Cursor
    HCURSOR                         m_cursor = nullptr;
    bool                            m_cursorShown = false;
  };

  // ---- The interface ----

  class Interface final : public IDirect3D9Ex {
  public:
    explicit Interface(bool ex);
    ~Interface();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override { return D3D_OK; }
    UINT STDMETHODCALLTYPE GetAdapterCount() override;
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* identifier) override;
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* displayMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat, D3DFORMAT backBufferFormat,
                                              BOOL windowed) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                                D3DRESOURCETYPE resourceType, D3DFORMAT format) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE multisample, DWORD* qualityLevels) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                                     D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT source,
                                                          D3DFORMAT target) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
                                           D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) override;
    // IDirect3D9Ex
    UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter, UINT mode,
                                                 D3DDISPLAYMODEEX* displayMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT adapter, D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) override;
    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
                                             D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* fullscreenMode,
                                             IDirect3DDevice9Ex** device) override;
    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT adapter, LUID* luid) override;

    static D3DCAPS9 makeCaps(UINT adapter);
    // Display modes of an adapter (monitor), sorted, without duplicates.
    const std::vector<D3DDISPLAYMODEEX>& modes(UINT adapter);

  private:
    struct Adapter {
      std::wstring deviceName;     // \\.\DISPLAYn
      HMONITOR monitor = nullptr;
      DXGI_ADAPTER_DESC1 desc = {};
      LARGE_INTEGER driverVersion = {};
      std::vector<D3DDISPLAYMODEEX> modes;
      bool modesListed = false;
    };
    void enumerate();
    std::atomic<ULONG> m_ref{1};
    bool m_ex;
    std::vector<Adapter> m_adapters;
    std::mutex m_mutex;
  };

  // Formats: whether a usage of a format works.
  bool supportsFormat(D3DFORMAT format, DWORD usage, D3DRESOURCETYPE type);
  bool isDepthFormat(D3DFORMAT format);

}
