#pragma once

// DirectDraw 7 and Direct3D 7 on the legacy core (src/legacy): what the
// games of DirectX 7 (Gothic II among them) call, the rest logged.

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include "../legacy/legacy_device.h"

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dxup::ddraw {

  class DirectDraw;
  class Surface;
  class Device7;

  // Reports a method the layer does not implement, once per name.
  void logUnsupported(const char* what);

  legacy::Format formatFromPixelFormat(const DDPIXELFORMAT& pf);
  void pixelFormatFromFormat(legacy::Format format, DDPIXELFORMAT& pf);

  // ---- Clipper ----

  class Clipper final : public IDirectDrawClipper {
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetClipList(LPRECT rect, LPRGNDATA data, LPDWORD size) override;
    HRESULT STDMETHODCALLTYPE GetHWnd(HWND* window) override;
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE IsClipListChanged(BOOL* changed) override;
    HRESULT STDMETHODCALLTYPE SetClipList(LPRGNDATA, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetHWnd(DWORD, HWND window) override { m_window = window; return DD_OK; }
    HWND window() const { return m_window; }
  private:
    std::atomic<ULONG> m_ref{1};
    HWND m_window = nullptr;
  };

  // ---- Palette ----

  class Palette final : public IDirectDrawPalette {
  public:
    explicit Palette(DWORD caps, const PALETTEENTRY* entries);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPDWORD caps) override;
    HRESULT STDMETHODCALLTYPE GetEntries(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY entries) override;
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE SetEntries(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY entries) override;
    // 0xAARRGGBB, alpha from the entries' flags when the palette has alpha.
    const uint32_t* argb() const { return m_argb; }
    uint32_t version() const { return m_version; }
  private:
    std::atomic<ULONG> m_ref{1};
    DWORD m_caps;
    PALETTEENTRY m_entries[256] = {};
    uint32_t m_argb[256] = {};
    uint32_t m_version = 0;
  };

  // ---- DirectDraw 1 interfaces ----
  // Our objects under their DirectDraw 1 interfaces, for components that ask
  // for them (DirectShow's multimedia streams make their video samples with
  // the game's DirectDraw): the same objects and references.

  class Surface1 final : public IDirectDrawSurface {
  public:
    explicit Surface1(Surface* owner) : m_owner(owner) {
      s_vtable = *reinterpret_cast<void* const*>(static_cast<IDirectDrawSurface*>(this));
    }
    Surface* owner() const { return m_owner; }
    // Ours, or null for another DirectDraw's surface.
    static Surface1* fromInterface(IDirectDrawSurface* surface);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE surface) override;
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override;
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD flags, LPDIRECTDRAWSURFACE surface) override;
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID context, LPDDENUMSURFACESCALLBACK callback) override;
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE target, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS caps, LPDIRECTDRAWSURFACE* surface) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS caps) override;
    HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER* clipper) override;
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD flags, LPDDCOLORKEY key) override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* dc) override;
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE* palette) override;
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT format) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC desc) override;
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Lock(LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE event) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER clipper) override;
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD flags, LPDDCOLORKEY key) override;
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE palette) override;
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID) override;
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) override { return DDERR_NOTAOVERLAYSURFACE; }
  private:
    Surface* m_owner;
    static inline const void* s_vtable = nullptr;
  };

  class DirectDraw1 final : public IDirectDraw {
  public:
    explicit DirectDraw1(DirectDraw* owner) : m_owner(owner) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* clipper, IUnknown* outer) override;
    HRESULT STDMETHODCALLTYPE CreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* palette, IUnknown* outer) override;
    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* surface, IUnknown* outer) override;
    HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD flags, LPDDSURFACEDESC desc, LPVOID context, LPDDENUMMODESCALLBACK callback) override;
    HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS driverCaps, LPDDCAPS helCaps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC desc) override;
    HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD count, LPDWORD codes) override;
    HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE*) override { return DDERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD frequency) override;
    HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD line) override;
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL status) override;
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override;
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD width, DWORD height, DWORD bpp) override;
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD flags, HANDLE event) override;
  private:
    DirectDraw* m_owner;
  };

  // ---- Surface ----

  class Surface final : public IDirectDrawSurface7, public IDirectDrawGammaControl {
  public:
    Surface(DirectDraw* dd, const DDSURFACEDESC2& desc, std::shared_ptr<legacy::Texture> texture, uint32_t subresource,
            Surface* owner);
    ~Surface();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IDirectDrawSurface7
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE7 surface) override;
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override;
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD flags, LPDIRECTDRAWSURFACE7 surface) override;
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID context, LPDDENUMSURFACESCALLBACK7 callback) override;
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE7 target, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* surface) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS2 caps) override;
    HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER* clipper) override;
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD flags, LPDDCOLORKEY key) override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* dc) override;
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE* palette) override;
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT format) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC2 desc) override;
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Lock(LPRECT rect, LPDDSURFACEDESC2 desc, DWORD flags, HANDLE) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER clipper) override;
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD flags, LPDDCOLORKEY key) override;
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE palette) override;
    HRESULT STDMETHODCALLTYPE Unlock(LPRECT rect) override;
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE7) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE GetDDInterface(LPVOID* dd) override;
    HRESULT STDMETHODCALLTYPE PageLock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, LPVOID data, DWORD size, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, LPVOID data, LPDWORD size) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetUniquenessValue(LPDWORD value) override;
    HRESULT STDMETHODCALLTYPE ChangeUniquenessValue() override { ++m_uniqueness; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetPriority(DWORD priority) override { m_priority = priority; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LPDWORD priority) override;
    HRESULT STDMETHODCALLTYPE SetLOD(DWORD lod) override { m_lod = lod; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetLOD(LPDWORD lod) override;
    // IDirectDrawGammaControl
    HRESULT STDMETHODCALLTYPE GetGammaRamp(DWORD flags, LPDDGAMMARAMP ramp) override;
    HRESULT STDMETHODCALLTYPE SetGammaRamp(DWORD flags, LPDDGAMMARAMP ramp) override;

    legacy::Texture* texture() const { return m_texture.get(); }
    uint32_t subresource() const { return m_subresource; }
    const DDSURFACEDESC2& desc() const { return m_desc; }
    bool isPrimary() const { return (m_desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0; }
    Surface* attachedDepth() const;
    void addChild(Surface* child) { m_attached.push_back(child); }
    void addCaps(DWORD caps) { m_desc.ddsCaps.dwCaps |= caps; }
    // The texture for the color key the device's state uses.
    void applyColorKey(bool enabled);
    // P8 textures: the attached palette's entries, when they changed.
    void updateTexturePalette();
    // Ours, or null for a surface of another DirectDraw (Windows' own).
    static Surface* fromInterface(IDirectDrawSurface7* surface);
    IDirectDrawSurface* surface1() { return &m_surface1; }

  private:
    HRESULT blt(LPRECT dstRect, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx);
    Surface* stageForeign(IDirectDrawSurface7* foreign);
    HRESULT presentTo(const RECT* screenRect, Surface* source, const RECT* sourceRect, DWORD syncInterval);
    // A surface's texture as the GPU samples it: application memory (and
    // the palette of P8 surfaces) goes through m_gpuStage. Null if none.
    legacy::Texture* gpuSource(Surface* source, uint32_t& subresource);
    // The window a windowed primary shows in (its clipper's, else the
    // cooperative level's).
    HWND targetWindow() const;
    // Windowed primary: a blit (or colour fill) into the picture of the
    // window's client area at its screen rectangle, then shown.
    HRESULT composeWindowed(const RECT* dstRect, Surface* source, const RECT* srcRect, bool colorKey, uint32_t key,
                            bool fill, uint32_t fillArgb);
    // Shows the composed picture: at once for blits covering the window,
    // else at most once per refresh (the rest on the next blit or timer).
    HRESULT presentComposed(bool whole);
    HRESULT presentPendingComposition(DWORD syncInterval = 0);
    static void CALLBACK presentTimer(HWND window, UINT, UINT_PTR id, DWORD);
    void stopPresentTimer();

    DirectDraw* m_dd;
    DDSURFACEDESC2 m_desc;
    std::shared_ptr<legacy::Texture> m_texture;
    uint32_t m_subresource;
    Surface* m_owner;                      // complex surface this one is part of
    std::vector<Surface*> m_attached;      // children (owned) and attached z-buffers (referenced)
    std::vector<Surface*> m_attachedRefs;  // attached with AddAttachedSurface
    std::atomic<ULONG> m_ref{0};
    Clipper* m_clipper = nullptr;
    Palette* m_palette = nullptr;
    uint32_t m_paletteVersion = UINT32_MAX;
    DDCOLORKEY m_srcBltKey = {};
    bool m_hasSrcBltKey = false;
    DWORD m_priority = 0, m_lod = 0, m_uniqueness = 1;
    std::unordered_map<std::string, std::vector<uint8_t>> m_privateData;
    // GetDC
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    void* m_dcBits = nullptr;
    std::vector<uint32_t> m_dcOriginal;   // the DIB as GetDC filled it: ReleaseDC writes back what changed
    DDGAMMARAMP m_gamma = {};
    // Pixels of other DirectDraws' surfaces blitted into this one.
    Surface* m_foreignStage = nullptr;
    // Application memory as a GPU texture (gpuSource).
    std::shared_ptr<legacy::Texture> m_gpuStage;
    // Windowed primary: the window's picture, and when it was last shown.
    std::shared_ptr<legacy::Texture> m_composed;
    HWND m_composedWindow = nullptr;
    int64_t m_lastPresent = 0;
    int64_t m_refreshTicks = 0;
    bool m_presentPending = false;
    HWND m_timerWindow = nullptr;
    Surface1 m_surface1{this};
    static inline const void* s_vtable = nullptr;
  };

  // ---- Vertex buffer ----

  class VertexBuffer7 final : public IDirect3DVertexBuffer7 {
  public:
    VertexBuffer7(DirectDraw* dd, const D3DVERTEXBUFFERDESC& desc);
    ~VertexBuffer7();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Lock(DWORD flags, LPVOID* data, LPDWORD size) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(DWORD op, DWORD dstIndex, DWORD count, LPDIRECT3DVERTEXBUFFER7 src,
                                              DWORD srcIndex, LPDIRECT3DDEVICE7 device, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetVertexBufferDesc(LPD3DVERTEXBUFFERDESC desc) override;
    HRESULT STDMETHODCALLTYPE Optimize(LPDIRECT3DDEVICE7, DWORD) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE ProcessVerticesStrided(DWORD, DWORD, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD,
                                                     LPDIRECT3DDEVICE7, DWORD) override;
    legacy::Buffer* buffer() const { return m_buffer.get(); }
    uint32_t fvf() const { return m_desc.dwFVF; }
    uint32_t stride() const { return m_stride; }
  private:
    DirectDraw* m_dd;
    D3DVERTEXBUFFERDESC m_desc;
    uint32_t m_stride;
    std::unique_ptr<legacy::Buffer> m_buffer;
    std::atomic<ULONG> m_ref{1};
  };

  // ---- Device ----

  class Device7 final : public IDirect3DDevice7 {
  public:
    Device7(DirectDraw* dd, Surface* target, const GUID& type);
    ~Device7();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPD3DDEVICEDESC7 desc) override;
    HRESULT STDMETHODCALLTYPE EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(LPDIRECT3D7* d3d) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(LPDIRECTDRAWSURFACE7 surface, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(LPDIRECTDRAWSURFACE7* surface) override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, LPD3DMATRIX matrix) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, LPD3DMATRIX matrix) override;
    HRESULT STDMETHODCALLTYPE SetViewport(LPD3DVIEWPORT7 viewport) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, LPD3DMATRIX matrix) override;
    HRESULT STDMETHODCALLTYPE GetViewport(LPD3DVIEWPORT7 viewport) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(LPD3DMATERIAL7 material) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(LPD3DMATERIAL7 material) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, LPD3DLIGHT7 light) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD index, LPD3DLIGHT7 light) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, LPDWORD value) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(LPDWORD handle) override;
    HRESULT STDMETHODCALLTYPE PreLoad(LPDIRECTDRAWSURFACE7) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID vertices, DWORD count, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID vertices, DWORD vertexCount,
                                                   LPWORD indices, DWORD indexCount, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(LPD3DCLIPSTATUS status) override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(LPD3DCLIPSTATUS status) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
                                                   DWORD count, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
                                                          DWORD vertexCount, LPWORD indices, DWORD indexCount, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count,
                                              DWORD flags) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start,
                                                     DWORD count, LPWORD indices, DWORD indexCount, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE ComputeSphereVisibility(LPD3DVECTOR, LPD3DVALUE, DWORD count, DWORD, LPDWORD results) override;
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, LPDIRECTDRAWSURFACE7* surface) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, LPDIRECTDRAWSURFACE7 surface) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, LPDWORD value) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(LPDWORD passes) override;
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, LPDWORD handle) override;
    HRESULT STDMETHODCALLTYPE Load(LPDIRECTDRAWSURFACE7 dst, LPPOINT dstPoint, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect,
                                   DWORD flags) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, BOOL* enable) override;
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, D3DVALUE* plane) override;
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, D3DVALUE* plane) override;
    HRESULT STDMETHODCALLTYPE GetInfo(DWORD, LPVOID, DWORD) override { return S_FALSE; }

  private:
    struct StateBlock;
    void bindTargets();
    void prepareDraw();
    // The states a block of the type holds (as Windows' Direct3D 7), with
    // the device's values.
    StateBlock capture(D3DSTATEBLOCKTYPE type);
    // The block's states take the device's values (lights that exist).
    void refresh(StateBlock& block);
    void apply(const StateBlock& block);
    bool lightExists(DWORD index) const { return index < m_lightSet.size() && m_lightSet[index]; }

    DirectDraw* m_dd;
    legacy::Device* m_device;
    Surface* m_target = nullptr;
    Surface* m_textures[8] = {};
    std::vector<bool> m_lightSet;   // lights set or enabled (others do not exist)
    std::atomic<ULONG> m_ref{1};
    D3DCLIPSTATUS m_clipStatus = {};
    float m_clipPlanes[6][4] = {};
    bool m_colorKeyEnable = false;
    std::vector<std::unique_ptr<StateBlock>> m_stateBlocks;
    std::unique_ptr<StateBlock> m_recording;
    GUID m_type;
  };

  // ---- DirectDraw and Direct3D ----

  class Direct3D7 final : public IDirect3D7 {
  public:
    explicit Direct3D7(DirectDraw* dd) : m_dd(dd) { }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE EnumDevices(LPD3DENUMDEVICESCALLBACK7 callback, LPVOID context) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID type, LPDIRECTDRAWSURFACE7 surface, LPDIRECT3DDEVICE7* device) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(LPD3DVERTEXBUFFERDESC desc, LPDIRECT3DVERTEXBUFFER7* vb, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE EnumZBufferFormats(REFCLSID type, LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) override;
    HRESULT STDMETHODCALLTYPE EvictManagedTextures() override { return D3D_OK; }
  private:
    DirectDraw* m_dd;
  };

  class DirectDraw final : public IDirectDraw7 {
  public:
    DirectDraw();
    ~DirectDraw();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* clipper, IUnknown* outer) override;
    HRESULT STDMETHODCALLTYPE CreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* palette, IUnknown* outer) override;
    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* surface, IUnknown* outer) override;
    HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD flags, LPDDSURFACEDESC2 desc, LPVOID context, LPDDENUMMODESCALLBACK2 callback) override;
    HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS driverCaps, LPDDCAPS helCaps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC2 desc) override;
    HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD count, LPDWORD codes) override;
    HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE7*) override { return DDERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD frequency) override;
    HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD line) override { if (line) *line = 0; return DDERR_VERTICALBLANKINPROGRESS; }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL status) override { if (status) *status = TRUE; return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override;
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD width, DWORD height, DWORD bpp, DWORD refresh, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS2 caps, LPDWORD total, LPDWORD free) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceFromDC(HDC, LPDIRECTDRAWSURFACE7*) override { return DDERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE RestoreAllSurfaces() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 identifier, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE StartModeTest(LPSIZE, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EvaluateMode(DWORD, DWORD*) override { return DDERR_UNSUPPORTED; }

    // For the other objects.
    legacy::Device* legacyDevice();
    Direct3D7* direct3d() { return &m_d3d; }
    HWND window() const { return m_window; }
    bool fullscreen() const { return m_fullscreen; }
    DWORD cooperativeFlags() const { return m_cooperativeFlags; }
    uint32_t modeWidth() const;
    uint32_t modeHeight() const;
    // 16 or 32 (SetDisplayMode refuses others).
    uint32_t modeBpp() const { return m_modeBpp ? m_modeBpp : 32; }
    legacy::Format displayFormat() const { return modeBpp() == 16 ? legacy::FormatR5G6B5 : legacy::FormatX8R8G8B8; }

  private:
    void coverMonitor();
    std::atomic<ULONG> m_ref{1};
    Direct3D7 m_d3d{this};
    DirectDraw1 m_dd1{this};
    std::unique_ptr<legacy::Device> m_legacy;
    HWND m_window = nullptr;
    DWORD m_cooperativeFlags = 0;
    bool m_fullscreen = false;
    DWORD m_modeWidth = 0, m_modeHeight = 0, m_modeBpp = 0;
    LONG m_savedStyle = 0;
    RECT m_savedRect = {};
  };

}
