// Spatial upscaling at Present (dxup.conf dxgi.upscale): a back buffer
// smaller than the window is scaled up into the swap chain's buffer by
// DXUP rather than stretched bilinearly by the system. A Catmull-Rom filter
// (five bilinear taps) clamped to the four nearest texels against ringing,
// then contrast-adaptive sharpening at the output resolution.

#include "d3d12_backend_context.h"

#include <cstring>

#ifdef _WIN32
#include <d3dcompiler.h>
#endif

namespace dxup::d3d12 {

#ifdef _WIN32
  namespace {

    const char kScaleShader[] =
      "Texture2D<float4> src : register(t0);\n"
      "SamplerState linearClamp : register(s0);\n"
      "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
      "  float2 size;\n"
      "  src.GetDimensions(size.x, size.y);\n"
      "  const float2 p = uv * size - 0.5f;\n"
      "  const float2 base = floor(p);\n"
      "  const float2 f = p - base;\n"
      "  const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));\n"
      "  const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);\n"
      "  const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));\n"
      "  const float2 w3 = f * f * (-0.5f + 0.5f * f);\n"
      "  const float2 w12 = w1 + w2;\n"
      "  const float2 t0 = (base - 0.5f) / size;\n"
      "  const float2 t12 = (base + 0.5f + w2 / w12) / size;\n"
      "  const float2 t3 = (base + 2.5f) / size;\n"
      // The four corner taps carry little weight: left out, the rest renormalized.
      "  const float wn = w12.x * w0.y, ww = w0.x * w12.y, wc = w12.x * w12.y, we = w3.x * w12.y, ws = w12.x * w3.y;\n"
      "  float4 c = src.SampleLevel(linearClamp, float2(t12.x, t0.y), 0.0f) * wn\n"
      "           + src.SampleLevel(linearClamp, float2(t0.x, t12.y), 0.0f) * ww\n"
      "           + src.SampleLevel(linearClamp, t12, 0.0f) * wc\n"
      "           + src.SampleLevel(linearClamp, float2(t3.x, t12.y), 0.0f) * we\n"
      "           + src.SampleLevel(linearClamp, float2(t12.x, t3.y), 0.0f) * ws;\n"
      "  c /= wn + ww + wc + we + ws;\n"
      // No brighter or darker than the texels around the sample position.
      "  const int2 last = int2(size) - 1;\n"
      "  const int2 ip = int2(base);\n"
      "  const float4 a = src.Load(int3(clamp(ip, 0, last), 0));\n"
      "  const float4 b = src.Load(int3(clamp(ip + int2(1, 0), 0, last), 0));\n"
      "  const float4 d = src.Load(int3(clamp(ip + int2(0, 1), 0, last), 0));\n"
      "  const float4 e = src.Load(int3(clamp(ip + int2(1, 1), 0, last), 0));\n"
      "  return clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e)));\n"
      "}\n";

    // Contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT license):
    // a negative cross lobe, weaker where the neighbourhood is close to the
    // ends of [0, 1], so that it never clips.
    const char kSharpenShader[] =
      "Texture2D<float4> src : register(t0);\n"
      "cbuffer Constants : register(b0) { float sharpness; };\n"
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  uint width, height;\n"
      "  src.GetDimensions(width, height);\n"
      "  const int2 last = int2(width, height) - 1;\n"
      "  const int2 p = int2(pos.xy);\n"
      "  const float4 c = src.Load(int3(p, 0));\n"
      "  const float3 n = src.Load(int3(clamp(p + int2(0, -1), 0, last), 0)).rgb;\n"
      "  const float3 s = src.Load(int3(clamp(p + int2(0, 1), 0, last), 0)).rgb;\n"
      "  const float3 w = src.Load(int3(clamp(p + int2(-1, 0), 0, last), 0)).rgb;\n"
      "  const float3 e = src.Load(int3(clamp(p + int2(1, 0), 0, last), 0)).rgb;\n"
      "  const float3 mn = min(c.rgb, min(min(n, s), min(w, e)));\n"
      "  const float3 mx = max(c.rgb, max(max(n, s), max(w, e)));\n"
      "  const float3 amp = sqrt(saturate(min(mn, 1.0f - mx) / max(mx, 1.0f / 65536.0f)));\n"
      "  const float3 lobe = amp * (-1.0f / lerp(8.0f, 5.0f, sharpness));\n"
      "  const float3 result = (c.rgb + (n + s + w + e) * lobe) / (1.0f + 4.0f * lobe);\n"
      "  return float4(saturate(result), c.a);\n"
      "}\n";

    DXGI_FORMAT unormOf(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:                              return format;
      }
    }

    bool isFloatFormat(DXGI_FORMAT format) {
      return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT
          || format == DXGI_FORMAT_R11G11B10_FLOAT;
    }

  }
#endif

  ID3D12PipelineState* D3D12BackendContext::getOrCreateScalePso(uint32_t pass, DXGI_FORMAT format) {
#ifdef _WIN32
    const uint32_t key = (pass << 16) | static_cast<uint32_t>(format);
    auto it = m_scalePsoCache.find(key);
    if (it != m_scalePsoCache.end())
      return it->second.Get();
    ComPtr<ID3DBlob>& blob = m_scalePassPs[pass & 1];
    if (!blob) {
      const char* source = pass ? kSharpenShader : kScaleShader;
      ComPtr<ID3DBlob> errors;
      const HRESULT hr = D3DCompile(source, strlen(source), "scale_pass_ps", nullptr, nullptr, "main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "spatial upscale: shader compile failed: 0x%08x %s", hr,
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return nullptr;
      }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_mipGenRootSignature.Get();
    psoDesc.VS = { m_mipGenVsBytecode->GetBufferPointer(), m_mipGenVsBytecode->GetBufferSize() };
    psoDesc.PS = { blob->GetBufferPointer(), blob->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = format;
    psoDesc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = m_device->getDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "spatial upscale: PSO failed: pass=%u fmt=%u hr=0x%08x", pass,
        static_cast<uint32_t>(format), hr);
      return nullptr;
    }
    ID3D12PipelineState* result = pso.Get();
    m_scalePsoCache[key] = std::move(pso);
    return result;
#else
    (void)pass; (void)format;
    return nullptr;
#endif
  }

  void D3D12BackendContext::drawScalePass(ID3D12PipelineState* pso, D3D12BackendImage* dst, DXGI_FORMAT dstFormat,
                                          D3D12BackendImage* src, DXGI_FORMAT srcFormat, uint32_t constant) {
#ifdef _WIN32
    holdBatchResource(dst->getResource());
    holdBatchResource(src->getResource());
    transitionResource(src, kShaderReadState);
    transitionResource(dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
    flushBarriers();

    auto* device = m_device->getDevice();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srcFormat;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv = allocateShaderVisibleCbvSrvUav();
    const uint32_t srvIndex = m_cbvSrvUavHeapOffset - 1;
    device->CreateShaderResourceView(src->getResource(), &srvDesc, cpuSrv);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv;
    gpuSrv.ptr = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr
      + srvIndex * m_cbvSrvUavDescSize;

    if (!m_mipGenRtv.ptr)
      m_mipGenRtv = m_device->getDescriptorHeapManager().allocateRtv();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = dstFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(dst->getResource(), &rtvDesc, m_mipGenRtv);
    m_cmdList->OMSetRenderTargets(1, &m_mipGenRtv, FALSE, nullptr);

    const auto& dstDesc = dst->getDesc();
    m_cmdList->SetPipelineState(pso);
    m_cmdList->SetGraphicsRootSignature(m_mipGenRootSignature.Get());
    invalidateRootTableCache();
    ID3D12DescriptorHeap* heaps[] = { m_shaderVisibleCbvSrvUavHeap.Get(), m_shaderVisibleSamplerHeap.Get() };
    m_cmdList->SetDescriptorHeaps(2, heaps);
    m_cmdList->SetGraphicsRootDescriptorTable(0, gpuSrv);
    m_cmdList->SetGraphicsRoot32BitConstant(1, constant, 0);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(dstDesc.width);
    viewport.Height = static_cast<float>(dstDesc.height);
    viewport.MaxDepth = 1.0f;
    m_cmdList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(dstDesc.width), static_cast<LONG>(dstDesc.height) };
    m_cmdList->RSSetScissorRects(1, &scissor);
    m_cmdList->DrawInstanced(3, 1, 0, 0);
#else
    (void)pso; (void)dst; (void)dstFormat; (void)src; (void)srcFormat; (void)constant;
#endif
  }

  bool D3D12BackendContext::scaleImage(IBackendImage* dstImage, IBackendImage* srcImage, float sharpness) {
#ifdef _WIN32
    ContextLock ctxLock(*this);
    auto* dst = static_cast<D3D12BackendImage*>(dstImage);
    auto* src = static_cast<D3D12BackendImage*>(srcImage);
    if (!dst || !src || m_deviceRemoved || src->getDesc().sampleCount > 1)
      return false;
    initMipGenResources();
    if (!m_mipGenRootSignature)
      return false;
    const DXGI_FORMAT dstFormat = unormOf(dst->getDesc().format);
    const DXGI_FORMAT srcFormat = unormOf(src->getDesc().format);
    // Sharpening clamps to [0, 1]: not for HDR buffers.
    const bool sharpen = sharpness > 0.0f && !isFloatFormat(dstFormat);
    ID3D12PipelineState* scalePso = getOrCreateScalePso(0, dstFormat);
    ID3D12PipelineState* sharpenPso = sharpen ? getOrCreateScalePso(1, dstFormat) : nullptr;
    if (!scalePso || (sharpen && !sharpenPso))
      return false;

    // The scaled image before sharpening, at the output size.
    D3D12BackendImage* scaled = dst;
    if (sharpen) {
      const auto& dstDesc = dst->getDesc();
      const ImageDesc* have = m_scaleIntermediate ? &m_scaleIntermediate->getDesc() : nullptr;
      if (!have || have->width != dstDesc.width || have->height != dstDesc.height || have->format != dstFormat) {
        ImageDesc desc = {};
        desc.type = ImageType::Tex2D;
        desc.width = dstDesc.width;
        desc.height = dstDesc.height;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = dstFormat;
        desc.sampleCount = 1;
        desc.bindFlags = BindFlags::RenderTarget | BindFlags::ShaderResource;
        if (m_scaleIntermediate)
          synchronize();
        m_scaleIntermediate = m_device->createImage(desc);
      }
      if (!m_scaleIntermediate)
        return false;
      scaled = static_cast<D3D12BackendImage*>(m_scaleIntermediate.get());
    }

    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onInternal(m_cmdList.Get(), "spatial upscale", dst->getResource(), dstFormat);
    drawScalePass(scalePso, scaled, dstFormat, src, srcFormat, 0);
    if (sharpen) {
      uint32_t bits;
      std::memcpy(&bits, &sharpness, sizeof(bits));
      drawScalePass(sharpenPso, dst, dstFormat, scaled, dstFormat, bits);
    }
    // What comes next (the FPS overlay, Present) copies into the buffer or
    // moves it on from here.
    transitionResource(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    flushBarriers();

    // Give the application its state back.
    restoreAfterInternalPass();
    m_copyOpsSinceFlush++;
    return true;
#else
    (void)dstImage; (void)srcImage; (void)sharpness;
    return false;
#endif
  }

}
