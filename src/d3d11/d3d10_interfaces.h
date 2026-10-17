#pragma once

// Direct3D 10 on the D3D11 implementation. Windows' d3d10.dll creates its
// devices through D3D11CreateDeviceAndSwapChain and asks the device for
// ID3D10Device; every D3D11 object answers the D3D10 interface of its kind
// with an object embedded in it, which shares its reference count, private
// data and identity and forwards to the D3D11 methods.

#ifdef _WIN32

#include <d3d10_1.h>
#include <d3d11_4.h>

#include <cstring>

namespace dxup {

  // ---- Descriptions ----

  namespace d3d10 {

    // D3D10 numbers the keyed-mutex and GDI flags differently.
    inline UINT miscFlagsTo11(UINT flags) {
      UINT result = flags & (D3D10_RESOURCE_MISC_GENERATE_MIPS | D3D10_RESOURCE_MISC_SHARED
        | D3D10_RESOURCE_MISC_TEXTURECUBE);
      if (flags & D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX)
        result |= D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      if (flags & D3D10_RESOURCE_MISC_GDI_COMPATIBLE)
        result |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
      return result;
    }

    inline UINT miscFlagsTo10(UINT flags) {
      UINT result = flags & (D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_SHARED
        | D3D11_RESOURCE_MISC_TEXTURECUBE);
      if (flags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
        result |= D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      if (flags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE)
        result |= D3D10_RESOURCE_MISC_GDI_COMPATIBLE;
      return result;
    }

    inline D3D11_BUFFER_DESC bufferDescTo11(const D3D10_BUFFER_DESC& desc) {
      D3D11_BUFFER_DESC result = {};
      result.ByteWidth      = desc.ByteWidth;
      result.Usage          = D3D11_USAGE(desc.Usage);
      result.BindFlags      = desc.BindFlags;
      result.CPUAccessFlags = desc.CPUAccessFlags;
      result.MiscFlags      = miscFlagsTo11(desc.MiscFlags);
      return result;
    }

    // Texture descriptions have the same layout in both versions.
    template<typename Desc11, typename Desc10>
    Desc11 textureDescTo11(const Desc10& desc) {
      static_assert(sizeof(Desc10) == sizeof(Desc11), "texture descriptions differ");
      Desc11 result;
      std::memcpy(&result, &desc, sizeof(result));
      result.MiscFlags = miscFlagsTo11(desc.MiscFlags);
      return result;
    }

    template<typename Desc10, typename Desc11>
    Desc10 textureDescTo10(const Desc11& desc) {
      static_assert(sizeof(Desc10) == sizeof(Desc11), "texture descriptions differ");
      Desc10 result;
      std::memcpy(&result, &desc, sizeof(result));
      result.MiscFlags = miscFlagsTo10(desc.MiscFlags);
      return result;
    }

    // One blend factor and operation for all targets in D3D10.
    inline D3D11_BLEND_DESC blendDescTo11(const D3D10_BLEND_DESC& desc) {
      D3D11_BLEND_DESC result = {};
      result.AlphaToCoverageEnable = desc.AlphaToCoverageEnable;
      for (UINT i = 0; i < 8; i++) {
        auto& target = result.RenderTarget[i];
        target.BlendEnable           = desc.BlendEnable[i];
        target.SrcBlend              = D3D11_BLEND(desc.SrcBlend);
        target.DestBlend             = D3D11_BLEND(desc.DestBlend);
        target.BlendOp               = D3D11_BLEND_OP(desc.BlendOp);
        target.SrcBlendAlpha         = D3D11_BLEND(desc.SrcBlendAlpha);
        target.DestBlendAlpha        = D3D11_BLEND(desc.DestBlendAlpha);
        target.BlendOpAlpha          = D3D11_BLEND_OP(desc.BlendOpAlpha);
        target.RenderTargetWriteMask = desc.RenderTargetWriteMask[i];
        if (i && (target.BlendEnable != result.RenderTarget[0].BlendEnable
               || target.RenderTargetWriteMask != result.RenderTarget[0].RenderTargetWriteMask))
          result.IndependentBlendEnable = TRUE;
      }
      return result;
    }

    inline D3D11_BLEND_DESC blendDesc1To11(const D3D10_BLEND_DESC1& desc) {
      static_assert(sizeof(D3D10_BLEND_DESC1) == sizeof(D3D11_BLEND_DESC), "blend descriptions differ");
      D3D11_BLEND_DESC result;
      std::memcpy(&result, &desc, sizeof(result));
      return result;
    }

    inline D3D11_SHADER_RESOURCE_VIEW_DESC srvDescTo11(const D3D10_SHADER_RESOURCE_VIEW_DESC1& desc) {
      static_assert(sizeof(D3D10_SHADER_RESOURCE_VIEW_DESC1) == sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC),
        "view descriptions differ");
      D3D11_SHADER_RESOURCE_VIEW_DESC result;
      std::memcpy(&result, &desc, sizeof(result));
      return result;
    }

    inline D3D11_SHADER_RESOURCE_VIEW_DESC srvDescTo11(const D3D10_SHADER_RESOURCE_VIEW_DESC& desc) {
      D3D10_SHADER_RESOURCE_VIEW_DESC1 desc1 = {};
      std::memcpy(&desc1, &desc, sizeof(desc));
      return srvDescTo11(desc1);
    }

    inline D3D11_DEPTH_STENCIL_VIEW_DESC dsvDescTo11(const D3D10_DEPTH_STENCIL_VIEW_DESC& desc) {
      D3D11_DEPTH_STENCIL_VIEW_DESC result = {};
      result.Format        = desc.Format;
      result.ViewDimension = D3D11_DSV_DIMENSION(desc.ViewDimension);
      static_assert(sizeof(desc.Texture2DArray) == sizeof(result.Texture2DArray), "view descriptions differ");
      std::memcpy(&result.Texture2DArray, &desc.Texture2DArray, sizeof(result.Texture2DArray));
      return result;
    }

    inline D3D10_DEPTH_STENCIL_VIEW_DESC dsvDescTo10(const D3D11_DEPTH_STENCIL_VIEW_DESC& desc) {
      D3D10_DEPTH_STENCIL_VIEW_DESC result = {};
      result.Format        = desc.Format;
      result.ViewDimension = D3D10_DSV_DIMENSION(desc.ViewDimension);
      std::memcpy(&result.Texture2DArray, &desc.Texture2DArray, sizeof(result.Texture2DArray));
      return result;
    }

    // The D3D10 interface of a D3D11 object (a reference to it).
    template<typename T10, typename T11>
    T10* to10(T11* object) {
      T10* result = nullptr;
      if (object)
        object->QueryInterface(__uuidof(T10), reinterpret_cast<void**>(&result));
      return result;
    }

    // As to10, consuming the reference to the D3D11 object.
    template<typename T10, typename T11>
    T10* take10(T11* object) {
      T10* result = to10<T10>(object);
      if (object)
        object->Release();
      return result;
    }

    // The immediate context of a device child's device, without a reference
    // (it lives as long as the device).
    inline ID3D11DeviceContext* immediateContext(ID3D11DeviceChild* child) {
      ID3D11Device* device = nullptr;
      child->GetDevice(&device);
      ID3D11DeviceContext* context = nullptr;
      device->GetImmediateContext(&context);
      context->Release();
      device->Release();
      return context;
    }

  }

  // ---- Device children ----

  // A D3D10 interface embedded in the D3D11 object it stands for.
  template<typename Iface, typename Iface11>
  class D3D10DeviceChild : public Iface {
  public:
    explicit D3D10DeviceChild(Iface11* parent) : m_parent(parent) { }

    D3D10DeviceChild(const D3D10DeviceChild&) = delete;
    D3D10DeviceChild& operator=(const D3D10DeviceChild&) = delete;

    Iface11* d3d11() const { return m_parent; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      return m_parent->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_parent->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_parent->Release(); }

    void STDMETHODCALLTYPE GetDevice(ID3D10Device** ppDevice) override {
      if (!ppDevice)
        return;
      ID3D11Device* device = nullptr;
      m_parent->GetDevice(&device);
      *ppDevice = d3d10::take10<ID3D10Device>(device);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
      return m_parent->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
      return m_parent->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
      return m_parent->SetPrivateDataInterface(guid, pData);
    }

  protected:
    Iface11* m_parent;
  };

  // Resources map themselves in D3D10, through the immediate context.
  template<typename Iface, typename Iface11, D3D10_RESOURCE_DIMENSION Dimension>
  class D3D10Resource : public D3D10DeviceChild<Iface, Iface11> {
  public:
    using D3D10DeviceChild<Iface, Iface11>::D3D10DeviceChild;

    void STDMETHODCALLTYPE GetType(D3D10_RESOURCE_DIMENSION* pResourceDimension) override {
      if (pResourceDimension)
        *pResourceDimension = Dimension;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override {
      this->m_parent->SetEvictionPriority(EvictionPriority);
    }
    UINT STDMETHODCALLTYPE GetEvictionPriority() override {
      return this->m_parent->GetEvictionPriority();
    }

  protected:
    HRESULT map(UINT subresource, D3D10_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
      if (!mapped)
        return E_INVALIDARG;
      return d3d10::immediateContext(this->m_parent)->Map(this->m_parent, subresource, D3D11_MAP(type), flags, mapped);
    }
    void unmap(UINT subresource) {
      d3d10::immediateContext(this->m_parent)->Unmap(this->m_parent, subresource);
    }
  };

  class D3D10Buffer final : public D3D10Resource<ID3D10Buffer, ID3D11Buffer, D3D10_RESOURCE_DIMENSION_BUFFER> {
  public:
    using D3D10Resource::D3D10Resource;

    HRESULT STDMETHODCALLTYPE Map(D3D10_MAP MapType, UINT MapFlags, void** ppData) override {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const HRESULT hr = map(0, MapType, MapFlags, &mapped);
      if (ppData)
        *ppData = SUCCEEDED(hr) ? mapped.pData : nullptr;
      return hr;
    }
    void STDMETHODCALLTYPE Unmap() override { unmap(0); }

    void STDMETHODCALLTYPE GetDesc(D3D10_BUFFER_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D11_BUFFER_DESC desc = {};
      m_parent->GetDesc(&desc);
      pDesc->ByteWidth      = desc.ByteWidth;
      pDesc->Usage          = D3D10_USAGE(desc.Usage);
      pDesc->BindFlags      = desc.BindFlags;
      pDesc->CPUAccessFlags = desc.CPUAccessFlags;
      pDesc->MiscFlags      = d3d10::miscFlagsTo10(desc.MiscFlags);
    }
  };

  class D3D10Texture1D final : public D3D10Resource<ID3D10Texture1D, ID3D11Texture1D, D3D10_RESOURCE_DIMENSION_TEXTURE1D> {
  public:
    using D3D10Resource::D3D10Resource;

    HRESULT STDMETHODCALLTYPE Map(UINT Subresource, D3D10_MAP MapType, UINT MapFlags, void** ppData) override {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const HRESULT hr = map(Subresource, MapType, MapFlags, &mapped);
      if (ppData)
        *ppData = SUCCEEDED(hr) ? mapped.pData : nullptr;
      return hr;
    }
    void STDMETHODCALLTYPE Unmap(UINT Subresource) override { unmap(Subresource); }

    void STDMETHODCALLTYPE GetDesc(D3D10_TEXTURE1D_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D11_TEXTURE1D_DESC desc = {};
      m_parent->GetDesc(&desc);
      *pDesc = d3d10::textureDescTo10<D3D10_TEXTURE1D_DESC>(desc);
    }
  };

  class D3D10Texture2D final : public D3D10Resource<ID3D10Texture2D, ID3D11Texture2D, D3D10_RESOURCE_DIMENSION_TEXTURE2D> {
  public:
    using D3D10Resource::D3D10Resource;

    HRESULT STDMETHODCALLTYPE Map(UINT Subresource, D3D10_MAP MapType, UINT MapFlags,
                                  D3D10_MAPPED_TEXTURE2D* pMappedTex2D) override {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const HRESULT hr = map(Subresource, MapType, MapFlags, &mapped);
      if (pMappedTex2D) {
        pMappedTex2D->pData    = SUCCEEDED(hr) ? mapped.pData : nullptr;
        pMappedTex2D->RowPitch = SUCCEEDED(hr) ? mapped.RowPitch : 0;
      }
      return hr;
    }
    void STDMETHODCALLTYPE Unmap(UINT Subresource) override { unmap(Subresource); }

    void STDMETHODCALLTYPE GetDesc(D3D10_TEXTURE2D_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D11_TEXTURE2D_DESC desc = {};
      m_parent->GetDesc(&desc);
      *pDesc = d3d10::textureDescTo10<D3D10_TEXTURE2D_DESC>(desc);
    }
  };

  class D3D10Texture3D final : public D3D10Resource<ID3D10Texture3D, ID3D11Texture3D, D3D10_RESOURCE_DIMENSION_TEXTURE3D> {
  public:
    using D3D10Resource::D3D10Resource;

    HRESULT STDMETHODCALLTYPE Map(UINT Subresource, D3D10_MAP MapType, UINT MapFlags,
                                  D3D10_MAPPED_TEXTURE3D* pMappedTex3D) override {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const HRESULT hr = map(Subresource, MapType, MapFlags, &mapped);
      if (pMappedTex3D) {
        pMappedTex3D->pData      = SUCCEEDED(hr) ? mapped.pData : nullptr;
        pMappedTex3D->RowPitch   = SUCCEEDED(hr) ? mapped.RowPitch : 0;
        pMappedTex3D->DepthPitch = SUCCEEDED(hr) ? mapped.DepthPitch : 0;
      }
      return hr;
    }
    void STDMETHODCALLTYPE Unmap(UINT Subresource) override { unmap(Subresource); }

    void STDMETHODCALLTYPE GetDesc(D3D10_TEXTURE3D_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D11_TEXTURE3D_DESC desc = {};
      m_parent->GetDesc(&desc);
      *pDesc = d3d10::textureDescTo10<D3D10_TEXTURE3D_DESC>(desc);
    }
  };

  // ---- Views ----

  template<typename Iface, typename Iface11>
  class D3D10View : public D3D10DeviceChild<Iface, Iface11> {
  public:
    using D3D10DeviceChild<Iface, Iface11>::D3D10DeviceChild;

    void STDMETHODCALLTYPE GetResource(ID3D10Resource** ppResource) override {
      if (!ppResource)
        return;
      ID3D11Resource* resource = nullptr;
      this->m_parent->GetResource(&resource);
      *ppResource = d3d10::take10<ID3D10Resource>(resource);
    }
  };

  class D3D10ShaderResourceView final : public D3D10View<ID3D10ShaderResourceView1, ID3D11ShaderResourceView> {
  public:
    using D3D10View::D3D10View;

    void STDMETHODCALLTYPE GetDesc(D3D10_SHADER_RESOURCE_VIEW_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D10_SHADER_RESOURCE_VIEW_DESC1 desc = {};
      GetDesc1(&desc);
      std::memcpy(pDesc, &desc, sizeof(*pDesc));
      // D3D10.0 has no cube arrays.
      if (desc.ViewDimension == D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY)
        pDesc->ViewDimension = D3D10_SRV_DIMENSION_TEXTURECUBE;
    }
    void STDMETHODCALLTYPE GetDesc1(D3D10_SHADER_RESOURCE_VIEW_DESC1* pDesc) override {
      if (!pDesc)
        return;
      D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
      m_parent->GetDesc(&desc);
      std::memcpy(pDesc, &desc, sizeof(*pDesc));
    }
  };

  class D3D10RenderTargetView final : public D3D10View<ID3D10RenderTargetView, ID3D11RenderTargetView> {
  public:
    using D3D10View::D3D10View;

    void STDMETHODCALLTYPE GetDesc(D3D10_RENDER_TARGET_VIEW_DESC* pDesc) override {
      if (!pDesc)
        return;
      static_assert(sizeof(D3D10_RENDER_TARGET_VIEW_DESC) == sizeof(D3D11_RENDER_TARGET_VIEW_DESC),
        "view descriptions differ");
      D3D11_RENDER_TARGET_VIEW_DESC desc = {};
      m_parent->GetDesc(&desc);
      std::memcpy(pDesc, &desc, sizeof(*pDesc));
    }
  };

  class D3D10DepthStencilView final : public D3D10View<ID3D10DepthStencilView, ID3D11DepthStencilView> {
  public:
    using D3D10View::D3D10View;

    void STDMETHODCALLTYPE GetDesc(D3D10_DEPTH_STENCIL_VIEW_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
      m_parent->GetDesc(&desc);
      *pDesc = d3d10::dsvDescTo10(desc);
    }
  };

  // ---- Shaders, input layouts, states ----

  template<typename Iface, typename Iface11>
  class D3D10PlainChild final : public D3D10DeviceChild<Iface, Iface11> {
  public:
    using D3D10DeviceChild<Iface, Iface11>::D3D10DeviceChild;
  };

  using D3D10VertexShader   = D3D10PlainChild<ID3D10VertexShader, ID3D11VertexShader>;
  using D3D10GeometryShader = D3D10PlainChild<ID3D10GeometryShader, ID3D11GeometryShader>;
  using D3D10PixelShader    = D3D10PlainChild<ID3D10PixelShader, ID3D11PixelShader>;
  using D3D10InputLayout    = D3D10PlainChild<ID3D10InputLayout, ID3D11InputLayout>;

  class D3D10BlendState final : public D3D10DeviceChild<ID3D10BlendState1, ID3D11BlendState> {
  public:
    using D3D10DeviceChild::D3D10DeviceChild;

    void STDMETHODCALLTYPE GetDesc(D3D10_BLEND_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D11_BLEND_DESC desc = {};
      m_parent->GetDesc(&desc);
      const auto& first = desc.RenderTarget[0];
      pDesc->AlphaToCoverageEnable = desc.AlphaToCoverageEnable;
      pDesc->SrcBlend       = D3D10_BLEND(first.SrcBlend);
      pDesc->DestBlend      = D3D10_BLEND(first.DestBlend);
      pDesc->BlendOp        = D3D10_BLEND_OP(first.BlendOp);
      pDesc->SrcBlendAlpha  = D3D10_BLEND(first.SrcBlendAlpha);
      pDesc->DestBlendAlpha = D3D10_BLEND(first.DestBlendAlpha);
      pDesc->BlendOpAlpha   = D3D10_BLEND_OP(first.BlendOpAlpha);
      for (UINT i = 0; i < 8; i++) {
        const auto& target = desc.IndependentBlendEnable ? desc.RenderTarget[i] : first;
        pDesc->BlendEnable[i]           = target.BlendEnable;
        pDesc->RenderTargetWriteMask[i] = target.RenderTargetWriteMask;
      }
    }
    void STDMETHODCALLTYPE GetDesc1(D3D10_BLEND_DESC1* pDesc) override {
      if (!pDesc)
        return;
      D3D11_BLEND_DESC desc = {};
      m_parent->GetDesc(&desc);
      std::memcpy(pDesc, &desc, sizeof(*pDesc));
    }
  };

  class D3D10DepthStencilState final : public D3D10DeviceChild<ID3D10DepthStencilState, ID3D11DepthStencilState> {
  public:
    using D3D10DeviceChild::D3D10DeviceChild;

    void STDMETHODCALLTYPE GetDesc(D3D10_DEPTH_STENCIL_DESC* pDesc) override {
      static_assert(sizeof(D3D10_DEPTH_STENCIL_DESC) == sizeof(D3D11_DEPTH_STENCIL_DESC), "descriptions differ");
      if (pDesc)
        m_parent->GetDesc(reinterpret_cast<D3D11_DEPTH_STENCIL_DESC*>(pDesc));
    }
  };

  class D3D10RasterizerState final : public D3D10DeviceChild<ID3D10RasterizerState, ID3D11RasterizerState> {
  public:
    using D3D10DeviceChild::D3D10DeviceChild;

    void STDMETHODCALLTYPE GetDesc(D3D10_RASTERIZER_DESC* pDesc) override {
      static_assert(sizeof(D3D10_RASTERIZER_DESC) == sizeof(D3D11_RASTERIZER_DESC), "descriptions differ");
      if (pDesc)
        m_parent->GetDesc(reinterpret_cast<D3D11_RASTERIZER_DESC*>(pDesc));
    }
  };

  class D3D10SamplerState final : public D3D10DeviceChild<ID3D10SamplerState, ID3D11SamplerState> {
  public:
    using D3D10DeviceChild::D3D10DeviceChild;

    void STDMETHODCALLTYPE GetDesc(D3D10_SAMPLER_DESC* pDesc) override {
      static_assert(sizeof(D3D10_SAMPLER_DESC) == sizeof(D3D11_SAMPLER_DESC), "descriptions differ");
      if (pDesc)
        m_parent->GetDesc(reinterpret_cast<D3D11_SAMPLER_DESC*>(pDesc));
    }
  };

  // ---- Queries ----

  // Queries and predicates (counters do not exist, see CheckCounterInfo).
  class D3D10Query final : public D3D10DeviceChild<ID3D10Predicate, ID3D11Query> {
  public:
    using D3D10DeviceChild::D3D10DeviceChild;

    void STDMETHODCALLTYPE Begin() override {
      d3d10::immediateContext(m_parent)->Begin(m_parent);
    }
    void STDMETHODCALLTYPE End() override {
      d3d10::immediateContext(m_parent)->End(m_parent);
    }

    HRESULT STDMETHODCALLTYPE GetData(void* pData, UINT DataSize, UINT GetDataFlags) override {
      // D3D10's pipeline statistics stop before the tessellation and
      // compute counters.
      D3D11_QUERY_DESC desc = {};
      m_parent->GetDesc(&desc);
      ID3D11DeviceContext* context = d3d10::immediateContext(m_parent);
      if (desc.Query == D3D11_QUERY_PIPELINE_STATISTICS && pData) {
        if (DataSize != sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS))
          return E_INVALIDARG;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics = {};
        const HRESULT hr = context->GetData(m_parent, &statistics, sizeof(statistics), GetDataFlags);
        if (hr == S_OK)
          std::memcpy(pData, &statistics, sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS));
        return hr;
      }
      return context->GetData(m_parent, pData, DataSize, GetDataFlags);
    }

    UINT STDMETHODCALLTYPE GetDataSize() override {
      D3D11_QUERY_DESC desc = {};
      m_parent->GetDesc(&desc);
      if (desc.Query == D3D11_QUERY_PIPELINE_STATISTICS)
        return sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS);
      return m_parent->GetDataSize();
    }

    void STDMETHODCALLTYPE GetDesc(D3D10_QUERY_DESC* pDesc) override {
      if (!pDesc)
        return;
      D3D11_QUERY_DESC desc = {};
      m_parent->GetDesc(&desc);
      pDesc->Query     = D3D10_QUERY(desc.Query);
      pDesc->MiscFlags = desc.MiscFlags;
    }
  };

}

#endif // _WIN32
