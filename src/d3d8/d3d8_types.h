#pragma once

// The interfaces and structures of Direct3D 8 that differ from Direct3D 9
// (the Windows SDK has no d3d8.h any more). Types they share - formats,
// matrices, lights, materials, rectangles - are d3d9types.h's.

#include <windows.h>
#include <unknwn.h>
#include <d3d9types.h>
#include <d3d9caps.h>

struct IDirect3D8;
struct IDirect3DDevice8;
struct IDirect3DResource8;
struct IDirect3DBaseTexture8;
struct IDirect3DTexture8;
struct IDirect3DVolumeTexture8;
struct IDirect3DCubeTexture8;
struct IDirect3DVertexBuffer8;
struct IDirect3DIndexBuffer8;
struct IDirect3DSurface8;
struct IDirect3DVolume8;
struct IDirect3DSwapChain8;

#define D3D8_SDK_VERSION 220

#ifndef D3DCREATE_FPU_PRESERVE
#define D3DCREATE_FPU_PRESERVE               0x00000002L
#define D3DCREATE_MULTITHREADED              0x00000004L
#define D3DCREATE_PUREDEVICE                 0x00000010L
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING  0x00000020L
#define D3DCREATE_HARDWARE_VERTEXPROCESSING  0x00000040L
#define D3DCREATE_MIXED_VERTEXPROCESSING     0x00000080L
#endif

// ---- Structures ----

struct D3DPRESENT_PARAMETERS8 {
  UINT                BackBufferWidth;
  UINT                BackBufferHeight;
  D3DFORMAT           BackBufferFormat;
  UINT                BackBufferCount;
  D3DMULTISAMPLE_TYPE MultiSampleType;
  D3DSWAPEFFECT       SwapEffect;
  HWND                hDeviceWindow;
  BOOL                Windowed;
  BOOL                EnableAutoDepthStencil;
  D3DFORMAT           AutoDepthStencilFormat;
  DWORD               Flags;
  UINT                FullScreen_RefreshRateInHz;
  UINT                FullScreen_PresentationInterval;
};

// D3DSWAPEFFECT_COPY_VSYNC of Direct3D 8.
constexpr D3DSWAPEFFECT D3DSWAPEFFECT_COPY_VSYNC8 = D3DSWAPEFFECT(4);

struct D3DADAPTER_IDENTIFIER8 {
  char          Driver[MAX_DEVICE_IDENTIFIER_STRING];
  char          Description[MAX_DEVICE_IDENTIFIER_STRING];
  LARGE_INTEGER DriverVersion;
  DWORD         VendorId;
  DWORD         DeviceId;
  DWORD         SubSysId;
  DWORD         Revision;
  GUID          DeviceIdentifier;
  DWORD         WHQLLevel;
};

struct D3DSURFACE_DESC8 {
  D3DFORMAT           Format;
  D3DRESOURCETYPE     Type;
  DWORD               Usage;
  D3DPOOL             Pool;
  UINT                Size;
  D3DMULTISAMPLE_TYPE MultiSampleType;
  UINT                Width;
  UINT                Height;
};

struct D3DVOLUME_DESC8 {
  D3DFORMAT       Format;
  D3DRESOURCETYPE Type;
  DWORD           Usage;
  D3DPOOL         Pool;
  UINT            Size;
  UINT            Width;
  UINT            Height;
  UINT            Depth;
};

// D3DCAPS8: the start of D3DCAPS9 (MaxPixelShaderValue is PixelShader1xMaxValue).
struct D3DCAPS8 {
  D3DDEVTYPE DeviceType;
  UINT       AdapterOrdinal;
  DWORD      Caps;
  DWORD      Caps2;
  DWORD      Caps3;
  DWORD      PresentationIntervals;
  DWORD      CursorCaps;
  DWORD      DevCaps;
  DWORD      PrimitiveMiscCaps;
  DWORD      RasterCaps;
  DWORD      ZCmpCaps;
  DWORD      SrcBlendCaps;
  DWORD      DestBlendCaps;
  DWORD      AlphaCmpCaps;
  DWORD      ShadeCaps;
  DWORD      TextureCaps;
  DWORD      TextureFilterCaps;
  DWORD      CubeTextureFilterCaps;
  DWORD      VolumeTextureFilterCaps;
  DWORD      TextureAddressCaps;
  DWORD      VolumeTextureAddressCaps;
  DWORD      LineCaps;
  DWORD      MaxTextureWidth, MaxTextureHeight;
  DWORD      MaxVolumeExtent;
  DWORD      MaxTextureRepeat;
  DWORD      MaxTextureAspectRatio;
  DWORD      MaxAnisotropy;
  float      MaxVertexW;
  float      GuardBandLeft;
  float      GuardBandTop;
  float      GuardBandRight;
  float      GuardBandBottom;
  float      ExtentsAdjust;
  DWORD      StencilCaps;
  DWORD      FVFCaps;
  DWORD      TextureOpCaps;
  DWORD      MaxTextureBlendStages;
  DWORD      MaxSimultaneousTextures;
  DWORD      VertexProcessingCaps;
  DWORD      MaxActiveLights;
  DWORD      MaxUserClipPlanes;
  DWORD      MaxVertexBlendMatrices;
  DWORD      MaxVertexBlendMatrixIndex;
  float      MaxPointSize;
  DWORD      MaxPrimitiveCount;
  DWORD      MaxVertexIndex;
  DWORD      MaxStreams;
  DWORD      MaxStreamStride;
  DWORD      VertexShaderVersion;
  DWORD      MaxVertexShaderConst;
  DWORD      PixelShaderVersion;
  float      MaxPixelShaderValue;
};

// Texture stage states of Direct3D 8 that Direct3D 9 made sampler states.
enum : DWORD {
  D3DTSS8_ADDRESSU = 13, D3DTSS8_ADDRESSV = 14, D3DTSS8_BORDERCOLOR = 15, D3DTSS8_MAGFILTER = 16,
  D3DTSS8_MINFILTER = 17, D3DTSS8_MIPFILTER = 18, D3DTSS8_MIPMAPLODBIAS = 19, D3DTSS8_MAXMIPLEVEL = 20,
  D3DTSS8_MAXANISOTROPY = 21, D3DTSS8_ADDRESSW = 25,
};

// Render states of Direct3D 8 that Direct3D 9 dropped.
enum : DWORD {
  D3DRS8_LINEPATTERN = 10, D3DRS8_ZVISIBLE = 30, D3DRS8_EDGEANTIALIAS = 40, D3DRS8_ZBIAS = 47,
  D3DRS8_SOFTWAREVERTEXPROCESSING = 153, D3DRS8_PATCHSEGMENTS = 164,
};

// ---- Interfaces ----

struct __declspec(uuid("1DD9E8DA-1C77-4D40-B0CF-98FEFDFF9512")) IDirect3D8 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* initializeFunction) = 0;
  virtual UINT STDMETHODCALLTYPE GetAdapterCount() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier) = 0;
  virtual UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* displayMode) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) = 0;
  virtual HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat,
                                                    D3DFORMAT backBufferFormat, BOOL windowed) = 0;
  virtual HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                                      D3DRESOURCETYPE resourceType, D3DFORMAT checkFormat) = 0;
  virtual HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT surfaceFormat,
                                                               BOOL windowed, D3DMULTISAMPLE_TYPE multisample) = 0;
  virtual HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                                           D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* caps) = 0;
  virtual HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
                                                 D3DPRESENT_PARAMETERS8* params, IDirect3DDevice8** device) = 0;
};

struct __declspec(uuid("1B36BB7B-09B7-410A-B445-7D1430D7B33F")) IDirect3DResource8 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** device) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) = 0;
  virtual DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) = 0;
  virtual DWORD STDMETHODCALLTYPE GetPriority() = 0;
  virtual void STDMETHODCALLTYPE PreLoad() = 0;
  virtual D3DRESOURCETYPE STDMETHODCALLTYPE GetType() = 0;
};

struct __declspec(uuid("B4211CFA-51B9-4A9F-AB78-DB99B2BB678E")) IDirect3DBaseTexture8 : public IDirect3DResource8 {
  virtual DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) = 0;
  virtual DWORD STDMETHODCALLTYPE GetLOD() = 0;
  virtual DWORD STDMETHODCALLTYPE GetLevelCount() = 0;
};

struct __declspec(uuid("E4CDD575-2866-4F01-B12E-7EECE1EC9358")) IDirect3DTexture8 : public IDirect3DBaseTexture8 {
  virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface8** surface) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) = 0;
  virtual HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* rect) = 0;
};

struct __declspec(uuid("4B8AAAFA-140F-42BA-9131-597EAFAA2EAD")) IDirect3DVolumeTexture8 : public IDirect3DBaseTexture8 {
  virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DVOLUME_DESC8* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, IDirect3DVolume8** volume) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) = 0;
  virtual HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX* box) = 0;
};

struct __declspec(uuid("3EE5B968-2ACA-4C34-8BB5-7E0C3D19B750")) IDirect3DCubeTexture8 : public IDirect3DBaseTexture8 {
  virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** surface) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                                             DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) = 0;
  virtual HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) = 0;
};

struct __declspec(uuid("8AEEEAC7-05F9-44D4-B591-000B0DF1CB95")) IDirect3DVertexBuffer8 : public IDirect3DResource8 {
  virtual HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE Unlock() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* desc) = 0;
};

struct __declspec(uuid("0E689C9A-053D-44A0-9D92-DB0E3D750F86")) IDirect3DIndexBuffer8 : public IDirect3DResource8 {
  virtual HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE Unlock() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* desc) = 0;
};

struct __declspec(uuid("B96EEBCA-B326-4EA5-882F-2FF5BAE021DD")) IDirect3DSurface8 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** device) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** container) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC8* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockRect() = 0;
};

struct __declspec(uuid("BD7349F5-14F1-42E4-9C79-972380DB40C0")) IDirect3DVolume8 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** device) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** container) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC8* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockBox() = 0;
};

struct __declspec(uuid("928C088B-76B9-4C6B-A536-A590853876CD")) IDirect3DSwapChain8 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Present(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** buffer) = 0;
};

struct __declspec(uuid("7385E5DF-8FE8-41D5-86B6-D7B48547B6CF")) IDirect3DDevice8 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE TestCooperativeLevel() = 0;
  virtual UINT STDMETHODCALLTYPE GetAvailableTextureMem() = 0;
  virtual HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD bytes) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** d3d) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8* caps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* mode) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* bitmap) = 0;
  virtual void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) = 0;
  virtual BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* params, IDirect3DSwapChain8** swapChain) = 0;
  virtual HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS8* params) = 0;
  virtual HRESULT STDMETHODCALLTYPE Present(const RECT* sourceRect, const RECT* destRect, HWND window, const RGNDATA* dirty) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** buffer) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* status) = 0;
  virtual void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3DGAMMARAMP* ramp) = 0;
  virtual void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP* ramp) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, IDirect3DTexture8** texture) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                                        D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture8** texture) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                      IDirect3DCubeTexture8** texture) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                       IDirect3DVertexBuffer8** buffer) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                      IDirect3DIndexBuffer8** buffer) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                       BOOL lockable, IDirect3DSurface8** surface) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format,
                                                              D3DMULTISAMPLE_TYPE multisample, IDirect3DSurface8** surface) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface) = 0;
  virtual HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8* source, const RECT* sourceRects, UINT count,
                                              IDirect3DSurface8* destination, const POINT* destPoints) = 0;
  virtual HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8* destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8* renderTarget, IDirect3DSurface8* depthStencil) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8** renderTarget) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8** depthStencil) = 0;
  virtual HRESULT STDMETHODCALLTYPE BeginScene() = 0;
  virtual HRESULT STDMETHODCALLTYPE EndScene() = 0;
  virtual HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                                          DWORD stencil) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) = 0;
  virtual HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* viewport) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* viewport) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* material) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* material) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* light) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3DLIGHT9* light) = 0;
  virtual HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, BOOL* enable) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float* plane) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, float* plane) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE BeginStateBlock() = 0;
  virtual HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD token) = 0;
  virtual HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD token) = 0;
  virtual HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD token) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* status) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* status) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture8** texture) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture8* texture) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) = 0;
  virtual HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetInfo(DWORD id, void* info, DWORD size) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* entries) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* palette) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices,
                                                         UINT startIndex, UINT primitiveCount) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices,
                                                           UINT primitiveCount, const void* indices, D3DFORMAT indexFormat,
                                                           const void* vertices, UINT stride) = 0;
  virtual HRESULT STDMETHODCALLTYPE ProcessVertices(UINT sourceStart, UINT destIndex, UINT count,
                                                    IDirect3DVertexBuffer8* destination, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle,
                                                       DWORD usage) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD reg, void* data, DWORD count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD handle, void* data, DWORD* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer8* buffer, UINT stride) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, IDirect3DVertexBuffer8** buffer, UINT* stride) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8* indices, UINT baseVertexIndex) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8** indices, UINT* baseVertexIndex) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* function, DWORD* handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD reg, void* data, DWORD count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD handle, void* data, DWORD* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float* segments, const D3DRECTPATCH_INFO* info) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float* segments, const D3DTRIPATCH_INFO* info) = 0;
  virtual HRESULT STDMETHODCALLTYPE DeletePatch(UINT handle) = 0;
};
