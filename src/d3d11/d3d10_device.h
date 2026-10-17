#pragma once

// ID3D10Device1 of a D3D11 device: embedded in it, sharing its reference
// count and identity, and forwarding to the device and its immediate
// context (D3D10 has no contexts).

#ifdef _WIN32

#include "d3d10_interfaces.h"

#include <atomic>

namespace dxup {

  namespace d3d10 {

    template<typename Wrapper, typename T10>
    auto unwrap(T10* object) -> decltype(static_cast<Wrapper*>(object)->d3d11()) {
      return object ? static_cast<Wrapper*>(object)->d3d11() : nullptr;
    }

    inline ID3D11Resource* unwrapResource(ID3D10Resource* resource) {
      if (!resource)
        return nullptr;
      D3D10_RESOURCE_DIMENSION dimension = D3D10_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);
      switch (dimension) {
        case D3D10_RESOURCE_DIMENSION_BUFFER:
          return unwrap<D3D10Buffer>(static_cast<ID3D10Buffer*>(resource));
        case D3D10_RESOURCE_DIMENSION_TEXTURE1D:
          return unwrap<D3D10Texture1D>(static_cast<ID3D10Texture1D*>(resource));
        case D3D10_RESOURCE_DIMENSION_TEXTURE2D:
          return unwrap<D3D10Texture2D>(static_cast<ID3D10Texture2D*>(resource));
        case D3D10_RESOURCE_DIMENSION_TEXTURE3D:
          return unwrap<D3D10Texture3D>(static_cast<ID3D10Texture3D*>(resource));
        default:
          return nullptr;
      }
    }

    // Arrays of bound objects, D3D10 to D3D11.
    template<typename Wrapper, typename T11, typename T10, size_t N>
    void unwrapArray(T11* (&target)[N], UINT count, T10* const* objects) {
      for (UINT i = 0; i < count && i < N; i++)
        target[i] = objects ? unwrap<Wrapper>(objects[i]) : nullptr;
    }

    // And back: references to the D3D10 interfaces of D3D11 references.
    template<typename T10, typename T11>
    void takeArray(T10** target, UINT count, T11* const* objects) {
      for (UINT i = 0; i < count; i++)
        target[i] = take10<T10>(objects[i]);
    }

  }

  class D3D10Device final : public ID3D10Device1 {
  public:
    explicit D3D10Device(ID3D11Device* device) : m_device(device) { }

    D3D10Device(const D3D10Device&) = delete;
    D3D10Device& operator=(const D3D10Device&) = delete;

    void setCreationFlags(UINT flags) { m_creationFlags = flags; }

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      return m_device->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_device->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_device->Release(); }

    // ---- Shader stages ----

    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                ID3D10Buffer* const* ppConstantBuffers) override {
      ID3D11Buffer* buffers[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10Buffer>(buffers, NumBuffers, ppConstantBuffers);
      ctx()->VSSetConstantBuffers(StartSlot, NumBuffers, buffers);
    }
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                ID3D10Buffer* const* ppConstantBuffers) override {
      ID3D11Buffer* buffers[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10Buffer>(buffers, NumBuffers, ppConstantBuffers);
      ctx()->PSSetConstantBuffers(StartSlot, NumBuffers, buffers);
    }
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                ID3D10Buffer* const* ppConstantBuffers) override {
      ID3D11Buffer* buffers[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10Buffer>(buffers, NumBuffers, ppConstantBuffers);
      ctx()->GSSetConstantBuffers(StartSlot, NumBuffers, buffers);
    }

    void STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews,
                                                ID3D10ShaderResourceView* const* ppShaderResourceViews) override {
      ID3D11ShaderResourceView* views[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10ShaderResourceView>(views, NumViews, ppShaderResourceViews);
      ctx()->VSSetShaderResources(StartSlot, NumViews, views);
    }
    void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews,
                                                ID3D10ShaderResourceView* const* ppShaderResourceViews) override {
      ID3D11ShaderResourceView* views[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10ShaderResourceView>(views, NumViews, ppShaderResourceViews);
      ctx()->PSSetShaderResources(StartSlot, NumViews, views);
    }
    void STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews,
                                                ID3D10ShaderResourceView* const* ppShaderResourceViews) override {
      ID3D11ShaderResourceView* views[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10ShaderResourceView>(views, NumViews, ppShaderResourceViews);
      ctx()->GSSetShaderResources(StartSlot, NumViews, views);
    }

    void STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                         ID3D10SamplerState* const* ppSamplers) override {
      ID3D11SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10SamplerState>(samplers, NumSamplers, ppSamplers);
      ctx()->VSSetSamplers(StartSlot, NumSamplers, samplers);
    }
    void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                         ID3D10SamplerState* const* ppSamplers) override {
      ID3D11SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10SamplerState>(samplers, NumSamplers, ppSamplers);
      ctx()->PSSetSamplers(StartSlot, NumSamplers, samplers);
    }
    void STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                         ID3D10SamplerState* const* ppSamplers) override {
      ID3D11SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10SamplerState>(samplers, NumSamplers, ppSamplers);
      ctx()->GSSetSamplers(StartSlot, NumSamplers, samplers);
    }

    void STDMETHODCALLTYPE VSSetShader(ID3D10VertexShader* pVertexShader) override {
      ctx()->VSSetShader(d3d10::unwrap<D3D10VertexShader>(pVertexShader), nullptr, 0);
    }
    void STDMETHODCALLTYPE PSSetShader(ID3D10PixelShader* pPixelShader) override {
      ctx()->PSSetShader(d3d10::unwrap<D3D10PixelShader>(pPixelShader), nullptr, 0);
    }
    void STDMETHODCALLTYPE GSSetShader(ID3D10GeometryShader* pShader) override {
      ctx()->GSSetShader(d3d10::unwrap<D3D10GeometryShader>(pShader), nullptr, 0);
    }

    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppConstantBuffers) override {
      ID3D11Buffer* buffers[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
      if (!ppConstantBuffers || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        return;
      ctx()->VSGetConstantBuffers(StartSlot, NumBuffers, buffers);
      d3d10::takeArray(ppConstantBuffers, NumBuffers, buffers);
    }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppConstantBuffers) override {
      ID3D11Buffer* buffers[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
      if (!ppConstantBuffers || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        return;
      ctx()->PSGetConstantBuffers(StartSlot, NumBuffers, buffers);
      d3d10::takeArray(ppConstantBuffers, NumBuffers, buffers);
    }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppConstantBuffers) override {
      ID3D11Buffer* buffers[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
      if (!ppConstantBuffers || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        return;
      ctx()->GSGetConstantBuffers(StartSlot, NumBuffers, buffers);
      d3d10::takeArray(ppConstantBuffers, NumBuffers, buffers);
    }

    void STDMETHODCALLTYPE VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView** ppViews) override {
      ID3D11ShaderResourceView* views[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
      if (!ppViews || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
      ctx()->VSGetShaderResources(StartSlot, NumViews, views);
      d3d10::takeArray(ppViews, NumViews, views);
    }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView** ppViews) override {
      ID3D11ShaderResourceView* views[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
      if (!ppViews || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
      ctx()->PSGetShaderResources(StartSlot, NumViews, views);
      d3d10::takeArray(ppViews, NumViews, views);
    }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView** ppViews) override {
      ID3D11ShaderResourceView* views[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
      if (!ppViews || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
      ctx()->GSGetShaderResources(StartSlot, NumViews, views);
      d3d10::takeArray(ppViews, NumViews, views);
    }

    void STDMETHODCALLTYPE VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState** ppSamplers) override {
      ID3D11SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
      if (!ppSamplers || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return;
      ctx()->VSGetSamplers(StartSlot, NumSamplers, samplers);
      d3d10::takeArray(ppSamplers, NumSamplers, samplers);
    }
    void STDMETHODCALLTYPE PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState** ppSamplers) override {
      ID3D11SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
      if (!ppSamplers || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return;
      ctx()->PSGetSamplers(StartSlot, NumSamplers, samplers);
      d3d10::takeArray(ppSamplers, NumSamplers, samplers);
    }
    void STDMETHODCALLTYPE GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState** ppSamplers) override {
      ID3D11SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
      if (!ppSamplers || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return;
      ctx()->GSGetSamplers(StartSlot, NumSamplers, samplers);
      d3d10::takeArray(ppSamplers, NumSamplers, samplers);
    }

    void STDMETHODCALLTYPE VSGetShader(ID3D10VertexShader** ppVertexShader) override {
      if (!ppVertexShader)
        return;
      ID3D11VertexShader* shader = nullptr;
      ctx()->VSGetShader(&shader, nullptr, nullptr);
      *ppVertexShader = d3d10::take10<ID3D10VertexShader>(shader);
    }
    void STDMETHODCALLTYPE PSGetShader(ID3D10PixelShader** ppPixelShader) override {
      if (!ppPixelShader)
        return;
      ID3D11PixelShader* shader = nullptr;
      ctx()->PSGetShader(&shader, nullptr, nullptr);
      *ppPixelShader = d3d10::take10<ID3D10PixelShader>(shader);
    }
    void STDMETHODCALLTYPE GSGetShader(ID3D10GeometryShader** ppGeometryShader) override {
      if (!ppGeometryShader)
        return;
      ID3D11GeometryShader* shader = nullptr;
      ctx()->GSGetShader(&shader, nullptr, nullptr);
      *ppGeometryShader = d3d10::take10<ID3D10GeometryShader>(shader);
    }

    // ---- Input assembler ----

    void STDMETHODCALLTYPE IASetInputLayout(ID3D10InputLayout* pInputLayout) override {
      ctx()->IASetInputLayout(d3d10::unwrap<D3D10InputLayout>(pInputLayout));
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppVertexBuffers,
                                              const UINT* pStrides, const UINT* pOffsets) override {
      ID3D11Buffer* buffers[D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10Buffer>(buffers, NumBuffers, ppVertexBuffers);
      ctx()->IASetVertexBuffers(StartSlot, NumBuffers, buffers, pStrides, pOffsets);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D10Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override {
      ctx()->IASetIndexBuffer(d3d10::unwrap<D3D10Buffer>(pIndexBuffer), Format, Offset);
    }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY Topology) override {
      ctx()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(Topology));
    }

    void STDMETHODCALLTYPE IAGetInputLayout(ID3D10InputLayout** ppInputLayout) override {
      if (!ppInputLayout)
        return;
      ID3D11InputLayout* layout = nullptr;
      ctx()->IAGetInputLayout(&layout);
      *ppInputLayout = d3d10::take10<ID3D10InputLayout>(layout);
    }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppVertexBuffers,
                                              UINT* pStrides, UINT* pOffsets) override {
      ID3D11Buffer* buffers[D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
      if (NumBuffers > D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        return;
      ctx()->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers ? buffers : nullptr, pStrides, pOffsets);
      if (ppVertexBuffers)
        d3d10::takeArray(ppVertexBuffers, NumBuffers, buffers);
    }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D10Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override {
      ID3D11Buffer* buffer = nullptr;
      ctx()->IAGetIndexBuffer(pIndexBuffer ? &buffer : nullptr, Format, Offset);
      if (pIndexBuffer)
        *pIndexBuffer = d3d10::take10<ID3D10Buffer>(buffer);
    }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY* pTopology) override {
      if (pTopology)
        ctx()->IAGetPrimitiveTopology(reinterpret_cast<D3D11_PRIMITIVE_TOPOLOGY*>(pTopology));
    }

    // ---- Draws ----

    void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override {
      ctx()->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
    }
    void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override {
      ctx()->Draw(VertexCount, StartVertexLocation);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
                                                INT BaseVertexLocation, UINT StartInstanceLocation) override {
      ctx()->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
        StartInstanceLocation);
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation,
                                         UINT StartInstanceLocation) override {
      ctx()->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
    void STDMETHODCALLTYPE DrawAuto() override { ctx()->DrawAuto(); }

    // ---- Predication ----

    void STDMETHODCALLTYPE SetPredication(ID3D10Predicate* pPredicate, BOOL PredicateValue) override {
      // ID3D11Predicate adds nothing to ID3D11Query.
      ctx()->SetPredication(reinterpret_cast<ID3D11Predicate*>(d3d10::unwrap<D3D10Query>(pPredicate)), PredicateValue);
    }
    void STDMETHODCALLTYPE GetPredication(ID3D10Predicate** ppPredicate, BOOL* pPredicateValue) override {
      ID3D11Predicate* predicate = nullptr;
      ctx()->GetPredication(ppPredicate ? &predicate : nullptr, pPredicateValue);
      if (ppPredicate)
        *ppPredicate = d3d10::take10<ID3D10Predicate>(predicate);
    }

    // ---- Output merger ----

    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D10RenderTargetView* const* ppRenderTargetViews,
                                              ID3D10DepthStencilView* pDepthStencilView) override {
      ID3D11RenderTargetView* views[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
      d3d10::unwrapArray<D3D10RenderTargetView>(views, NumViews, ppRenderTargetViews);
      ctx()->OMSetRenderTargets(NumViews, views, d3d10::unwrap<D3D10DepthStencilView>(pDepthStencilView));
    }
    void STDMETHODCALLTYPE OMSetBlendState(ID3D10BlendState* pBlendState, const FLOAT BlendFactor[4],
                                           UINT SampleMask) override {
      ctx()->OMSetBlendState(
        d3d10::unwrap<D3D10BlendState>(static_cast<ID3D10BlendState1*>(pBlendState)), BlendFactor, SampleMask);
    }
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D10DepthStencilState* pDepthStencilState, UINT StencilRef) override {
      ctx()->OMSetDepthStencilState(d3d10::unwrap<D3D10DepthStencilState>(pDepthStencilState), StencilRef);
    }

    void STDMETHODCALLTYPE OMGetRenderTargets(UINT NumViews, ID3D10RenderTargetView** ppRenderTargetViews,
                                              ID3D10DepthStencilView** ppDepthStencilView) override {
      ID3D11RenderTargetView* views[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
      ID3D11DepthStencilView* depth = nullptr;
      if (NumViews > D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return;
      ctx()->OMGetRenderTargets(ppRenderTargetViews ? NumViews : 0, ppRenderTargetViews ? views : nullptr,
        ppDepthStencilView ? &depth : nullptr);
      if (ppRenderTargetViews)
        d3d10::takeArray(ppRenderTargetViews, NumViews, views);
      if (ppDepthStencilView)
        *ppDepthStencilView = d3d10::take10<ID3D10DepthStencilView>(depth);
    }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D10BlendState** ppBlendState, FLOAT BlendFactor[4],
                                           UINT* pSampleMask) override {
      ID3D11BlendState* state = nullptr;
      ctx()->OMGetBlendState(ppBlendState ? &state : nullptr, BlendFactor, pSampleMask);
      if (ppBlendState)
        *ppBlendState = d3d10::take10<ID3D10BlendState>(state);
    }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D10DepthStencilState** ppDepthStencilState, UINT* pStencilRef) override {
      ID3D11DepthStencilState* state = nullptr;
      ctx()->OMGetDepthStencilState(ppDepthStencilState ? &state : nullptr, pStencilRef);
      if (ppDepthStencilState)
        *ppDepthStencilState = d3d10::take10<ID3D10DepthStencilState>(state);
    }

    // ---- Stream output ----

    void STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D10Buffer* const* ppSOTargets, const UINT* pOffsets) override {
      ID3D11Buffer* buffers[D3D10_SO_BUFFER_SLOT_COUNT] = {};
      d3d10::unwrapArray<D3D10Buffer>(buffers, NumBuffers, ppSOTargets);
      ctx()->SOSetTargets(NumBuffers, buffers, pOffsets);
    }
    void STDMETHODCALLTYPE SOGetTargets(UINT NumBuffers, ID3D10Buffer** ppSOTargets, UINT* pOffsets) override {
      ID3D11Buffer* buffers[D3D10_SO_BUFFER_SLOT_COUNT] = {};
      if (NumBuffers > D3D10_SO_BUFFER_SLOT_COUNT)
        return;
      if (ppSOTargets) {
        ctx()->SOGetTargets(NumBuffers, buffers);
        d3d10::takeArray(ppSOTargets, NumBuffers, buffers);
      }
      // D3D11 does not report the offsets.
      if (pOffsets) {
        for (UINT i = 0; i < NumBuffers; i++)
          pOffsets[i] = 0;
      }
    }

    // ---- Rasterizer ----

    void STDMETHODCALLTYPE RSSetState(ID3D10RasterizerState* pRasterizerState) override {
      ctx()->RSSetState(d3d10::unwrap<D3D10RasterizerState>(pRasterizerState));
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D10_VIEWPORT* pViewports) override {
      D3D11_VIEWPORT viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
      if (NumViewports > D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
        return;
      for (UINT i = 0; i < NumViewports && pViewports; i++) {
        viewports[i].TopLeftX = FLOAT(pViewports[i].TopLeftX);
        viewports[i].TopLeftY = FLOAT(pViewports[i].TopLeftY);
        viewports[i].Width    = FLOAT(pViewports[i].Width);
        viewports[i].Height   = FLOAT(pViewports[i].Height);
        viewports[i].MinDepth = pViewports[i].MinDepth;
        viewports[i].MaxDepth = pViewports[i].MaxDepth;
      }
      ctx()->RSSetViewports(NumViewports, viewports);
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D10_RECT* pRects) override {
      ctx()->RSSetScissorRects(NumRects, pRects);
    }

    void STDMETHODCALLTYPE RSGetState(ID3D10RasterizerState** ppRasterizerState) override {
      if (!ppRasterizerState)
        return;
      ID3D11RasterizerState* state = nullptr;
      ctx()->RSGetState(&state);
      *ppRasterizerState = d3d10::take10<ID3D10RasterizerState>(state);
    }
    void STDMETHODCALLTYPE RSGetViewports(UINT* NumViewports, D3D10_VIEWPORT* pViewports) override {
      if (!NumViewports)
        return;
      if (!pViewports) {
        ctx()->RSGetViewports(NumViewports, nullptr);
        return;
      }
      D3D11_VIEWPORT viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
      UINT count = (std::min)(*NumViewports, UINT(D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
      ctx()->RSGetViewports(&count, viewports);
      for (UINT i = 0; i < *NumViewports; i++) {
        const D3D11_VIEWPORT& viewport = viewports[i < D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ? i : 0];
        const bool valid = i < count;
        pViewports[i].TopLeftX = valid ? INT(viewport.TopLeftX) : 0;
        pViewports[i].TopLeftY = valid ? INT(viewport.TopLeftY) : 0;
        pViewports[i].Width    = valid ? UINT(viewport.Width) : 0;
        pViewports[i].Height   = valid ? UINT(viewport.Height) : 0;
        pViewports[i].MinDepth = valid ? viewport.MinDepth : 0.0f;
        pViewports[i].MaxDepth = valid ? viewport.MaxDepth : 0.0f;
      }
      *NumViewports = count;
    }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* NumRects, D3D10_RECT* pRects) override {
      ctx()->RSGetScissorRects(NumRects, pRects);
    }

    // ---- Resources ----

    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D10Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
                                                 UINT DstZ, ID3D10Resource* pSrcResource, UINT SrcSubresource,
                                                 const D3D10_BOX* pSrcBox) override {
      ctx()->CopySubresourceRegion(d3d10::unwrapResource(pDstResource), DstSubresource, DstX, DstY, DstZ,
        d3d10::unwrapResource(pSrcResource), SrcSubresource, reinterpret_cast<const D3D11_BOX*>(pSrcBox));
    }
    void STDMETHODCALLTYPE CopyResource(ID3D10Resource* pDstResource, ID3D10Resource* pSrcResource) override {
      ctx()->CopyResource(d3d10::unwrapResource(pDstResource), d3d10::unwrapResource(pSrcResource));
    }
    void STDMETHODCALLTYPE UpdateSubresource(ID3D10Resource* pDstResource, UINT DstSubresource, const D3D10_BOX* pDstBox,
                                             const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override {
      ctx()->UpdateSubresource(d3d10::unwrapResource(pDstResource), DstSubresource,
        reinterpret_cast<const D3D11_BOX*>(pDstBox), pSrcData, SrcRowPitch, SrcDepthPitch);
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D10RenderTargetView* pRenderTargetView,
                                                 const FLOAT ColorRGBA[4]) override {
      if (pRenderTargetView)
        ctx()->ClearRenderTargetView(d3d10::unwrap<D3D10RenderTargetView>(pRenderTargetView), ColorRGBA);
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D10DepthStencilView* pDepthStencilView, UINT ClearFlags,
                                                 FLOAT Depth, UINT8 Stencil) override {
      if (pDepthStencilView)
        ctx()->ClearDepthStencilView(d3d10::unwrap<D3D10DepthStencilView>(pDepthStencilView), ClearFlags, Depth, Stencil);
    }
    void STDMETHODCALLTYPE GenerateMips(ID3D10ShaderResourceView* pShaderResourceView) override {
      if (pShaderResourceView)
        ctx()->GenerateMips(d3d10::unwrap<D3D10ShaderResourceView>(pShaderResourceView));
    }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D10Resource* pDstResource, UINT DstSubresource,
                                              ID3D10Resource* pSrcResource, UINT SrcSubresource,
                                              DXGI_FORMAT Format) override {
      ctx()->ResolveSubresource(d3d10::unwrapResource(pDstResource), DstSubresource,
        d3d10::unwrapResource(pSrcResource), SrcSubresource, Format);
    }

    // ---- Device ----

    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return m_device->GetDeviceRemovedReason(); }
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override { return m_device->SetExceptionMode(RaiseFlags); }
    UINT STDMETHODCALLTYPE GetExceptionMode() override { return m_device->GetExceptionMode(); }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
      return m_device->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
      return m_device->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
      return m_device->SetPrivateDataInterface(guid, pData);
    }

    void STDMETHODCALLTYPE ClearState() override { ctx()->ClearState(); }
    void STDMETHODCALLTYPE Flush() override { ctx()->Flush(); }

    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D10_BUFFER_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData,
                                           ID3D10Buffer** ppBuffer) override {
      if (!pDesc)
        return E_INVALIDARG;
      const D3D11_BUFFER_DESC desc = d3d10::bufferDescTo11(*pDesc);
      ID3D11Buffer* buffer = nullptr;
      const HRESULT hr = m_device->CreateBuffer(&desc, reinterpret_cast<const D3D11_SUBRESOURCE_DATA*>(pInitialData),
        ppBuffer ? &buffer : nullptr);
      if (ppBuffer)
        *ppBuffer = SUCCEEDED(hr) ? d3d10::take10<ID3D10Buffer>(buffer) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D10_TEXTURE1D_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData,
                                              ID3D10Texture1D** ppTexture1D) override {
      if (!pDesc)
        return E_INVALIDARG;
      const auto desc = d3d10::textureDescTo11<D3D11_TEXTURE1D_DESC>(*pDesc);
      ID3D11Texture1D* texture = nullptr;
      const HRESULT hr = m_device->CreateTexture1D(&desc, reinterpret_cast<const D3D11_SUBRESOURCE_DATA*>(pInitialData),
        ppTexture1D ? &texture : nullptr);
      if (ppTexture1D)
        *ppTexture1D = SUCCEEDED(hr) ? d3d10::take10<ID3D10Texture1D>(texture) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D10_TEXTURE2D_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData,
                                              ID3D10Texture2D** ppTexture2D) override {
      if (!pDesc)
        return E_INVALIDARG;
      const auto desc = d3d10::textureDescTo11<D3D11_TEXTURE2D_DESC>(*pDesc);
      ID3D11Texture2D* texture = nullptr;
      const HRESULT hr = m_device->CreateTexture2D(&desc, reinterpret_cast<const D3D11_SUBRESOURCE_DATA*>(pInitialData),
        ppTexture2D ? &texture : nullptr);
      if (ppTexture2D)
        *ppTexture2D = SUCCEEDED(hr) ? d3d10::take10<ID3D10Texture2D>(texture) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D10_TEXTURE3D_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData,
                                              ID3D10Texture3D** ppTexture3D) override {
      if (!pDesc)
        return E_INVALIDARG;
      const auto desc = d3d10::textureDescTo11<D3D11_TEXTURE3D_DESC>(*pDesc);
      ID3D11Texture3D* texture = nullptr;
      const HRESULT hr = m_device->CreateTexture3D(&desc, reinterpret_cast<const D3D11_SUBRESOURCE_DATA*>(pInitialData),
        ppTexture3D ? &texture : nullptr);
      if (ppTexture3D)
        *ppTexture3D = SUCCEEDED(hr) ? d3d10::take10<ID3D10Texture3D>(texture) : nullptr;
      return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D10Resource* pResource,
                                                       const D3D10_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                       ID3D10ShaderResourceView** ppSRView) override {
      D3D10_SHADER_RESOURCE_VIEW_DESC1 desc1 = {};
      if (pDesc)
        std::memcpy(&desc1, pDesc, sizeof(*pDesc));
      return CreateShaderResourceView1(pResource, pDesc ? &desc1 : nullptr,
        reinterpret_cast<ID3D10ShaderResourceView1**>(ppSRView));
    }
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(ID3D10Resource* pResource,
                                                        const D3D10_SHADER_RESOURCE_VIEW_DESC1* pDesc,
                                                        ID3D10ShaderResourceView1** ppSRView) override {
      ID3D11Resource* resource = d3d10::unwrapResource(pResource);
      if (!resource)
        return E_INVALIDARG;
      D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
      if (pDesc)
        desc = d3d10::srvDescTo11(*pDesc);
      ID3D11ShaderResourceView* view = nullptr;
      const HRESULT hr = m_device->CreateShaderResourceView(resource, pDesc ? &desc : nullptr, ppSRView ? &view : nullptr);
      if (ppSRView)
        *ppSRView = SUCCEEDED(hr) ? d3d10::take10<ID3D10ShaderResourceView1>(view) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D10Resource* pResource, const D3D10_RENDER_TARGET_VIEW_DESC* pDesc,
                                                     ID3D10RenderTargetView** ppRTView) override {
      ID3D11Resource* resource = d3d10::unwrapResource(pResource);
      if (!resource)
        return E_INVALIDARG;
      ID3D11RenderTargetView* view = nullptr;
      const HRESULT hr = m_device->CreateRenderTargetView(resource,
        reinterpret_cast<const D3D11_RENDER_TARGET_VIEW_DESC*>(pDesc), ppRTView ? &view : nullptr);
      if (ppRTView)
        *ppRTView = SUCCEEDED(hr) ? d3d10::take10<ID3D10RenderTargetView>(view) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D10Resource* pResource, const D3D10_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                     ID3D10DepthStencilView** ppDepthStencilView) override {
      ID3D11Resource* resource = d3d10::unwrapResource(pResource);
      if (!resource)
        return E_INVALIDARG;
      D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
      if (pDesc)
        desc = d3d10::dsvDescTo11(*pDesc);
      ID3D11DepthStencilView* view = nullptr;
      const HRESULT hr = m_device->CreateDepthStencilView(resource, pDesc ? &desc : nullptr,
        ppDepthStencilView ? &view : nullptr);
      if (ppDepthStencilView)
        *ppDepthStencilView = SUCCEEDED(hr) ? d3d10::take10<ID3D10DepthStencilView>(view) : nullptr;
      return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D10_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
                                                const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
                                                ID3D10InputLayout** ppInputLayout) override {
      static_assert(sizeof(D3D10_INPUT_ELEMENT_DESC) == sizeof(D3D11_INPUT_ELEMENT_DESC), "descriptions differ");
      ID3D11InputLayout* layout = nullptr;
      const HRESULT hr = m_device->CreateInputLayout(reinterpret_cast<const D3D11_INPUT_ELEMENT_DESC*>(pInputElementDescs),
        NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout ? &layout : nullptr);
      if (ppInputLayout)
        *ppInputLayout = SUCCEEDED(hr) ? d3d10::take10<ID3D10InputLayout>(layout) : nullptr;
      return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                 ID3D10VertexShader** ppVertexShader) override {
      ID3D11VertexShader* shader = nullptr;
      const HRESULT hr = m_device->CreateVertexShader(pShaderBytecode, BytecodeLength, nullptr,
        ppVertexShader ? &shader : nullptr);
      if (ppVertexShader)
        *ppVertexShader = SUCCEEDED(hr) ? d3d10::take10<ID3D10VertexShader>(shader) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                   ID3D10GeometryShader** ppGeometryShader) override {
      ID3D11GeometryShader* shader = nullptr;
      const HRESULT hr = m_device->CreateGeometryShader(pShaderBytecode, BytecodeLength, nullptr,
        ppGeometryShader ? &shader : nullptr);
      if (ppGeometryShader)
        *ppGeometryShader = SUCCEEDED(hr) ? d3d10::take10<ID3D10GeometryShader>(shader) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                                   const D3D10_SO_DECLARATION_ENTRY* pSODeclaration,
                                                                   UINT NumEntries, UINT OutputStreamStride,
                                                                   ID3D10GeometryShader** ppGeometryShader) override {
      if (NumEntries > D3D10_SO_SINGLE_BUFFER_COMPONENT_LIMIT || (NumEntries && !pSODeclaration))
        return E_INVALIDARG;
      D3D11_SO_DECLARATION_ENTRY entries[D3D10_SO_SINGLE_BUFFER_COMPONENT_LIMIT] = {};
      // D3D10 gives the stride of the one buffer; with several buffers each
      // holds one element, as wide as its components.
      UINT strides[D3D10_SO_BUFFER_SLOT_COUNT] = {};
      UINT bufferCount = 0;
      for (UINT i = 0; i < NumEntries; i++) {
        const auto& entry = pSODeclaration[i];
        entries[i].Stream         = 0;
        entries[i].SemanticName   = entry.SemanticName;
        entries[i].SemanticIndex  = entry.SemanticIndex;
        entries[i].StartComponent = entry.StartComponent;
        entries[i].ComponentCount = entry.ComponentCount;
        entries[i].OutputSlot     = entry.OutputSlot;
        if (entry.OutputSlot >= D3D10_SO_BUFFER_SLOT_COUNT)
          return E_INVALIDARG;
        strides[entry.OutputSlot] += UINT(entry.ComponentCount) * 4;
        bufferCount = (std::max)(bufferCount, UINT(entry.OutputSlot) + 1);
      }
      if (bufferCount <= 1) {
        strides[0] = OutputStreamStride;
        bufferCount = 1;
      }
      ID3D11GeometryShader* shader = nullptr;
      const HRESULT hr = m_device->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength,
        NumEntries ? entries : nullptr, NumEntries, strides, bufferCount, 0, nullptr,
        ppGeometryShader ? &shader : nullptr);
      if (ppGeometryShader)
        *ppGeometryShader = SUCCEEDED(hr) ? d3d10::take10<ID3D10GeometryShader>(shader) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                ID3D10PixelShader** ppPixelShader) override {
      ID3D11PixelShader* shader = nullptr;
      const HRESULT hr = m_device->CreatePixelShader(pShaderBytecode, BytecodeLength, nullptr,
        ppPixelShader ? &shader : nullptr);
      if (ppPixelShader)
        *ppPixelShader = SUCCEEDED(hr) ? d3d10::take10<ID3D10PixelShader>(shader) : nullptr;
      return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D10_BLEND_DESC* pBlendStateDesc,
                                               ID3D10BlendState** ppBlendState) override {
      if (!pBlendStateDesc)
        return E_INVALIDARG;
      const D3D11_BLEND_DESC desc = d3d10::blendDescTo11(*pBlendStateDesc);
      ID3D11BlendState* state = nullptr;
      const HRESULT hr = m_device->CreateBlendState(&desc, ppBlendState ? &state : nullptr);
      if (ppBlendState)
        *ppBlendState = SUCCEEDED(hr) ? d3d10::take10<ID3D10BlendState>(state) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateBlendState1(const D3D10_BLEND_DESC1* pBlendStateDesc,
                                                ID3D10BlendState1** ppBlendState) override {
      if (!pBlendStateDesc)
        return E_INVALIDARG;
      const D3D11_BLEND_DESC desc = d3d10::blendDesc1To11(*pBlendStateDesc);
      ID3D11BlendState* state = nullptr;
      const HRESULT hr = m_device->CreateBlendState(&desc, ppBlendState ? &state : nullptr);
      if (ppBlendState)
        *ppBlendState = SUCCEEDED(hr) ? d3d10::take10<ID3D10BlendState1>(state) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D10_DEPTH_STENCIL_DESC* pDepthStencilDesc,
                                                      ID3D10DepthStencilState** ppDepthStencilState) override {
      ID3D11DepthStencilState* state = nullptr;
      const HRESULT hr = m_device->CreateDepthStencilState(
        reinterpret_cast<const D3D11_DEPTH_STENCIL_DESC*>(pDepthStencilDesc), ppDepthStencilState ? &state : nullptr);
      if (ppDepthStencilState)
        *ppDepthStencilState = SUCCEEDED(hr) ? d3d10::take10<ID3D10DepthStencilState>(state) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D10_RASTERIZER_DESC* pRasterizerDesc,
                                                    ID3D10RasterizerState** ppRasterizerState) override {
      ID3D11RasterizerState* state = nullptr;
      const HRESULT hr = m_device->CreateRasterizerState(
        reinterpret_cast<const D3D11_RASTERIZER_DESC*>(pRasterizerDesc), ppRasterizerState ? &state : nullptr);
      if (ppRasterizerState)
        *ppRasterizerState = SUCCEEDED(hr) ? d3d10::take10<ID3D10RasterizerState>(state) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                 ID3D10SamplerState** ppSamplerState) override {
      ID3D11SamplerState* state = nullptr;
      const HRESULT hr = m_device->CreateSamplerState(
        reinterpret_cast<const D3D11_SAMPLER_DESC*>(pSamplerDesc), ppSamplerState ? &state : nullptr);
      if (ppSamplerState)
        *ppSamplerState = SUCCEEDED(hr) ? d3d10::take10<ID3D10SamplerState>(state) : nullptr;
      return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D10_QUERY_DESC* pQueryDesc, ID3D10Query** ppQuery) override {
      if (!pQueryDesc)
        return E_INVALIDARG;
      const D3D11_QUERY_DESC desc = { D3D11_QUERY(pQueryDesc->Query), pQueryDesc->MiscFlags };
      ID3D11Query* query = nullptr;
      const HRESULT hr = m_device->CreateQuery(&desc, ppQuery ? &query : nullptr);
      if (ppQuery)
        *ppQuery = SUCCEEDED(hr) ? d3d10::take10<ID3D10Query>(query) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D10_QUERY_DESC* pPredicateDesc,
                                              ID3D10Predicate** ppPredicate) override {
      if (!pPredicateDesc)
        return E_INVALIDARG;
      const D3D11_QUERY_DESC desc = { D3D11_QUERY(pPredicateDesc->Query), pPredicateDesc->MiscFlags };
      ID3D11Predicate* predicate = nullptr;
      const HRESULT hr = m_device->CreatePredicate(&desc, ppPredicate ? &predicate : nullptr);
      if (ppPredicate)
        *ppPredicate = SUCCEEDED(hr) ? d3d10::take10<ID3D10Predicate>(predicate) : nullptr;
      return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D10_COUNTER_DESC* pCounterDesc, ID3D10Counter** ppCounter) override {
      static_assert(sizeof(D3D10_COUNTER_DESC) == sizeof(D3D11_COUNTER_DESC), "descriptions differ");
      if (ppCounter)
        *ppCounter = nullptr;
      // The device has no counters: D3D11 explains why it fails.
      const HRESULT hr = m_device->CreateCounter(reinterpret_cast<const D3D11_COUNTER_DESC*>(pCounterDesc), nullptr);
      return FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override {
      return m_device->CheckFormatSupport(Format, pFormatSupport);
    }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount,
                                                            UINT* pNumQualityLevels) override {
      return m_device->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels);
    }
    void STDMETHODCALLTYPE CheckCounterInfo(D3D10_COUNTER_INFO* pCounterInfo) override {
      static_assert(sizeof(D3D10_COUNTER_INFO) == sizeof(D3D11_COUNTER_INFO), "descriptions differ");
      m_device->CheckCounterInfo(reinterpret_cast<D3D11_COUNTER_INFO*>(pCounterInfo));
    }
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D10_COUNTER_DESC* pDesc, D3D10_COUNTER_TYPE* pType,
                                           UINT* pActiveCounters, LPSTR szName, UINT* pNameLength, LPSTR szUnits,
                                           UINT* pUnitsLength, LPSTR szDescription, UINT* pDescriptionLength) override {
      return m_device->CheckCounter(reinterpret_cast<const D3D11_COUNTER_DESC*>(pDesc),
        reinterpret_cast<D3D11_COUNTER_TYPE*>(pType), pActiveCounters, szName, pNameLength, szUnits, pUnitsLength,
        szDescription, pDescriptionLength);
    }

    UINT STDMETHODCALLTYPE GetCreationFlags() override { return m_creationFlags; }

    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void** ppResource) override {
      if (!ppResource)
        return E_INVALIDARG;
      *ppResource = nullptr;
      ID3D11Resource* resource = nullptr;
      HRESULT hr = m_device->OpenSharedResource(hResource, __uuidof(ID3D11Resource), reinterpret_cast<void**>(&resource));
      if (FAILED(hr))
        return hr;
      hr = resource->QueryInterface(ReturnedInterface, ppResource);
      resource->Release();
      return hr;
    }

    void STDMETHODCALLTYPE SetTextFilterSize(UINT Width, UINT Height) override {
      m_textFilterWidth = Width;
      m_textFilterHeight = Height;
    }
    void STDMETHODCALLTYPE GetTextFilterSize(UINT* pWidth, UINT* pHeight) override {
      if (pWidth)
        *pWidth = m_textFilterWidth;
      if (pHeight)
        *pHeight = m_textFilterHeight;
    }

    D3D10_FEATURE_LEVEL1 STDMETHODCALLTYPE GetFeatureLevel() override {
      return D3D10_FEATURE_LEVEL1(m_device->GetFeatureLevel());
    }

  private:
    // The immediate context lives as long as the device.
    ID3D11DeviceContext* ctx() {
      ID3D11DeviceContext* context = m_context.load(std::memory_order_acquire);
      if (!context) {
        m_device->GetImmediateContext(&context);
        context->Release();
        m_context.store(context, std::memory_order_release);
      }
      return context;
    }

    ID3D11Device*                     m_device;
    std::atomic<ID3D11DeviceContext*> m_context{nullptr};
    UINT                              m_creationFlags = 0;
    UINT                              m_textFilterWidth = 0;
    UINT                              m_textFilterHeight = 0;
  };

}

#endif // _WIN32
