// Temporal upscaling in place of a game's anti-aliasing pass: DXUP's own
// pass turns the game's depth and velocity into what DLSS, FSR and XeSS
// take (depth as R32_FLOAT, motion in render pixels for every pixel), then
// the upscaler records its work on the context's command list.

#include "d3d12_backend_context.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <d3dcompiler.h>
#endif

namespace dxup::d3d12 {

  namespace {

    UpscalerFactory g_upscalerFactory = nullptr;

#ifdef _WIN32
    // The motion vector pass. Unreal Engine 4 writes velocity only for
    // moving objects (x = 0 elsewhere, encoded as 0.499 * 0.5 * v + 0.5);
    // the camera's motion comes from the depth and ClipToPrevClip, as in its
    // TAA. Motion points from this frame's pixel to the previous frame's,
    // in render pixels.
    const char kMotionShader[] =
      "Texture2D<float> depthTex : register(t0);\n"
      "Texture2D<float4> velocityTex : register(t1);\n"
      "RWTexture2D<float2> motionOut : register(u0);\n"
      "RWTexture2D<float> depthOut : register(u1);\n"
      "cbuffer Constants : register(b0) {\n"
      "  float4 row0, row1, row2, row3;\n"
      "  float2 viewSize, invViewSize;\n"
      "  float2 velocityScale;\n"
      "  float velocityMode, pad;\n"
      "};\n"
      "[numthreads(8, 8, 1)]\n"
      "void main(uint2 id : SV_DispatchThreadID) {\n"
      "  if (any(float2(id) >= viewSize))\n"
      "    return;\n"
      "  const float depth = depthTex.Load(int3(id, 0));\n"
      "  depthOut[id] = depth;\n"
      "  const float4 velocity = velocityTex.Load(int3(id, 0));\n"
      "  if (velocityMode > 0.5 && velocityMode < 1.5) {\n"
      "    motionOut[id] = velocity.xy * velocityScale;\n"
      "    return;\n"
      "  }\n"
      "  float2 screenMotion;\n"
      "  if (velocityMode < 0.5 && velocity.x > 0.0) {\n"
      "    screenMotion = velocity.xy * (1.0 / (0.499 * 0.5)) - (32767.0 / 65535.0) / (0.499 * 0.5);\n"
      "  } else {\n"
      "    const float2 uv = (float2(id) + 0.5) * invViewSize;\n"
      "    const float2 screen = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
      "    const float4 previous = screen.x * row0 + screen.y * row1 + depth * row2 + row3;\n"
      "    screenMotion = screen - previous.xy / previous.w;\n"
      "  }\n"
      "  motionOut[id] = float2(-screenMotion.x, screenMotion.y) * 0.5 * viewSize;\n"
      "}\n";

    struct MotionConstants {
      float rows[16];
      float viewSize[2];
      float invViewSize[2];
      float velocityScale[2];
      float velocityMode;
      float pad;
    };

    // Velocity calibration: on a sparse grid of moving pixels, the color
    // difference to the previous frame without motion and with each of the
    // six candidate readings of the velocity (relative to the brightness, at
    // most 4 per pixel), summed as fixed point.
    const char kVelocityCalShader[] =
      "Texture2D<float4> current : register(t0);\n"
      "Texture2D<float4> previous : register(t1);\n"
      "Texture2D<float4> velocityTex : register(t2);\n"
      "RWByteAddressBuffer result : register(u0);\n"
      "cbuffer Constants : register(b0) {\n"
      "  float2 viewSize; uint slot, pad;\n"
      "  float4 scales[3];\n"
      "};\n"
      "float difference(float3 a, float3 b) {\n"
      "  const float level = max(dot(a + b, 1.0f / 6.0f), 1e-3f);\n"
      "  return min(dot(abs(a - b), 1.0f) / level, 4.0f);\n"
      "}\n"
      "float3 bilinear(float2 q, float2 size) {\n"
      "  const float2 p = q - 0.5f;\n"
      "  const int2 i = int2(floor(p));\n"
      "  const float2 f = p - floor(p);\n"
      "  const int2 last = int2(size) - 1;\n"
      "  const float3 a = previous.Load(int3(clamp(i, 0, last), 0)).rgb;\n"
      "  const float3 b = previous.Load(int3(clamp(i + int2(1, 0), 0, last), 0)).rgb;\n"
      "  const float3 c = previous.Load(int3(clamp(i + int2(0, 1), 0, last), 0)).rgb;\n"
      "  const float3 d = previous.Load(int3(clamp(i + int2(1, 1), 0, last), 0)).rgb;\n"
      "  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);\n"
      "}\n"
      "[numthreads(8, 8, 1)]\n"
      "void main(uint2 id : SV_DispatchThreadID) {\n"
      "  const uint2 p = id * 4 + 2;\n"
      "  if (any(float2(p) >= viewSize))\n"
      "    return;\n"
      "  const float2 v = velocityTex.Load(int3(p, 0)).xy;\n"
      "  if (all(abs(v) < 1e-6f))\n"
      "    return;\n"
      "  const float3 c = current.Load(int3(p, 0)).rgb;\n"
      "  const uint base = slot * 128;\n"
      "  uint ignored;\n"
      "  const float still = difference(c, previous.Load(int3(p, 0)).rgb);\n"
      "  result.InterlockedAdd(base + 100, 1, ignored);\n"
      "  result.InterlockedAdd(base + 96, uint(still * 1024.0f), ignored);\n"
      "  [unroll] for (uint k = 0; k < 6; k++) {\n"
      "    const float2 scale = (k & 1) ? scales[k >> 1].zw : scales[k >> 1].xy;\n"
      "    const float2 q = float2(p) + 0.5f + v * scale;\n"
      "    float e = still;\n"
      "    if (all(q >= 0.0f) && all(q < viewSize))\n"
      "      e = difference(c, bilinear(q, viewSize));\n"
      "    result.InterlockedAdd(base + k * 4, uint(e * 1024.0f), ignored);\n"
      "  }\n"
      "}\n";

    struct VelocityCalConstants {
      float    viewSize[2];
      uint32_t slot, pad;
      float    scales[12];
    };

    // Camera calibration: on a sparse grid, the color difference to the
    // previous frame without motion and after reprojecting with each of
    // three ClipToPrevClip candidates (rows as the motion pass uses them).
    const char kCameraCalShader[] =
      "Texture2D<float4> current : register(t0);\n"
      "Texture2D<float4> previous : register(t1);\n"
      "Texture2D<float> depthTex : register(t2);\n"
      "StructuredBuffer<float4> rows : register(t3);\n"
      "RWByteAddressBuffer result : register(u0);\n"
      "cbuffer Constants : register(b0) {\n"
      "  float2 viewSize; uint slot, count;\n"
      "};\n"
      "groupshared uint sums[26];\n"
      "float difference(float3 a, float3 b) {\n"
      "  const float level = max(dot(a + b, 1.0f / 6.0f), 1e-3f);\n"
      "  return min(dot(abs(a - b), 1.0f) / level, 4.0f);\n"
      "}\n"
      "float3 bilinear(float2 q, float2 size) {\n"
      "  const float2 p = q - 0.5f;\n"
      "  const int2 i = int2(floor(p));\n"
      "  const float2 f = p - floor(p);\n"
      "  const int2 last = int2(size) - 1;\n"
      "  const float3 a = previous.Load(int3(clamp(i, 0, last), 0)).rgb;\n"
      "  const float3 b = previous.Load(int3(clamp(i + int2(1, 0), 0, last), 0)).rgb;\n"
      "  const float3 c = previous.Load(int3(clamp(i + int2(0, 1), 0, last), 0)).rgb;\n"
      "  const float3 d = previous.Load(int3(clamp(i + int2(1, 1), 0, last), 0)).rgb;\n"
      "  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);\n"
      "}\n"
      "[numthreads(8, 8, 1)]\n"
      "void main(uint2 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {\n"
      "  if (index < 26)\n"
      "    sums[index] = 0;\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  const uint2 p = id * 4 + 2;\n"
      "  if (all(float2(p) < viewSize)) {\n"
      "    const float3 c = current.Load(int3(p, 0)).rgb;\n"
      "    const float depth = depthTex.Load(int3(p, 0));\n"
      "    const float still = difference(c, previous.Load(int3(p, 0)).rgb);\n"
      "    InterlockedAdd(sums[25], 1);\n"
      "    InterlockedAdd(sums[24], uint(still * 1024.0f));\n"
      "    const float2 uv = (float2(p) + 0.5f) / viewSize;\n"
      "    const float2 screen = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);\n"
      "    for (uint k = 0; k < count; k++) {\n"
      "      const float4 prev = screen.x * rows[k * 4] + screen.y * rows[k * 4 + 1] + depth * rows[k * 4 + 2]\n"
      "        + rows[k * 4 + 3];\n"
      "      float e = 4.0f;\n"
      "      if (prev.w > 1e-6f) {\n"
      "        const float2 s = prev.xy / prev.w;\n"
      "        const float2 q = float2(s.x * 0.5f + 0.5f, 0.5f - s.y * 0.5f) * viewSize;\n"
      "        e = (all(q >= 0.0f) && all(q < viewSize)) ? difference(c, bilinear(q, viewSize)) : still;\n"
      "      }\n"
      "      InterlockedAdd(sums[k], uint(e * 1024.0f));\n"
      "    }\n"
      "  }\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  uint ignored;\n"
      "  if (index < 26 && sums[index])\n"
      "    result.InterlockedAdd(slot * 128 + index * 4, sums[index], ignored);\n"
      "}\n";

    struct CameraCalConstants {
      float    viewSize[2];
      uint32_t slot, count;
    };

    // A result slot (16 of them, one per frame in flight): an error sum per
    // candidate, then the error of no motion and the number of samples, all
    // in 1/1024.
    constexpr uint32_t kCalSlotBytes = 128;
    constexpr uint32_t kCalStill = kCameraCandidates;
    constexpr uint32_t kCalCount = kCameraCandidates + 1;
    static_assert(kCalCount < kCalSlotBytes / 4, "calibration slot too small");

    // The depth plane of a depth format as a shader reads it.
    DXGI_FORMAT depthReadFormat(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:     return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:             return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:             return DXGI_FORMAT_R16_UNORM;
        default:                                return format;
      }
    }
#endif

  }

  void setUpscalerFactory(UpscalerFactory factory) {
    g_upscalerFactory = factory;
  }

#ifdef _WIN32

  bool D3D12BackendContext::createUpscaleMotionPass() {
    if (m_upscaleMotionPso)
      return true;
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = sizeof(MotionConstants) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;

    auto* device = m_device->getDevice();
    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
      serialized.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr))
      hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_upscaleMotionRootSignature));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "upscaler: motion root signature failed: 0x%08x", static_cast<unsigned>(hr));
      return false;
    }

    ComPtr<ID3DBlob> bytecode;
    hr = D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, "upscale_motion", nullptr, nullptr, "main", "cs_5_0",
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "upscaler: motion shader failed: 0x%08x %s", static_cast<unsigned>(hr),
        errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_upscaleMotionRootSignature.Get();
    pso.CS = { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
    hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_upscaleMotionPso));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "upscaler: motion pipeline failed: 0x%08x", static_cast<unsigned>(hr));
      return false;
    }
    return true;
  }

  bool D3D12BackendContext::createVelocityCalibration(D3D12BackendImage* color) {
    auto* device = m_device->getDevice();
    const D3D12_RESOURCE_DESC colorDesc = color->getResource()->GetDesc();
    if (m_velocityCalPrevious) {
      const D3D12_RESOURCE_DESC have = m_velocityCalPrevious->GetDesc();
      if (have.Width == colorDesc.Width && have.Height == colorDesc.Height && have.Format == colorDesc.Format)
        return true;
      // Calibrations recorded before may still read the old one.
      retireResource(m_velocityCalPrevious);
    }
    if (!m_velocityCalPso) {
      D3D12_DESCRIPTOR_RANGE ranges[2] = {};
      ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[0].NumDescriptors = 3;
      ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      ranges[1].NumDescriptors = 1;
      ranges[1].OffsetInDescriptorsFromTableStart = 3;
      D3D12_ROOT_PARAMETER params[2] = {};
      params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[0].DescriptorTable.NumDescriptorRanges = 2;
      params[0].DescriptorTable.pDescriptorRanges = ranges;
      params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[1].Constants.Num32BitValues = sizeof(VelocityCalConstants) / 4;
      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = 2;
      desc.pParameters = params;
      ComPtr<ID3DBlob> serialized, errors, bytecode;
      HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(),
        errors.GetAddressOf());
      if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
          IID_PPV_ARGS(&m_velocityCalRootSignature));
      if (SUCCEEDED(hr))
        hr = D3DCompile(kVelocityCalShader, sizeof(kVelocityCalShader) - 1, "velocity_calibration", nullptr, nullptr,
          "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
      if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_velocityCalRootSignature.Get();
        pso.CS = { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_velocityCalPso));
      }
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "upscalerAuto: velocity calibration pass failed: 0x%08x %s", static_cast<unsigned>(hr),
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
      }
      // 16 result slots, zeros to clear one, readback memory.
      D3D12_HEAP_PROPERTIES heap = {};
      D3D12_RESOURCE_DESC buffer = {};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = 16 * kCalSlotBytes;
      buffer.Height = 1;
      buffer.DepthOrArraySize = 1;
      buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_velocityCalResults));
      buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
      heap.Type = D3D12_HEAP_TYPE_READBACK;
      if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr, IID_PPV_ARGS(&m_velocityCalReadback));
      buffer.Width = kCalSlotBytes;
      heap.Type = D3D12_HEAP_TYPE_UPLOAD;
      if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr, IID_PPV_ARGS(&m_velocityCalZeros));
      void* zeros = nullptr;
      if (SUCCEEDED(hr) && SUCCEEDED(m_velocityCalZeros->Map(0, nullptr, &zeros))) {
        std::memset(zeros, 0, kCalSlotBytes);
        m_velocityCalZeros->Unmap(0, nullptr);
      }
      if (FAILED(hr)) {
        logMessage(LogLevel::Error, "upscalerAuto: velocity calibration buffers failed: 0x%08x", static_cast<unsigned>(hr));
        m_velocityCalPso.Reset();
        return false;
      }
    }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = colorDesc;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Alignment = 0;
    desc.MipLevels = 1;
    desc.DepthOrArraySize = 1;
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_velocityCalPrevious));
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "upscalerAuto: velocity calibration color copy failed: 0x%08x", static_cast<unsigned>(hr));
      return false;
    }
    return true;
  }

  void D3D12BackendContext::calibrateVelocity(D3D12BackendImage* color, DXGI_FORMAT colorFormat, D3D12BackendImage* velocity,
                                              DXGI_FORMAT velocityFormat, float viewWidth, float viewHeight,
                                              const std::shared_ptr<VelocityCalibration>& calibration) {
    ContextLock ctxLock(*this);
    if (m_deviceRemoved || !color || !velocity || !calibration || color->getDesc().sampleCount > 1
     || color->getDesc().mipLevels > 1 || color->getDesc().arraySize > 1)
      return;
    // Earlier frames' results that the GPU has finished.
    const uint64_t completed = m_fence->GetCompletedValue();
    void* mapped = nullptr;
    for (uint32_t slot = 0; slot < 16; slot++) {
      if (!m_velocityCalFence[slot] || m_velocityCalFence[slot] > completed || !m_velocityCalReadback)
        continue;
      if (!mapped && FAILED(m_velocityCalReadback->Map(0, nullptr, &mapped)))
        break;
      const auto* values = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) + slot * kCalSlotBytes);
      if (values[kCalCount] >= 64) {
        std::lock_guard<std::mutex> lock(calibration->mutex);
        for (uint32_t k = 0; k < 6; k++)
          calibration->error[k] += double(values[k]) / (1024.0 * values[kCalCount]);
        calibration->still += double(values[kCalStill]) / (1024.0 * values[kCalCount]);
        calibration->frames++;
      }
      m_velocityCalFence[slot] = 0;
    }
    if (mapped) {
      const D3D12_RANGE none = { 0, 0 };
      m_velocityCalReadback->Unmap(0, &none);
    }

    const bool first = !m_velocityCalPrevious;
    if (!createVelocityCalibration(color))
      return;
    ensureDescriptorHeapSpace();
    holdBatchResource(color->getResource());
    holdBatchResource(velocity->getResource());
    const uint32_t slot = m_velocityCalSlot;
    const bool measure = !first && !m_velocityCalFence[slot];
    if (measure) {
      transitionResource(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      transitionResource(velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      flushBarriers();
      // Previous color readable, the slot cleared.
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = m_velocityCalPrevious.Get();
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      m_cmdList->ResourceBarrier(1, &barrier);
      m_cmdList->CopyBufferRegion(m_velocityCalResults.Get(), slot * kCalSlotBytes, m_velocityCalZeros.Get(), 0,
        kCalSlotBytes);
      barrier.Transition.pResource = m_velocityCalResults.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      m_cmdList->ResourceBarrier(1, &barrier);

      auto* device = m_device->getDevice();
      const D3D12_CPU_DESCRIPTOR_HANDLE firstHandle = allocateShaderVisibleCbvSrvUav();
      const uint32_t firstIndex = m_cbvSrvUavHeapOffset - 1;
      for (uint32_t i = 1; i < 4; i++)
        allocateShaderVisibleCbvSrvUav();
      auto handle = [&](uint32_t i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = firstHandle;
        h.ptr += SIZE_T(i) * m_cbvSrvUavDescSize;
        return h;
      };
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      srv.Format = colorFormat;
      device->CreateShaderResourceView(color->getResource(), &srv, handle(0));
      device->CreateShaderResourceView(m_velocityCalPrevious.Get(), &srv, handle(1));
      srv.Format = velocityFormat;
      device->CreateShaderResourceView(velocity->getResource(), &srv, handle(2));
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
      uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      uav.Format = DXGI_FORMAT_R32_TYPELESS;
      uav.Buffer.NumElements = 16 * kCalSlotBytes / 4;
      uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      device->CreateUnorderedAccessView(m_velocityCalResults.Get(), nullptr, &uav, handle(3));
      D3D12_GPU_DESCRIPTOR_HANDLE table = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
      table.ptr += uint64_t(firstIndex) * m_cbvSrvUavDescSize;

      VelocityCalConstants constants = {};
      constants.viewSize[0] = viewWidth;
      constants.viewSize[1] = viewHeight;
      constants.slot = slot;
      for (uint32_t k = 0; k < 6; k++)
        velocityCandidateScale(k, viewWidth, viewHeight, constants.scales + k * 2);
      ID3D12DescriptorHeap* heaps[] = { m_shaderVisibleCbvSrvUavHeap.Get(), m_shaderVisibleSamplerHeap.Get() };
      m_cmdList->SetDescriptorHeaps(2, heaps);
      m_cmdList->SetComputeRootSignature(m_velocityCalRootSignature.Get());
      m_cmdList->SetPipelineState(m_velocityCalPso.Get());
      m_cmdList->SetComputeRootDescriptorTable(0, table);
      m_cmdList->SetComputeRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
      const uint32_t width = uint32_t(viewWidth), height = uint32_t(viewHeight);
      m_cmdList->Dispatch((width / 4 + 7) / 8, (height / 4 + 7) / 8, 1);

      // The slot to readback memory; the previous color back to copy.
      D3D12_RESOURCE_BARRIER after[2] = {};
      after[0] = barrier;
      after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      after[1] = after[0];
      after[1].Transition.pResource = m_velocityCalPrevious.Get();
      after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      m_cmdList->ResourceBarrier(2, after);
      m_cmdList->CopyBufferRegion(m_velocityCalReadback.Get(), slot * kCalSlotBytes, m_velocityCalResults.Get(),
        slot * kCalSlotBytes, kCalSlotBytes);
      after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      m_cmdList->ResourceBarrier(1, after);
      // Results arrive with the submission the open list becomes.
      m_velocityCalFence[slot] = m_fenceValue + 1;
      m_velocityCalSlot = (slot + 1) % 16;
    }
    // This frame's color for the next frame.
    transitionResource(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
    flushBarriers();
    m_cmdList->CopyResource(m_velocityCalPrevious.Get(), color->getResource());
    restoreAfterForeignCommands();
  }

  D3D12BackendImage* D3D12BackendContext::shaderReadableDepth(D3D12BackendImage* depth) {
    const DXGI_FORMAT format = depth->getResource()->GetDesc().Format;
    const bool typed = format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT
      || format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    if (!typed)
      return depth;
    const ImageDesc& depthDesc = depth->getDesc();
    const ImageDesc* have = m_injectDepth ? &m_injectDepth->getDesc() : nullptr;
    if (!have || have->width != depthDesc.width || have->height != depthDesc.height || have->format != format) {
      if (m_injectDepth)
        synchronize();
      ImageDesc desc = {};
      desc.type = ImageType::Tex2D;
      desc.width = depthDesc.width;
      desc.height = depthDesc.height;
      desc.depth = 1;
      desc.mipLevels = 1;
      desc.arraySize = 1;
      desc.format = format;
      desc.sampleCount = 1;
      // Depth and shader reads: created in the typeless format.
      desc.bindFlags = BindFlags::DepthStencil | BindFlags::ShaderResource;
      m_injectDepth = m_device->createImage(desc);
    }
    if (!m_injectDepth)
      return nullptr;
    auto* copy = static_cast<D3D12BackendImage*>(m_injectDepth.get());
    copyImage(copy, ImageSubresource{}, depth, ImageSubresource{});
    return copy;
  }

  void D3D12BackendContext::calibrateCamera(D3D12BackendImage* color, DXGI_FORMAT colorFormat, D3D12BackendImage* depth,
                                            DXGI_FORMAT depthFormat, uint32_t count, const float (*clipRows)[16],
                                            const std::shared_ptr<VelocityCalibration>& calibration, uint32_t generation) {
    ContextLock ctxLock(*this);
    if (m_deviceRemoved || !color || !depth || !calibration || color->getDesc().sampleCount > 1
     || depth->getDesc().sampleCount > 1 || color->getDesc().mipLevels > 1 || count > kCameraCandidates)
      return;
    depth = shaderReadableDepth(depth);
    if (!depth)
      return;
    // The velocity calibration's slots, previous color and readback serve
    // this one too (only one of them runs).
    const uint64_t completed = m_fence->GetCompletedValue();
    void* mapped = nullptr;
    for (uint32_t slot = 0; slot < 16; slot++) {
      if (!m_velocityCalFence[slot] || m_velocityCalFence[slot] > completed || !m_velocityCalReadback)
        continue;
      if (!mapped && FAILED(m_velocityCalReadback->Map(0, nullptr, &mapped)))
        break;
      const auto* values = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) + slot * kCalSlotBytes);
      // Frames in which the image hardly changed tell nothing about the
      // camera (resampling the previous one only blurs it).
      if (values[kCalCount] >= 64 && double(values[kCalStill]) / (1024.0 * values[kCalCount]) >= 0.005) {
        std::lock_guard<std::mutex> lock(calibration->mutex);
        if (m_cameraCalGeneration[slot] == calibration->generation) {
          for (uint32_t k = 0; k < kCameraCandidates; k++)
            calibration->error[k] += double(values[k]) / (1024.0 * values[kCalCount]);
          calibration->still += double(values[kCalStill]) / (1024.0 * values[kCalCount]);
          calibration->frames++;
        }
      }
      m_velocityCalFence[slot] = 0;
    }
    if (mapped) {
      const D3D12_RANGE none = { 0, 0 };
      m_velocityCalReadback->Unmap(0, &none);
    }
    const bool first = !m_velocityCalPrevious;
    if (!createVelocityCalibration(color))
      return;
    auto* device = m_device->getDevice();
    if (!m_cameraCalPso) {
      D3D12_DESCRIPTOR_RANGE ranges[2] = {};
      ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[0].NumDescriptors = 3;
      ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      ranges[1].NumDescriptors = 1;
      ranges[1].OffsetInDescriptorsFromTableStart = 3;
      D3D12_ROOT_PARAMETER params[3] = {};
      params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[0].DescriptorTable.NumDescriptorRanges = 2;
      params[0].DescriptorTable.pDescriptorRanges = ranges;
      params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[1].Constants.Num32BitValues = sizeof(CameraCalConstants) / 4;
      params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      params[2].Descriptor.ShaderRegister = 3;
      D3D12_ROOT_SIGNATURE_DESC desc = {};
      desc.NumParameters = 3;
      desc.pParameters = params;
      ComPtr<ID3DBlob> serialized, errors, bytecode;
      HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(),
        errors.GetAddressOf());
      if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
          IID_PPV_ARGS(&m_cameraCalRootSignature));
      if (SUCCEEDED(hr))
        hr = D3DCompile(kCameraCalShader, sizeof(kCameraCalShader) - 1, "camera_calibration", nullptr, nullptr,
          "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.ReleaseAndGetAddressOf());
      if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_cameraCalRootSignature.Get();
        pso.CS = { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_cameraCalPso));
      }
      // The candidates' rows, a set per result slot (rewritten only once the
      // slot's results are back).
      if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 16 * kCameraCandidates * 64;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr, IID_PPV_ARGS(&m_cameraCalRows));
        void* data = nullptr;
        if (SUCCEEDED(hr))
          hr = m_cameraCalRows->Map(0, nullptr, &data);
        m_cameraCalRowsData = static_cast<float*>(data);
      }
      if (FAILED(hr)) {
        m_cameraCalPso.Reset();
        logMessage(LogLevel::Error, "upscalerInject: camera calibration pass failed: 0x%08x %s", static_cast<unsigned>(hr),
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return;
      }
    }
    ensureDescriptorHeapSpace();
    holdBatchResource(color->getResource());
    holdBatchResource(depth->getResource());
    const uint32_t slot = m_velocityCalSlot;
    if (!first && !m_velocityCalFence[slot]) {
      constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      transitionResource(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      transitionResource(depth, kRead);
      flushBarriers();
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = m_velocityCalPrevious.Get();
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      m_cmdList->ResourceBarrier(1, &barrier);
      m_cmdList->CopyBufferRegion(m_velocityCalResults.Get(), slot * kCalSlotBytes, m_velocityCalZeros.Get(), 0,
        kCalSlotBytes);
      barrier.Transition.pResource = m_velocityCalResults.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      m_cmdList->ResourceBarrier(1, &barrier);

      const D3D12_CPU_DESCRIPTOR_HANDLE firstHandle = allocateShaderVisibleCbvSrvUav();
      const uint32_t firstIndex = m_cbvSrvUavHeapOffset - 1;
      for (uint32_t i = 1; i < 4; i++)
        allocateShaderVisibleCbvSrvUav();
      auto handle = [&](uint32_t i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = firstHandle;
        h.ptr += SIZE_T(i) * m_cbvSrvUavDescSize;
        return h;
      };
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      srv.Format = colorFormat;
      device->CreateShaderResourceView(color->getResource(), &srv, handle(0));
      device->CreateShaderResourceView(m_velocityCalPrevious.Get(), &srv, handle(1));
      srv.Format = depthReadFormat(depthFormat != DXGI_FORMAT_UNKNOWN ? depthFormat : depth->getDesc().format);
      device->CreateShaderResourceView(depth->getResource(), &srv, handle(2));
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
      uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      uav.Format = DXGI_FORMAT_R32_TYPELESS;
      uav.Buffer.NumElements = 16 * kCalSlotBytes / 4;
      uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      device->CreateUnorderedAccessView(m_velocityCalResults.Get(), nullptr, &uav, handle(3));
      D3D12_GPU_DESCRIPTOR_HANDLE table = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
      table.ptr += uint64_t(firstIndex) * m_cbvSrvUavDescSize;

      CameraCalConstants constants = {};
      constants.viewSize[0] = float(color->getDesc().width);
      constants.viewSize[1] = float(color->getDesc().height);
      constants.slot = slot;
      constants.count = count;
      if (count)
        std::memcpy(m_cameraCalRowsData + size_t(slot) * kCameraCandidates * 16, clipRows, sizeof(float) * 16 * count);
      m_cameraCalGeneration[slot] = generation;
      ID3D12DescriptorHeap* heaps[] = { m_shaderVisibleCbvSrvUavHeap.Get(), m_shaderVisibleSamplerHeap.Get() };
      m_cmdList->SetDescriptorHeaps(2, heaps);
      m_cmdList->SetComputeRootSignature(m_cameraCalRootSignature.Get());
      m_cmdList->SetPipelineState(m_cameraCalPso.Get());
      m_cmdList->SetComputeRootDescriptorTable(0, table);
      m_cmdList->SetComputeRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
      m_cmdList->SetComputeRootShaderResourceView(2,
        m_cameraCalRows->GetGPUVirtualAddress() + uint64_t(slot) * kCameraCandidates * 64);
      const uint32_t width = color->getDesc().width, height = color->getDesc().height;
      m_cmdList->Dispatch((width / 4 + 7) / 8, (height / 4 + 7) / 8, 1);

      D3D12_RESOURCE_BARRIER after[2] = {};
      after[0] = barrier;
      after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      after[1] = after[0];
      after[1].Transition.pResource = m_velocityCalPrevious.Get();
      after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      m_cmdList->ResourceBarrier(2, after);
      m_cmdList->CopyBufferRegion(m_velocityCalReadback.Get(), slot * kCalSlotBytes, m_velocityCalResults.Get(),
        slot * kCalSlotBytes, kCalSlotBytes);
      after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      m_cmdList->ResourceBarrier(1, after);
      m_velocityCalFence[slot] = m_fenceValue + 1;
      m_velocityCalSlot = (slot + 1) % 16;
    }
    transitionResource(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
    flushBarriers();
    m_cmdList->CopyResource(m_velocityCalPrevious.Get(), color->getResource());
    restoreAfterForeignCommands();
  }

  bool D3D12BackendContext::runInjectedUpscaler(D3D12BackendImage* sceneColor, UpscaleRequest request) {
    ContextLock ctxLock(*this);
    if (m_deviceRemoved || !sceneColor || !request.depth)
      return false;
    const ImageDesc& sceneDesc = sceneColor->getDesc();
    if (sceneDesc.sampleCount > 1 || sceneDesc.mipLevels > 1 || sceneDesc.arraySize > 1)
      return false;
    request.depth = shaderReadableDepth(request.depth);
    if (!request.depth)
      return false;
    // The input: a copy of the scene color in its own format. The output: a
    // typed format that UAVs can write (sRGB stored as its UNORM bits).
    auto unorm = [](DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:                                return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:                                 return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:                                                               return format;
      }
    };
    const DXGI_FORMAT typed = unorm(sceneDesc.format);
    auto ensure = [&](Rc<IBackendImage>& image, bool uav) {
      const ImageDesc* have = image ? &image->getDesc() : nullptr;
      if (have && have->width == sceneDesc.width && have->height == sceneDesc.height && have->format == typed)
        return true;
      if (image)
        synchronize();
      ImageDesc desc = {};
      desc.type = ImageType::Tex2D;
      desc.width = sceneDesc.width;
      desc.height = sceneDesc.height;
      desc.depth = 1;
      desc.mipLevels = 1;
      desc.arraySize = 1;
      desc.format = typed;
      desc.sampleCount = 1;
      desc.bindFlags = BindFlags::ShaderResource | (uav ? BindFlags::UnorderedAccess : BindFlags::ShaderResource);
      image = m_device->createImage(desc);
      return image != nullptr;
    };
    if (!ensure(m_injectInput, false) || !ensure(m_injectOutput, true))
      return false;
    auto* input = static_cast<D3D12BackendImage*>(m_injectInput.get());
    auto* output = static_cast<D3D12BackendImage*>(m_injectOutput.get());
    if (!request.prepareOnly)
      copyImage(input, ImageSubresource{}, sceneColor, ImageSubresource{});
    request.color = input;
    request.output = output;
    // Camera motion only (velocityMode 2): the depth stands in for the
    // velocity texture the motion pass binds but does not read.
    if (!request.velocity) {
      request.velocity = request.depth;
      request.velocityFormat = depthReadFormat(request.depthFormat != DXGI_FORMAT_UNKNOWN
        ? request.depthFormat : request.depth->getDesc().format);
    }
    if (!runUpscaler(request) || request.prepareOnly)
      return false;
    copyImage(sceneColor, ImageSubresource{}, output, ImageSubresource{});
    return true;
  }

  bool D3D12BackendContext::ensureUpscaleTextures(uint32_t width, uint32_t height) {
    if (m_upscaleMotion && m_upscaleTextureSize[0] == width && m_upscaleTextureSize[1] == height)
      return true;
    // Commands recorded before may still use the previous ones.
    retireResource(m_upscaleMotion);
    retireResource(m_upscaleDepth);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto* device = m_device->getDevice();
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_upscaleMotion));
    if (SUCCEEDED(hr)) {
      desc.Format = DXGI_FORMAT_R32_FLOAT;
      hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_upscaleDepth));
    }
    if (FAILED(hr)) {
      logMessage(LogLevel::Error, "upscaler: %ux%u motion textures failed: 0x%08x", width, height, static_cast<unsigned>(hr));
      m_upscaleMotion.Reset();
      m_upscaleDepth.Reset();
      m_upscaleTextureSize[0] = m_upscaleTextureSize[1] = 0;
      return false;
    }
    m_upscaleTextureSize[0] = width;
    m_upscaleTextureSize[1] = height;
    return true;
  }

  void D3D12BackendContext::restoreAfterForeignCommands() {
    // The upscalers bind descriptor heaps, a root signature and pipelines of
    // their own: everything DXUP set goes onto the list again, as after a
    // command list reset.
    if (m_shaderVisibleCbvSrvUavHeap && m_shaderVisibleSamplerHeap) {
      ID3D12DescriptorHeap* heaps[] = {
        m_shaderVisibleCbvSrvUavHeap.Get(),
        m_shaderVisibleSamplerHeap.Get()
      };
      m_cmdList->SetDescriptorHeaps(2, heaps);
    }
    if (m_currentRootSignature)
      m_cmdList->SetGraphicsRootSignature(m_currentRootSignature);
    if (m_computeRootSignature)
      m_cmdList->SetComputeRootSignature(m_computeRootSignature);
    invalidateRootCache(false);
    invalidateRootCache(true);
    initNullDescriptorsAndRootTables();
    markAllDescriptorStagesDirty();
    m_psoDirtyAfterFlush = true;

    m_predicationApplied = false;
    applyPredication();

    for (uint32_t i = 0; i < kMaxVertexBuffers; i++) {
      if (m_trackedVBs[i].bound)
        m_cmdList->IASetVertexBuffers(i, 1, &m_trackedVBs[i].view);
    }
    if (m_trackedIBBound)
      m_cmdList->IASetIndexBuffer(&m_trackedIB);
    if (m_trackedViewportCount)
      m_cmdList->RSSetViewports(m_trackedViewportCount, m_trackedViewports);
    if (m_trackedScissorCount)
      m_cmdList->RSSetScissorRects(m_trackedScissorCount, m_trackedScissors);
    m_cmdList->OMSetStencilRef(m_trackedStencilRef);
    m_cmdList->OMSetBlendFactor(m_trackedBlendFactor);
    if (m_trackedTopology != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
      m_cmdList->IASetPrimitiveTopology(m_trackedTopology);
    if (m_trackedRTVCount || m_trackedDSVBound) {
      m_cmdList->OMSetRenderTargets(
        m_trackedRTVCount,
        m_trackedRTVCount ? m_trackedRTVs : nullptr,
        FALSE,
        m_trackedDSVBound ? &m_trackedDSV : nullptr);
    }
    m_trackedOmBindingValid = true;
    if (m_soBufferCount)
      m_cmdList->SOSetTargets(0, m_soBufferCount, m_soViews);
  }

  bool D3D12BackendContext::runUpscaler(const UpscaleRequest& request) {
    ContextLock ctxLock(*this);
    if (m_deviceRemoved || request.kind == upscale::Kind::Off || !g_upscalerFactory)
      return false;
    if (!request.color || !request.depth || !request.velocity || !request.output)
      return false;
    if (request.reset)
      m_upscalerReset = true;

    // The camera data as the game wrote it for this pass.
    auto read = [](const UpscaleRequest::Constant& constant, float* out, uint32_t size) {
      if (!constant.buffer)
        return false;
      const auto* data = static_cast<const uint8_t*>(constant.buffer->getPersistentMappedData());
      const BufferDesc& desc = constant.buffer->getDesc();
      const uint64_t dataSize = desc.dataSize ? desc.dataSize : desc.size;
      if (!data || uint64_t(constant.offset) + size > dataSize)
        return false;
      std::memcpy(out, data + constant.offset, size);
      return true;
    };
    float viewSize[2] = { float(request.color->getDesc().width), float(request.color->getDesc().height) };
    float jitter[2] = {};
    MotionConstants constants = {};
    if (request.directJitter)
      std::memcpy(jitter, request.jitterValue, sizeof(jitter));
    if (request.directClip)
      std::memcpy(constants.rows, request.clipRows, sizeof(constants.rows));
    if ((request.viewSize.buffer && !read(request.viewSize, viewSize, sizeof(viewSize)))
     || (!request.directJitter && !read(request.jitter, jitter, sizeof(jitter)))
     || (request.velocityMode == 0 && !request.directClip
      && !read(request.clipToPrevClip, constants.rows, sizeof(constants.rows)))) {
      static bool warned = false;
      if (!warned)
        logMessage(LogLevel::Warn, "upscaler: the pass's constants are not readable, the game's pass runs");
      warned = true;
      return false;
    }

    const auto& colorDesc = request.color->getDesc();
    const auto& outputDesc = request.output->getDesc();
    const auto& depthDesc = request.depth->getDesc();
    const uint32_t renderWidth = (std::min)(static_cast<uint32_t>(viewSize[0] + 0.5f), colorDesc.width);
    const uint32_t renderHeight = (std::min)(static_cast<uint32_t>(viewSize[1] + 0.5f), colorDesc.height);
    if (!renderWidth || !renderHeight || depthDesc.sampleCount > 1 || colorDesc.sampleCount > 1
     || depthDesc.width < renderWidth || depthDesc.height < renderHeight
     || request.velocity->getDesc().width < renderWidth || request.velocity->getDesc().height < renderHeight)
      return false;

    // One upscaler for the kind and sizes; another one when they change.
    const uint32_t sizes[4] = { colorDesc.width, colorDesc.height, outputDesc.width, outputDesc.height };
    const uint32_t settings = (request.hdr ? 1u : 0u) | (request.depthInverted ? 2u : 0u);
    const bool sameSetup = m_upscalerKind == request.kind && std::equal(sizes, sizes + 4, m_upscalerSize)
      && m_upscalerSettings == settings;
    auto pendingDone = [&]() {
      return m_upscalerPending.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    };
    if (sameSetup && m_upscalerPending.valid()) {
      if (!pendingDone())
        return false;
      PendingUpscaler made = m_upscalerPending.get();
      m_upscaler = std::move(made.upscaler);
      if (!m_upscaler) {
        logMessage(LogLevel::Warn, "upscaler: %s not available (%s), the game's pass runs",
          upscale::kindName(request.kind), made.error.c_str());
        m_upscalerFailed = true;
        return false;
      }
      logMessage(LogLevel::Warn, "upscaler: %s for %ux%u (render up to %ux%u, color fmt %u, output fmt %u)",
        upscale::kindName(request.kind), outputDesc.width, outputDesc.height, colorDesc.width, colorDesc.height,
        static_cast<unsigned>(colorDesc.format), static_cast<unsigned>(outputDesc.format));
      m_upscalerReset = true;
    }
    if (!sameSetup || (!m_upscaler && !m_upscalerFailed)) {
      // One for another setup is still being made: it goes first.
      if (m_upscalerPending.valid()) {
        if (!pendingDone())
          return false;
        m_upscalerPending.get();
      }
      m_upscalerSettings = settings;
      if (m_upscaler) {
        synchronize();
        m_upscaler.reset();
      }
      m_upscalerKind = request.kind;
      std::copy(sizes, sizes + 4, m_upscalerSize);
      m_upscalerFailed = false;
      if (isTypelessFormat(colorDesc.format) || isTypelessFormat(outputDesc.format)) {
        logMessage(LogLevel::Warn, "upscaler: typeless color (%u) or output (%u), the game's pass runs",
          static_cast<unsigned>(colorDesc.format), static_cast<unsigned>(outputDesc.format));
        m_upscalerFailed = true;
        return false;
      }
      upscale::CreateInfo info;
      info.kind = request.kind;
      info.device = m_device->getDevice();
      info.maxRenderWidth = colorDesc.width;
      info.maxRenderHeight = colorDesc.height;
      info.outputWidth = outputDesc.width;
      info.outputHeight = outputDesc.height;
      info.settings.hdr = request.hdr;
      info.settings.depthInverted = request.depthInverted;
      const UpscalerFactory factory = g_upscalerFactory;
      // The last one has handed over its upscaler: it is done or about to be.
      if (m_upscalerThread.joinable())
        m_upscalerThread.join();
      auto promise = std::make_shared<std::promise<PendingUpscaler>>();
      m_upscalerPending = promise->get_future();
      try {
        m_upscalerThread = Thread([factory, info, promise] {
          PendingUpscaler made;
          try {
            made.upscaler = factory(info, made.error);
          } catch (const std::exception& e) {
            made.upscaler.reset();
            made.error = e.what();
          }
          promise->set_value(std::move(made));
        });
      } catch (const std::system_error&) {
        m_upscalerPending = {};
        m_upscalerFailed = true;
      }
      return false;
    }
    if (!m_upscaler)
      return false;
    if (request.prepareOnly)
      return true;
    if (!createUpscaleMotionPass())
      return false;

    // Room for the pass's descriptors and the tables set again afterwards;
    // this may submit the list, so it comes before anything is recorded.
    ensureDescriptorHeapSpace();
    if (!ensureUpscaleTextures(colorDesc.width, colorDesc.height))
      return false;

    if (m_gpuProfiler.capturing())
      m_gpuProfiler.onDispatch(m_cmdList.raw(), m_computeShaderHash);
    if (m_recentOpsEnabled)
      DXUP_RECENT_OP("Upscale %s %ux%u -> %ux%u", upscale::kindName(request.kind), renderWidth, renderHeight,
        outputDesc.width, outputDesc.height);

    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
      | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transitionResource(request.depth, kRead);
    transitionResource(request.velocity, kRead);
    transitionResource(request.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionResource(request.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    holdBatchResource(request.depth->getResource());
    holdBatchResource(request.velocity->getResource());
    holdBatchResource(request.color->getResource());
    holdBatchResource(request.output->getResource());
    flushBarriers();

    // The motion pass: depth and velocity in, motion and depth out.
    auto* device = m_device->getDevice();
    const D3D12_CPU_DESCRIPTOR_HANDLE first = allocateShaderVisibleCbvSrvUav();
    const uint32_t firstIndex = m_cbvSrvUavHeapOffset - 1;
    for (uint32_t i = 1; i < 4; i++)
      allocateShaderVisibleCbvSrvUav();
    auto handle = [&](uint32_t i) {
      D3D12_CPU_DESCRIPTOR_HANDLE h = first;
      h.ptr += SIZE_T(i) * m_cbvSrvUavDescSize;
      return h;
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = depthReadFormat(request.depthFormat != DXGI_FORMAT_UNKNOWN ? request.depthFormat : depthDesc.format);
    device->CreateShaderResourceView(request.depth->getResource(), &srv, handle(0));
    srv.Format = request.velocityFormat != DXGI_FORMAT_UNKNOWN ? request.velocityFormat : request.velocity->getDesc().format;
    device->CreateShaderResourceView(request.velocity->getResource(), &srv, handle(1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    device->CreateUnorderedAccessView(m_upscaleMotion.Get(), nullptr, &uav, handle(2));
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateUnorderedAccessView(m_upscaleDepth.Get(), nullptr, &uav, handle(3));
    D3D12_GPU_DESCRIPTOR_HANDLE table = m_shaderVisibleCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += uint64_t(firstIndex) * m_cbvSrvUavDescSize;

    constants.viewSize[0] = float(renderWidth);
    constants.viewSize[1] = float(renderHeight);
    constants.invViewSize[0] = 1.0f / float(renderWidth);
    constants.invViewSize[1] = 1.0f / float(renderHeight);
    if (request.velocityMode == 1)
      velocityCandidateScale(request.velocityCandidate, float(renderWidth), float(renderHeight), constants.velocityScale);
    constants.velocityMode = float(request.velocityMode);
    m_cmdList->SetComputeRootSignature(m_upscaleMotionRootSignature.Get());
    m_cmdList->SetPipelineState(m_upscaleMotionPso.Get());
    m_cmdList->SetComputeRootDescriptorTable(0, table);
    m_cmdList->SetComputeRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
    m_cmdList->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER barriers[3] = {};
    for (uint32_t i = 0; i < 2; i++) {
      barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[i].Transition.pResource = i ? m_upscaleDepth.Get() : m_upscaleMotion.Get();
      barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    m_cmdList->ResourceBarrier(2, barriers);

    // The upscaler, with its own frame timing.
    LARGE_INTEGER now = {}, frequency = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float frameTimeMs = 16.7f;
    if (m_upscalerLastTime && frequency.QuadPart)
      frameTimeMs = (std::min)(float(double(now.QuadPart - m_upscalerLastTime) * 1000.0 / double(frequency.QuadPart)), 100.0f);
    m_upscalerLastTime = now.QuadPart;

    upscale::Frame frame;
    frame.color = request.color->getResource();
    frame.depth = m_upscaleDepth.Get();
    frame.motion = m_upscaleMotion.Get();
    frame.output = request.output->getResource();
    frame.renderWidth = renderWidth;
    frame.renderHeight = renderHeight;
    frame.outputWidth = outputDesc.width;
    frame.outputHeight = outputDesc.height;
    frame.jitterX = jitter[0] * request.jitterScale[0];
    frame.jitterY = jitter[1] * request.jitterScale[1];
    frame.sharpness = request.sharpness;
    frame.frameTimeMs = frameTimeMs;
    frame.reset = m_upscalerReset;
    const bool evaluated = m_upscaler->evaluate(m_cmdList.raw(), frame);
    m_upscalerReset = false;

    for (uint32_t i = 0; i < 2; i++)
      std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].UAV.pResource = frame.output;
    m_cmdList->ResourceBarrier(3, barriers);
    restoreAfterForeignCommands();

    m_drawOpsSinceFlush++;
    if (!evaluated) {
      logMessage(LogLevel::Warn, "upscaler: %s evaluation failed, the game's pass runs from now on",
        upscale::kindName(request.kind));
      m_upscalerFailed = true;
      synchronize();
      m_upscaler.reset();
      return false;
    }
    return true;
  }

#endif

}
