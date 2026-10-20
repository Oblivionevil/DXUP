#pragma once

// Direct3D 8 as wrappers of DXUP's Direct3D 9 objects: each Direct3D 8
// object holds one reference to its Direct3D 9 twin and counts its own;
// the device finds the wrapper of a twin (GetTexture, GetRenderTarget).
// Vertex shaders of Direct3D 8 become a declaration and a vs_1_1 shader
// with dcl instructions for the registers their declaration loads.

#include "d3d8_types.h"

#include <d3d9.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxup::d3d8 {

  class Interface8;
  class Device8;

  void logUnsupported(const char* what);
  D3DPRESENT_PARAMETERS toParams9(const D3DPRESENT_PARAMETERS8& params);
  void fromParams9(const D3DPRESENT_PARAMETERS& params9, D3DPRESENT_PARAMETERS8& params);
  UINT surfaceSize(D3DFORMAT format, UINT width, UINT height, UINT depth = 1);

  // ---- Wrappers ----

  template<typename Iface8, typename Iface9>
  class Wrapper : public Iface8 {
  public:
    Wrapper(Device8* device, Iface9* object);
    virtual ~Wrapper();
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
      const ULONG count = --m_ref;
      if (!count)
        delete this;
      return count;
    }
    Iface9* d3d9() const { return m_object; }
    // A reference unless the last one is gone (the wrapper is going away).
    bool tryAddRef() {
      ULONG count = m_ref.load();
      while (count)
        if (m_ref.compare_exchange_weak(count, count + 1))
          return true;
      return false;
    }
  protected:
    std::atomic<ULONG> m_ref{1};
    Device8* m_device;
    Iface9* m_object;
  };

  template<typename Iface8, typename Iface9>
  class ResourceWrapper : public Wrapper<Iface8, Iface9> {
  public:
    using Wrapper<Iface8, Iface9>::Wrapper;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** device) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
      return this->m_object->SetPrivateData(guid, data, size, flags);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
      return this->m_object->GetPrivateData(guid, data, size);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override { return this->m_object->FreePrivateData(guid); }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override { return this->m_object->SetPriority(priority); }
    DWORD STDMETHODCALLTYPE GetPriority() override { return this->m_object->GetPriority(); }
    void STDMETHODCALLTYPE PreLoad() override { this->m_object->PreLoad(); }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return this->m_object->GetType(); }
  };

  class Surface8 final : public Wrapper<IDirect3DSurface8, IDirect3DSurface9> {
  public:
    using Wrapper::Wrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** device) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
      return m_object->SetPrivateData(guid, data, size, flags);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
      return m_object->GetPrivateData(guid, data, size);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override { return m_object->FreePrivateData(guid); }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC8* desc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override {
      return m_object->LockRect(locked, rect, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockRect() override { return m_object->UnlockRect(); }
  };

  class Volume8 final : public Wrapper<IDirect3DVolume8, IDirect3DVolume9> {
  public:
    using Wrapper::Wrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** device) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
      return m_object->SetPrivateData(guid, data, size, flags);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
      return m_object->GetPrivateData(guid, data, size);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override { return m_object->FreePrivateData(guid); }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC8* desc) override;
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override {
      return m_object->LockBox(locked, box, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockBox() override { return m_object->UnlockBox(); }
  };

  template<typename Iface8, typename Iface9>
  class TextureWrapper : public ResourceWrapper<Iface8, Iface9> {
  public:
    using ResourceWrapper<Iface8, Iface9>::ResourceWrapper;
    DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override { return this->m_object->SetLOD(lod); }
    DWORD STDMETHODCALLTYPE GetLOD() override { return this->m_object->GetLOD(); }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return this->m_object->GetLevelCount(); }
  };

  class Texture8 final : public TextureWrapper<IDirect3DTexture8, IDirect3DTexture9> {
  public:
    using TextureWrapper::TextureWrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override {
      return m_object->LockRect(level, locked, rect, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override { return m_object->UnlockRect(level); }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* rect) override { return m_object->AddDirtyRect(rect); }
  };

  class CubeTexture8 final : public TextureWrapper<IDirect3DCubeTexture8, IDirect3DCubeTexture9> {
  public:
    using TextureWrapper::TextureWrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                                       DWORD flags) override {
      return m_object->LockRect(face, level, locked, rect, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override { return m_object->UnlockRect(face, level); }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) override {
      return m_object->AddDirtyRect(face, rect);
    }
  };

  class VolumeTexture8 final : public TextureWrapper<IDirect3DVolumeTexture8, IDirect3DVolumeTexture9> {
  public:
    using TextureWrapper::TextureWrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DVOLUME_DESC8* desc) override;
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, IDirect3DVolume8** volume) override;
    HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override {
      return m_object->LockBox(level, locked, box, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) override { return m_object->UnlockBox(level); }
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX* box) override { return m_object->AddDirtyBox(box); }
  };

  class VertexBuffer8 final : public ResourceWrapper<IDirect3DVertexBuffer8, IDirect3DVertexBuffer9> {
  public:
    using ResourceWrapper::ResourceWrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override {
      return m_object->Lock(offset, size, reinterpret_cast<void**>(data), flags);
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { return m_object->Unlock(); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* desc) override { return m_object->GetDesc(desc); }
  };

  class IndexBuffer8 final : public ResourceWrapper<IDirect3DIndexBuffer8, IDirect3DIndexBuffer9> {
  public:
    using ResourceWrapper::ResourceWrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override {
      return m_object->Lock(offset, size, reinterpret_cast<void**>(data), flags);
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { return m_object->Unlock(); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* desc) override { return m_object->GetDesc(desc); }
  };

  class SwapChain8 final : public Wrapper<IDirect3DSwapChain8, IDirect3DSwapChain9> {
  public:
    using Wrapper::Wrapper;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty) override {
      return m_object->Present(sourceRect, destRect, window, dirty, 0);
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** buffer) override;
  };

  // ---- The device ----

  class Device8 final : public IDirect3DDevice8 {
  public:
    Device8(Interface8* parent, IDirect3DDevice9* device);
    ~Device8();

    // The wrapper of a Direct3D 9 object, made the first time; it gets a
    // reference (the object's is taken over or released).
    template<typename W, typename Iface9>
    W* wrap(Iface9* object);
    void forget(IUnknown* object, IUnknown* wrapper);
    IDirect3DDevice9* d3d9() const { return m_device; }
    void addRefInternal() { ++m_internal; }
    void releaseInternal();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return m_device->TestCooperativeLevel(); }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return m_device->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD) override { return m_device->EvictManagedResources(); }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** d3d) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8* caps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* mode) override { return m_device->GetDisplayMode(0, mode); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) override {
      return m_device->GetCreationParameters(params);
    }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* bitmap) override;
    void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) override { m_device->SetCursorPosition(x, y, flags); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override { return m_device->ShowCursor(show); }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* params, IDirect3DSwapChain8** swapChain) override;
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS8* params) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty) override {
      return m_device->Present(sourceRect, destRect, window, dirty);
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** buffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* status) override { return m_device->GetRasterStatus(0, status); }
    void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3DGAMMARAMP* ramp) override { m_device->SetGammaRamp(0, flags, ramp); }
    void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP* ramp) override { m_device->GetGammaRamp(0, ramp); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                            IDirect3DTexture8** texture) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, IDirect3DVolumeTexture8** texture) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DCubeTexture8** texture) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                 IDirect3DVertexBuffer8** buffer) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DIndexBuffer8** buffer) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                 BOOL lockable, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                        IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface) override;
    HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8* source, const RECT* sourceRects, UINT count,
                                        IDirect3DSurface8* destination, const POINT* destPoints) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination) override;
    HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8* destination) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8* renderTarget, IDirect3DSurface8* depthStencil) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8** renderTarget) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8** depthStencil) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override { return m_device->BeginScene(); }
    HRESULT STDMETHODCALLTYPE EndScene() override { return m_device->EndScene(); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override {
      return m_device->Clear(count, rects, flags, color, z, stencil);
    }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override {
      return m_device->SetTransform(state, matrix);
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) override {
      return m_device->GetTransform(state, matrix);
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override {
      return m_device->MultiplyTransform(state, matrix);
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* viewport) override { return m_device->SetViewport(viewport); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* viewport) override { return m_device->GetViewport(viewport); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* material) override { return m_device->SetMaterial(material); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* material) override { return m_device->GetMaterial(material); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* light) override { return m_device->SetLight(index, light); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3DLIGHT9* light) override { return m_device->GetLight(index, light); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override { return m_device->LightEnable(index, enable); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, BOOL* enable) override { return m_device->GetLightEnable(index, enable); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float* plane) override { return m_device->SetClipPlane(index, plane); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, float* plane) override { return m_device->GetClipPlane(index, plane); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* token) override;
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD token) override;
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD token) override;
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD token) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* status) override { return m_device->SetClipStatus(status); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* status) override { return m_device->GetClipStatus(status); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture8** texture) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture8* texture) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override { return m_device->ValidateDevice(passes); }
    HRESULT STDMETHODCALLTYPE GetInfo(DWORD, void*, DWORD) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) override {
      return m_device->SetPaletteEntries(palette, entries);
    }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* entries) override {
      return m_device->GetPaletteEntries(palette, entries);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) override { return m_device->SetCurrentTexturePalette(palette); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* palette) override { return m_device->GetCurrentTexturePalette(palette); }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) override {
      return m_device->DrawPrimitive(type, startVertex, primitiveCount);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT startIndex,
                                                   UINT primitiveCount) override {
      return m_device->DrawIndexedPrimitive(type, INT(m_baseVertexIndex), minIndex, numVertices, startIndex, primitiveCount);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primitiveCount,
                                                     const void* indices, D3DFORMAT indexFormat, const void* vertices,
                                                     UINT stride) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT sourceStart, UINT destIndex, UINT count, IDirect3DVertexBuffer8* destination,
                                              DWORD flags) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle,
                                                 DWORD usage) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* handle) override;
    HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) override {
      return m_device->SetVertexShaderConstantF(reg, static_cast<const float*>(data), count);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD reg, void* data, DWORD count) override {
      return m_device->GetVertexShaderConstantF(reg, static_cast<float*>(data), count);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD handle, void* data, DWORD* size) override;
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer8* buffer, UINT stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, IDirect3DVertexBuffer8** buffer, UINT* stride) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8* indices, UINT baseVertexIndex) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8** indices, UINT* baseVertexIndex) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* function, DWORD* handle) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* handle) override;
    HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) override {
      return m_device->SetPixelShaderConstantF(reg, static_cast<const float*>(data), count);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD reg, void* data, DWORD count) override {
      return m_device->GetPixelShaderConstantF(reg, static_cast<float*>(data), count);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD handle, void* data, DWORD* size) override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) override;
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) override;
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override { return D3DERR_INVALIDCALL; }

  private:
    struct VertexShader {
      std::vector<DWORD> declaration;     // with the end token
      std::vector<DWORD> function;        // empty: fixed function
      IDirect3DVertexDeclaration9* decl = nullptr;
      IDirect3DVertexShader9* shader = nullptr;
      std::vector<std::pair<DWORD, std::array<float, 4>>> constants;   // loaded with the shader
    };
    struct PixelShader {
      std::vector<DWORD> function;
      IDirect3DPixelShader9* shader = nullptr;
    };
    IDirect3DBaseTexture8* wrapTexture(IDirect3DBaseTexture9* texture);

    std::atomic<ULONG> m_ref{1};
    std::atomic<ULONG> m_internal{0};
    Interface8* m_parent;
    IDirect3DDevice9* m_device;
    std::recursive_mutex m_mutex;
    std::unordered_map<IUnknown*, IUnknown*> m_wrappers;   // Direct3D 9 object -> wrapper
    std::vector<std::unique_ptr<VertexShader>> m_vertexShaders;
    std::vector<std::unique_ptr<PixelShader>> m_pixelShaders;
    // Direct3D 8 state Direct3D 9's state blocks do not hold: the base
    // vertex index of SetIndices and the shader handles.
    struct StateBlock8 {
      IDirect3DStateBlock9* block = nullptr;
      bool hasIndices = false, hasVertexShader = false, hasPixelShader = false;
      UINT baseVertexIndex = 0;
      DWORD vertexShader = 0, pixelShader = 0;
    };
    std::vector<StateBlock8> m_stateBlocks;
    std::unique_ptr<StateBlock8> m_recording;   // between BeginStateBlock and EndStateBlock
    DWORD m_vertexShaderHandle = 0;
    DWORD m_pixelShaderHandle = 0;
    UINT m_baseVertexIndex = 0;
    UINT m_streamStrides[16] = {};
    std::map<DWORD, DWORD> m_droppedRenderStates;
  };

  // ---- The interface ----

  class Interface8 final : public IDirect3D8 {
  public:
    explicit Interface8(IDirect3D9* d3d9);
    ~Interface8();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override { return D3D_OK; }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_d3d9->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier) override;
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* displayMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) override {
      return m_d3d9->GetAdapterDisplayMode(adapter, mode);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat, D3DFORMAT backBufferFormat,
                                              BOOL windowed) override {
      return m_d3d9->CheckDeviceType(adapter, type, displayFormat, backBufferFormat, windowed);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                                D3DRESOURCETYPE resourceType, D3DFORMAT format) override {
      return m_d3d9->CheckDeviceFormat(adapter, type, adapterFormat, usage, resourceType, format);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE multisample) override {
      return m_d3d9->CheckDeviceMultiSampleType(adapter, type, format, windowed, multisample, nullptr);
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                                     D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat) override {
      return m_d3d9->CheckDepthStencilMatch(adapter, type, adapterFormat, renderTargetFormat, depthStencilFormat);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* caps) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override { return m_d3d9->GetAdapterMonitor(adapter); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
                                           D3DPRESENT_PARAMETERS8* params, IDirect3DDevice8** device) override;
    static void toCaps8(const D3DCAPS9& caps9, D3DCAPS8& caps);
  private:
    std::atomic<ULONG> m_ref{1};
    IDirect3D9* m_d3d9;
  };

}
