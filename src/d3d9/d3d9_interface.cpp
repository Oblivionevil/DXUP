// IDirect3D9(Ex): adapters, display modes, capabilities, format support.

#include "d3d9_objects.h"

#include "../legacy/legacy_formats.h"
#include "../util/util_gpu_spoof.h"
#include "../util/util_types.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <float.h>

namespace dxup::d3d9 {

  // ============================================================
  // Formats
  // ============================================================

  bool isDepthFormat(D3DFORMAT format) {
    switch (format) {
      case D3DFMT_D16_LOCKABLE: case D3DFMT_D32: case D3DFMT_D15S1: case D3DFMT_D24S8: case D3DFMT_D24X8:
      case D3DFMT_D24X4S4: case D3DFMT_D16: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24FS8:
        return true;
      default:
        return format == D3DFORMAT(legacy::FormatINTZ);
    }
  }

  namespace {
    bool isTextureFormat(D3DFORMAT format) {
      switch (format) {
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_A8: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_A16B16G16R16: case D3DFMT_L8:
        case D3DFMT_A8L8: case D3DFMT_A4L4: case D3DFMT_V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_L16:
        case D3DFMT_Q16W16V16U16: case D3DFMT_R16F: case D3DFMT_G16R16F: case D3DFMT_A16B16G16R16F: case D3DFMT_R32F:
        case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F: case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3:
        case D3DFMT_DXT4: case D3DFMT_DXT5: case D3DFMT_R8G8B8:
          return true;
        default:
          return format == D3DFORMAT(legacy::FormatATI1) || format == D3DFORMAT(legacy::FormatATI2);
      }
    }

    bool isRenderTargetFormat(D3DFORMAT format) {
      switch (format) {
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_G16R16:
        case D3DFMT_A2R10G10B10: case D3DFMT_A16B16G16R16: case D3DFMT_R16F: case D3DFMT_G16R16F:
        case D3DFMT_A16B16G16R16F: case D3DFMT_R32F: case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F: case D3DFMT_A8:
          return true;
        default:
          return format == D3DFORMAT(legacy::FormatNULL);
      }
    }

    bool isDisplayFormat(D3DFORMAT format) {
      return format == D3DFMT_X8R8G8B8 || format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5
          || format == D3DFMT_A2R10G10B10;
    }

    bool isBackBufferFormat(D3DFORMAT format) {
      return isDisplayFormat(format) || format == D3DFMT_A8R8G8B8 || format == D3DFMT_A1R5G5B5;
    }
  }

  bool supportsFormat(D3DFORMAT format, DWORD usage, D3DRESOURCETYPE type) {
    if (usage & D3DUSAGE_DEPTHSTENCIL) {
      if (!isDepthFormat(format))
        return false;
      if (format == D3DFMT_D32 || format == D3DFMT_D15S1 || format == D3DFMT_D24X4S4)
        return false;
      // Shadow maps and INTZ: depth textures shaders read.
      return type == D3DRTYPE_SURFACE || type == D3DRTYPE_TEXTURE || type == D3DRTYPE_CUBETEXTURE;
    }
    if (usage & D3DUSAGE_RENDERTARGET) {
      if (!isRenderTargetFormat(format) || type == D3DRTYPE_VOLUMETEXTURE)
        return false;
      if (format == D3DFORMAT(legacy::FormatNULL))
        return type == D3DRTYPE_SURFACE || type == D3DRTYPE_TEXTURE;
      return true;
    }
    if (usage & D3DUSAGE_QUERY_VERTEXTEXTURE) {
      return format == D3DFMT_R32F || format == D3DFMT_G32R32F || format == D3DFMT_A32B32G32R32F || format == D3DFMT_R16F
          || format == D3DFMT_G16R16F || format == D3DFMT_A16B16G16R16F || format == D3DFMT_A8R8G8B8
          || format == D3DFMT_X8R8G8B8 || format == D3DFMT_L8;
    }
    if (usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE)) {
      const bool srgb = format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8B8G8R8
        || format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 || format == D3DFMT_DXT4
        || format == D3DFMT_DXT5;
      if (!srgb)
        return false;
      if ((usage & D3DUSAGE_QUERY_SRGBWRITE) && !isRenderTargetFormat(format))
        return false;
    }
    if (usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING) {
      if (!isRenderTargetFormat(format))
        return false;
    }
    if (type == D3DRTYPE_VERTEXBUFFER)
      return format == D3DFMT_VERTEXDATA;
    if (type == D3DRTYPE_INDEXBUFFER)
      return format == D3DFMT_INDEX16 || format == D3DFMT_INDEX32;
    if (type == D3DRTYPE_SURFACE)
      return isTextureFormat(format) || isRenderTargetFormat(format) || format == D3DFMT_YUY2 || format == D3DFMT_UYVY;
    if (type == D3DRTYPE_VOLUMETEXTURE || type == D3DRTYPE_VOLUME) {
      const legacy::FormatInfo info = legacy::formatInfo(legacy::Format(format));
      return isTextureFormat(format) && !info.depth;
    }
    return isTextureFormat(format) || isDepthFormat(format);
  }

  // ============================================================
  // Interface
  // ============================================================

  Interface::Interface(bool ex) : m_ex(ex) {
    enumerate();
  }

  Interface::~Interface() = default;

  HRESULT Interface::QueryInterface(REFIID riid, void** object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9) || (m_ex && riid == __uuidof(IDirect3D9Ex))) {
      AddRef();
      *object = static_cast<IDirect3D9Ex*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG Interface::Release() {
    const ULONG count = --m_ref;
    if (!count)
      delete this;
    return count;
  }

  void Interface::enumerate() {
    using CreateFactory = HRESULT (WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
      IDXGIAdapter1* adapter = nullptr;
      for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
          adapter->Release();
          continue;
        }
        LARGE_INTEGER version = {};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
          DXGI_OUTPUT_DESC outputDesc = {};
          output->GetDesc(&outputDesc);
          Adapter entry;
          entry.deviceName = outputDesc.DeviceName;
          entry.monitor = outputDesc.Monitor;
          entry.desc = desc;
          entry.driverVersion = version;
          // The primary monitor is adapter 0.
          MONITORINFO info = { sizeof(info) };
          GetMonitorInfoW(outputDesc.Monitor, &info);
          if (info.dwFlags & MONITORINFOF_PRIMARY)
            m_adapters.insert(m_adapters.begin(), std::move(entry));
          else
            m_adapters.push_back(std::move(entry));
          output->Release();
        }
        adapter->Release();
      }
      factory->Release();
    }
    if (m_adapters.empty()) {
      Adapter entry;
      entry.monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
      MONITORINFOEXW info = {};
      info.cbSize = sizeof(info);
      GetMonitorInfoW(entry.monitor, &info);
      entry.deviceName = info.szDevice;
      wcscpy_s(entry.desc.Description, L"DXUP Direct3D 9");
      m_adapters.push_back(std::move(entry));
    }
  }

  UINT Interface::GetAdapterCount() {
    return UINT(m_adapters.size());
  }

  HRESULT Interface::GetAdapterIdentifier(UINT adapter, DWORD, D3DADAPTER_IDENTIFIER9* identifier) {
    if (adapter >= m_adapters.size() || !identifier)
      return D3DERR_INVALIDCALL;
    const Adapter& a = m_adapters[adapter];
    // The spoofed GPU (dxgi.spoofVendor) when DXGI did not show it already.
    DXGI_ADAPTER_DESC1 desc = a.desc;
    spoofAdapterDesc(desc);
    LARGE_INTEGER driverVersion = a.driverVersion;
    spoofDriverVersion(a.desc.VendorId, driverVersion.QuadPart);
    *identifier = {};
    const char* driver = "d3d9.dll";
    switch (desc.VendorId) {
      case 0x10de: driver = "nvd3dum.dll"; break;
      case 0x1002: driver = "aticfx32.dll"; break;
      case 0x8086: driver = "igdumd32.dll"; break;
      default: break;
    }
    strcpy_s(identifier->Driver, driver);
    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, identifier->Description, sizeof(identifier->Description), nullptr, nullptr);
    WideCharToMultiByte(CP_ACP, 0, a.deviceName.c_str(), -1, identifier->DeviceName, sizeof(identifier->DeviceName), nullptr, nullptr);
    identifier->DriverVersion = driverVersion;
    identifier->VendorId = desc.VendorId;
    identifier->DeviceId = desc.DeviceId;
    identifier->SubSysId = desc.SubSysId;
    identifier->Revision = desc.Revision;
    // A stable identifier per adapter.
    identifier->DeviceIdentifier = { 0xd7b71e3e, WORD(desc.VendorId), WORD(desc.DeviceId),
      { BYTE(desc.AdapterLuid.LowPart), BYTE(desc.AdapterLuid.LowPart >> 8), BYTE(desc.AdapterLuid.LowPart >> 16),
        BYTE(desc.AdapterLuid.LowPart >> 24), BYTE(desc.SubSysId), BYTE(desc.SubSysId >> 8), BYTE(desc.Revision),
        BYTE(adapter) } };
    identifier->WHQLLevel = 1;
    return D3D_OK;
  }

  const std::vector<D3DDISPLAYMODEEX>& Interface::modes(UINT adapter) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Adapter& a = m_adapters[adapter];
    if (a.modesListed)
      return a.modes;
    a.modesListed = true;
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(a.deviceName.empty() ? nullptr : a.deviceName.c_str(), i, &mode); i++) {
      if (mode.dmBitsPerPel != 32 || mode.dmPelsWidth < 320 || mode.dmPelsHeight < 200)
        continue;
      D3DDISPLAYMODEEX m = {};
      m.Size = sizeof(m);
      m.Width = mode.dmPelsWidth;
      m.Height = mode.dmPelsHeight;
      m.RefreshRate = mode.dmDisplayFrequency;
      m.Format = D3DFMT_X8R8G8B8;
      m.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
      a.modes.push_back(m);
    }
    std::sort(a.modes.begin(), a.modes.end(), [](const D3DDISPLAYMODEEX& x, const D3DDISPLAYMODEEX& y) {
      if (x.Width != y.Width) return x.Width < y.Width;
      if (x.Height != y.Height) return x.Height < y.Height;
      return x.RefreshRate < y.RefreshRate;
    });
    a.modes.erase(std::unique(a.modes.begin(), a.modes.end(), [](const D3DDISPLAYMODEEX& x, const D3DDISPLAYMODEEX& y) {
      return x.Width == y.Width && x.Height == y.Height && x.RefreshRate == y.RefreshRate;
    }), a.modes.end());
    return a.modes;
  }

  UINT Interface::GetAdapterModeCount(UINT adapter, D3DFORMAT format) {
    D3DDISPLAYMODEFILTER filter = { sizeof(filter), format, D3DSCANLINEORDERING_PROGRESSIVE };
    return GetAdapterModeCountEx(adapter, &filter);
  }

  HRESULT Interface::EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* displayMode) {
    if (!displayMode)
      return D3DERR_INVALIDCALL;
    D3DDISPLAYMODEFILTER filter = { sizeof(filter), format, D3DSCANLINEORDERING_PROGRESSIVE };
    D3DDISPLAYMODEEX m = { sizeof(m) };
    const HRESULT hr = EnumAdapterModesEx(adapter, &filter, mode, &m);
    if (SUCCEEDED(hr))
      *displayMode = { m.Width, m.Height, m.RefreshRate, m.Format };
    return hr;
  }

  UINT Interface::GetAdapterModeCountEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter) {
    if (adapter >= m_adapters.size())
      return 0;
    // 16-bit modes as the 32-bit ones (Windows emulates them).
    if (filter && !isDisplayFormat(filter->Format))
      return 0;
    return UINT(modes(adapter).size());
  }

  HRESULT Interface::EnumAdapterModesEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter, UINT mode,
                                        D3DDISPLAYMODEEX* displayMode) {
    if (adapter >= m_adapters.size() || !displayMode)
      return D3DERR_INVALIDCALL;
    if (filter && !isDisplayFormat(filter->Format))
      return D3DERR_INVALIDCALL;
    const auto& list = modes(adapter);
    if (mode >= list.size())
      return D3DERR_INVALIDCALL;
    *displayMode = list[mode];
    if (filter)
      displayMode->Format = filter->Format;
    return D3D_OK;
  }

  HRESULT Interface::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) {
    if (!mode)
      return D3DERR_INVALIDCALL;
    D3DDISPLAYMODEEX m = { sizeof(m) };
    const HRESULT hr = GetAdapterDisplayModeEx(adapter, &m, nullptr);
    if (SUCCEEDED(hr))
      *mode = { m.Width, m.Height, m.RefreshRate, m.Format };
    return hr;
  }

  HRESULT Interface::GetAdapterDisplayModeEx(UINT adapter, D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) {
    if (adapter >= m_adapters.size() || !mode)
      return D3DERR_INVALIDCALL;
    DEVMODEW current = {};
    current.dmSize = sizeof(current);
    const std::wstring& name = m_adapters[adapter].deviceName;
    if (!EnumDisplaySettingsW(name.empty() ? nullptr : name.c_str(), ENUM_CURRENT_SETTINGS, &current))
      return D3DERR_INVALIDCALL;
    mode->Size = sizeof(*mode);
    mode->Width = current.dmPelsWidth;
    mode->Height = current.dmPelsHeight;
    mode->RefreshRate = current.dmDisplayFrequency;
    mode->Format = D3DFMT_X8R8G8B8;
    mode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    if (rotation)
      *rotation = D3DDISPLAYROTATION_IDENTITY;
    return D3D_OK;
  }

  HRESULT Interface::CheckDeviceType(UINT adapter, D3DDEVTYPE, D3DFORMAT displayFormat, D3DFORMAT backBufferFormat, BOOL windowed) {
    if (adapter >= m_adapters.size())
      return D3DERR_INVALIDCALL;
    if (windowed && displayFormat == D3DFMT_UNKNOWN)
      displayFormat = D3DFMT_X8R8G8B8;
    if (!isDisplayFormat(displayFormat))
      return D3DERR_NOTAVAILABLE;
    if (backBufferFormat == D3DFMT_UNKNOWN && windowed)
      return D3D_OK;
    return isBackBufferFormat(backBufferFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  HRESULT Interface::CheckDeviceFormat(UINT adapter, D3DDEVTYPE, D3DFORMAT, DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format) {
    if (adapter >= m_adapters.size())
      return D3DERR_INVALIDCALL;
    if (!supportsFormat(format, usage, type))
      return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && !isRenderTargetFormat(format))
      return D3DOK_NOAUTOGEN;
    return D3D_OK;
  }

  HRESULT Interface::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE, D3DFORMAT format, BOOL, D3DMULTISAMPLE_TYPE multisample,
                                                DWORD* qualityLevels) {
    if (adapter >= m_adapters.size())
      return D3DERR_INVALIDCALL;
    if (qualityLevels)
      *qualityLevels = 0;
    if (multisample == D3DMULTISAMPLE_NONE) {
      if (qualityLevels) *qualityLevels = 1;
      return D3D_OK;
    }
    if (!isRenderTargetFormat(format) && !isDepthFormat(format))
      return D3DERR_NOTAVAILABLE;
    if (format == D3DFORMAT(legacy::FormatNULL) || format == D3DFMT_A32B32G32R32F)
      return D3DERR_NOTAVAILABLE;
    if (multisample == D3DMULTISAMPLE_NONMASKABLE) {
      if (qualityLevels) *qualityLevels = 3;   // 2, 4, 8 samples
      return D3D_OK;
    }
    if (multisample == D3DMULTISAMPLE_2_SAMPLES || multisample == D3DMULTISAMPLE_4_SAMPLES
     || multisample == D3DMULTISAMPLE_8_SAMPLES) {
      if (qualityLevels) *qualityLevels = 1;
      return D3D_OK;
    }
    return D3DERR_NOTAVAILABLE;
  }

  HRESULT Interface::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT depthStencilFormat) {
    if (adapter >= m_adapters.size())
      return D3DERR_INVALIDCALL;
    return isDepthFormat(depthStencilFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  HRESULT Interface::CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE, D3DFORMAT source, D3DFORMAT target) {
    if (adapter >= m_adapters.size())
      return D3DERR_INVALIDCALL;
    return (isRenderTargetFormat(source) || source == D3DFMT_YUY2 || source == D3DFMT_UYVY) && isBackBufferFormat(target)
      ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  HRESULT Interface::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) {
    if (adapter >= m_adapters.size() || !caps)
      return D3DERR_INVALIDCALL;
    if (type != D3DDEVTYPE_HAL)
      return D3DERR_NOTAVAILABLE;
    *caps = makeCaps(adapter);
    return D3D_OK;
  }

  D3DCAPS9 Interface::makeCaps(UINT adapter) {
    D3DCAPS9 c = {};
    c.DeviceType = D3DDEVTYPE_HAL;
    c.AdapterOrdinal = adapter;
    c.Caps = D3DCAPS_READ_SCANLINE;
    c.Caps2 = D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_CANCALIBRATEGAMMA | D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_DYNAMICTEXTURES
            | D3DCAPS2_CANMANAGERESOURCE;
    c.Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM
            | D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION;
    c.PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_TWO
                            | D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR;
    c.CursorCaps = D3DCURSORCAPS_COLOR | D3DCURSORCAPS_LOWRES;
    c.DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY
              | D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY
              | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM
              | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT
              | D3DDEVCAPS_CANBLTSYSTONONLOCAL | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
    c.PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW
                        | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | D3DPMISCCAPS_CLIPTLVERTS
                        | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_INDEPENDENTWRITEMASKS
                        | D3DPMISCCAPS_PERSTAGECONSTANT | D3DPMISCCAPS_FOGANDSPECULARALPHA | D3DPMISCCAPS_SEPARATEALPHABLEND
                        | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING
                        | D3DPMISCCAPS_FOGVERTEXCLAMPED | D3DPMISCCAPS_POSTBLENDSRGBCONVERT;
    c.RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE
                 | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ANISOTROPY
                 | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS
                 | D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
    c.ZCmpCaps = c.AlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL
                                | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    c.SrcBlendCaps = c.DestBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR
      | D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA
      | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT
      | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR;
    c.ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND
                | D3DPSHADECAPS_FOGGOURAUD;
    c.TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PROJECTED
                  | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP
                  | D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;
    const DWORD filters = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC
      | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR
      | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    c.TextureFilterCaps = c.CubeTextureFilterCaps = c.VolumeTextureFilterCaps = filters;
    c.TextureAddressCaps = c.VolumeTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR
      | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    c.LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG
               | D3DLINECAPS_ANTIALIAS;
    c.MaxTextureWidth = c.MaxTextureHeight = 16384;
    c.MaxVolumeExtent = 2048;
    c.MaxTextureRepeat = 8192;
    c.MaxTextureAspectRatio = 16384;
    c.MaxAnisotropy = 16;
    c.MaxVertexW = 1e10f;
    c.GuardBandLeft = c.GuardBandTop = -32768.0f;
    c.GuardBandRight = c.GuardBandBottom = 32768.0f;
    c.ExtentsAdjust = 0.0f;
    c.StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT
                  | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR
                  | D3DSTENCILCAPS_TWOSIDED;
    c.FVFCaps = 8 | D3DFVFCAPS_PSIZE;
    c.TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE
      | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED
      | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA
      | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM
      | D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_PREMODULATE | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR
      | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA | D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR
      | D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE
      | D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP;
    c.MaxTextureBlendStages = 8;
    c.MaxSimultaneousTextures = 8;
    c.VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS
                           | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TEXGEN_SPHEREMAP;
    c.MaxActiveLights = 8;
    c.MaxUserClipPlanes = 6;
    c.MaxVertexBlendMatrices = 4;
    c.MaxVertexBlendMatrixIndex = 0;
    // Point sprites: quads from a geometry shader.
    c.MaxPointSize = 256.0f;
    c.MaxPrimitiveCount = 0x555555;
    c.MaxVertexIndex = 0xffffff;
    c.MaxStreams = 16;
    c.MaxStreamStride = 508;
    c.VertexShaderVersion = D3DVS_VERSION(3, 0);
    c.MaxVertexShaderConst = 256;
    c.PixelShaderVersion = D3DPS_VERSION(3, 0);
    c.PixelShader1xMaxValue = 65504.0f;
    c.DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET
               | D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;
    c.MasterAdapterOrdinal = 0;
    c.AdapterOrdinalInGroup = 0;
    c.NumberOfAdaptersInGroup = 1;
    c.DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N
                | D3DDTCAPS_USHORT4N | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    c.NumSimultaneousRTs = 4;
    c.StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT
                            | D3DPTFILTERCAPS_MAGFLINEAR;
    c.VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
    c.VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    c.VS20Caps.NumTemps = D3DVS20_MAX_NUMTEMPS;
    c.VS20Caps.StaticFlowControlDepth = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;
    c.PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION
                    | D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    c.PS20Caps.DynamicFlowControlDepth = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    c.PS20Caps.NumTemps = D3DPS20_MAX_NUMTEMPS;
    c.PS20Caps.StaticFlowControlDepth = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
    c.PS20Caps.NumInstructionSlots = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;
    c.VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT
                              | D3DPTFILTERCAPS_MAGFLINEAR;
    c.MaxVShaderInstructionsExecuted = 0xffffffffu;
    c.MaxPShaderInstructionsExecuted = 0xffffffffu;
    c.MaxVertexShader30InstructionSlots = 32768;
    c.MaxPixelShader30InstructionSlots = 32768;
    return c;
  }

  namespace {
    // Direct3D 8/9 run the x87 unit at single precision, rounding to
    // nearest, unless the application asks to keep its setting; 32-bit
    // games depend on it either way.
    void setFpuState(DWORD behavior) {
#if defined(_M_IX86)
      if (!(behavior & D3DCREATE_FPU_PRESERVE)) {
        unsigned int control = 0;
        _controlfp_s(&control, _PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC);
      }
#else
      (void)behavior;
#endif
    }
  }

  HMONITOR Interface::GetAdapterMonitor(UINT adapter) {
    return adapter < m_adapters.size() ? m_adapters[adapter].monitor : nullptr;
  }

  HRESULT Interface::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
                                  D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) {
    if (!device || !params || adapter >= m_adapters.size())
      return D3DERR_INVALIDCALL;
    *device = nullptr;
    setFpuState(behavior);
    auto* object = new Device(this, adapter, type, focusWindow, behavior, m_ex);
    object->AddRef();
    const HRESULT hr = object->init(params, nullptr);
    if (FAILED(hr)) {
      object->Release();
      return hr;
    }
    *device = object;
    return D3D_OK;
  }

  HRESULT Interface::CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
                                    D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* fullscreenMode,
                                    IDirect3DDevice9Ex** device) {
    if (!device || !params || adapter >= m_adapters.size())
      return D3DERR_INVALIDCALL;
    *device = nullptr;
    setFpuState(behavior);
    auto* object = new Device(this, adapter, type, focusWindow, behavior, true);
    object->AddRef();
    const HRESULT hr = object->init(params, fullscreenMode);
    if (FAILED(hr)) {
      object->Release();
      return hr;
    }
    *device = object;
    return D3D_OK;
  }

  HRESULT Interface::GetAdapterLUID(UINT adapter, LUID* luid) {
    if (adapter >= m_adapters.size() || !luid)
      return D3DERR_INVALIDCALL;
    *luid = m_adapters[adapter].desc.AdapterLuid;
    return D3D_OK;
  }

}
