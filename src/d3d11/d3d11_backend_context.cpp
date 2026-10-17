#include "d3d11_backend_context.h"

#ifndef _WIN32
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

namespace dxup {

  D3D11BackendContext::D3D11BackendContext(Rc<IBackendContext> backendCtx)
  : m_ctx(std::move(backendCtx)) {
  }

  D3D11BackendContext::~D3D11BackendContext() {
  }

  // ============================================================
  // Input Assembler Stage
  // ============================================================

  void D3D11BackendContext::IASetVertexBuffers(
    uint32_t startSlot, uint32_t numBuffers,
    IBackendBuffer* const* buffers,
    const uint32_t* strides, const uint32_t* offsets) {
    for (uint32_t i = 0; i < numBuffers; i++) {
      m_ctx->bindVertexBuffer(startSlot + i, buffers[i], offsets[i], strides[i]);
    }
  }

  void D3D11BackendContext::IASetIndexBuffer(IBackendBuffer* buffer, uint32_t offset, IndexType format) {
    m_ctx->bindIndexBuffer(buffer, offset, format);
  }

  void D3D11BackendContext::IASetPrimitiveTopology(PrimitiveTopology topology) {
    m_ctx->setPrimitiveTopology(topology);
  }

  // ============================================================
  // Shader Constant Buffers
  // ============================================================

  void D3D11BackendContext::setConstantBuffersForStage(
    ShaderStage stage, uint32_t startSlot,
    uint32_t numBuffers, IBackendBuffer* const* buffers) {
    for (uint32_t i = 0; i < numBuffers; i++) {
      m_ctx->bindConstantBuffer(stage, startSlot + i, buffers[i]);
    }
  }

  void D3D11BackendContext::VSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers) {
    setConstantBuffersForStage(ShaderStage::Vertex, startSlot, numBuffers, buffers);
  }

  void D3D11BackendContext::PSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers) {
    setConstantBuffersForStage(ShaderStage::Pixel, startSlot, numBuffers, buffers);
  }

  void D3D11BackendContext::GSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers) {
    setConstantBuffersForStage(ShaderStage::Geometry, startSlot, numBuffers, buffers);
  }

  void D3D11BackendContext::HSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers) {
    setConstantBuffersForStage(ShaderStage::Hull, startSlot, numBuffers, buffers);
  }

  void D3D11BackendContext::DSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers) {
    setConstantBuffersForStage(ShaderStage::Domain, startSlot, numBuffers, buffers);
  }

  void D3D11BackendContext::CSSetConstantBuffers(uint32_t startSlot, uint32_t numBuffers, IBackendBuffer* const* buffers) {
    setConstantBuffersForStage(ShaderStage::Compute, startSlot, numBuffers, buffers);
  }

  // ============================================================
  // Shader Resources
  // ============================================================

  void D3D11BackendContext::setShaderResourcesForStage(
    ShaderStage stage, uint32_t startSlot,
    uint32_t numViews, IBackendImageView* const* views) {
    for (uint32_t i = 0; i < numViews; i++) {
      m_ctx->bindShaderResource(stage, startSlot + i, views[i]);
    }
  }

  void D3D11BackendContext::VSSetShaderResources(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views) {
    setShaderResourcesForStage(ShaderStage::Vertex, startSlot, numViews, views);
  }

  void D3D11BackendContext::PSSetShaderResources(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views) {
    setShaderResourcesForStage(ShaderStage::Pixel, startSlot, numViews, views);
  }

  void D3D11BackendContext::CSSetShaderResources(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views) {
    setShaderResourcesForStage(ShaderStage::Compute, startSlot, numViews, views);
  }

  // ============================================================
  // Samplers
  // ============================================================

  void D3D11BackendContext::setSamplersForStage(
    ShaderStage stage, uint32_t startSlot,
    uint32_t numSamplers, IBackendSampler* const* samplers) {
    for (uint32_t i = 0; i < numSamplers; i++) {
      m_ctx->bindSampler(stage, startSlot + i, samplers[i]);
    }
  }

  void D3D11BackendContext::PSSetSamplers(uint32_t startSlot, uint32_t numSamplers, IBackendSampler* const* samplers) {
    setSamplersForStage(ShaderStage::Pixel, startSlot, numSamplers, samplers);
  }

  void D3D11BackendContext::VSSetSamplers(uint32_t startSlot, uint32_t numSamplers, IBackendSampler* const* samplers) {
    setSamplersForStage(ShaderStage::Vertex, startSlot, numSamplers, samplers);
  }

  // ============================================================
  // UAVs
  // ============================================================

  void D3D11BackendContext::CSSetUnorderedAccessViews(uint32_t startSlot, uint32_t numViews, IBackendImageView* const* views) {
    for (uint32_t i = 0; i < numViews; i++) {
      m_ctx->bindUnorderedAccess(ShaderStage::Compute, startSlot + i, views[i]);
    }
  }

  // ============================================================
  // Output Merger
  // ============================================================

  void D3D11BackendContext::OMSetRenderTargets(uint32_t numViews, IBackendImageView* const* rtvs, IBackendImageView* dsv) {
    // Need to cast away const for the interface
    m_ctx->bindRenderTargets(numViews, const_cast<IBackendImageView**>(rtvs), dsv);
  }

  void D3D11BackendContext::OMSetBlendState(const BlendState& state, const float blendFactor[4], uint32_t sampleMask) {
    (void)sampleMask;
    m_ctx->setBlendState(state);
    if (blendFactor)
      m_ctx->setBlendFactor(blendFactor);
  }

  void D3D11BackendContext::OMSetDepthStencilState(const DepthStencilState& state, uint32_t stencilRef) {
    m_ctx->setDepthStencilState(state);
    m_ctx->setStencilRef(stencilRef);
  }

  // ============================================================
  // Rasterizer
  // ============================================================

  void D3D11BackendContext::ClearState() {
    m_ctx->clearState();
  }

  void D3D11BackendContext::RSSetViewports(uint32_t numViewports, const Viewport* viewports) {
    m_ctx->setViewports(numViewports, viewports);
  }

  void D3D11BackendContext::RSSetScissorRects(uint32_t numRects, const Rect* rects) {
    m_ctx->setScissorRects(numRects, rects);
  }

  void D3D11BackendContext::RSSetState(const RasterizerState& state) {
    m_ctx->setRasterizerState(state);
  }

  // ============================================================
  // Draw Commands
  // ============================================================

  void D3D11BackendContext::Draw(uint32_t vertexCount, uint32_t startVertexLocation) {
    m_ctx->draw(vertexCount, 1, startVertexLocation, 0);
  }

  void D3D11BackendContext::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) {
    m_ctx->drawIndexed(indexCount, 1, startIndexLocation, baseVertexLocation, 0);
  }

  void D3D11BackendContext::DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                                           uint32_t startVertexLocation, uint32_t startInstanceLocation) {
    m_ctx->draw(vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
  }

  void D3D11BackendContext::DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                                                  uint32_t startIndexLocation, int32_t baseVertexLocation,
                                                  uint32_t startInstanceLocation) {
    m_ctx->drawIndexed(indexCountPerInstance, instanceCount, startIndexLocation,
                       baseVertexLocation, startInstanceLocation);
  }

  // ============================================================
  // Compute
  // ============================================================

  void D3D11BackendContext::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
    m_ctx->dispatch(x, y, z);
  }

  // ============================================================
  // Copy Operations
  // ============================================================

  void D3D11BackendContext::CopyResource(IBackendBuffer* dst, IBackendBuffer* src) {
    uint64_t size = (dst->getDesc().size < src->getDesc().size)
      ? dst->getDesc().size : src->getDesc().size;
    m_ctx->copyBuffer(dst, 0, src, 0, size);
  }

  void D3D11BackendContext::CopySubresourceRegion(IBackendImage* dst, uint32_t dstSubresource,
                                                    IBackendImage* src, uint32_t srcSubresource) {
    ImageSubresource dstSubres;
    dstSubres.mipLevel = dstSubresource % dst->getDesc().mipLevels;
    dstSubres.arrayLayer = dstSubresource / dst->getDesc().mipLevels;

    ImageSubresource srcSubres;
    srcSubres.mipLevel = srcSubresource % src->getDesc().mipLevels;
    srcSubres.arrayLayer = srcSubresource / src->getDesc().mipLevels;

    m_ctx->copyImage(dst, dstSubres, src, srcSubres);
  }

  void D3D11BackendContext::UpdateSubresource(IBackendBuffer* buffer, const void* data, uint64_t size) {
    m_ctx->updateBuffer(buffer, 0, size, data);
  }

  // ============================================================
  // Map/Unmap
  // ============================================================

  void* D3D11BackendContext::Map(IBackendBuffer* buffer, MapMode mode) {
    return m_ctx->mapBuffer(buffer, mode);
  }

  void D3D11BackendContext::Unmap(IBackendBuffer* buffer) {
    m_ctx->unmapBuffer(buffer);
  }

  // ============================================================
  // Clear
  // ============================================================

  void D3D11BackendContext::ClearRenderTargetView(IBackendImageView* rtv, const float color[4]) {
    m_ctx->clearRenderTarget(rtv, color);
  }

  void D3D11BackendContext::ClearDepthStencilView(IBackendImageView* dsv, uint32_t clearFlags, float depth, uint8_t stencil) {
    m_ctx->clearDepthStencil(dsv, depth, stencil, clearFlags);
  }

  // ============================================================
  // Misc
  // ============================================================

  void D3D11BackendContext::GenerateMips(IBackendImageView* srv) {
    m_ctx->generateMips(srv);
  }

  void D3D11BackendContext::ResolveSubresource(IBackendImage* dst, uint32_t dstSubres,
                                                IBackendImage* src, uint32_t srcSubres, DXGI_FORMAT format) {
    m_ctx->resolveSubresource(dst, dstSubres, src, srcSubres, format);
  }

  void D3D11BackendContext::Flush() {
    m_ctx->flush();
  }

}
