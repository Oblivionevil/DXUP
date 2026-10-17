#pragma once

#include "../directx/directx_backend.h"
#include "d3d11_d3d12_bridge.h"

#ifdef _WIN32
#include <d3d11.h>
#endif

namespace dxup {

  // ============================================================
  // D3D11 Backend Immediate Context
  //
  // Wraps IBackendContext to provide D3D11-compatible operations.
  // This serves as the bridge between D3D11 API calls and the
  // DirectX backend abstraction.
  // ============================================================
  class D3D11BackendContext {
  public:
    D3D11BackendContext(Rc<IBackendContext> backendCtx);
    ~D3D11BackendContext();

    IBackendContext* getBackend() const { return m_ctx.get(); }

    // ---- Input Assembler Stage ----
    void IASetVertexBuffers(uint32_t startSlot, uint32_t numBuffers,
                            IBackendBuffer* const* buffers,
                            const uint32_t* strides, const uint32_t* offsets);
    void IASetIndexBuffer(IBackendBuffer* buffer, uint32_t offset, IndexType format);
    void IASetPrimitiveTopology(PrimitiveTopology topology);

    // ---- Vertex/Pixel/etc Shader Stage ----
    void VSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers);
    void PSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers);
    void GSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers);
    void HSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers);
    void DSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers);
    void CSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers);

    void VSSetShaderResources(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views);
    void PSSetShaderResources(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views);
    void CSSetShaderResources(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views);

    void PSSetSamplers(uint32_t startSlot, uint32_t numSamplers, IBackendSampler* const* samplers);
    void VSSetSamplers(uint32_t startSlot, uint32_t numSamplers, IBackendSampler* const* samplers);

    void CSSetUnorderedAccessViews(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views);

    // ---- Output Merger Stage ----
    void OMSetRenderTargets(uint32_t numViews, IBackendImageView* const* rtvs, IBackendImageView* dsv);
    void OMSetBlendState(const BlendState& state, const float blendFactor[4], uint32_t sampleMask);
    void OMSetDepthStencilState(const DepthStencilState& state, uint32_t stencilRef);

    // ---- Rasterizer Stage ----
    void ClearState();
    void RSSetViewports(uint32_t numViewports, const Viewport* viewports);
    void RSSetScissorRects(uint32_t numRects, const Rect* rects);
    void RSSetState(const RasterizerState& state);

    // ---- Draw Commands ----
    void Draw(uint32_t vertexCount, uint32_t startVertexLocation);
    void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation);
    void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                       uint32_t startVertexLocation, uint32_t startInstanceLocation);
    void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                              uint32_t startIndexLocation, int32_t baseVertexLocation,
                              uint32_t startInstanceLocation);

    // ---- Compute ----
    void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ);

    // ---- Copy Operations ----
    void CopyResource(IBackendBuffer* dst, IBackendBuffer* src);
    void CopySubresourceRegion(IBackendImage* dst, uint32_t dstSubresource,
                                IBackendImage* src, uint32_t srcSubresource);
    void UpdateSubresource(IBackendBuffer* buffer, const void* data, uint64_t size);

    // ---- Map/Unmap ----
    void* Map(IBackendBuffer* buffer, MapMode mode);
    void Unmap(IBackendBuffer* buffer);

    // ---- Clear ----
    void ClearRenderTargetView(IBackendImageView* rtv, const float color[4]);
    void ClearDepthStencilView(IBackendImageView* dsv, uint32_t clearFlags, float depth, uint8_t stencil);

    // ---- Misc ----
    void GenerateMips(IBackendImageView* srv);
    void ResolveSubresource(IBackendImage* dst, uint32_t dstSubres,
                             IBackendImage* src, uint32_t srcSubres, DXGI_FORMAT format);
    void Flush();

  private:
    Rc<IBackendContext> m_ctx;

    void setConstantBuffersForStage(ShaderStage stage, uint32_t startSlot,
                                     uint32_t numBuffers, IBackendBuffer* const* buffers);
    void setShaderResourcesForStage(ShaderStage stage, uint32_t startSlot,
                                     uint32_t numViews, IBackendImageView* const* views);
    void setSamplersForStage(ShaderStage stage, uint32_t startSlot,
                              uint32_t numSamplers, IBackendSampler* const* samplers);
  };

}
