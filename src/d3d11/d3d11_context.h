#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <d3d11_1.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../directx/directx_backend.h"
#include "../util/util_types.h"
#include "../util/com_private_data.h"
#include "d3d11_d3d12_bridge.h"
#include "../d3d12/d3d12_backend_context.h"
#include "d3d11_binding_hazards.h"
#include "d3d11_pipeline_key.h"
#include "d3d11_taa_auto.h"
#include "d3d11_taa_inject.h"

namespace dxup {

  // Forward declarations - defined in d3d11_device.h
  class D3D11Device;
  class D3D11Buffer;
  class D3D11Texture1D;
  class D3D11Texture2D;
  class D3D11Texture3D;
  class D3D11ShaderResourceView;
  class D3D11RenderTargetView;
  class D3D11DepthStencilView;
  class D3D11UnorderedAccessView;
  class D3D11SamplerState;
  class D3D11VertexShader;
  class D3D11PixelShader;
  class D3D11GeometryShader;
  class D3D11HullShader;
  class D3D11DomainShader;
  class D3D11ComputeShader;
  class D3D11InputLayout;
  class D3D11BlendState;
  class D3D11RasterizerState;
  class D3D11DepthStencilState;
  class D3D11Query;

  // ============================================================
  // Constants
  // ============================================================
  static constexpr uint32_t D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX = 32;
  static constexpr uint32_t D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX = 14;
  static constexpr uint32_t D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX = 128;
  static constexpr uint32_t D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT_MAX = 16;
  static constexpr uint32_t D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX = 8;
  static constexpr uint32_t D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX = 16;
  static constexpr uint32_t D3D11_SO_BUFFER_SLOT_COUNT_MAX = 4;
  static constexpr uint32_t D3D11_1_UAV_SLOT_COUNT_MAX = 64;
  static constexpr uint32_t D3D11_PS_CS_UAV_REGISTER_COUNT_MAX = 8;
  static constexpr uint32_t SHADER_STAGE_COUNT = 6;

  inline bool getD3D11DllSiblingPath(char* path, size_t pathSize, const char* fileName) {
    if (!path || pathSize == 0 || !fileName)
      return false;

    path[0] = '\0';
    HMODULE hModule = nullptr;
    GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&getD3D11DllSiblingPath),
      &hModule);

    if (hModule)
      GetModuleFileNameA(hModule, path, static_cast<DWORD>(pathSize));

    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) {
      *(lastSlash + 1) = '\0';
      strcat_s(path, pathSize, fileName);
    } else {
      strcpy_s(path, pathSize, fileName);
    }

    return path[0] != '\0';
  }

  inline bool renameSrvUavUpdatesEnabled() {
    static bool enabled = [] {
      char markerPath[MAX_PATH] = {};
      if (!getD3D11DllSiblingPath(markerPath, MAX_PATH, "dxup_rename_srv_uav_updates.txt"))
        strcpy_s(markerPath, MAX_PATH, "dxup_rename_srv_uav_updates.txt");

      FILE* marker = fopen(markerPath, "r");
      if (marker) {
        fclose(marker);
        return true;
      }
      return false;
    }();
    return enabled;
  }

  inline bool batchSrvUavUpdatesEnabled() {
    static bool enabled = [] {
      char markerPath[MAX_PATH] = {};
      if (!getD3D11DllSiblingPath(markerPath, MAX_PATH, "dxup_batch_srv_uav_updates.txt"))
        strcpy_s(markerPath, MAX_PATH, "dxup_batch_srv_uav_updates.txt");

      FILE* marker = fopen(markerPath, "r");
      if (marker) {
        fclose(marker);
        return true;
      }
      return false;
    }();
    return enabled;
  }

  // ============================================================
  // D3D11ImmediateContext
  //
  // COM implementation of ID3D11DeviceContext that wraps the
  // DirectX backend context.
  // ============================================================
  // Holds the device lock for the call when multithread protection is on
  // (ID3D10Multithread / ID3D11Multithread); deferred contexts are not
  // protected, as in D3D11.
  struct D3D11ContextMtGuard {
    std::recursive_mutex* mutex = nullptr;
    D3D11ContextMtGuard(D3D11Device* device, bool deferred) {
      if (!deferred && device && device->GetMultithread().isProtected()) {
        mutex = &device->GetMultithread().mutex();
        mutex->lock();
      }
    }
    ~D3D11ContextMtGuard() {
      if (mutex)
        mutex->unlock();
    }
    D3D11ContextMtGuard(const D3D11ContextMtGuard&) = delete;
    D3D11ContextMtGuard& operator=(const D3D11ContextMtGuard&) = delete;
  };

#define DXUP_CONTEXT_MT_GUARD D3D11ContextMtGuard mtGuard(m_device, m_isDeferred)

  class D3D11ImmediateContext final : public ID3D11DeviceContext4 {
  public:

    D3D11ImmediateContext(
        D3D11Device*        pDevice,
        Rc<IBackendContext>  backendCtx,
        bool                isDeferred = false)
    : m_refCount   (1)
    , m_device     (pDevice)
    , m_backendCtx (std::move(backendCtx))
    , m_isDeferred (isDeferred)
    , m_outputMergerRebinds(m_backendCtx && m_backendCtx->outputMergerStateCanNeedRebind())
    , m_infoLogging(d3d11FinalPassTraceEnabled()) {
      m_state = ContextState{};
      // A deferred context is a device child and keeps the device alive.
      // The immediate context shares the device's reference count instead.
      if (m_isDeferred && m_device)
        m_device->AddRef();
    }

    ~D3D11ImmediateContext() {
      flushPendingBufferUpdates();
      // Make the backend forget its bindings first: releasing ours may
      // destroy views whose descriptors it would otherwise reapply when its
      // command list is reset during teardown.
      if (m_backendCtx)
        m_backendCtx->releaseBindings();
      releaseAllBindings();
      m_bufferOverrides.clear();
      m_recordedDiscards.clear();
      if (m_activeContextState) {
        static_cast<D3D11DeviceChild<ID3DDeviceContextState>*>(m_activeContextState)->ReleasePrivate();
        m_activeContextState = nullptr;
      }

      if (m_isDeferred && m_device) {
        m_backendCtx = nullptr;
        m_device->Release();
      }
    }

    // ============================================================
    // IUnknown
    // ============================================================

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID  riid,
        void**  ppvObject) override {
      if (!ppvObject)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11DeviceContext)
       || riid == __uuidof(ID3D11DeviceContext1)
       || riid == __uuidof(ID3D11DeviceContext2)
       || riid == __uuidof(ID3D11DeviceContext3)
       || riid == __uuidof(ID3D11DeviceContext4)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }

      logMessage(LogLevel::Debug, "D3D11ImmediateContext::QueryInterface unsupported riid=%s",
        formatGuid(riid).c_str());
      return E_NOINTERFACE;
    }

    // As in native D3D11, references to the immediate context are references
    // to the device, which owns the context and destroys it with itself.
    ULONG STDMETHODCALLTYPE AddRef() override {
      if (!m_isDeferred)
        return m_device->AddRef();
      return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      if (!m_isDeferred)
        return m_device->Release();

      ULONG ref = m_refCount.fetch_sub(1, std::memory_order_release) - 1;
      if (ref == 0 && !processExiting()) {
        std::atomic_thread_fence(std::memory_order_acquire);
        delete this;
      }
      return ref;
    }

    // ============================================================
    // ID3D11DeviceChild
    // ============================================================

    void STDMETHODCALLTYPE GetDevice(
        ID3D11Device** ppDevice) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!ppDevice) return;
      // D3D11Device inherits from ID3D11Device.
      // AddRef is handled by the caller convention: we AddRef here.
      *ppDevice = reinterpret_cast<ID3D11Device*>(m_device);
      if (*ppDevice)
        (*ppDevice)->AddRef();
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID guid,
        UINT*   pDataSize,
        void*   pData) override {
      DXUP_CONTEXT_MT_GUARD;
      return m_privateData.getData(guid, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID     guid,
        UINT        DataSize,
        const void* pData) override {
      DXUP_CONTEXT_MT_GUARD;
      return m_privateData.setData(guid, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
        REFGUID         guid,
        const IUnknown* pData) override {
      DXUP_CONTEXT_MT_GUARD;
      return m_privateData.setInterface(guid, pData);
    }

    // ============================================================
    // ID3D11DeviceContext - Context Type
    // ============================================================

    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override {
      DXUP_CONTEXT_MT_GUARD;
      return m_isDeferred ? D3D11_DEVICE_CONTEXT_DEFERRED : D3D11_DEVICE_CONTEXT_IMMEDIATE;
    }

    UINT STDMETHODCALLTYPE GetContextFlags() override {
      DXUP_CONTEXT_MT_GUARD;
      return 0;
    }

    // ============================================================
    // ID3D11DeviceContext - Input Assembler Stage
    // ============================================================

    void STDMETHODCALLTYPE IASetInputLayout(
        ID3D11InputLayout* pInputLayout) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!replaceContextReference(m_state.ia.inputLayout, pInputLayout))
        return;
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE IASetVertexBuffers(
        UINT         StartSlot,
        UINT         NumBuffers,
        ID3D11Buffer* const* ppVertexBuffers,
        const UINT*  pStrides,
        const UINT*  pOffsets) override {
      DXUP_CONTEXT_MT_GUARD;
      prefetchForBind<D3D11Buffer>(ppVertexBuffers, m_state.ia.vertexBuffers, StartSlot, NumBuffers,
        D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX);
      for (UINT i = 0; i < NumBuffers; i++) {
        UINT slot = StartSlot + i;
        if (slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX)
          break;

        ID3D11Buffer* buffer = ppVertexBuffers ? ppVertexBuffers[i] : nullptr;
        UINT stride = pStrides ? pStrides[i] : 0;
        UINT offset = pOffsets ? pOffsets[i] : 0;
        // Binding a buffer for writing unbinds it from the input assembler,
        // so one still in its slot is readable: setting it again needs no
        // hazard check.
        if (m_state.ia.vertexBuffers[slot] == buffer
         && m_state.ia.vertexStrides[slot] == stride
         && m_state.ia.vertexOffsets[slot] == offset)
          continue;
        buffer = readableBuffer(buffer);
        if (m_state.ia.vertexBuffers[slot] == buffer
         && m_state.ia.vertexStrides[slot] == stride
         && m_state.ia.vertexOffsets[slot] == offset)
          continue;

        replaceContextReference(m_state.ia.vertexBuffers[slot], buffer);
        m_state.ia.vertexStrides[slot] = stride;
        m_state.ia.vertexOffsets[slot] = offset;

        IBackendBuffer* backendBuf = getBackendBuffer(buffer);
        m_backendCtx->bindVertexBuffer(slot, backendBuf, offset, stride);
      }
    }

    void STDMETHODCALLTYPE IASetIndexBuffer(
        ID3D11Buffer*  pIndexBuffer,
        DXGI_FORMAT    Format,
        UINT           Offset) override {
      DXUP_CONTEXT_MT_GUARD;
      // As for vertex buffers: the bound index buffer is readable.
      if (m_state.ia.indexBuffer == pIndexBuffer
       && m_state.ia.indexFormat == Format
       && m_state.ia.indexOffset == Offset)
        return;
      pIndexBuffer = readableBuffer(pIndexBuffer);
      if (m_state.ia.indexBuffer == pIndexBuffer
       && m_state.ia.indexFormat == Format
       && m_state.ia.indexOffset == Offset)
        return;

      const bool indexFormatChanged = m_state.ia.indexFormat != Format;
      replaceContextReference(m_state.ia.indexBuffer, pIndexBuffer);
      m_state.ia.indexFormat = Format;
      m_state.ia.indexOffset = Offset;

      IndexType idxType = (Format == DXGI_FORMAT_R32_UINT)
        ? IndexType::UInt32 : IndexType::UInt16;

      IBackendBuffer* backendBuf = getBackendBuffer(pIndexBuffer);
      m_backendCtx->bindIndexBuffer(backendBuf, Offset, idxType);

      if (indexFormatChanged) {
        switch (m_state.ia.topology) {
          case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
          case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
          case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
          case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
            markPipelineDirty();
            break;
          default:
            break;
        }
      }
    }

    void STDMETHODCALLTYPE IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY Topology) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_state.ia.topology == Topology)
        return;
      m_state.ia.topology = Topology;
      m_backendCtx->setPrimitiveTopology(convertTopology(Topology));
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE IAGetInputLayout(
        ID3D11InputLayout** ppInputLayout) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!ppInputLayout) return;
      *ppInputLayout = m_state.ia.inputLayout;
      if (*ppInputLayout)
        (*ppInputLayout)->AddRef();
    }

    void STDMETHODCALLTYPE IAGetVertexBuffers(
        UINT          StartSlot,
        UINT          NumBuffers,
        ID3D11Buffer** ppVertexBuffers,
        UINT*         pStrides,
        UINT*         pOffsets) override {
      DXUP_CONTEXT_MT_GUARD;
      for (UINT i = 0; i < NumBuffers; i++) {
        UINT slot = StartSlot + i;
        if (ppVertexBuffers) {
          ppVertexBuffers[i] = (slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX)
            ? m_state.ia.vertexBuffers[slot] : nullptr;
          if (ppVertexBuffers[i])
            ppVertexBuffers[i]->AddRef();
        }
        if (pStrides) {
          pStrides[i] = (slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX)
            ? m_state.ia.vertexStrides[slot] : 0;
        }
        if (pOffsets) {
          pOffsets[i] = (slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX)
            ? m_state.ia.vertexOffsets[slot] : 0;
        }
      }
    }

    void STDMETHODCALLTYPE IAGetIndexBuffer(
        ID3D11Buffer** pIndexBuffer,
        DXGI_FORMAT*   Format,
        UINT*          Offset) override {
      DXUP_CONTEXT_MT_GUARD;
      if (pIndexBuffer) {
        *pIndexBuffer = m_state.ia.indexBuffer;
        if (*pIndexBuffer)
          (*pIndexBuffer)->AddRef();
      }
      if (Format) *Format = m_state.ia.indexFormat;
      if (Offset) *Offset = m_state.ia.indexOffset;
    }

    void STDMETHODCALLTYPE IAGetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY* pTopology) override {
      DXUP_CONTEXT_MT_GUARD;
      if (pTopology) *pTopology = m_state.ia.topology;
    }

    // ============================================================
    // ID3D11DeviceContext - Vertex Shader Stage
    // ============================================================

    void STDMETHODCALLTYPE VSSetShader(
        ID3D11VertexShader*         pVertexShader,
        ID3D11ClassInstance* const* ppClassInstances,
        UINT                        NumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      auto* shader = reinterpret_cast<ID3D11DeviceChild*>(pVertexShader);
      if (!replaceBinding<D3D11VertexShader>(m_state.vs.shader, shader))
        return;
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE VSSetConstantBuffers(
        UINT                StartSlot,
        UINT                NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers(ShaderStage::Vertex, m_state.vs, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE VSSetShaderResources(
        UINT                              StartSlot,
        UINT                              NumViews,
        ID3D11ShaderResourceView* const*  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      setShaderResources(ShaderStage::Vertex, m_state.vs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE VSSetSamplers(
        UINT                         StartSlot,
        UINT                         NumSamplers,
        ID3D11SamplerState* const*   ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      setSamplers(ShaderStage::Vertex, m_state.vs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    void STDMETHODCALLTYPE VSGetShader(
        ID3D11VertexShader**   ppVertexShader,
        ID3D11ClassInstance**  ppClassInstances,
        UINT*                  pNumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppVertexShader) {
        *ppVertexShader = reinterpret_cast<ID3D11VertexShader*>(m_state.vs.shader);
        if (*ppVertexShader)
          (*ppVertexShader)->AddRef();
      }
      if (pNumClassInstances) *pNumClassInstances = 0;
    }

    void STDMETHODCALLTYPE VSGetConstantBuffers(
        UINT           StartSlot,
        UINT           NumBuffers,
        ID3D11Buffer** ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers(m_state.vs.constantBuffers, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE VSGetShaderResources(
        UINT                        StartSlot,
        UINT                        NumViews,
        ID3D11ShaderResourceView**  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      getShaderResources(m_state.vs.srvs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE VSGetSamplers(
        UINT                  StartSlot,
        UINT                  NumSamplers,
        ID3D11SamplerState**  ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      getSamplers(m_state.vs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    // ============================================================
    // ID3D11DeviceContext - Hull Shader Stage
    // ============================================================

    void STDMETHODCALLTYPE HSSetShader(
        ID3D11HullShader*           pHullShader,
        ID3D11ClassInstance* const* ppClassInstances,
        UINT                        NumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      auto* shader = reinterpret_cast<ID3D11DeviceChild*>(pHullShader);
      if (!replaceBinding<D3D11HullShader>(m_state.hs.shader, shader))
        return;
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE HSSetConstantBuffers(
        UINT                StartSlot,
        UINT                NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers(ShaderStage::Hull, m_state.hs, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE HSSetShaderResources(
        UINT                              StartSlot,
        UINT                              NumViews,
        ID3D11ShaderResourceView* const*  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      setShaderResources(ShaderStage::Hull, m_state.hs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE HSSetSamplers(
        UINT                         StartSlot,
        UINT                         NumSamplers,
        ID3D11SamplerState* const*   ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      setSamplers(ShaderStage::Hull, m_state.hs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    void STDMETHODCALLTYPE HSGetShader(
        ID3D11HullShader**     ppHullShader,
        ID3D11ClassInstance**  ppClassInstances,
        UINT*                  pNumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppHullShader) {
        *ppHullShader = reinterpret_cast<ID3D11HullShader*>(m_state.hs.shader);
        if (*ppHullShader)
          (*ppHullShader)->AddRef();
      }
      if (pNumClassInstances) *pNumClassInstances = 0;
    }

    void STDMETHODCALLTYPE HSGetConstantBuffers(
        UINT           StartSlot,
        UINT           NumBuffers,
        ID3D11Buffer** ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers(m_state.hs.constantBuffers, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE HSGetShaderResources(
        UINT                        StartSlot,
        UINT                        NumViews,
        ID3D11ShaderResourceView**  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      getShaderResources(m_state.hs.srvs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE HSGetSamplers(
        UINT                  StartSlot,
        UINT                  NumSamplers,
        ID3D11SamplerState**  ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      getSamplers(m_state.hs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    // ============================================================
    // ID3D11DeviceContext - Domain Shader Stage
    // ============================================================

    void STDMETHODCALLTYPE DSSetShader(
        ID3D11DomainShader*         pDomainShader,
        ID3D11ClassInstance* const* ppClassInstances,
        UINT                        NumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      auto* shader = reinterpret_cast<ID3D11DeviceChild*>(pDomainShader);
      if (!replaceBinding<D3D11DomainShader>(m_state.ds.shader, shader))
        return;
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE DSSetConstantBuffers(
        UINT                StartSlot,
        UINT                NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers(ShaderStage::Domain, m_state.ds, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE DSSetShaderResources(
        UINT                              StartSlot,
        UINT                              NumViews,
        ID3D11ShaderResourceView* const*  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      setShaderResources(ShaderStage::Domain, m_state.ds, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE DSSetSamplers(
        UINT                         StartSlot,
        UINT                         NumSamplers,
        ID3D11SamplerState* const*   ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      setSamplers(ShaderStage::Domain, m_state.ds.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    void STDMETHODCALLTYPE DSGetShader(
        ID3D11DomainShader**   ppDomainShader,
        ID3D11ClassInstance**  ppClassInstances,
        UINT*                  pNumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppDomainShader) {
        *ppDomainShader = reinterpret_cast<ID3D11DomainShader*>(m_state.ds.shader);
        if (*ppDomainShader)
          (*ppDomainShader)->AddRef();
      }
      if (pNumClassInstances) *pNumClassInstances = 0;
    }

    void STDMETHODCALLTYPE DSGetConstantBuffers(
        UINT           StartSlot,
        UINT           NumBuffers,
        ID3D11Buffer** ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers(m_state.ds.constantBuffers, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE DSGetShaderResources(
        UINT                        StartSlot,
        UINT                        NumViews,
        ID3D11ShaderResourceView**  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      getShaderResources(m_state.ds.srvs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE DSGetSamplers(
        UINT                  StartSlot,
        UINT                  NumSamplers,
        ID3D11SamplerState**  ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      getSamplers(m_state.ds.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    // ============================================================
    // ID3D11DeviceContext - Geometry Shader Stage
    // ============================================================

    void STDMETHODCALLTYPE GSSetShader(
        ID3D11GeometryShader*       pShader,
        ID3D11ClassInstance* const* ppClassInstances,
        UINT                        NumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      auto* shader = reinterpret_cast<ID3D11DeviceChild*>(pShader);
      if (!replaceBinding<D3D11GeometryShader>(m_state.gs.shader, shader))
        return;
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE GSSetConstantBuffers(
        UINT                StartSlot,
        UINT                NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers(ShaderStage::Geometry, m_state.gs, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE GSSetShaderResources(
        UINT                              StartSlot,
        UINT                              NumViews,
        ID3D11ShaderResourceView* const*  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      setShaderResources(ShaderStage::Geometry, m_state.gs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE GSSetSamplers(
        UINT                         StartSlot,
        UINT                         NumSamplers,
        ID3D11SamplerState* const*   ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      setSamplers(ShaderStage::Geometry, m_state.gs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    void STDMETHODCALLTYPE GSGetShader(
        ID3D11GeometryShader** ppGeometryShader,
        ID3D11ClassInstance**  ppClassInstances,
        UINT*                  pNumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppGeometryShader) {
        *ppGeometryShader = reinterpret_cast<ID3D11GeometryShader*>(m_state.gs.shader);
        if (*ppGeometryShader)
          (*ppGeometryShader)->AddRef();
      }
      if (pNumClassInstances) *pNumClassInstances = 0;
    }

    void STDMETHODCALLTYPE GSGetConstantBuffers(
        UINT           StartSlot,
        UINT           NumBuffers,
        ID3D11Buffer** ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers(m_state.gs.constantBuffers, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE GSGetShaderResources(
        UINT                        StartSlot,
        UINT                        NumViews,
        ID3D11ShaderResourceView**  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      getShaderResources(m_state.gs.srvs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE GSGetSamplers(
        UINT                  StartSlot,
        UINT                  NumSamplers,
        ID3D11SamplerState**  ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      getSamplers(m_state.gs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    // ============================================================
    // ID3D11DeviceContext - Pixel Shader Stage
    // ============================================================

    void STDMETHODCALLTYPE PSSetShader(
        ID3D11PixelShader*          pPixelShader,
        ID3D11ClassInstance* const* ppClassInstances,
        UINT                        NumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      auto* shader = reinterpret_cast<ID3D11DeviceChild*>(pPixelShader);
      if (!replaceBinding<D3D11PixelShader>(m_state.ps.shader, shader))
        return;
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE PSSetConstantBuffers(
        UINT                StartSlot,
        UINT                NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers(ShaderStage::Pixel, m_state.ps, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE PSSetShaderResources(
        UINT                              StartSlot,
        UINT                              NumViews,
        ID3D11ShaderResourceView* const*  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      setShaderResources(ShaderStage::Pixel, m_state.ps, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE PSSetSamplers(
        UINT                         StartSlot,
        UINT                         NumSamplers,
        ID3D11SamplerState* const*   ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      setSamplers(ShaderStage::Pixel, m_state.ps.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    void STDMETHODCALLTYPE PSGetShader(
        ID3D11PixelShader**    ppPixelShader,
        ID3D11ClassInstance**  ppClassInstances,
        UINT*                  pNumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppPixelShader) {
        *ppPixelShader = reinterpret_cast<ID3D11PixelShader*>(m_state.ps.shader);
        if (*ppPixelShader)
          (*ppPixelShader)->AddRef();
      }
      if (pNumClassInstances) *pNumClassInstances = 0;
    }

    void STDMETHODCALLTYPE PSGetConstantBuffers(
        UINT           StartSlot,
        UINT           NumBuffers,
        ID3D11Buffer** ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers(m_state.ps.constantBuffers, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE PSGetShaderResources(
        UINT                        StartSlot,
        UINT                        NumViews,
        ID3D11ShaderResourceView**  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      getShaderResources(m_state.ps.srvs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE PSGetSamplers(
        UINT                  StartSlot,
        UINT                  NumSamplers,
        ID3D11SamplerState**  ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      getSamplers(m_state.ps.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    // ============================================================
    // ID3D11DeviceContext - Compute Shader Stage
    // ============================================================

    void STDMETHODCALLTYPE CSSetShader(
        ID3D11ComputeShader*        pComputeShader,
        ID3D11ClassInstance* const* ppClassInstances,
        UINT                        NumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      auto* shader = reinterpret_cast<ID3D11DeviceChild*>(pComputeShader);
      if (!replaceBinding<D3D11ComputeShader>(m_state.cs.shader, shader))
        return;
      markPipelineDirty();
    }

    void STDMETHODCALLTYPE CSSetConstantBuffers(
        UINT                StartSlot,
        UINT                NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers(ShaderStage::Compute, m_state.cs, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE CSSetShaderResources(
        UINT                              StartSlot,
        UINT                              NumViews,
        ID3D11ShaderResourceView* const*  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      setShaderResources(ShaderStage::Compute, m_state.cs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE CSSetSamplers(
        UINT                         StartSlot,
        UINT                         NumSamplers,
        ID3D11SamplerState* const*   ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      setSamplers(ShaderStage::Compute, m_state.cs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(
        UINT                              StartSlot,
        UINT                              NumUAVs,
        ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
        const UINT*                       pUAVInitialCounts) override {
      DXUP_CONTEXT_MT_GUARD;
      for (UINT i = 0; i < NumUAVs; i++) {
        UINT slot = StartSlot + i;
        if (slot >= D3D11_1_UAV_SLOT_COUNT_MAX)
          break;

        ID3D11UnorderedAccessView* view = ppUnorderedAccessViews
          ? ppUnorderedAccessViews[i]
          : nullptr;
        if (replaceContextReference(m_state.cs.uavs[slot], view)) {
          if (view && slot >= m_state.cs.uavSlotEnd)
            m_state.cs.uavSlotEnd = slot + 1;
          bindUnorderedAccessView(ShaderStage::Compute, slot, view);
          if (view)
            unbindForWrite(unorderedAccessRange(view), WriteSource::Compute);
        }

        if (pUAVInitialCounts && pUAVInitialCounts[i] != 0xFFFFFFFF && view) {
          IBackendBufferView* backendView = getBackendBufferUAV(view);
          if (backendView)
            m_backendCtx->setUnorderedAccessCounter(backendView, pUAVInitialCounts[i]);
        }
      }
    }

    void STDMETHODCALLTYPE CSGetShader(
        ID3D11ComputeShader**  ppComputeShader,
        ID3D11ClassInstance**  ppClassInstances,
        UINT*                  pNumClassInstances) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppComputeShader) {
        *ppComputeShader = reinterpret_cast<ID3D11ComputeShader*>(m_state.cs.shader);
        if (*ppComputeShader)
          (*ppComputeShader)->AddRef();
      }
      if (pNumClassInstances) *pNumClassInstances = 0;
    }

    void STDMETHODCALLTYPE CSGetConstantBuffers(
        UINT           StartSlot,
        UINT           NumBuffers,
        ID3D11Buffer** ppConstantBuffers) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers(m_state.cs.constantBuffers, StartSlot, NumBuffers, ppConstantBuffers);
    }

    void STDMETHODCALLTYPE CSGetShaderResources(
        UINT                        StartSlot,
        UINT                        NumViews,
        ID3D11ShaderResourceView**  ppShaderResourceViews) override {
      DXUP_CONTEXT_MT_GUARD;
      getShaderResources(m_state.cs.srvs, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE CSGetSamplers(
        UINT                  StartSlot,
        UINT                  NumSamplers,
        ID3D11SamplerState**  ppSamplers) override {
      DXUP_CONTEXT_MT_GUARD;
      getSamplers(m_state.cs.samplers, StartSlot, NumSamplers, ppSamplers);
    }

    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(
        UINT                        StartSlot,
        UINT                        NumUAVs,
        ID3D11UnorderedAccessView** ppUnorderedAccessViews) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!ppUnorderedAccessViews) return;
      for (UINT i = 0; i < NumUAVs; i++) {
        UINT slot = StartSlot + i;
        ppUnorderedAccessViews[i] = (slot < D3D11_1_UAV_SLOT_COUNT_MAX)
          ? m_state.cs.uavs[slot] : nullptr;
        if (ppUnorderedAccessViews[i])
          ppUnorderedAccessViews[i]->AddRef();
      }
    }

    // ============================================================
    // ID3D11DeviceContext - Output Merger Stage
    // ============================================================

    void STDMETHODCALLTYPE OMSetRenderTargets(
        UINT                          NumViews,
        ID3D11RenderTargetView* const* ppRenderTargetViews,
        ID3D11DepthStencilView*       pDepthStencilView) override {
      DXUP_CONTEXT_MT_GUARD;
      setOutputMergerRenderTargets(
        NumViews,
        ppRenderTargetViews,
        pDepthStencilView,
        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX);
    }

    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(
        UINT                              NumRTVs,
        ID3D11RenderTargetView* const*    ppRenderTargetViews,
        ID3D11DepthStencilView*           pDepthStencilView,
        UINT                              UAVStartSlot,
        UINT                              NumUAVs,
        ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
        const UINT*                       pUAVInitialCounts) override {
      DXUP_CONTEXT_MT_GUARD;

      static int omSetRtUavLogCount = 0;
      if (omSetRtUavLogCount < 48) {
        logMessage(LogLevel::Info,
          "OMSetRTAndUAV: tid=%lu numRT=%u keepRT=%u dsv=%p uavStart=%u numUAV=%u keepUAV=%u rtv0=%p uav0=%p",
          static_cast<unsigned long>(currentThreadId()),
          NumRTVs,
          NumRTVs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? 1u : 0u,
          pDepthStencilView,
          UAVStartSlot,
          NumUAVs,
          NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS ? 1u : 0u,
          ppRenderTargetViews ? ppRenderTargetViews[0] : nullptr,
          ppUnorderedAccessViews ? ppUnorderedAccessViews[0] : nullptr);
        omSetRtUavLogCount++;
      }

      // Render targets and unordered access views of the same part of a
      // resource: D3D11 ignores the call.
      if (NumUAVs != 0 && NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS && ppUnorderedAccessViews) {
        const bool newTargets = NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
        const UINT rtvCount = newTargets
          ? (std::min)(NumRTVs, UINT(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX))
          : m_state.om.numRenderTargets;
        for (UINT i = 0; i < NumUAVs; i++) {
          ID3D11UnorderedAccessView* uav = ppUnorderedAccessViews[i];
          if (!uav)
            continue;
          const D3D11ViewRange uavRange = unorderedAccessRange(uav);
          for (UINT rt = 0; rt < rtvCount; rt++) {
            ID3D11RenderTargetView* rtv = newTargets
              ? (ppRenderTargetViews ? ppRenderTargetViews[rt] : nullptr)
              : m_state.om.renderTargets[rt];
            if (rtv && viewResource(rtv) == uavRange.resource && viewRangesOverlap(renderTargetRange(rtv), uavRange)) {
              logMessage(LogLevel::Warn, "OMSetRenderTargetsAndUnorderedAccessViews: render target and UAV overlap, call ignored");
              return;
            }
          }
        }
      }

      if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        const UINT maxRenderTargetCount = (NumUAVs == 0 || NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
          ? UINT(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX)
          : UAVStartSlot;

        setOutputMergerRenderTargets(
          NumRTVs,
          ppRenderTargetViews,
          pDepthStencilView,
          maxRenderTargetCount);
      }

      if (NumUAVs == 0 || NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
        return;

      if (NumRTVs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        truncateOutputMergerRenderTargets(UAVStartSlot);

      clearPixelUnorderedAccessViewsBefore(getActiveRenderTargetCount());

      for (UINT i = 0; i < NumUAVs; i++) {
        UINT slot = UAVStartSlot + i;
        if (slot < D3D11_PS_CS_UAV_REGISTER_COUNT_MAX) {
          ID3D11UnorderedAccessView* view = ppUnorderedAccessViews
            ? ppUnorderedAccessViews[i]
            : nullptr;
          if (replaceContextReference(m_state.om.uavs[slot], view)) {
            bindUnorderedAccessView(ShaderStage::Pixel, slot, view);
            if (view)
              unbindForWrite(unorderedAccessRange(view), WriteSource::OutputMerger);
          }

          if (pUAVInitialCounts && pUAVInitialCounts[i] != 0xFFFFFFFF && view) {
            IBackendBufferView* backendView = getBackendBufferUAV(view);
            if (backendView)
              m_backendCtx->setUnorderedAccessCounter(backendView, pUAVInitialCounts[i]);
          }
        }
      }

      static int omSetRtUavStateLogCount = 0;
      if (omSetRtUavStateLogCount < 48) {
        logMessage(LogLevel::Info,
          "OMSetRTAndUAV-state: tid=%lu activeRT=%u dsv=%p uav0=%p uav1=%p",
          static_cast<unsigned long>(currentThreadId()),
          getActiveRenderTargetCount(),
          m_state.om.depthStencilView,
          m_state.om.uavs[0],
          m_state.om.uavs[1]);
        omSetRtUavStateLogCount++;
      }
    }

    void STDMETHODCALLTYPE OMSetBlendState(
        ID3D11BlendState* pBlendState,
        const FLOAT       BlendFactor[4],
        UINT              SampleMask) override {
      DXUP_CONTEXT_MT_GUARD;
      const bool psoChanged = replaceContextReference(m_state.om.blendState, pBlendState)
                           || m_state.om.sampleMask != SampleMask;
      m_state.om.sampleMask = SampleMask;
      if (BlendFactor) {
        if (std::memcmp(m_state.om.blendFactor, BlendFactor, sizeof(float) * 4) != 0) {
          std::memcpy(m_state.om.blendFactor, BlendFactor, sizeof(float) * 4);
          m_backendCtx->setBlendFactor(BlendFactor);
        }
      }
      if (psoChanged)
        markPipelineDirty();
    }

    void STDMETHODCALLTYPE OMSetDepthStencilState(
        ID3D11DepthStencilState* pDepthStencilState,
        UINT                     StencilRef) override {
      DXUP_CONTEXT_MT_GUARD;
      const bool usedDepthStencil = depthStencilUsed();
      const bool psoChanged = replaceContextReference(m_state.om.depthStencilState, pDepthStencilState);
      // A bound depth view decides the forced multisampling scope only
      // while draws use it.
      if (psoChanged && m_state.om.depthStencilView && usedDepthStencil != depthStencilUsed())
        m_msaaScopeDirty = true;
      const bool stencilChanged = m_state.om.stencilRef != StencilRef;
      m_state.om.stencilRef = StencilRef;
      if (stencilChanged)
        m_backendCtx->setStencilRef(StencilRef);
      if (psoChanged)
        markPipelineDirty();
    }

    void STDMETHODCALLTYPE OMGetRenderTargets(
        UINT                        NumViews,
        ID3D11RenderTargetView**    ppRenderTargetViews,
        ID3D11DepthStencilView**    ppDepthStencilView) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppRenderTargetViews) {
        for (UINT i = 0; i < NumViews; i++) {
          ppRenderTargetViews[i] = (i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX)
            ? m_state.om.renderTargets[i] : nullptr;
          if (ppRenderTargetViews[i])
            ppRenderTargetViews[i]->AddRef();
        }
      }
      if (ppDepthStencilView) {
        *ppDepthStencilView = m_state.om.depthStencilView;
        if (*ppDepthStencilView)
          (*ppDepthStencilView)->AddRef();
      }
    }

    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(
        UINT                        NumRTVs,
        ID3D11RenderTargetView**    ppRenderTargetViews,
        ID3D11DepthStencilView**    ppDepthStencilView,
        UINT                        UAVStartSlot,
        UINT                        NumUAVs,
        ID3D11UnorderedAccessView** ppUnorderedAccessViews) override {
      DXUP_CONTEXT_MT_GUARD;
      OMGetRenderTargets(NumRTVs, ppRenderTargetViews, ppDepthStencilView);

      if (ppUnorderedAccessViews) {
        for (UINT i = 0; i < NumUAVs; i++) {
          UINT slot = UAVStartSlot + i;
          ppUnorderedAccessViews[i] = (slot < D3D11_PS_CS_UAV_REGISTER_COUNT_MAX)
            ? m_state.om.uavs[slot] : nullptr;
          if (ppUnorderedAccessViews[i])
            ppUnorderedAccessViews[i]->AddRef();
        }
      }
    }

    void STDMETHODCALLTYPE OMGetBlendState(
        ID3D11BlendState** ppBlendState,
        FLOAT              BlendFactor[4],
        UINT*              pSampleMask) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppBlendState) {
        *ppBlendState = m_state.om.blendState;
        if (*ppBlendState)
          (*ppBlendState)->AddRef();
      }
      if (BlendFactor)
        std::memcpy(BlendFactor, m_state.om.blendFactor, sizeof(float) * 4);
      if (pSampleMask)
        *pSampleMask = m_state.om.sampleMask;
    }

    void STDMETHODCALLTYPE OMGetDepthStencilState(
        ID3D11DepthStencilState** ppDepthStencilState,
        UINT*                     pStencilRef) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppDepthStencilState) {
        *ppDepthStencilState = m_state.om.depthStencilState;
        if (*ppDepthStencilState)
          (*ppDepthStencilState)->AddRef();
      }
      if (pStencilRef)
        *pStencilRef = m_state.om.stencilRef;
    }

    // ============================================================
    // ID3D11DeviceContext - Rasterizer Stage
    // ============================================================

    void STDMETHODCALLTYPE RSSetViewports(
        UINT              NumViewports,
        const D3D11_VIEWPORT* pViewports) override {
      DXUP_CONTEXT_MT_GUARD;
      // D3D11 ignores the call for a viewport out of bounds or with a depth
      // range outside [0, 1] or inverted; D3D12 would draw with it.
      if (NumViewports > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX
       || (NumViewports && !pViewports))
        return;
      // The bound viewports passed the checks below.
      if (m_state.rs.numViewports == NumViewports
       && (!NumViewports || std::memcmp(m_state.rs.viewports, pViewports, sizeof(D3D11_VIEWPORT) * NumViewports) == 0))
        return;
      for (UINT i = 0; i < NumViewports; i++) {
        const D3D11_VIEWPORT& v = pViewports[i];
        if (!(v.MinDepth >= 0.0f && v.MaxDepth <= 1.0f && v.MinDepth <= v.MaxDepth
           && v.Width >= 0.0f && v.Height >= 0.0f
           && v.TopLeftX >= float(D3D11_VIEWPORT_BOUNDS_MIN) && v.TopLeftY >= float(D3D11_VIEWPORT_BOUNDS_MIN)
           && v.TopLeftX + v.Width <= float(D3D11_VIEWPORT_BOUNDS_MAX)
           && v.TopLeftY + v.Height <= float(D3D11_VIEWPORT_BOUNDS_MAX)))
          return;
      }

      m_state.rs.numViewports = NumViewports;
      if (pViewports && NumViewports > 0) {
        Viewport backendViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX];
        for (UINT i = 0; i < NumViewports; i++) {
          m_state.rs.viewports[i] = pViewports[i];
          backendViewports[i].x        = pViewports[i].TopLeftX;
          backendViewports[i].y        = pViewports[i].TopLeftY;
          backendViewports[i].width    = pViewports[i].Width;
          backendViewports[i].height   = pViewports[i].Height;
          backendViewports[i].minDepth = pViewports[i].MinDepth;
          backendViewports[i].maxDepth = pViewports[i].MaxDepth;
        }
        m_backendCtx->setViewports(NumViewports, backendViewports);
      } else {
        // No viewports: nothing is rasterized.
        m_backendCtx->setViewports(0, nullptr);
      }
      m_injectViewportJittered = false;
      if (!rasterizerScissorEnabled())
        applyRasterizerScissorState();
    }

    void STDMETHODCALLTYPE RSSetScissorRects(
        UINT             NumRects,
        const D3D11_RECT* pRects) override {
      DXUP_CONTEXT_MT_GUARD;
      if (NumRects <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX
       && m_state.rs.numScissorRects == NumRects
       && (!NumRects || !pRects
        || std::memcmp(m_state.rs.scissorRects, pRects, sizeof(D3D11_RECT) * NumRects) == 0))
        return;

      m_state.rs.numScissorRects = NumRects;
      if (pRects && NumRects > 0) {
        for (UINT i = 0; i < NumRects; i++) {
          m_state.rs.scissorRects[i] = pRects[i];
        }
      }
      if (rasterizerScissorEnabled())
        applyRasterizerScissorState();
    }

    void STDMETHODCALLTYPE RSSetState(
        ID3D11RasterizerState* pRasterizerState) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!replaceContextReference(m_state.rs.state, pRasterizerState))
        return;
      markPipelineDirty();
      applyRasterizerScissorState();
    }

    void STDMETHODCALLTYPE RSGetViewports(
        UINT*          pNumViewports,
        D3D11_VIEWPORT* pViewports) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pNumViewports) return;
      if (!pViewports) {
        *pNumViewports = m_state.rs.numViewports;
        return;
      }
      UINT count = (*pNumViewports < m_state.rs.numViewports) ? *pNumViewports : m_state.rs.numViewports;
      for (UINT i = 0; i < count; i++)
        pViewports[i] = m_state.rs.viewports[i];
      *pNumViewports = count;
    }

    void STDMETHODCALLTYPE RSGetScissorRects(
        UINT*      pNumRects,
        D3D11_RECT* pRects) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pNumRects) return;
      if (!pRects) {
        *pNumRects = m_state.rs.numScissorRects;
        return;
      }
      UINT count = (*pNumRects < m_state.rs.numScissorRects) ? *pNumRects : m_state.rs.numScissorRects;
      for (UINT i = 0; i < count; i++)
        pRects[i] = m_state.rs.scissorRects[i];
      *pNumRects = count;
    }

    void STDMETHODCALLTYPE RSGetState(
        ID3D11RasterizerState** ppRasterizerState) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!ppRasterizerState) return;
      *ppRasterizerState = m_state.rs.state;
      if (*ppRasterizerState)
        (*ppRasterizerState)->AddRef();
    }

    // ============================================================
    // ID3D11DeviceContext - Stream Output Stage
    // ============================================================

    void STDMETHODCALLTYPE SOSetTargets(
        UINT                NumBuffers,
        ID3D11Buffer* const* ppSOTargets,
        const UINT*         pOffsets) override {
      DXUP_CONTEXT_MT_GUARD;
      IBackendBuffer* backendBuffers[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};
      uint32_t backendOffsets[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};

      for (UINT i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT_MAX; i++) {
        if (i < NumBuffers && ppSOTargets) {
          replaceContextReference(m_state.so.targets[i], ppSOTargets[i]);
          m_state.so.offsets[i] = pOffsets ? pOffsets[i] : 0;
          backendBuffers[i] = getBackendBuffer(ppSOTargets[i]);
          backendOffsets[i] = pOffsets ? pOffsets[i] : 0;
        } else {
          clearContextReference(m_state.so.targets[i]);
          m_state.so.offsets[i] = 0;
        }
      }
      m_backendCtx->bindStreamOutputTargets(NumBuffers, backendBuffers, backendOffsets);
      for (UINT i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT_MAX; i++) {
        if (m_state.so.targets[i])
          unbindForWrite(wholeBufferRange(m_state.so.targets[i]), WriteSource::StreamOutput);
      }
    }

    void rebindStreamOutputTargets() {
      IBackendBuffer* backendBuffers[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};
      uint32_t backendOffsets[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};
      UINT count = 0;
      for (UINT i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT_MAX; i++) {
        backendBuffers[i] = getBackendBuffer(m_state.so.targets[i]);
        backendOffsets[i] = m_state.so.offsets[i];
        if (m_state.so.targets[i])
          count = i + 1;
      }
      m_backendCtx->bindStreamOutputTargets(count, backendBuffers, backendOffsets);
    }

    void STDMETHODCALLTYPE SOGetTargets(
        UINT           NumBuffers,
        ID3D11Buffer** ppSOTargets) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!ppSOTargets) return;
      for (UINT i = 0; i < NumBuffers; i++) {
        ppSOTargets[i] = (i < D3D11_SO_BUFFER_SLOT_COUNT_MAX)
          ? m_state.so.targets[i] : nullptr;
        if (ppSOTargets[i])
          ppSOTargets[i]->AddRef();
      }
    }

    // ============================================================
    // ID3D11DeviceContext - Draw Commands
    // ============================================================

    void STDMETHODCALLTYPE Draw(
        UINT VertexCount,
        UINT StartVertexLocation) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDrawState("Draw");
      if (!prepareRecorded(RecordOp::Draw, VertexCount, 1, StartVertexLocation, 0)) return;
      flushPendingBufferUpdates();
      rebindGraphicsDrawState();
      m_backendCtx->draw(VertexCount, 1, StartVertexLocation, 0);
    }

    void STDMETHODCALLTYPE DrawIndexed(
        UINT IndexCount,
        UINT StartIndexLocation,
        INT  BaseVertexLocation) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDrawState("DrawIndexed");
      if (m_infoLogging)
        logTrackedFinalPassState("DrawIndexed", IndexCount, 1u, StartIndexLocation, BaseVertexLocation, 0u);
      if (!prepareRecorded(RecordOp::DrawIndexed, IndexCount, 1, StartIndexLocation, BaseVertexLocation)) return;
      flushPendingBufferUpdates();
      rebindGraphicsDrawState();
      m_backendCtx->drawIndexed(IndexCount, 1, StartIndexLocation, BaseVertexLocation, 0);
    }

    void STDMETHODCALLTYPE DrawInstanced(
        UINT VertexCountPerInstance,
        UINT InstanceCount,
        UINT StartVertexLocation,
        UINT StartInstanceLocation) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDrawState("DrawInstanced");
      if (!prepareRecorded(RecordOp::Draw, VertexCountPerInstance, InstanceCount, StartVertexLocation, 0)) return;
      flushPendingBufferUpdates();
      rebindGraphicsDrawState();
      m_backendCtx->draw(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }

    void STDMETHODCALLTYPE DrawIndexedInstanced(
        UINT IndexCountPerInstance,
        UINT InstanceCount,
        UINT StartIndexLocation,
        INT  BaseVertexLocation,
        UINT StartInstanceLocation) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDrawState("DrawIndexedInstanced");
      if (m_infoLogging)
        logTrackedFinalPassState("DrawIndexedInstanced", IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
      if (!prepareRecorded(RecordOp::DrawIndexed, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation)) return;
      flushPendingBufferUpdates();
      rebindGraphicsDrawState();
      m_backendCtx->drawIndexed(IndexCountPerInstance, InstanceCount,
                                StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }

    void STDMETHODCALLTYPE DrawAuto() override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDrawState("DrawAuto");
      // Draws what stream output wrote into the buffer in vertex slot 0.
      IBackendBuffer* vertexBuffer = getBackendBuffer(m_state.ia.vertexBuffers[0]);
      const uint32_t stride = m_state.ia.vertexStrides[0];
      if (!vertexBuffer || !stride) return;

      if (!prepareRecorded(RecordOp::DrawAuto, 0, 1, 0, 0)) return;
      flushPendingBufferUpdates();
      rebindGraphicsDrawState();
      m_backendCtx->drawAuto(vertexBuffer, stride);
    }

    void STDMETHODCALLTYPE DrawInstancedIndirect(
        ID3D11Buffer* pBufferForArgs,
        UINT          AlignedByteOffsetForArgs) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDrawState(("DrawInstancedIndirect" + traceIndirectArgs(pBufferForArgs, AlignedByteOffsetForArgs, 4)).c_str());
      if (!pBufferForArgs) return;
      if (!prepareRecorded(RecordOp::DrawIndirect, 0, 0, AlignedByteOffsetForArgs, 0)) return;
      flushPendingBufferUpdates();
      rebindGraphicsDrawState();
      IBackendBuffer* backendBuf = getBackendBuffer(pBufferForArgs);
      if (backendBuf) {
        m_backendCtx->drawIndirect(backendBuf, AlignedByteOffsetForArgs);
      }
    }

    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(
        ID3D11Buffer* pBufferForArgs,
        UINT          AlignedByteOffsetForArgs) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDrawState(("DrawIndexedInstancedIndirect" + traceIndirectArgs(pBufferForArgs, AlignedByteOffsetForArgs, 5)).c_str());
      if (!pBufferForArgs) return;
      if (!prepareRecorded(RecordOp::DrawIndirect, 0, 0, AlignedByteOffsetForArgs, 0)) return;
      flushPendingBufferUpdates();
      rebindGraphicsDrawState();
      IBackendBuffer* backendBuf = getBackendBuffer(pBufferForArgs);
      if (backendBuf) {
        m_backendCtx->drawIndexedIndirect(backendBuf, AlignedByteOffsetForArgs);
      }
    }

    // ============================================================
    // ID3D11DeviceContext - Dispatch
    // ============================================================

    // A dispatch with its shader hash (as the GPU profiler and the shader
    // dumps name it), group counts, the views with their resources, and the
    // constant buffers; with dxup_trace_cbs.txt their contents go to
    // dxup_trace_<call>_cb<slot>.bin next to the DLL (immediate context only).
    void traceDispatch(const char* kind, UINT x = 0, UINT y = 0, UINT z = 0) {
      if (m_probing)
        return;
      const uint32_t call = m_device->diagTraceIndex.load(std::memory_order_relaxed) + 1;
      std::string line = kind;
      uint64_t hash = 0;
      if (auto* cs = static_cast<D3D11ComputeShader*>(m_state.cs.shader))
        if (auto* backend = cs->GetBackendShader())
          hash = backend->getBytecodeHash();
      line += formatTrace(" cs=%p hash=%016llx groups=%u,%u,%u", static_cast<void*>(m_state.cs.shader),
        static_cast<unsigned long long>(hash), x, y, z);
      for (UINT i = 0; i < 32; i++) {
        if (ID3D11ShaderResourceView* view = m_state.cs.srvs[i]) {
          ComPtr<ID3D11Resource> resource;
          view->GetResource(resource.GetAddressOf());
          line += formatTrace(" cs.t%u(%s = %s)", i, traceView(view, false).c_str(), describeTraceResource(resource.Get()).c_str());
        }
      }
      for (UINT i = 0; i < 8; i++) {
        if (ID3D11UnorderedAccessView* view = m_state.cs.uavs[i]) {
          ComPtr<ID3D11Resource> resource;
          view->GetResource(resource.GetAddressOf());
          line += formatTrace(" cs.u%u(%s = %s)", i, traceView(view, true).c_str(), describeTraceResource(resource.Get()).c_str());
        }
      }
      for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX; i++) {
        ID3D11Buffer* buffer = m_state.cs.constantBuffers[i];
        if (!buffer)
          continue;
        D3D11_BUFFER_DESC desc = {};
        buffer->GetDesc(&desc);
        line += formatTrace(" cs.cb%u(%p size=%u first=%u count=%u%s)", i, static_cast<void*>(buffer), desc.ByteWidth,
          m_state.cs.constantBufferFirst[i], m_state.cs.constantBufferCount[i],
          traceConstantBuffersEnabled() && dumpTraceBuffer(buffer, call, i) ? " dumped" : "");
      }
      traceCall(line);
    }

    static std::string describeTraceResource(ID3D11Resource* resource) {
      if (!resource)
        return "null";
      D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dim);
      char out[160];
      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d = {};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&d);
        sprintf_s(out, "tex2d %ux%u fmt=%u mips=%u arr=%u samples=%u bind=0x%x", d.Width, d.Height,
          static_cast<unsigned>(d.Format), d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.BindFlags);
      } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d = {};
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&d);
        sprintf_s(out, "tex3d %ux%ux%u fmt=%u mips=%u bind=0x%x", d.Width, d.Height, d.Depth,
          static_cast<unsigned>(d.Format), d.MipLevels, d.BindFlags);
      } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d = {};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
        sprintf_s(out, "buf size=%u stride=%u usage=%u bind=0x%x misc=0x%x", d.ByteWidth, d.StructureByteStride,
          static_cast<unsigned>(d.Usage), d.BindFlags, d.MiscFlags);
      } else {
        sprintf_s(out, "res dim=%u", static_cast<unsigned>(dim));
      }
      return out;
    }

    // The buffer as the GPU work before it left it, into a file next to the
    // DLL. Immediate context only: a deferred one cannot read back.
    bool dumpTraceBuffer(ID3D11Buffer* buffer, uint32_t call, UINT slot) {
      if (m_isDeferred || m_probing)
        return false;
      D3D11_BUFFER_DESC desc = {};
      buffer->GetDesc(&desc);
      const UINT size = (std::min)(desc.ByteWidth, 65536u);
      D3D11_BUFFER_DESC stagingDesc = { size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
      ComPtr<ID3D11Buffer> staging;
      if (!size || FAILED(m_device->CreateBuffer(&stagingDesc, nullptr, staging.GetAddressOf())))
        return false;
      char path[MAX_PATH] = {};
      HMODULE module = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&d3d11InfoLoggingEnabled), &module);
      if (!module || !GetModuleFileNameA(module, path, MAX_PATH) || !strrchr(path, '\\'))
        return false;
      char name[64];
      sprintf_s(name, "dxup_trace_%u_cb%u.bin", call, slot);
      strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), name);
      m_probing = true;
      const D3D11_BOX box = { 0, 0, 0, size, 1, 1 };
      CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buffer, 0, &box);
      bool written = false;
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (SUCCEEDED(Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        if (FILE* file = fopen(path, "wb")) {
          written = fwrite(mapped.pData, 1, size, file) == size;
          fclose(file);
        }
        Unmap(staging.Get(), 0);
      }
      m_probing = false;
      return written;
    }

    void STDMETHODCALLTYPE Dispatch(
        UINT ThreadGroupCountX,
        UINT ThreadGroupCountY,
        UINT ThreadGroupCountZ) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDispatch("Dispatch", ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
      const bool computePrepared = prepareComputePipeline();
      if (m_historyOn && !m_isDeferred)
        recordCall(RecordOp::Dispatch, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ, 0, !computePrepared);
      if (!computePrepared) return;
      flushPendingBufferUpdates();
      if (m_injectTaa)
        injectObserveDispatch();
      rebindComputeDispatchState();
      if (m_overrides.upscaler && m_overrides.upscalerAuto && !m_isDeferred)
        observeAutoTaa();
      if (m_overrides.upscaler && dispatchUpscaler(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ))
        return;
      m_backendCtx->dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }

    // Unreal Engine 4's temporal AA (TAAU) compute pass, as DXUP replaces it
    // with an upscaler: its scene color, depth, velocity and output slots,
    // and where its constants hold the view size and jitter (TAA parameters)
    // and ClipToPrevClip (View uniform buffer), in bytes from the bound range.
    struct UpscaleProfile {
      uint64_t shaderHash;
      UINT     colorSlot, depthSlot, velocitySlot, outputSlot;
      UINT     paramsBuffer, viewSizeOffset, jitterOffset;
      UINT     viewBuffer, clipToPrevClipOffset;
      // Profiles found by d3d11.upscalerAuto: the jitter's own buffer and
      // units, and a view size taken from the color texture.
      UINT     jitterBuffer = ~0u;
      float    jitterScale[2] = { 1.0f, 1.0f };
      bool     viewSizeFromTexture = false;
    };

    // d3d11.upscalerAuto: the dispatch's textures and, while the detector
    // samples, its constant buffers as the worker sees them.
    void observeAutoTaa() {
      if (!m_autoTaa)
        m_autoTaa = std::make_unique<AutoTaa>();
      auto* cs = static_cast<D3D11ComputeShader*>(m_state.cs.shader);
      IBackendShader* shader = cs ? cs->GetBackendShader() : nullptr;
      if (!shader)
        return;
      const uint64_t hash = shader->getBytecodeHash();
      if (m_autoTaa->state() == AutoTaa::State::Searching) {
        AutoTaa::TextureUse srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX];
        AutoTaa::TextureUse uavs[D3D11_1_UAV_SLOT_COUNT_MAX];
        size_t srvCount = 0, uavCount = 0;
        auto describe = [](ID3D11Resource* resource, DXGI_FORMAT format, UINT slot, AutoTaa::TextureUse& use) {
          D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
          if (!resource)
            return false;
          resource->GetType(&dim);
          if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            return false;
          D3D11_TEXTURE2D_DESC desc = {};
          static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
          use.resource = resource;
          use.viewFormat = format;
          use.width = desc.Width;
          use.height = desc.Height;
          use.slot = slot;
          use.depth = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
          return true;
        };
        const UINT srvEnd = (std::min)(m_state.cs.srvSlotEnd, UINT(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX));
        for (UINT slot = 0; slot < srvEnd; slot++) {
          auto* view = static_cast<D3D11ShaderResourceView*>(m_state.cs.srvs[slot]);
          if (!view)
            continue;
          D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
          view->GetDesc(&desc);
          if (describe(view->GetResourceRaw(), desc.Format, slot, srvs[srvCount]))
            srvCount++;
        }
        const UINT uavEnd = (std::min)(m_state.cs.uavSlotEnd, UINT(D3D11_1_UAV_SLOT_COUNT_MAX));
        for (UINT slot = 0; slot < uavEnd; slot++) {
          auto* view = static_cast<D3D11UnorderedAccessView*>(m_state.cs.uavs[slot]);
          if (!view)
            continue;
          D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
          view->GetDesc(&desc);
          if (describe(view->GetResourceRaw(), desc.Format, slot, uavs[uavCount]))
            uavCount++;
        }
        m_autoTaa->observeDispatch(hash, srvs, srvCount, uavs, uavCount);
      } else if (m_autoTaa->wantsSnapshot(hash)) {
        struct Range { d3d12::D3D12BackendBuffer* buffer; UINT offset, size; };
        std::vector<Range> ranges(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX);
        const UINT cbEnd = (std::min)(m_state.cs.cbSlotEnd, UINT(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX));
        for (UINT slot = 0; slot < cbEnd; slot++) {
          ID3D11Buffer* buffer = m_state.cs.constantBuffers[slot];
          ranges[slot] = { buffer ? static_cast<d3d12::D3D12BackendBuffer*>(getBackendBuffer(buffer)) : nullptr,
                           m_state.cs.constantBufferFirst[slot] * 16, m_state.cs.constantBufferCount[slot] * 16 };
        }
        auto sink = m_autoTaa->sink();
        const uint64_t frame = m_autoTaa->frameIndex();
        m_backendCtx->executeOnBackend([sink, frame, ranges](IBackendContext*) {
          taa_auto::FrameSnapshot snapshot(ranges.size());
          for (size_t slot = 0; slot < ranges.size(); slot++) {
            const Range& range = ranges[slot];
            if (!range.buffer)
              continue;
            const auto* data = static_cast<const uint8_t*>(range.buffer->getPersistentMappedData());
            const BufferDesc& desc = range.buffer->getDesc();
            const uint64_t dataSize = desc.dataSize ? desc.dataSize : desc.size;
            if (!data || range.offset >= dataSize)
              continue;
            uint64_t size = dataSize - range.offset;
            if (range.size)
              size = (std::min)(size, uint64_t(range.size));
            size = (std::min)(size, uint64_t(8192));
            snapshot[slot].assign(data + range.offset, data + range.offset + size);
          }
          std::lock_guard<std::mutex> lock(sink->mutex);
          sink->frames.push_back(std::move(snapshot));
          sink->frameIndex.push_back(frame);
        });
      }
    }

    // At each Present of the swap chain (application thread).
    void NoteFramePresented() {
      DXUP_CONTEXT_MT_GUARD;
      if (m_historyOn && !m_isDeferred) {
        m_historyFrame++;
        m_history[m_historyFrame % kHistoryFrames].clear();
      }
      if (m_autoTaa)
        m_autoTaa->endFrame();
      if (m_injectTaa) {
        if (m_injectTaa->atPresent())
          runInjectedPass();
        m_injectTaa->endFrame();
      }
    }

    // ---- Draw history (glitch reports) ----
    // With saved frames on (dxup_frame_dump.txt), each draw, dispatch and
    // clear of the immediate context leaves a compact record, and the last
    // frames' records stay. When the swap chain sees a frame flicker, it
    // logs the records of that frame against the frame before: draws only
    // one of them has, and draws whose shaders, targets, textures, constant
    // buffers or states changed; that names the draw behind the glitch.
    // Objects are named by their addresses, shaders by bytecode hash (as
    // the shader dumps name them).
    enum class RecordOp : uint8_t { Draw, DrawIndexed, DrawAuto, DrawIndirect, Dispatch, ClearColor, ClearDepth };

    struct DrawRecord {
      RecordOp    op = RecordOp::Draw;
      bool        dropped = false;   // the pipeline could not be prepared: not drawn
      bool        swapChainTarget = false;
      uint8_t     targets = 0;       // bound render targets
      uint32_t    count = 0, instances = 0, start = 0;
      int32_t     base = 0;
      uint64_t    vs = 0, ps = 0;    // compute shader in ps for dispatches
      const void* target = nullptr;  // render target 0 (or the cleared view)
      const void* depth = nullptr;   // depth-stencil view
      uint16_t    width = 0, height = 0, format = 0;
      const void* srv[4] = {};
      uint16_t    srvWidth[4] = {}, srvHeight[4] = {}, srvFormat[4] = {};
      const void* cb[2] = {};        // vertex and pixel (or compute) b0
      uint32_t    cbFirst[2] = {};
      const void* blend = nullptr;
      const void* depthState = nullptr;
      const void* raster = nullptr;
      uint32_t    stencilRef = 0;
      float       viewport[2] = {};
      float       value[4] = {};     // clear color, or depth, stencil, flags
    };

    static constexpr uint32_t kHistoryFrames = 8;
    std::vector<DrawRecord> m_history[kHistoryFrames];
    uint32_t                m_historyFrame = 0;
    bool                    m_historyOn = d3d11GlitchReportsEnabled();

    static void describeImageView(IBackendImageView* view, uint16_t& width, uint16_t& height, uint16_t& format) {
      if (!view || !view->getImage())
        return;
      const ImageDesc& desc = view->getImage()->getDesc();
      width = uint16_t((std::min)(desc.width, 65535u));
      height = uint16_t((std::min)(desc.height, 65535u));
      format = uint16_t(view->getDesc().format ? view->getDesc().format : desc.format);
    }

    template <typename Shader>
    static uint64_t shaderHash(ID3D11DeviceChild* shader) {
      IBackendShader* backend = shader ? reinterpret_cast<Shader*>(shader)->GetBackendShader() : nullptr;
      return backend ? backend->getBytecodeHash() : 0;
    }

    bool prepareRecorded(RecordOp op, uint32_t count, uint32_t instances, uint32_t start, int32_t base) {
      const bool prepared = preparePipeline();
      if (m_historyOn && !m_isDeferred)
        recordCall(op, count, instances, start, base, !prepared);
      return prepared;
    }

    void recordCall(RecordOp op, uint32_t count, uint32_t instances, uint32_t start, int32_t base, bool dropped) {
      DrawRecord r;
      r.op = op;
      r.dropped = dropped;
      r.count = count;
      r.instances = instances;
      r.start = start;
      r.base = base;
      const bool compute = op == RecordOp::Dispatch;
      const ShaderStageState& stage = compute ? m_state.cs : m_state.ps;
      if (compute) {
        r.ps = shaderHash<D3D11ComputeShader>(m_state.cs.shader);
      } else {
        r.vs = shaderHash<D3D11VertexShader>(m_state.vs.shader);
        r.ps = shaderHash<D3D11PixelShader>(m_state.ps.shader);
        r.targets = uint8_t(m_state.om.numRenderTargets);
        r.target = m_state.om.renderTargets[0];
        r.swapChainTarget = isSwapChainView(m_state.om.renderTargets[0]);
        r.depth = m_state.om.depthStencilView;
        describeImageView(getBackendRTV(m_state.om.renderTargets[0]), r.width, r.height, r.format);
        if (!r.width && m_state.om.depthStencilView)
          describeImageView(getBackendDSV(m_state.om.depthStencilView), r.width, r.height, r.format);
        r.cb[0] = m_state.vs.constantBuffers[0];
        r.cbFirst[0] = m_state.vs.constantBufferFirst[0];
        r.blend = m_state.om.blendState;
        r.depthState = m_state.om.depthStencilState;
        r.raster = m_state.rs.state;
        r.stencilRef = m_state.om.stencilRef;
        if (m_state.rs.numViewports) {
          r.viewport[0] = m_state.rs.viewports[0].Width;
          r.viewport[1] = m_state.rs.viewports[0].Height;
        }
      }
      r.cb[1] = stage.constantBuffers[0];
      r.cbFirst[1] = stage.constantBufferFirst[0];
      for (uint32_t slot = 0; slot < 4; slot++) {
        r.srv[slot] = stage.srvs[slot];
        describeImageView(getBackendImageSRV(stage.srvs[slot]), r.srvWidth[slot], r.srvHeight[slot], r.srvFormat[slot]);
      }
      m_history[m_historyFrame % kHistoryFrames].push_back(r);
    }

    void recordClear(RecordOp op, ID3D11View* view, IBackendImageView* backendView, const float value[4]) {
      DrawRecord r;
      r.op = op;
      r.target = view;
      r.swapChainTarget = isSwapChainView(view);
      describeImageView(backendView, r.width, r.height, r.format);
      std::memcpy(r.value, value, sizeof(r.value));
      m_history[m_historyFrame % kHistoryFrames].push_back(r);
    }

    // A view of a swap chain buffer (they carry kDxupSwapChainBufferGuid).
    static bool isSwapChainView(ID3D11View* view) {
      if (!view)
        return false;
      ComPtr<ID3D11Resource> resource;
      view->GetResource(resource.GetAddressOf());
      UINT marker = 0, size = sizeof(marker);
      return resource && SUCCEEDED(resource->GetPrivateData(kDxupSwapChainBufferGuid, &size, &marker)) && marker;
    }

    static std::string describeRecord(const DrawRecord& r) {
      static const char* kOps[] = { "Draw", "DrawIndexed", "DrawAuto", "DrawIndirect", "Dispatch", "ClearColor", "ClearDepth" };
      char text[640];
      if (r.op == RecordOp::ClearColor || r.op == RecordOp::ClearDepth) {
        sprintf_s(text, "%s %p %ux%u/%u value=%g,%g,%g,%g", kOps[uint32_t(r.op)], r.target, r.width, r.height, r.format,
          r.value[0], r.value[1], r.value[2], r.value[3]);
        return text;
      }
      int length = sprintf_s(text, "%s%s n=%u x%u start=%u base=%d %s=%016llX", kOps[uint32_t(r.op)],
        r.dropped ? " DROPPED" : "", r.count, r.instances, r.start, r.base, r.op == RecordOp::Dispatch ? "cs" : "ps",
        static_cast<unsigned long long>(r.ps));
      if (r.op != RecordOp::Dispatch)
        length += sprintf_s(text + length, sizeof(text) - length, " vs=%016llX rt=%p %ux%u/%u targets=%u ds=%p vp=%gx%g blend=%p dss=%p rs=%p ref=%u",
          static_cast<unsigned long long>(r.vs), r.target, r.width, r.height, r.format, r.targets, r.depth,
          r.viewport[0], r.viewport[1], r.blend, r.depthState, r.raster, r.stencilRef);
      for (uint32_t slot = 0; slot < 4; slot++) {
        if (r.srv[slot])
          length += sprintf_s(text + length, sizeof(text) - length, " t%u=%p %ux%u/%u", slot, r.srv[slot],
            r.srvWidth[slot], r.srvHeight[slot], r.srvFormat[slot]);
      }
      sprintf_s(text + length, sizeof(text) - length, " cb=%p+%u,%p+%u", r.cb[0], r.cbFirst[0], r.cb[1], r.cbFirst[1]);
      return text;
    }

    // The same call in two frames: kind, shaders and the target's size and
    // format (the swap chain's buffers take turns from frame to frame).
    static bool sameCall(const DrawRecord& a, const DrawRecord& b) {
      return a.op == b.op && a.vs == b.vs && a.ps == b.ps && a.width == b.width && a.height == b.height
          && a.format == b.format;
    }

    // What else differs between two records of the same call.
    static std::string recordChanges(const DrawRecord& before, const DrawRecord& now) {
      std::string changes;
      char text[160];
      if (before.target != now.target && !(before.swapChainTarget && now.swapChainTarget)) {
        sprintf_s(text, " rt %p -> %p", before.target, now.target);
        changes += text;
      }
      if (before.dropped != now.dropped)
        changes += now.dropped ? " now DROPPED" : " was dropped";
      if (before.count != now.count || before.instances != now.instances || before.start != now.start || before.base != now.base) {
        sprintf_s(text, " count %u x%u start %u base %d -> %u x%u start %u base %d", before.count, before.instances,
          before.start, before.base, now.count, now.instances, now.start, now.base);
        changes += text;
      }
      for (uint32_t slot = 0; slot < 4; slot++) {
        if (before.srv[slot] != now.srv[slot]) {
          sprintf_s(text, " t%u %p %ux%u/%u -> %p %ux%u/%u", slot, before.srv[slot], before.srvWidth[slot],
            before.srvHeight[slot], before.srvFormat[slot], now.srv[slot], now.srvWidth[slot], now.srvHeight[slot],
            now.srvFormat[slot]);
          changes += text;
        }
      }
      for (uint32_t stage = 0; stage < 2; stage++) {
        if (before.cb[stage] != now.cb[stage]) {
          sprintf_s(text, " cb%u %p -> %p", stage, before.cb[stage], now.cb[stage]);
          changes += text;
        }
      }
      if (before.depth != now.depth) {
        sprintf_s(text, " ds %p -> %p", before.depth, now.depth);
        changes += text;
      }
      if (before.blend != now.blend || before.depthState != now.depthState || before.raster != now.raster
       || before.stencilRef != now.stencilRef) {
        sprintf_s(text, " states %p,%p,%p,%u -> %p,%p,%p,%u", before.blend, before.depthState, before.raster,
          before.stencilRef, now.blend, now.depthState, now.raster, now.stencilRef);
        changes += text;
      }
      if (std::memcmp(before.value, now.value, sizeof(now.value)) != 0) {
        sprintf_s(text, " value %g,%g,%g,%g -> %g,%g,%g,%g", before.value[0], before.value[1], before.value[2],
          before.value[3], now.value[0], now.value[1], now.value[2], now.value[3]);
        changes += text;
      }
      return changes;
    }

  public:
    // The number of calls of the frame presented `back` presents ago.
    size_t HistoryCalls(uint32_t back) const {
      if (!m_historyOn || m_isDeferred || back >= kHistoryFrames || m_historyFrame < back)
        return SIZE_MAX;
      return m_history[(m_historyFrame - back) % kHistoryFrames].size();
    }

    // The calls of the frame presented `back` presents ago (0: the frame
    // being presented) against the frame before it, into the log (glitch
    // `event` of the swap chain, at its present `present`). With `full`,
    // also every call of the glitch frame.
    void LogDrawHistory(uint32_t back, uint32_t present, uint32_t event, bool full) {
      if (!m_historyOn || m_isDeferred || back + 1 >= kHistoryFrames || m_historyFrame < back + 1)
        return;
      const auto& now = m_history[(m_historyFrame - back) % kHistoryFrames];
      const auto& before = m_history[(m_historyFrame - back - 1) % kHistoryFrames];
      uint32_t dropped = 0;
      for (const auto& r : now)
        dropped += r.dropped ? 1u : 0u;
      logMessage(LogLevel::Warn, "glitch %u: the frame before (present %u) has %zu calls, the glitch frame (present %u) %zu, %u of them dropped; differences:",
        event, present - 1, before.size(), present, now.size(), dropped);
      // Calls match in order by kind, shaders and target; a call with no
      // match among the next 64 is in one frame only.
      constexpr size_t kWindow = 64;
      uint32_t lines = 0, suppressed = 0;
      auto line = [&](const char* what, size_t index, const std::string& text) {
        if (lines++ < 200)
          logMessage(LogLevel::Warn, "glitch %u:   %s #%zu %s", event, what, index, text.c_str());
        else
          suppressed++;
      };
      size_t i = 0, j = 0;
      while (i < before.size() || j < now.size()) {
        if (i < before.size() && j < now.size() && sameCall(before[i], now[j])) {
          const std::string changes = recordChanges(before[i], now[j]);
          if (!changes.empty())
            line("changed", j, describeRecord(now[j]) + " |" + changes);
          i++, j++;
          continue;
        }
        size_t aheadNow = j, aheadBefore = i;
        while (aheadNow < now.size() && aheadNow < j + kWindow && !(i < before.size() && sameCall(before[i], now[aheadNow])))
          aheadNow++;
        if (i < before.size() && aheadNow < now.size() && aheadNow < j + kWindow) {
          for (; j < aheadNow; j++)
            line("only in the glitch frame", j, describeRecord(now[j]));
          continue;
        }
        while (aheadBefore < before.size() && aheadBefore < i + kWindow && !(j < now.size() && sameCall(before[aheadBefore], now[j])))
          aheadBefore++;
        if (j < now.size() && aheadBefore < before.size() && aheadBefore < i + kWindow) {
          for (; i < aheadBefore; i++)
            line("only in the frame before", i, describeRecord(before[i]));
          continue;
        }
        if (i < before.size())
          line("only in the frame before", i, describeRecord(before[i])), i++;
        if (j < now.size())
          line("only in the glitch frame", j, describeRecord(now[j])), j++;
      }
      if (suppressed)
        logMessage(LogLevel::Warn, "glitch %u:   %u more differences", event, suppressed);
      if (!lines)
        logMessage(LogLevel::Warn, "glitch %u:   none: the same calls with the same objects (the glitch comes from the data or the GPU)", event);
      if (full) {
        for (size_t index = 0; index < now.size() && index < 4000; index++)
          logMessage(LogLevel::Warn, "glitch %u: call #%zu %s", event, index, describeRecord(now[index]).c_str());
      }
    }

    // d3d11.upscalerInject: a draw is seen before it is recorded. Scene
    // draws get the jittered viewport and hand the camera constants over
    // once a frame; other draws may be where the upscaler goes in.
    void injectObserveDraw() {
      if (!m_injectTaa) {
        m_injectTaa = std::make_unique<InjectTaa>();
        logMessage(LogLevel::Warn, "upscalerInject: looking for the scene");
      }
      if (m_injectTaa->state() == InjectTaa::State::Failed) {
        if (m_injectViewportJittered)
          setInjectJitter(false);
        return;
      }
      auto* dsvView = static_cast<D3D11DepthStencilView*>(m_state.om.depthStencilView);
      auto* rtv = static_cast<D3D11RenderTargetView*>(m_state.om.renderTargets[0]);
      D3D11_DEPTH_STENCIL_DESC ds = {};
      ds.DepthEnable = TRUE;
      ds.DepthFunc = D3D11_COMPARISON_LESS;
      if (m_state.om.depthStencilState)
        m_state.om.depthStencilState->GetDesc(&ds);
      const bool greater = ds.DepthFunc == D3D11_COMPARISON_GREATER || ds.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
      const bool scene = m_injectTaa->observeDraw(dsvView ? dsvView->GetResourceRaw() : nullptr,
        rtv ? rtv->GetResourceRaw() : nullptr, ds.DepthEnable != FALSE, greater);
      if (scene) {
        m_injectTaa->noteSceneDraw();
        // Scene-depth draws after the upscaler (interface) are not jittered.
        const bool jitter = (m_injectTaa->state() == InjectTaa::State::Active || m_injectTaa->stillUpscaling())
          && !m_injectTaa->inserted() && m_injectReady->load(std::memory_order_relaxed);
        if (jitter != m_injectViewportJittered || (jitter && m_injectJitterFrame != m_injectTaa->frame()))
          setInjectJitter(jitter);
        if (const uint32_t wanted = m_injectTaa->cameraSlotsWanted())
          snapshotInjectCameras(wanted);
        return;
      }
      if (m_injectViewportJittered)
        setInjectJitter(false);
      uint64_t hash = 0;
      if (auto* ps = static_cast<D3D11PixelShader*>(m_state.ps.shader))
        hash = ps->GetBackendShader() ? ps->GetBackendShader()->getBytecodeHash() : 0;
      else if (auto* vs = static_cast<D3D11VertexShader*>(m_state.vs.shader))
        hash = vs->GetBackendShader() ? vs->GetBackendShader()->getBytecodeHash() : 0;
      if (m_injectTaa->wantsColorReads() && readsResource(m_state.ps, m_injectTaa->color()))
        m_injectTaa->noteColorRead(hash);
      if (m_injectTaa->beforeOtherEvent(hash))
        runInjectedPass();
    }

    template<typename Stage>   // ShaderStageState, declared further down
    static bool readsResource(const Stage& stage, ID3D11Resource* resource) {
      const UINT end = (std::min)(stage.srvSlotEnd, UINT(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX));
      for (UINT slot = 0; slot < end; slot++) {
        if (stage.srvs[slot] && static_cast<D3D11ShaderResourceView*>(stage.srvs[slot])->GetResourceRaw() == resource)
          return true;
      }
      return false;
    }

    void injectObserveDispatch() {
      if (m_injectTaa->state() == InjectTaa::State::Failed)
        return;
      auto* cs = static_cast<D3D11ComputeShader*>(m_state.cs.shader);
      const uint64_t hash = cs && cs->GetBackendShader() ? cs->GetBackendShader()->getBytecodeHash() : 0;
      if (m_injectTaa->wantsColorReads() && readsResource(m_state.cs, m_injectTaa->color()))
        m_injectTaa->noteColorRead(hash);
      if (m_injectTaa->beforeOtherEvent(hash))
        runInjectedPass();
    }

    // The bound viewports, shifted by this frame's jitter or not.
    void setInjectJitter(bool on) {
      m_injectViewportJittered = on;
      m_injectJitterFrame = m_injectTaa ? m_injectTaa->frame() : 0;
      if (!m_state.rs.numViewports)
        return;
      float jitter[2] = {};
      if (on)
        m_injectTaa->jitter(jitter);
      Viewport viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX];
      for (UINT i = 0; i < m_state.rs.numViewports; i++) {
        const auto& viewport = m_state.rs.viewports[i];
        viewports[i].x        = viewport.TopLeftX + jitter[0];
        viewports[i].y        = viewport.TopLeftY + jitter[1];
        viewports[i].width    = viewport.Width;
        viewports[i].height   = viewport.Height;
        viewports[i].minDepth = viewport.MinDepth;
        viewports[i].maxDepth = viewport.MaxDepth;
      }
      m_backendCtx->setViewports(m_state.rs.numViewports, viewports);
    }

    // The vertex shader constant buffers as the frame's first scene draw
    // that binds each sees them, from the application thread's copy: the
    // worker may come to the draw after the next frame's camera was written.
    void snapshotInjectCameras(uint32_t wanted) {
      for (UINT slot = 0; slot < taa_inject::CameraStore::kSlots; slot++) {
        if ((wanted & (1u << slot)) && m_state.vs.constantBuffers[slot] && snapshotInjectCamera(slot))
          m_injectTaa->cameraSlotTaken(slot);
      }
    }
    bool snapshotInjectCamera(UINT slot) {
      auto* buffer = static_cast<D3D11Buffer*>(m_state.vs.constantBuffers[slot]);
      if (!buffer)
        return false;
      const uint8_t* data = buffer->GetShadowData();
      uint64_t dataSize = buffer->GetShadowSize();
      if (!data) {
        auto* backendBuffer = static_cast<d3d12::D3D12BackendBuffer*>(getBackendBuffer(buffer));
        data = backendBuffer ? static_cast<const uint8_t*>(backendBuffer->getPersistentMappedData()) : nullptr;
        if (data) {
          const BufferDesc& desc = backendBuffer->getDesc();
          dataSize = desc.dataSize ? desc.dataSize : desc.size;
        }
      }
      const UINT offset = m_state.vs.constantBufferFirst[slot] * 16;
      const UINT count = m_state.vs.constantBufferCount[slot] * 16;
      if (!data || offset >= dataSize)
        return false;
      uint64_t size = (std::min)(dataSize - offset, uint64_t(4096));
      if (count)
        size = (std::min)(size, uint64_t(count));
      m_injectTaa->cameras()->put(slot, m_injectTaa->frame(), std::vector<uint8_t>(data + offset, data + offset + size));
      return true;
    }

    // The upscaler (or, while calibrating, the camera measurement) on the
    // scene color as it is now.
    void runInjectedPass() {
      m_injectTaa->markInserted();
      if (m_injectViewportJittered)
        setInjectJitter(false);
      ID3D11Resource* color = m_injectTaa->color();
      ID3D11Resource* depth = m_injectTaa->depth();
      readForcedMsaa(color);
      readForcedMsaa(depth, false);
      auto* colorImage = static_cast<d3d12::D3D12BackendImage*>(getBackendImage(color));
      auto* depthImage = static_cast<d3d12::D3D12BackendImage*>(getBackendImage(depth));
      if (!colorImage || !depthImage)
        return;
      auto cameras = m_injectTaa->cameras();
      const uint64_t frame = m_injectTaa->frame();
      // A typed view of the color (typeless textures read as their usual type).
      DXGI_FORMAT colorFormat = colorImage->getDesc().format;
      switch (colorFormat) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: colorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  colorFormat = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: colorFormat = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        default: break;
      }
      d3d12::UpscaleRequest request;
      request.kind = static_cast<upscale::Kind>(m_overrides.upscaler);
      request.depth = depthImage;
      request.velocityMode = 2;
      request.directJitter = true;
      m_injectTaa->jitter(request.jitterValue);
      request.directClip = true;
      request.jitterScale[0] = m_overrides.upscalerJitterScale[0];
      request.jitterScale[1] = m_overrides.upscalerJitterScale[1];
      request.sharpness = m_overrides.upscalerSharpness;
      request.hdr = colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || colorFormat == DXGI_FORMAT_R11G11B10_FLOAT
                 || colorFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
      request.depthInverted = m_injectTaa->depthInverted();
      request.reset = m_injectTaa->takeHistoryReset();
      if (m_injectTaa->stillUpscaling()) {
        // The camera stands still: no motion. Every eighth frame the image
        // is compared with the one eight frames before.
        std::memset(request.clipRows, 0, sizeof(request.clipRows));
        request.clipRows[0] = request.clipRows[5] = request.clipRows[10] = request.clipRows[15] = 1.0f;
        // The camera moved in this frame: no history (no smear of the still
        // frames); the motion is measured from the next frame on.
        if (m_injectTaa->resetOnCameraChange() && taa_inject::cameraBuffersMoved(*cameras, frame) == 1)
          request.reset = true;
        auto ready = m_injectReady;
        auto probe = m_injectTaa->stillProbe();
        const bool probeFrame = m_injectTaa->stillProbeFrame();
        m_backendCtx->executeOnBackend([colorImage, colorFormat, depthImage, request, ready, probe, probeFrame](IBackendContext* backend) {
          auto* d3d12Backend = static_cast<d3d12::D3D12BackendContext*>(backend);
          if (probeFrame && probe)
            d3d12Backend->calibrateCamera(colorImage, colorFormat, depthImage, DXGI_FORMAT_UNKNOWN, 0, nullptr, probe, 0);
          ready->store(d3d12Backend->runInjectedUpscaler(colorImage, request), std::memory_order_relaxed);
        });
        writeForcedMsaaImage(color, true);
        return;
      }
      if (m_injectTaa->state() == InjectTaa::State::Calibrating) {
        // The upscaler is made while the camera is measured.
        if (m_injectPreparedFor != color) {
          m_injectPreparedFor = color;
          d3d12::UpscaleRequest prepare = request;
          prepare.prepareOnly = true;
          m_backendCtx->executeOnBackend([colorImage, prepare](IBackendContext* backend) {
            static_cast<d3d12::D3D12BackendContext*>(backend)->runInjectedUpscaler(colorImage, prepare);
          });
        }
        auto candidates = m_injectTaa->calibrationCandidates();
        if (candidates.empty())
          return;
        auto calibration = m_injectTaa->calibration();
        const uint32_t generation = calibration->generation;   // written on this thread only
        m_backendCtx->executeOnBackend([colorImage, colorFormat, depthImage, cameras, frame, candidates, calibration, generation](IBackendContext* backend) {
          std::vector<uint8_t> current, previous;
          float rows[d3d12::kCameraCandidates][16] = {};
          const size_t count = (std::min)(candidates.size(), size_t(d3d12::kCameraCandidates));
          for (size_t k = 0; k < count; k++) {
            if (!cameras->pair(candidates[k].slot, frame, current, previous)
             || !taa_inject::clipToPrevClip(current, previous, candidates[k].offset, candidates[k].layout, rows[k])) {
              std::memset(rows[k], 0, sizeof(rows[k]));
              rows[k][0] = rows[k][5] = rows[k][10] = rows[k][15] = 1.0f;
            }
          }
          static_cast<d3d12::D3D12BackendContext*>(backend)->calibrateCamera(colorImage, colorFormat, depthImage,
            DXGI_FORMAT_UNKNOWN, uint32_t(count), rows, calibration, generation);
        });
        return;
      }
      const taa_inject::Candidate camera = m_injectTaa->camera();
      auto ready = m_injectReady;
      m_backendCtx->executeOnBackend([colorImage, request, cameras, frame, camera, ready](IBackendContext* backend) mutable {
        std::vector<uint8_t> current, previous;
        if (!cameras->pair(camera.slot, frame, current, previous)
         || !taa_inject::clipToPrevClip(current, previous, camera.offset, camera.layout, request.clipRows)) {
          std::memset(request.clipRows, 0, sizeof(request.clipRows));
          request.clipRows[0] = request.clipRows[5] = request.clipRows[10] = request.clipRows[15] = 1.0f;
        }
        ready->store(static_cast<d3d12::D3D12BackendContext*>(backend)->runInjectedUpscaler(colorImage, request),
          std::memory_order_relaxed);
      });
      // The image holds the result now, all of it.
      writeForcedMsaaImage(color, true);
    }

    static const UpscaleProfile* findUpscaleProfile(uint64_t hash, uint64_t userHash) {
      // Kena: Bridge of Spirits (UE 4.26, TAAU at 67 % screen percentage).
      static const UpscaleProfile kKena = { 0xf9954c1d5d2d2546ull, 1, 2, 3, 0, 0, 27 * 16, 28 * 16, 1, 122 * 16 };
      if (hash == kKena.shaderHash)
        return &kKena;
      // A shader the user names gets the same layout.
      static UpscaleProfile user = kKena;
      if (userHash && hash == userHash) {
        user.shaderHash = userHash;
        return &user;
      }
      return nullptr;
    }

    // Records the upscaler in place of the dispatch when the bound compute
    // shader is a known TAA pass. The worker runs the game's dispatch
    // instead when the upscaler cannot run.
    bool dispatchUpscaler(UINT x, UINT y, UINT z) {
      if (m_isDeferred)
        return false;
      auto* cs = static_cast<D3D11ComputeShader*>(m_state.cs.shader);
      IBackendShader* shader = cs ? cs->GetBackendShader() : nullptr;
      const uint64_t hash = shader ? shader->getBytecodeHash() : 0;
      const UpscaleProfile* profile = shader && m_overrides.upscalerBuiltin
        ? findUpscaleProfile(hash, m_overrides.upscalerShader) : nullptr;
      UpscaleProfile found = {};
      uint32_t velocityMode = 0, velocityCandidate = 0;
      const bool calibrating = !profile && m_autoTaa && m_autoTaa->state() == AutoTaa::State::Calibrating
        && m_autoTaa->candidateProfile().shaderHash == hash;
      if (!profile && m_autoTaa && (calibrating || (m_autoTaa->profile() && m_autoTaa->profile()->shaderHash == hash))) {
        const AutoTaa::Profile& p = calibrating ? m_autoTaa->candidateProfile() : *m_autoTaa->profile();
        velocityMode = p.velocityMode;
        velocityCandidate = p.velocityCandidate;
        found = { p.shaderHash, p.colorSlot, p.depthSlot, p.velocitySlot, p.outputSlot,
                  p.viewSizeBuffer, p.viewSizeOffset, p.jitterOffset, p.clipBuffer, p.clipOffset };
        found.jitterBuffer = p.jitterBuffer;
        found.jitterScale[0] = p.jitterScale[0];
        found.jitterScale[1] = p.jitterScale[1];
        found.viewSizeFromTexture = p.viewSizeFromTexture;
        profile = &found;
        m_autoTaa->noteProfileUsed();
      }
      if (!profile)
        return false;

      auto image = [](ID3D11View* view, DXGI_FORMAT* format) -> d3d12::D3D12BackendImage* {
        if (!view)
          return nullptr;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.GetAddressOf());
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (resource)
          resource->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          return nullptr;
        if (format) {
          D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
          static_cast<ID3D11ShaderResourceView*>(view)->GetDesc(&desc);
          *format = desc.Format;
        }
        return static_cast<d3d12::D3D12BackendImage*>(getBackendImage(resource.Get()));
      };
      auto constant = [this](UINT slot, UINT offset) {
        d3d12::UpscaleRequest::Constant result;
        if (ID3D11Buffer* buffer = m_state.cs.constantBuffers[slot]) {
          result.buffer = static_cast<d3d12::D3D12BackendBuffer*>(getBackendBuffer(buffer));
          result.offset = m_state.cs.constantBufferFirst[slot] * 16 + offset;
        }
        return result;
      };

      d3d12::UpscaleRequest request;
      request.kind = static_cast<upscale::Kind>(m_overrides.upscaler);
      request.color = image(m_state.cs.srvs[profile->colorSlot], nullptr);
      request.depth = image(m_state.cs.srvs[profile->depthSlot], &request.depthFormat);
      request.velocity = image(m_state.cs.srvs[profile->velocitySlot], &request.velocityFormat);
      request.output = image(m_state.cs.uavs[profile->outputSlot], nullptr);
      if (!profile->viewSizeFromTexture)
        request.viewSize = constant(profile->paramsBuffer, profile->viewSizeOffset);
      request.jitter = constant(profile->jitterBuffer != ~0u ? profile->jitterBuffer : profile->paramsBuffer,
        profile->jitterOffset);
      request.clipToPrevClip = constant(profile->viewBuffer, profile->clipToPrevClipOffset);
      request.jitterScale[0] = m_overrides.upscalerJitterScale[0] * profile->jitterScale[0];
      request.jitterScale[1] = m_overrides.upscalerJitterScale[1] * profile->jitterScale[1];
      request.sharpness = m_overrides.upscalerSharpness;
      request.velocityMode = velocityMode;
      request.velocityCandidate = velocityCandidate;
      if (calibrating) {
        // The game's pass runs; its color and velocity are measured.
        DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
        image(m_state.cs.srvs[profile->colorSlot], &colorFormat);
        if (!request.color || !request.velocity || colorFormat == DXGI_FORMAT_UNKNOWN)
          return false;
        auto calibration = m_autoTaa->calibration();
        m_backendCtx->executeOnBackend([request, colorFormat, calibration](IBackendContext* backend) {
          auto* context = static_cast<d3d12::D3D12BackendContext*>(backend);
          float viewSize[2] = { float(request.color->getDesc().width), float(request.color->getDesc().height) };
          if (request.viewSize.buffer) {
            const auto* data = static_cast<const uint8_t*>(request.viewSize.buffer->getPersistentMappedData());
            const BufferDesc& desc = request.viewSize.buffer->getDesc();
            const uint64_t size = desc.dataSize ? desc.dataSize : desc.size;
            if (!data || request.viewSize.offset + sizeof(viewSize) > size)
              return;
            std::memcpy(viewSize, data + request.viewSize.offset, sizeof(viewSize));
          }
          context->calibrateVelocity(request.color, colorFormat, request.velocity, request.velocityFormat,
            (std::min)(viewSize[0], float(request.color->getDesc().width)),
            (std::min)(viewSize[1], float(request.color->getDesc().height)), calibration);
        });
        return false;
      }
      if (!request.color || !request.depth || !request.velocity || !request.output
       || (!profile->viewSizeFromTexture && !request.viewSize.buffer) || !request.jitter.buffer
       || (velocityMode == 0 && !request.clipToPrevClip.buffer))
        return false;

      m_backendCtx->executeOnBackend([request, x, y, z](IBackendContext* backend) {
        auto* context = static_cast<d3d12::D3D12BackendContext*>(backend);
        if (!context->runUpscaler(request))
          context->dispatch(x, y, z);
      });
      return true;
    }

    void STDMETHODCALLTYPE DispatchIndirect(
        ID3D11Buffer* pBufferForArgs,
        UINT          AlignedByteOffsetForArgs) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceDispatch(("DispatchIndirect" + traceIndirectArgs(pBufferForArgs, AlignedByteOffsetForArgs, 3)).c_str());
      if (!pBufferForArgs) return;
      const bool computePrepared = prepareComputePipeline();
      if (m_historyOn && !m_isDeferred)
        recordCall(RecordOp::Dispatch, 0, 0, AlignedByteOffsetForArgs, 0, !computePrepared);
      if (!computePrepared) return;
      flushPendingBufferUpdates();
      rebindComputeDispatchState();
      IBackendBuffer* backendBuf = getBackendBuffer(pBufferForArgs);
      if (backendBuf)
        m_backendCtx->dispatchIndirect(backendBuf, AlignedByteOffsetForArgs);
    }

    // ============================================================
    // ID3D11DeviceContext - Resource Operations
    // ============================================================

    void STDMETHODCALLTYPE UpdateSubresource(
        ID3D11Resource* pDstResource,
        UINT            DstSubresource,
        const D3D11_BOX* pDstBox,
        const void*     pSrcData,
        UINT            SrcRowPitch,
        UINT            SrcDepthPitch) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pDstResource || !pSrcData) return;

      D3D11_RESOURCE_DIMENSION dim;
      pDstResource->GetType(&dim);
      if (!isUpdatableResource(pDstResource, dim))
        return;
      writeForcedMsaaImage(pDstResource, !pDstBox);

      if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        ID3D11Buffer* buf = static_cast<ID3D11Buffer*>(pDstResource);
        auto* trackedBuffer = static_cast<D3D11Buffer*>(buf);
        IBackendBuffer* backendBuf = getBackendBuffer(trackedBuffer);
        if (backendBuf) {
          const auto& bufferDesc = trackedBuffer->GetDescRef();
          uint64_t offset = pDstBox ? pDstBox->left : 0;
          uint64_t size   = pDstBox ? (pDstBox->right - pDstBox->left) : 0;
          // The application's data covers the D3D11 buffer; backend
          // constant buffers are padded beyond it.
          if (!pDstBox) {
            size = bufferDesc.ByteWidth;
          }
          // Empty ranges and ranges past the buffer do nothing, as in D3D11.
          if (pDstBox && (pDstBox->right <= pDstBox->left || pDstBox->right > bufferDesc.ByteWidth))
            return;
          bool renamed = false;
          bool queued = false;
          const UINT srvUavFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
          const bool fullBufferUpdate = offset == 0 && size == bufferDesc.ByteWidth;
          const bool renameEnabled = renameSrvUavUpdatesEnabled();
          const bool srvUavDefaultBuffer = bufferDesc.Usage == D3D11_USAGE_DEFAULT
                                        && bufferDesc.CPUAccessFlags == 0
                                        && (bufferDesc.BindFlags & srvUavFlags);
          if (renameEnabled
           && fullBufferUpdate
           && srvUavDefaultBuffer) {
            if (auto* backendDevice = m_device->GetBackendDevice()) {
              auto renamedBuffer = backendDevice->createBuffer(&bufferDesc, nullptr);
              if (renamedBuffer) {
                IBackendBuffer* newBackendBuffer = renamedBuffer.get();
                m_backendCtx->updateBuffer(newBackendBuffer, 0, size, pSrcData);
                if (m_isDeferred)
                  m_recordedDiscards.emplace_back(trackedBuffer, renamedBuffer);

                auto oldBackendBuffer = trackedBuffer->ReplaceBackendBuffer(std::move(renamedBuffer), m_backendCtx.get());
                void* oldBackendPtr = oldBackendBuffer.get();
                if (oldBackendBuffer)
                  m_backendCtx->retainTemporaryBuffer(std::move(oldBackendBuffer));
                rebindRenamedInputAssemblerBuffer(trackedBuffer);
                rebindBufferViewsForResource(pDstResource);

                static uint32_t s_srvUavRenameLogCount = 0;
                if (s_srvUavRenameLogCount < 64u) {
                  logMessage(
                    LogLevel::Info,
                    "UpdateSubresource-SRVUAV-rename: buf=%p oldBackend=%p newBackend=%p size=%u bind=0x%x",
                    trackedBuffer,
                    oldBackendPtr,
                    newBackendBuffer,
                    bufferDesc.ByteWidth,
                    bufferDesc.BindFlags);
                  s_srvUavRenameLogCount++;
                }
                renamed = true;
              }
            }
          }
          if (!renamed
           && batchSrvUavUpdatesEnabled()
           && !fullBufferUpdate
           && srvUavDefaultBuffer) {
            queued = queuePendingBufferUpdate(trackedBuffer, offset, size, pSrcData);
          }
          if (renameEnabled && !renamed && !queued && (bufferDesc.BindFlags & srvUavFlags)) {
            static uint32_t s_srvUavRenameSkipLogCount = 0;
            if (s_srvUavRenameSkipLogCount < 64u) {
              logMessage(
                LogLevel::Info,
                "UpdateSubresource-SRVUAV-skip: buf=%p box=%u off=%llu size=%llu byteWidth=%u usage=%u cpu=0x%x bind=0x%x full=%u",
                trackedBuffer,
                pDstBox ? 1u : 0u,
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size),
                bufferDesc.ByteWidth,
                static_cast<uint32_t>(bufferDesc.Usage),
                bufferDesc.CPUAccessFlags,
                bufferDesc.BindFlags,
                fullBufferUpdate ? 1u : 0u);
              s_srvUavRenameSkipLogCount++;
            }
          }
          if (!renamed && !queued)
            m_backendCtx->updateBuffer(backendBuf, offset, size, pSrcData);
          trackedBuffer->UpdateShadowData(offset, pSrcData, size);
        }
      } else {
        // Texture subresource update
        IBackendImage* backendImg = getBackendImage(pDstResource);
        if (!backendImg) return;

        logTrackedImageMutation(
          "UpdateSubresource-tracked",
          pDstResource,
          backendImg,
          DstSubresource,
          pDstBox,
          SrcRowPitch,
          SrcDepthPitch,
          pSrcData);

        const auto& imgDesc = backendImg->getDesc();
        uint32_t mipLevel   = DstSubresource % imgDesc.mipLevels;
        uint32_t arrayLayer  = DstSubresource / imgDesc.mipLevels;
        const uint32_t layers = imgDesc.type == ImageType::Tex3D ? 1u : (std::max)(imgDesc.arraySize, 1u);
        if (arrayLayer >= layers)
          return;

        uint32_t width  = (imgDesc.width  >> mipLevel) > 0 ? (imgDesc.width  >> mipLevel) : 1;
        uint32_t height = (imgDesc.height >> mipLevel) > 0 ? (imgDesc.height >> mipLevel) : 1;
        uint32_t depth  = (imgDesc.depth  >> mipLevel) > 0 ? (imgDesc.depth  >> mipLevel) : 1;

        if (pDstBox) {
          // Empty boxes and boxes past the subresource do nothing, as in
          // D3D11; D3D12 would reject the command list. Block formats
          // address whole blocks: a mip smaller than a block takes one.
          const uint32_t block = getFormatBlockSize(imgDesc.format);
          const uint32_t blockWidth = (width + block - 1) / block * block;
          const uint32_t blockHeight = (height + block - 1) / block * block;
          if (pDstBox->right <= pDstBox->left || pDstBox->bottom <= pDstBox->top || pDstBox->back <= pDstBox->front
           || pDstBox->right > blockWidth || pDstBox->bottom > blockHeight || pDstBox->back > depth)
            return;
          width  = pDstBox->right  - pDstBox->left;
          height = pDstBox->bottom - pDstBox->top;
          depth  = pDstBox->back   - pDstBox->front;
        }

        auto* backend = m_device->GetBackend();
        if (!backend) return;

        uint32_t copyRowBytes = computeRowPitch(width, imgDesc.format);
        uint32_t numRows      = computeNumRows(height, imgDesc.format);
        UploadLayout uploadLayout = getImageUploadLayout(
          m_device->GetBackendType(),
          backend,
          backendImg,
          DstSubresource,
          copyRowBytes,
          numRows,
          depth,
          pDstBox != nullptr);
        uint64_t slicePitch = static_cast<uint64_t>(uploadLayout.rowPitch) * uploadLayout.numRows;
        uint64_t totalSize  = uploadLayout.totalSize;
        // D3D11 reads the pitches only to step between rows and slices: a
        // single row (1D textures) or slice needs none.
        if (uploadLayout.numRows <= 1 && SrcRowPitch < uploadLayout.copyRowBytes)
          SrcRowPitch = uploadLayout.copyRowBytes;
        uint64_t srcDepthPitch = SrcDepthPitch && uploadLayout.depth > 1
          ? SrcDepthPitch : uint64_t(SrcRowPitch) * uploadLayout.numRows;

        BufferDesc uploadDesc = {};
        uploadDesc.size      = uploadSizeClass(totalSize);
        uploadDesc.usage     = ResourceUsage::Dynamic;
        uploadDesc.cpuAccess = CpuAccess::Write;
        auto uploadBuf = backend->createBuffer(uploadDesc);
        if (uploadBuf) {
          void* ptr = m_backendCtx->mapBuffer(uploadBuf.get(), MapMode::Write);
          if (ptr) {
            if (depth > 1 && srcDepthPitch < uint64_t(SrcRowPitch) * uploadLayout.numRows) {
              logMessage(LogLevel::Error,
                "UpdateSubresource depth pitch too small: fmt=%d %ux%ux%u sub=%u srcDepthPitch=%u required=%llu",
                imgDesc.format,
                width,
                height,
                depth,
                DstSubresource,
                SrcDepthPitch,
                static_cast<unsigned long long>(uint64_t(SrcRowPitch) * uploadLayout.numRows));
              m_backendCtx->unmapBuffer(uploadBuf.get());
              return;
            }

            auto* dst = static_cast<uint8_t*>(ptr);
            auto* src = static_cast<const uint8_t*>(pSrcData);
            for (uint32_t z = 0; z < uploadLayout.depth; z++) {
              auto* sliceSrc = src + z * srcDepthPitch;
              auto* sliceDst = dst + z * slicePitch;
              for (uint32_t row = 0; row < uploadLayout.numRows; row++) {
                copyUploadRow(sliceDst + row * uploadLayout.rowPitch, sliceSrc + row * SrcRowPitch,
                  uploadLayout.copyRowBytes, SrcRowPitch, imgDesc.format);
              }
            }
            m_backendCtx->unmapBuffer(uploadBuf.get());
            ImageSubresource subres = {};
            subres.mipLevel   = mipLevel;
            subres.arrayLayer = arrayLayer;

            if (pDstBox) {
              ImageCopyRegion region = {};
              region.x      = pDstBox->left;
              region.y      = pDstBox->top;
              region.z      = pDstBox->front;
              region.width  = width;
              region.height = height;
              region.depth  = depth;
              m_backendCtx->copyBufferToImage(backendImg, subres, uploadBuf.get(), 0, &region);
            } else {
              m_backendCtx->copyBufferToImage(backendImg, subres, uploadBuf.get(), 0);
            }

            m_backendCtx->retainTemporaryBuffer(uploadBuf);
          }
        }
      }
    }

    void STDMETHODCALLTYPE CopyResource(
        ID3D11Resource* pDstResource,
        ID3D11Resource* pSrcResource) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("CopyResource dst=%p src=%p", static_cast<void*>(pDstResource), static_cast<void*>(pSrcResource)));
      if (!pDstResource || !pSrcResource) return;
      flushPendingBufferUpdates();
      if (m_autoTaa)
        m_autoTaa->noteCopy(pDstResource, pSrcResource);

      D3D11_RESOURCE_DIMENSION dstDim, srcDim;
      pDstResource->GetType(&dstDim);
      pSrcResource->GetType(&srcDim);

      if (dstDim != srcDim) return;

      if (dstDim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto* dstBuffer = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(pDstResource));
        auto* srcBuffer = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(pSrcResource));
        IBackendBuffer* dstBuf = getBackendBuffer(dstBuffer);
        IBackendBuffer* srcBuf = getBackendBuffer(srcBuffer);
        // D3D11 copies only buffers of the same size and drops the call
        // otherwise.
        if (dstBuf && srcBuf && dstBuffer->GetDescRef().ByteWidth == srcBuffer->GetDescRef().ByteWidth) {
          uint64_t size = srcBuffer->GetDescRef().ByteWidth;
          m_backendCtx->copyBuffer(dstBuf, 0, srcBuf, 0, size);
          dstBuffer->CopyShadowDataFrom(srcBuffer, 0u, 0u, size);
        }
      } else {
        IBackendImage* dstImg = getBackendImage(pDstResource);
        IBackendImage* srcImg = getBackendImage(pSrcResource);
        if (dstImg && srcImg) {
          const auto& dstDesc = dstImg->getDesc();
          const auto& srcDesc = srcImg->getDesc();
          // D3D11 copies only textures of the same layout and drops the
          // call otherwise; D3D12 would reject the command list.
          if (dstDesc.width != srcDesc.width || dstDesc.height != srcDesc.height || dstDesc.depth != srcDesc.depth
           || dstDesc.mipLevels != srcDesc.mipLevels || dstDesc.arraySize != srcDesc.arraySize
           || dstDesc.sampleCount != srcDesc.sampleCount
           || getFormatBlockSize(dstDesc.format) != getFormatBlockSize(srcDesc.format))
            return;
          const bool shadowAlong = forcedMsaaShadowCopyable(pDstResource, pSrcResource);
          readForcedMsaa(pSrcResource, !shadowAlong);
          writeForcedMsaaImage(pDstResource, true);
          copyForcedMsaaShadow(pDstResource, pSrcResource, shadowAlong);
          uint32_t dstMipLevels = dstDesc.mipLevels ? dstDesc.mipLevels : 1u;
          uint32_t srcMipLevels = srcDesc.mipLevels ? srcDesc.mipLevels : 1u;
          uint32_t mipLevels = (std::min)(dstMipLevels, srcMipLevels);
          uint32_t dstArrayLayers = dstDesc.type == ImageType::Tex3D ? 1u : (dstDesc.arraySize ? dstDesc.arraySize : 1u);
          uint32_t srcArrayLayers = srcDesc.type == ImageType::Tex3D ? 1u : (srcDesc.arraySize ? srcDesc.arraySize : 1u);
          uint32_t arrayLayers = (std::min)(dstArrayLayers, srcArrayLayers);

          for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
              ImageSubresource dstSub = {};
              dstSub.mipLevel = mipLevel;
              dstSub.arrayLayer = arrayLayer;

              ImageSubresource srcSub = {};
              srcSub.mipLevel = mipLevel;
              srcSub.arrayLayer = arrayLayer;

              m_backendCtx->copyImage(dstImg, dstSub, srcImg, srcSub);
            }
          }
        }
      }
    }

    void STDMETHODCALLTYPE CopySubresourceRegion(
        ID3D11Resource* pDstResource,
        UINT            DstSubresource,
        UINT            DstX,
        UINT            DstY,
        UINT            DstZ,
        ID3D11Resource* pSrcResource,
        UINT            SrcSubresource,
        const D3D11_BOX* pSrcBox) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("CopySubresourceRegion dst=%p sub=%u at %u,%u,%u src=%p sub=%u box=%d", static_cast<void*>(pDstResource), DstSubresource, DstX, DstY, DstZ, static_cast<void*>(pSrcResource), SrcSubresource, pSrcBox ? 1 : 0));
      if (!pDstResource || !pSrcResource) return;
      flushPendingBufferUpdates();
      readForcedMsaa(pSrcResource);
      writeForcedMsaaImage(pDstResource);

      D3D11_RESOURCE_DIMENSION dstDim, srcDim;
      pDstResource->GetType(&dstDim);
      pSrcResource->GetType(&srcDim);

      if (dstDim == D3D11_RESOURCE_DIMENSION_BUFFER && srcDim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto* dstBuffer = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(pDstResource));
        auto* srcBuffer = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(pSrcResource));
        IBackendBuffer* dstBuf = getBackendBuffer(dstBuffer);
        IBackendBuffer* srcBuf = getBackendBuffer(srcBuffer);
        if (dstBuf && srcBuf) {
          // Ranges outside either buffer are cut off, as D3D11 does; D3D12
          // would reject the whole command list.
          const uint64_t srcSize = srcBuffer->GetDescRef().ByteWidth;
          const uint64_t dstSize = dstBuffer->GetDescRef().ByteWidth;
          uint64_t srcOffset = pSrcBox ? pSrcBox->left : 0;
          uint64_t srcEnd    = pSrcBox ? (std::min)(uint64_t(pSrcBox->right), srcSize) : srcSize;
          if (srcOffset >= srcEnd || DstX >= dstSize)
            return;
          uint64_t size = (std::min)(srcEnd - srcOffset, dstSize - DstX);
          m_backendCtx->copyBuffer(dstBuf, DstX, srcBuf, srcOffset, size);
          dstBuffer->CopyShadowDataFrom(srcBuffer, DstX, srcOffset, size);
        }
      } else {
        IBackendImage* dstImg = getBackendImage(pDstResource);
        IBackendImage* srcImg = getBackendImage(pSrcResource);
        if (dstImg && srcImg) {
          logTrackedImageCopy(
            "CopySubresourceRegion-tracked",
            pDstResource,
            dstImg,
            DstSubresource,
            DstX,
            DstY,
            DstZ,
            pSrcResource,
            srcImg,
            SrcSubresource,
            pSrcBox);

          ImageSubresource dstSub = {};
          uint32_t dstMipLevels = dstImg->getDesc().mipLevels ? dstImg->getDesc().mipLevels : 1u;
          dstSub.mipLevel   = DstSubresource % dstMipLevels;
          dstSub.arrayLayer = DstSubresource / dstMipLevels;
          ImageSubresource srcSub = {};
          uint32_t srcMipLevels = srcImg->getDesc().mipLevels ? srcImg->getDesc().mipLevels : 1u;
          srcSub.mipLevel   = SrcSubresource % srcMipLevels;
          srcSub.arrayLayer = SrcSubresource / srcMipLevels;

          const auto& dstDesc = dstImg->getDesc();
          const auto& srcDesc = srcImg->getDesc();
          const uint32_t dstLayers = dstDesc.type == ImageType::Tex3D ? 1u : (std::max)(dstDesc.arraySize, 1u);
          const uint32_t srcLayers = srcDesc.type == ImageType::Tex3D ? 1u : (std::max)(srcDesc.arraySize, 1u);
          if (dstSub.arrayLayer >= dstLayers || srcSub.arrayLayer >= srcLayers)
            return;

          ImageCopyRegion region = {};
          ImageCopyRegion* regionPtr = nullptr;
          if (pSrcBox) {
            region.x = pSrcBox->left;
            region.y = pSrcBox->top;
            region.z = pSrcBox->front;
            region.width = pSrcBox->right - pSrcBox->left;
            region.height = pSrcBox->bottom - pSrcBox->top;
            region.depth = pSrcBox->back - pSrcBox->front;
            regionPtr = &region;
            if (pSrcBox->right <= pSrcBox->left || pSrcBox->bottom <= pSrcBox->top || pSrcBox->back <= pSrcBox->front)
              return;
          }

          // Regions outside either subresource are cut off, as D3D11 does;
          // D3D12 would reject the whole command list. Formats of one element
          // size copy element for element, a block of a compressed format
          // for a texel of an uncompressed one: the cut is in elements.
          // D3D11 drops copies between formats of different element sizes.
          auto elementBytes = [](DXGI_FORMAT format) {
            const uint32_t blockBytes = getFormatBlockBytes(format);
            return blockBytes ? blockBytes : getFormatBpp(format);
          };
          const uint32_t dstElement = elementBytes(dstDesc.format);
          const uint32_t srcElement = elementBytes(srcDesc.format);
          if (dstElement && srcElement && dstElement != srcElement)
            return;
          if (dstElement == srcElement) {
            auto extent = [](uint32_t size, uint32_t mip) { return (std::max)(size >> mip, 1u); };
            auto elements = [](uint32_t texels, uint32_t block) { return (texels + block - 1) / block; };
            const uint32_t srcBlock = getFormatBlockSize(srcDesc.format);
            const uint32_t dstBlock = getFormatBlockSize(dstDesc.format);
            const uint32_t srcW = extent(srcDesc.width, srcSub.mipLevel);
            const uint32_t srcH = extent(srcDesc.height, srcSub.mipLevel);
            const uint32_t srcD = srcDesc.type == ImageType::Tex3D ? extent(srcDesc.depth, srcSub.mipLevel) : 1u;
            const uint32_t dstW = extent(dstDesc.width, dstSub.mipLevel);
            const uint32_t dstH = extent(dstDesc.height, dstSub.mipLevel);
            const uint32_t dstD = dstDesc.type == ImageType::Tex3D ? extent(dstDesc.depth, dstSub.mipLevel) : 1u;
            if (!regionPtr) {
              region = { 0, 0, 0, srcW, srcH, srcD };
              regionPtr = &region;
            }
            const uint32_t srcX = region.x / srcBlock, srcY = region.y / srcBlock;
            const uint32_t dstX = DstX / dstBlock, dstY = DstY / dstBlock;
            if (srcX >= elements(srcW, srcBlock) || srcY >= elements(srcH, srcBlock) || region.z >= srcD
             || dstX >= elements(dstW, dstBlock) || dstY >= elements(dstH, dstBlock) || DstZ >= dstD)
              return;
            const uint32_t width = (std::min)({ elements(region.x + region.width, srcBlock) - srcX,
              elements(srcW, srcBlock) - srcX, elements(dstW, dstBlock) - dstX });
            const uint32_t height = (std::min)({ elements(region.y + region.height, srcBlock) - srcY,
              elements(srcH, srcBlock) - srcY, elements(dstH, dstBlock) - dstY });
            // In source texels; a block at the edge of the source ends there.
            region.width  = (std::min)(width * srcBlock, srcW - region.x);
            region.height = (std::min)(height * srcBlock, srcH - region.y);
            region.depth  = (std::min)({ region.depth, srcD - region.z, dstD - DstZ });
            // Whole-subresource copies keep the fast path.
            if (!pSrcBox && region.width == srcW && region.height == srcH && region.depth == srcD
             && DstX == 0 && DstY == 0 && DstZ == 0 && srcBlock == dstBlock)
              regionPtr = nullptr;
          }

          m_backendCtx->copyImageRegion(dstImg, dstSub, DstX, DstY, DstZ, srcImg, srcSub, regionPtr);
        }
      }
    }

    void STDMETHODCALLTYPE CopyStructureCount(
        ID3D11Buffer*              pDstBuffer,
        UINT                       DstAlignedByteOffset,
        ID3D11UnorderedAccessView* pSrcView) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("CopyStructureCount dst=%p off=%u %s", static_cast<void*>(pDstBuffer), DstAlignedByteOffset, traceView(pSrcView, true).c_str()));
      if (!pDstBuffer || !pSrcView) return;
      flushPendingBufferUpdates();
      IBackendBuffer* backendDst = getBackendBuffer(pDstBuffer);
      IBackendBufferView* backendView = getBackendBufferUAV(pSrcView);
      if (backendDst && backendView)
        m_backendCtx->copyStructureCount(backendDst, DstAlignedByteOffset, backendView);
    }

    // ============================================================
    // ID3D11DeviceContext - Map / Unmap
    // ============================================================

    HRESULT STDMETHODCALLTYPE Map(
        ID3D11Resource*           pResource,
        UINT                      Subresource,
        D3D11_MAP                 MapType,
        UINT                      MapFlags,
        D3D11_MAPPED_SUBRESOURCE* pMappedResource) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pResource || !pMappedResource) return E_INVALIDARG;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed) && !m_probing)
        logMessage(LogLevel::Warn, "draw trace%s: Map %p sub=%u type=%u flags=0x%x",
          m_isDeferred ? " (deferred)" : "", static_cast<void*>(pResource), Subresource,
          static_cast<unsigned>(MapType), MapFlags);

      D3D11_RESOURCE_DIMENSION dim;
      pResource->GetType(&dim);

      if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto* trackedBuffer = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(pResource));
        MapMode mode = convertMapType(MapType);
        const auto& bufferDesc = trackedBuffer->GetDescRef();

        bool renamedForDiscard = false;
        void* discardedData = nullptr;

        // WRITE_DISCARD gives the buffer new memory: in place when the
        // backend can, else a new buffer that its views follow.
        if (bufferDesc.Usage == D3D11_USAGE_DYNAMIC && mode == MapMode::WriteDiscard) {
          discardedData = m_backendCtx->discardBuffer(getBackendBuffer(trackedBuffer));
          if (discardedData) {
            // The backend binds constant buffers again at their new memory.
            if (bufferDesc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
              rebindRenamedInputAssemblerBuffer(trackedBuffer);
            renamedForDiscard = true;
          }
          if (!discardedData) {
            if (auto* backendDevice = m_device->GetBackendDevice()) {
              auto renamedBuffer = backendDevice->createBuffer(&bufferDesc, nullptr);
              if (renamedBuffer) {
                IBackendBuffer* newBackendBuffer = renamedBuffer.get();
                if (m_isDeferred) {
                  renamedBuffer->pinnedContents = true;
                  m_recordedDiscards.emplace_back(trackedBuffer, renamedBuffer);
                  // Without views nothing else follows the buffer's memory:
                  // other contexts keep seeing the old contents until the
                  // command list runs, as in D3D11.
                  if (!(bufferDesc.BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))) {
                    m_bufferOverrides[trackedBuffer] = std::move(renamedBuffer);
                    if (bufferDesc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
                      rebindRenamedConstantBuffer(trackedBuffer);
                    if (bufferDesc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
                      rebindRenamedInputAssemblerBuffer(trackedBuffer);
                    renamedForDiscard = true;
                  }
                }
                if (renamedBuffer) {
                auto oldBackendBuffer = trackedBuffer->ReplaceBackendBuffer(std::move(renamedBuffer), m_backendCtx.get());
                void* oldBackendPtr = oldBackendBuffer.get();
                if (oldBackendBuffer)
                  m_backendCtx->retainTemporaryBuffer(std::move(oldBackendBuffer));

                if (bufferDesc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
                  rebindRenamedConstantBuffer(trackedBuffer);
                if (bufferDesc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
                  rebindRenamedInputAssemblerBuffer(trackedBuffer);
                if (bufferDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
                  rebindBufferViewsForResource(pResource);

                logMessage(
                  LogLevel::Info,
                  "Map-WRITE_DISCARD-rename: buf=%p oldBackend=%p newBackend=%p size=%u bind=0x%x cpu=0x%x",
                  trackedBuffer,
                  oldBackendPtr,
                  newBackendBuffer,
                  bufferDesc.ByteWidth,
                  bufferDesc.BindFlags,
                  bufferDesc.CPUAccessFlags);
                renamedForDiscard = true;
                }
              }
            }
          }
        }

        IBackendBuffer* backendBuf = getBackendBuffer(trackedBuffer);
        if (!backendBuf) return E_INVALIDARG;

        // Staging buffers wait for the GPU work that uses them, as in D3D11,
        // or report that it still runs.
        if (bufferDesc.Usage == D3D11_USAGE_STAGING) {
          flushPendingBufferUpdates();
          if (!m_backendCtx->waitForGpuUse(backendBuf->gpuUse, (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0))
            return DXGI_ERROR_WAS_STILL_DRAWING;
        } else if (mode == MapMode::Read || mode == MapMode::ReadWrite) {
          flushPendingBufferUpdates();
          m_backendCtx->synchronize();
        }

        const UINT trackedIaFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
        static uint32_t s_dynamicIaMapLogCount = 0;
        if ((bufferDesc.BindFlags & trackedIaFlags) && bufferDesc.ByteWidth >= (1u << 20) && s_dynamicIaMapLogCount < 128u) {
          logMessage(
            LogLevel::Info,
            "Map-dynamic-ia: buf=%p backend=%p mode=%u mapType=%u flags=0x%x size=%u bind=0x%x usage=%u cpu=0x%x",
            trackedBuffer,
            backendBuf,
            static_cast<uint32_t>(mode),
            static_cast<uint32_t>(MapType),
            MapFlags,
            bufferDesc.ByteWidth,
            bufferDesc.BindFlags,
            static_cast<uint32_t>(bufferDesc.Usage),
            bufferDesc.CPUAccessFlags);
          s_dynamicIaMapLogCount++;
        }

        MapMode backendMapMode = renamedForDiscard ? MapMode::Write : mode;
        void* ptr = discardedData ? discardedData : m_backendCtx->mapBuffer(backendBuf, backendMapMode);
        if (!ptr) {
          if (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT)
            return DXGI_ERROR_WAS_STILL_DRAWING;
          return E_FAIL;
        }

        pMappedResource->pData      = ptr;
        pMappedResource->RowPitch   = trackedBuffer->GetDescRef().ByteWidth;
        pMappedResource->DepthPitch = 0;

        if (trackedBuffer->HasShadowData()) {
          BufferMapInfo info = {};
          info.buffer  = trackedBuffer;
          info.mapMode = mode;
          info.data    = ptr;
          m_bufferMappings[pResource] = info;
        } else if (!m_bufferMappings.empty()) {
          m_bufferMappings.erase(pResource);
        }

        return S_OK;
      }

      // Texture mapping - use a staging buffer
      {
        IBackendImage* backendImg = getBackendImage(pResource);
        if (!backendImg) return E_INVALIDARG;

        // Staging textures are CPU-visible memory: returned directly once
        // the GPU finished the work that uses them.
        if (backendImg->isCpuAccessible()) {
          // An upload the GPU still copies from gets other memory with the
          // same contents instead of waiting, as D3D11 drivers rename it.
          const bool renamed = MapType != D3D11_MAP_READ && m_backendCtx->renameStagingImage(backendImg);
          if (!renamed && !m_backendCtx->waitForGpuUse(backendImg->gpuUse, (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0))
            return DXGI_ERROR_WAS_STILL_DRAWING;
          ImageSubresourceLayout layout;
          uint8_t* mapped = backendImg->mapStaging(Subresource, convertMapType(MapType), layout);
          if (!mapped)
            return E_INVALIDARG;
          pMappedResource->pData      = mapped;
          pMappedResource->RowPitch   = layout.rowPitch;
          pMappedResource->DepthPitch = layout.depthPitch;
          return S_OK;
        }

        const auto& imgDesc = backendImg->getDesc();
        uint32_t mipLevel   = Subresource % imgDesc.mipLevels;
        uint32_t arrayLayer  = Subresource / imgDesc.mipLevels;
        uint32_t mipW = (imgDesc.width  >> mipLevel) > 0 ? (imgDesc.width  >> mipLevel) : 1;
        uint32_t mipH = (imgDesc.height >> mipLevel) > 0 ? (imgDesc.height >> mipLevel) : 1;
        uint32_t mipD = (imgDesc.depth  >> mipLevel) > 0 ? (imgDesc.depth  >> mipLevel) : 1;

        auto* backend = m_device->GetBackend();
        if (!backend) return E_FAIL;

        MapMode mode = convertMapType(MapType);
        UploadLayout uploadLayout = getImageUploadLayout(
          m_device->GetBackendType(),
          backend,
          backendImg,
          Subresource,
          computeRowPitch(mipW, imgDesc.format),
          computeNumRows(mipH, imgDesc.format),
          mipD);
        uint64_t slicePitch = static_cast<uint64_t>(uploadLayout.rowPitch) * uploadLayout.numRows;

        BufferDesc stagingDesc = {};
        stagingDesc.size      = uploadLayout.totalSize;
        stagingDesc.usage     = ResourceUsage::Staging;
        stagingDesc.cpuAccess = (mode == MapMode::Read || mode == MapMode::ReadWrite)
          ? CpuAccess::Read
          : CpuAccess::Write;
        auto stagingBuf = backend->createBuffer(stagingDesc);
        if (!stagingBuf) return E_FAIL;

        // If reading, copy image data to staging buffer first
        if (mode == MapMode::Read || mode == MapMode::ReadWrite) {
          ImageSubresource subres = {};
          subres.mipLevel   = mipLevel;
          subres.arrayLayer = arrayLayer;
          m_backendCtx->copyImageToBuffer(stagingBuf.get(), 0, backendImg, subres);
          m_backendCtx->flush();
          m_backendCtx->synchronize();
        }

        void* ptr = m_backendCtx->mapBuffer(stagingBuf.get(), mode);
        if (!ptr) return E_FAIL;

        pMappedResource->pData      = ptr;
        pMappedResource->RowPitch   = uploadLayout.rowPitch;
        pMappedResource->DepthPitch = static_cast<UINT>(slicePitch);

        // Store the staging buffer for Unmap
        m_textureStagingBuffers[pResource] = TextureStagingInfo{
          stagingBuf, backendImg, Subresource, mode,
          uploadLayout.rowPitch, static_cast<uint32_t>(slicePitch)
        };
        return S_OK;
      }
    }

    void STDMETHODCALLTYPE Unmap(
        ID3D11Resource* pResource,
        UINT            Subresource) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pResource) return;

      D3D11_RESOURCE_DIMENSION dim;
      pResource->GetType(&dim);

      if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto* trackedBuffer = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(pResource));
        IBackendBuffer* backendBuf = getBackendBuffer(trackedBuffer);

        // Only buffers with CPU shadow copies are tracked while mapped.
        if (!m_bufferMappings.empty()) {
          auto mapIt = m_bufferMappings.find(pResource);
          if (mapIt != m_bufferMappings.end()) {
            const auto& info = mapIt->second;
            if (info.buffer && info.data && isWritableMapMode(info.mapMode))
              info.buffer->UpdateShadowData(0u, info.data, info.buffer->GetShadowSize());
            m_bufferMappings.erase(mapIt);
          }
        }

        if (backendBuf)
          m_backendCtx->unmapBuffer(backendBuf);
      } else {
        IBackendImage* image = getBackendImage(pResource);
        if (image && image->isCpuAccessible()) {
          image->unmapStaging(Subresource);
          return;
        }
        // Texture unmapping - flush staging buffer back to image if written
        auto it = m_textureStagingBuffers.find(pResource);
        if (it != m_textureStagingBuffers.end()) {
          auto& info = it->second;
          const auto& imgDesc = info.image->getDesc();
          uint32_t mipLevel   = info.subresource % imgDesc.mipLevels;
          uint32_t arrayLayer = info.subresource / imgDesc.mipLevels;
          uint32_t mipW = (imgDesc.width  >> mipLevel) > 0 ? (imgDesc.width  >> mipLevel) : 1;
          uint32_t mipH = (imgDesc.height >> mipLevel) > 0 ? (imgDesc.height >> mipLevel) : 1;
          uint32_t mipD = (imgDesc.depth  >> mipLevel) > 0 ? (imgDesc.depth  >> mipLevel) : 1;

          if (info.mapMode == MapMode::Write || info.mapMode == MapMode::ReadWrite
           || info.mapMode == MapMode::WriteDiscard
           || info.mapMode == MapMode::WriteNoOverwrite) {
            ImageSubresource subres = {};
            subres.mipLevel   = mipLevel;
            subres.arrayLayer = arrayLayer;

            auto* backend = m_device->GetBackend();
            if (!backend) {
              m_backendCtx->unmapBuffer(info.stagingBuffer.get());
              m_textureStagingBuffers.erase(it);
              return;
            }

            UploadLayout uploadLayout = getImageUploadLayout(
              m_device->GetBackendType(),
              backend,
              info.image,
              info.subresource,
              computeRowPitch(mipW, imgDesc.format),
              computeNumRows(mipH, imgDesc.format),
              mipD);

            if (info.stagingBuffer->getDesc().cpuAccess == CpuAccess::Read) {
              m_backendCtx->unmapBuffer(info.stagingBuffer.get());

              void* srcPtr = m_backendCtx->mapBuffer(info.stagingBuffer.get(), MapMode::Read);
              if (srcPtr) {
                BufferDesc uploadDesc = {};
                uploadDesc.size      = uploadLayout.totalSize;
                uploadDesc.usage     = ResourceUsage::Dynamic;
                uploadDesc.cpuAccess = CpuAccess::Write;

                auto uploadBuf = backend->createBuffer(uploadDesc);
                if (uploadBuf) {
                  void* dstPtr = m_backendCtx->mapBuffer(uploadBuf.get(), MapMode::Write);
                  if (dstPtr) {
                    std::memcpy(dstPtr, srcPtr, uploadLayout.totalSize);
                    m_backendCtx->unmapBuffer(uploadBuf.get());
                    m_backendCtx->copyBufferToImage(info.image, subres, uploadBuf.get(), 0);
                    m_backendCtx->retainTemporaryBuffer(uploadBuf);
                  }
                }

                m_backendCtx->unmapBuffer(info.stagingBuffer.get());
              }
            } else {
              m_backendCtx->unmapBuffer(info.stagingBuffer.get());
              m_backendCtx->copyBufferToImage(info.image, subres, info.stagingBuffer.get(), 0);
              m_backendCtx->retainTemporaryBuffer(info.stagingBuffer);
            }
          } else {
            m_backendCtx->unmapBuffer(info.stagingBuffer.get());
          }

          m_textureStagingBuffers.erase(it);
        }
      }
    }

    // ============================================================
    // ID3D11DeviceContext - Clear Operations
    // ============================================================

    void STDMETHODCALLTYPE ClearRenderTargetView(
        ID3D11RenderTargetView* pRenderTargetView,
        const FLOAT             ColorRGBA[4]) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pRenderTargetView || !ColorRGBA) return;
      IBackendImageView* backendRTV = getBackendRTV(pRenderTargetView);
      if (m_historyOn && !m_isDeferred)
        recordClear(RecordOp::ClearColor, pRenderTargetView, backendRTV, ColorRGBA);
      {
        auto* view = static_cast<D3D11RenderTargetView*>(pRenderTargetView);
        if (IBackendImageView* shadow = clearedForcedMsaaShadow(view->GetResourceRaw(), view->GetShadowView(), true))
          m_backendCtx->clearRenderTarget(shadow, ColorRGBA);
      }
      if (backendRTV) {
        logTrackedRenderTargetClear(pRenderTargetView, backendRTV, ColorRGBA);
        m_backendCtx->clearRenderTarget(backendRTV, ColorRGBA);
      }
    }

    void STDMETHODCALLTYPE ClearDepthStencilView(
        ID3D11DepthStencilView* pDepthStencilView,
        UINT                    ClearFlags,
        FLOAT                   Depth,
        UINT8                   Stencil) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pDepthStencilView) return;
      IBackendImageView* backendDSV = getBackendDSV(pDepthStencilView);
      if (m_historyOn && !m_isDeferred) {
        const float value[4] = { Depth, float(Stencil), float(ClearFlags), 0.0f };
        recordClear(RecordOp::ClearDepth, pDepthStencilView, backendDSV, value);
      }
      {
        auto* view = static_cast<D3D11DepthStencilView*>(pDepthStencilView);
        // All of it is cleared when every plane of the format is.
        const UINT allPlanes = hazard_detail::hasStencil(view->GetPipelineFormat())
          ? (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL) : D3D11_CLEAR_DEPTH;
        if (IBackendImageView* shadow = clearedForcedMsaaShadow(view->GetResourceRaw(), view->GetShadowView(),
              (ClearFlags & allPlanes) == allPlanes)) {
          m_backendCtx->clearDepthStencil(shadow, Depth, Stencil, ClearFlags);
          if (ClearFlags & D3D11_CLEAR_STENCIL) {
            D3D11Texture2D* texture = forcedMsaaTexture(view->GetResourceRaw());
            texture->SetMsaaImageStencilOld(false);
            texture->SetMsaaShadowStencilOld(false);
          }
        }
      }
      if (backendDSV) {
        m_backendCtx->clearDepthStencil(backendDSV, Depth, Stencil, ClearFlags);
      }
    }

    void STDMETHODCALLTYPE ClearState() override {
      DXUP_CONTEXT_MT_GUARD;
      m_backendCtx->clearState();
      releaseAllBindings();
      m_state = ContextState{};
      m_currentPipeline = nullptr;
      m_currentPipelineIsCompute = false;
      m_pipelineDirty = true;
      // The command list keeps its dynamic state: back to the defaults,
      // which later calls with default values take as unchanged.
      m_backendCtx->setBlendFactor(m_state.om.blendFactor);
      m_backendCtx->setStencilRef(m_state.om.stencilRef);
      m_backendCtx->setViewports(0, nullptr);
      m_injectViewportJittered = false;
    }

    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
        ID3D11UnorderedAccessView* pUnorderedAccessView,
        const UINT                 Values[4]) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall("ClearUnorderedAccessViewUint " + traceView(pUnorderedAccessView, true));
      if (!pUnorderedAccessView) return;
      flushPendingBufferUpdates();
      writeForcedMsaaImage(static_cast<D3D11UnorderedAccessView*>(pUnorderedAccessView)->GetResourceRaw(), true);
      IBackendImageView* imageView = getBackendImageUAV(pUnorderedAccessView);
      if (imageView) {
        m_backendCtx->clearUnorderedAccessViewUint(imageView, Values);
      } else {
        IBackendBufferView* bufferView = getBackendBufferUAV(pUnorderedAccessView);
        if (bufferView)
          m_backendCtx->clearUnorderedAccessViewUint(bufferView, Values);
      }
    }

    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
        ID3D11UnorderedAccessView* pUnorderedAccessView,
        const FLOAT                Values[4]) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall("ClearUnorderedAccessViewFloat " + traceView(pUnorderedAccessView, true));
      if (!pUnorderedAccessView) return;
      flushPendingBufferUpdates();
      writeForcedMsaaImage(static_cast<D3D11UnorderedAccessView*>(pUnorderedAccessView)->GetResourceRaw(), true);
      IBackendImageView* imageView = getBackendImageUAV(pUnorderedAccessView);
      if (imageView) {
        m_backendCtx->clearUnorderedAccessViewFloat(imageView, Values);
      } else {
        IBackendBufferView* bufferView = getBackendBufferUAV(pUnorderedAccessView);
        if (bufferView)
          m_backendCtx->clearUnorderedAccessViewFloat(bufferView, Values);
      }
    }

    // ============================================================
    // ID3D11DeviceContext - Miscellaneous
    // ============================================================

    void STDMETHODCALLTYPE GenerateMips(
        ID3D11ShaderResourceView* pShaderResourceView) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall("GenerateMips " + traceView(pShaderResourceView, false));
      if (!pShaderResourceView) return;
      // Only textures made for it get mips.
      ViewResourceInfo info;
      if (!getViewResourceInfo(static_cast<D3D11ShaderResourceView*>(pShaderResourceView)->GetResourceRaw(), info)
       || !(info.miscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS))
        return;
      flushPendingBufferUpdates();
      IBackendImageView* backendSRV = getBackendImageSRV(pShaderResourceView);
      if (backendSRV) {
        m_backendCtx->generateMips(backendSRV);
      }
    }

    void STDMETHODCALLTYPE ResolveSubresource(
        ID3D11Resource* pDstResource,
        UINT            DstSubresource,
        ID3D11Resource* pSrcResource,
        UINT            SrcSubresource,
        DXGI_FORMAT     Format) override {
      DXUP_CONTEXT_MT_GUARD;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("ResolveSubresource dst=%p sub=%u src=%p sub=%u fmt=%u", static_cast<void*>(pDstResource),
          DstSubresource, static_cast<void*>(pSrcResource), SrcSubresource, static_cast<unsigned>(Format)));
      if (!pDstResource || !pSrcResource) return;
      flushPendingBufferUpdates();
      IBackendImage* dstImg = getBackendImage(pDstResource);
      IBackendImage* srcImg = getBackendImage(pSrcResource);
      if (!dstImg || !srcImg)
        return;
      // D3D11 resolves a multisampled subresource into a single-sampled one
      // of its size in a format both can take, and drops other calls. A
      // single-sampled source is copied, as the drivers do: Just Cause 2
      // resolves its scene into the texture its post-processing reads also
      // with anti-aliasing off.
      const auto& dstDesc = dstImg->getDesc();
      const auto& srcDesc = srcImg->getDesc();
      const uint32_t dstMips = (std::max)(dstDesc.mipLevels, 1u);
      const uint32_t srcMips = (std::max)(srcDesc.mipLevels, 1u);
      const uint32_t dstMip = DstSubresource % dstMips, srcMip = SrcSubresource % srcMips;
      auto extent = [](uint32_t size, uint32_t mip) { return (std::max)(size >> mip, 1u); };
      auto viewable = [&](DXGI_FORMAT resourceFormat) {
        return Format == resourceFormat || getFormatFamily(Format) == resourceFormat;
      };
      if (dstDesc.sampleCount != 1
       || DstSubresource / dstMips >= (std::max)(dstDesc.arraySize, 1u)
       || SrcSubresource / srcMips >= (std::max)(srcDesc.arraySize, 1u)
       || extent(dstDesc.width, dstMip) != extent(srcDesc.width, srcMip)
       || extent(dstDesc.height, dstMip) != extent(srcDesc.height, srcMip)
       || isFamilyOnlyFormat(Format) || !viewable(dstDesc.format) || !viewable(srcDesc.format))
        return;
      if (srcDesc.sampleCount <= 1) {
        if (pDstResource == pSrcResource)
          return;
        // Textures with forced copies have a single subresource: the copy
        // is one of whole textures, as in CopyResource.
        const bool shadowAlong = forcedMsaaShadowCopyable(pDstResource, pSrcResource);
        readForcedMsaa(pSrcResource, !shadowAlong);
        writeForcedMsaaImage(pDstResource, true);
        copyForcedMsaaShadow(pDstResource, pSrcResource, shadowAlong);
        ImageSubresource dstSub = {};
        dstSub.mipLevel = dstMip;
        dstSub.arrayLayer = DstSubresource / dstMips;
        ImageSubresource srcSub = {};
        srcSub.mipLevel = srcMip;
        srcSub.arrayLayer = SrcSubresource / srcMips;
        m_backendCtx->copyImage(dstImg, dstSub, srcImg, srcSub);
        return;
      }
      // Textures with forced copies have a single subresource.
      writeForcedMsaaImage(pDstResource, true);
      m_backendCtx->resolveSubresource(dstImg, DstSubresource, srcImg, SrcSubresource, Format);
    }

    void STDMETHODCALLTYPE SetResourceMinLOD(
        ID3D11Resource* pResource,
        FLOAT           MinLOD) override {
      DXUP_CONTEXT_MT_GUARD;
      // D3D11 ignores the call for textures created without RESOURCE_CLAMP.
      IBackendImage* image = getBackendImage(pResource);
      if (!image || !(textureMiscFlags(pResource) & D3D11_RESOURCE_MISC_RESOURCE_CLAMP))
        return;
      const float clamped = MinLOD > 0.0f ? MinLOD : 0.0f;
      if (image->minLod.load(std::memory_order_relaxed) == clamped)
        return;
      image->minLod.store(clamped, std::memory_order_relaxed);
      m_backendCtx->setImageMinLod(image, clamped);
    }

    static UINT textureMiscFlags(ID3D11Resource* resource) {
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);
      switch (dimension) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
          D3D11_TEXTURE1D_DESC desc;
          static_cast<ID3D11Texture1D*>(resource)->GetDesc(&desc);
          return desc.MiscFlags;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
          D3D11_TEXTURE2D_DESC desc;
          static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
          return desc.MiscFlags;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
          D3D11_TEXTURE3D_DESC desc;
          static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
          return desc.MiscFlags;
        }
        default:
          return 0;
      }
    }

    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(
        ID3D11Resource* pResource) override {
      DXUP_CONTEXT_MT_GUARD;
      IBackendImage* image = getBackendImage(pResource);
      return image ? image->minLod.load(std::memory_order_relaxed) : 0.0f;
    }

    void STDMETHODCALLTYPE SetPredication(
        ID3D11Predicate* pPredicate,
        BOOL             PredicateValue) override {
      DXUP_CONTEXT_MT_GUARD;
      flushPendingBufferUpdates();
      ID3D11Predicate* previous = m_state.predicate;
      if (pPredicate)
        reinterpret_cast<D3D11Query*>(pPredicate)->AddRefPrivate();
      m_state.predicate      = pPredicate;
      m_state.predicateValue = PredicateValue;
      m_backendCtx->setPredication(
        pPredicate ? reinterpret_cast<D3D11Query*>(pPredicate)->GetBackendQuery() : nullptr,
        PredicateValue != FALSE);
      if (previous)
        reinterpret_cast<D3D11Query*>(previous)->ReleasePrivate();
    }

    void STDMETHODCALLTYPE GetPredication(
        ID3D11Predicate** ppPredicate,
        BOOL*             pPredicateValue) override {
      DXUP_CONTEXT_MT_GUARD;
      if (ppPredicate) {
        *ppPredicate = m_state.predicate;
        if (m_state.predicate)
          m_state.predicate->AddRef();
      }
      if (pPredicateValue) *pPredicateValue = m_state.predicate ? m_state.predicateValue : FALSE;
    }

    void STDMETHODCALLTYPE Flush() override {
      DXUP_CONTEXT_MT_GUARD;
      flushPendingBufferUpdates();
      m_backendCtx->flush();
      if (!m_isDeferred && m_device->hasSharedResources())
        m_backendCtx->waitForWorker();
    }

    // Submits the work recorded so far, then runs fn in order with it.
    void submitThen(std::function<void()> fn) {
      DXUP_CONTEXT_MT_GUARD;
      flushPendingBufferUpdates();
      m_backendCtx->flush();
      m_backendCtx->executeOnBackend([fn = std::move(fn)](IBackendContext*) { fn(); });
    }

    HRESULT STDMETHODCALLTYPE FinishCommandList(
        BOOL                 RestoreDeferredContextState,
        ID3D11CommandList**  ppCommandList) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!m_isDeferred)
        return DXGI_ERROR_INVALID_CALL;
      if (!ppCommandList)
        return E_INVALIDARG;
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        logMessage(LogLevel::Warn, "draw trace (deferred): FinishCommandList restore=%d", RestoreDeferredContextState);

      flushPendingBufferUpdates();
      auto commands = m_backendCtx->finishCommandList();
      if (!commands)
        return E_FAIL;
      *ppCommandList = new D3D11CommandList(m_device, std::move(commands), std::move(m_recordedDiscards),
        std::move(m_recordedMsaaUses));
      m_recordedDiscards.clear();
      m_recordedMsaaUses.clear();
      m_bufferOverrides.clear();

      // The next command list starts from the default state unless the
      // application keeps the current one, which then has to be recorded
      // into it again.
      if (RestoreDeferredContextState)
        reapplyBackendState();
      else
        ClearState();
      return S_OK;
    }

    void STDMETHODCALLTYPE ExecuteCommandList(
        ID3D11CommandList* pCommandList,
        BOOL               RestoreContextState) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pCommandList) return;

      auto* cmdList = static_cast<D3D11CommandList*>(pCommandList);
      const auto& commands = cmdList->GetBackendCommandList();
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        logMessage(LogLevel::Warn, "draw trace%s: ExecuteCommandList %p restore=%d",
          m_isDeferred ? " (deferred)" : "", static_cast<void*>(pCommandList), RestoreContextState);
      if (!commands)
        return;

      flushPendingBufferUpdates();
      beginForcedMsaaCommandList(cmdList->GetMsaaUses());
      m_backendCtx->executeCommandList(commands);
      endForcedMsaaCommandList(cmdList->GetMsaaUses());

      // The contents it gave buffers stay theirs: the last of each buffer
      // counts (the command list keeps the others alive).
      const auto& discards = cmdList->GetDiscards();
      std::unordered_set<D3D11Buffer*> applied;
      for (size_t index = discards.size(); index-- > 0;) {
        const auto& discard = discards[index];
        D3D11Buffer* buffer = discard.buffer();
        if (!applied.insert(buffer).second)
          continue;
        if (getBackendBuffer(buffer) == discard.contents().get())
          continue;
        // A deferred context running a list keeps the contents to itself
        // too, until its own list runs.
        const bool views = buffer->GetDescRef().BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        if (m_isDeferred) {
          m_recordedDiscards.emplace_back(buffer, discard.contents());
          if (!views) {
            m_bufferOverrides[buffer] = discard.contents();
            continue;
          }
        }
        auto previous = buffer->ReplaceBackendBuffer(discard.contents(), m_backendCtx.get());
        if (previous)
          m_backendCtx->retainTemporaryBuffer(std::move(previous));
      }

      // The command list leaves its own state bound: return to ours, or to
      // the default state.
      if (RestoreContextState)
        reapplyBackendState();
      else
        ClearState();
    }

    // ============================================================
    // ID3D11DeviceContext - Queries
    // ============================================================

    void STDMETHODCALLTYPE Begin(
        ID3D11Asynchronous* pAsync) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pAsync) return;
      IBackendQuery* backendQuery = getBackendQuery(pAsync);
      if (backendQuery)
        m_backendCtx->beginQuery(backendQuery);
    }

    void STDMETHODCALLTYPE End(
        ID3D11Asynchronous* pAsync) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pAsync) return;
      static_cast<D3D11Query*>(pAsync)->ResetFlushRequest();
      IBackendQuery* backendQuery = getBackendQuery(pAsync);
      if (backendQuery) {
        // Updates written with UpdateSubresource before End must be
        // recorded before it.
        flushPendingBufferUpdates();
        m_backendCtx->endQuery(backendQuery);
      }
    }

    HRESULT STDMETHODCALLTYPE GetData(
        ID3D11Asynchronous* pAsync,
        void*               pData,
        UINT                DataSize,
        UINT                GetDataFlags) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pAsync) return E_INVALIDARG;
      if (m_isDeferred) return DXGI_ERROR_INVALID_CALL;

      auto* query = static_cast<D3D11Query*>(pAsync);
      if (pData && DataSize != query->GetDataSize())
        return E_INVALIDARG;

      IBackendQuery* backendQuery = query->GetBackendQuery();
      if (!backendQuery) return E_FAIL;

      QueryData data;
      const QueryStatus status = backendQuery->getData(data);
      if (status == QueryStatus::Invalid)
        return DXGI_ERROR_INVALID_CALL;
      if (status != QueryStatus::Ready) {
        // A flush the worker has yet to run submits this query too. Kena
        // polls ~120 queries in a row after ending them; a submission for
        // each kept the CS worker busy for 2-3 ms, and frames alternated
        // between 3 and 16 ms.
        if (status == QueryStatus::NotSubmitted
         && !(GetDataFlags & D3D11_ASYNC_GETDATA_DONOTFLUSH)
         && !m_backendCtx->flushPending()
         && query->RequestFlush())
          Flush();
        return S_FALSE;
      }

      if (!pData)
        return S_OK;

      D3D11_QUERY_DESC queryDesc;
      query->GetDesc(&queryDesc);
      switch (queryDesc.Query) {
        case D3D11_QUERY_EVENT:
          *static_cast<BOOL*>(pData) = TRUE;
          break;
        case D3D11_QUERY_OCCLUSION:
          *static_cast<UINT64*>(pData) = data.samples;
          break;
        case D3D11_QUERY_OCCLUSION_PREDICATE:
          *static_cast<BOOL*>(pData) = data.samples != 0;
          break;
        case D3D11_QUERY_TIMESTAMP:
          *static_cast<UINT64*>(pData) = data.timestamp;
          break;
        case D3D11_QUERY_TIMESTAMP_DISJOINT: {
          auto* disjoint = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(pData);
          disjoint->Frequency = data.frequency;
          disjoint->Disjoint  = data.disjoint ? TRUE : FALSE;
          break;
        }
        case D3D11_QUERY_PIPELINE_STATISTICS: {
          // D3D11 and D3D12 share the counter order.
          static_assert(sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS) == sizeof(data.pipelineStatistics),
            "pipeline statistics layout");
          std::memcpy(pData, data.pipelineStatistics, sizeof(data.pipelineStatistics));
          break;
        }
        case D3D11_QUERY_SO_STATISTICS:
        case D3D11_QUERY_SO_STATISTICS_STREAM0:
        case D3D11_QUERY_SO_STATISTICS_STREAM1:
        case D3D11_QUERY_SO_STATISTICS_STREAM2:
        case D3D11_QUERY_SO_STATISTICS_STREAM3: {
          auto* stats = static_cast<D3D11_QUERY_DATA_SO_STATISTICS*>(pData);
          stats->NumPrimitivesWritten    = data.primitivesWritten;
          stats->PrimitivesStorageNeeded = data.primitivesStorageNeeded;
          break;
        }
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
          *static_cast<BOOL*>(pData) = data.primitivesStorageNeeded > data.primitivesWritten;
          break;
        default:
          return E_INVALIDARG;
      }
      return S_OK;
    }

    // ============================================================
    // Backend accessor
    // ============================================================

    IBackendContext* getBackendContext() const {
      return m_backendCtx.get();
    }

    void STDMETHODCALLTYPE CopySubresourceRegion1(
        ID3D11Resource* pDstResource,
        UINT            DstSubresource,
        UINT            DstX,
        UINT            DstY,
        UINT            DstZ,
        ID3D11Resource* pSrcResource,
        UINT            SrcSubresource,
        const D3D11_BOX* pSrcBox,
        UINT            CopyFlags) override {
      DXUP_CONTEXT_MT_GUARD;
      if (CopyFlags != 0)
        logMessage(LogLevel::Warn, "CopySubresourceRegion1 ignoring CopyFlags=0x%x", CopyFlags);
      CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
    }

    void STDMETHODCALLTYPE UpdateSubresource1(
        ID3D11Resource* pDstResource,
        UINT            DstSubresource,
        const D3D11_BOX* pDstBox,
        const void*     pSrcData,
        UINT            SrcRowPitch,
        UINT            SrcDepthPitch,
        UINT            CopyFlags) override {
      DXUP_CONTEXT_MT_GUARD;
      // NO_OVERWRITE and DISCARD only allow less ordering; the ordered
      // update gives the same result.
      (void)CopyFlags;
      UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    }

    void STDMETHODCALLTYPE DiscardResource(ID3D11Resource* pResource) override {
      DXUP_CONTEXT_MT_GUARD;
      discardForcedMsaa(pResource);
    }

    void STDMETHODCALLTYPE DiscardView(ID3D11View* pResourceView) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pResourceView)
        return;
      // Views of textures with forced copies cover all of them.
      ComPtr<ID3D11Resource> resource;
      pResourceView->GetResource(resource.GetAddressOf());
      discardForcedMsaa(resource.Get());
    }

    void STDMETHODCALLTYPE DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRect, UINT NumRects) override {
      DXUP_CONTEXT_MT_GUARD;
      (void)pRect;
      // Discarding parts leaves the rest as it is.
      if (!NumRects)
        DiscardView(pResourceView);
    }

    void STDMETHODCALLTYPE VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers1(ShaderStage::Vertex, m_state.vs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE HSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers1(ShaderStage::Hull, m_state.hs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE DSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers1(ShaderStage::Domain, m_state.ds, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE GSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers1(ShaderStage::Geometry, m_state.gs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers1(ShaderStage::Pixel, m_state.ps, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      setConstantBuffers1(ShaderStage::Compute, m_state.cs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE VSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers1(m_state.vs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE HSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers1(m_state.hs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE DSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers1(m_state.ds, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE GSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers1(m_state.gs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE PSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers1(m_state.ps, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE CSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override {
      DXUP_CONTEXT_MT_GUARD;
      getConstantBuffers1(m_state.cs, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
    }

    void STDMETHODCALLTYPE SwapDeviceContextState(ID3DDeviceContextState* pState, ID3DDeviceContextState** ppPreviousState) override;

    // ============================================================
    // ID3D11DeviceContext2: tiled resources map onto D3D12 reserved
    // resources and heaps; annotations are not recorded.
    // ============================================================

    TiledResourceRef tiledResourceRef(ID3D11Resource* resource) const {
      TiledResourceRef ref;
      if (!resource)
        return ref;
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);
      if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
        ref.buffer = getBackendBuffer(static_cast<ID3D11Buffer*>(resource));
      else
        ref.image = getBackendImage(resource);
      return ref;
    }

    static TileCoordinate tileCoordinate(const D3D11_TILED_RESOURCE_COORDINATE& c) {
      return { c.X, c.Y, c.Z, c.Subresource };
    }

    static TileRegionSize tileRegionSize(const D3D11_TILE_REGION_SIZE& size) {
      return { size.NumTiles, size.bUseBox ? 1 : 0, size.Width, size.Height, size.Depth };
    }

    HRESULT STDMETHODCALLTYPE UpdateTileMappings(
        ID3D11Resource* pTiledResource, UINT NumTiledResourceRegions,
        const D3D11_TILED_RESOURCE_COORDINATE* pTiledResourceRegionStartCoordinates,
        const D3D11_TILE_REGION_SIZE* pTiledResourceRegionSizes, ID3D11Buffer* pTilePool,
        UINT NumRanges, const UINT* pRangeFlags, const UINT* pTilePoolStartOffsets,
        const UINT* pRangeTileCounts, UINT Flags) override {
      DXUP_CONTEXT_MT_GUARD;
      TileMappingUpdate update;
      update.resource = tiledResourceRef(pTiledResource);
      if (!update.resource.buffer && !update.resource.image)
        return E_INVALIDARG;
      if (NumTiledResourceRegions && !pTiledResourceRegionStartCoordinates)
        return E_INVALIDARG;
      update.tilePool = pTilePool ? getBackendBuffer(pTilePool) : nullptr;
      for (UINT i = 0; i < NumTiledResourceRegions; i++) {
        update.coordinates.push_back(tileCoordinate(pTiledResourceRegionStartCoordinates[i]));
        if (pTiledResourceRegionSizes)
          update.sizes.push_back(tileRegionSize(pTiledResourceRegionSizes[i]));
      }
      if (pRangeFlags)
        update.rangeFlags.assign(pRangeFlags, pRangeFlags + NumRanges);
      if (pTilePoolStartOffsets)
        update.poolOffsets.assign(pTilePoolStartOffsets, pTilePoolStartOffsets + NumRanges);
      if (pRangeTileCounts)
        update.rangeTileCounts.assign(pRangeTileCounts, pRangeTileCounts + NumRanges);
      // Without any array the single range covers every tile.
      if (update.rangeFlags.empty() && update.poolOffsets.empty() && update.rangeTileCounts.empty() && NumRanges > 1)
        return E_INVALIDARG;
      update.flags = Flags;
      flushPendingBufferUpdates();
      m_backendCtx->updateTileMappings(update);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CopyTileMappings(
        ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestRegionStartCoordinate,
        ID3D11Resource* pSourceTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pSourceRegionStartCoordinate,
        const D3D11_TILE_REGION_SIZE* pTileRegionSize, UINT Flags) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pDestRegionStartCoordinate || !pSourceRegionStartCoordinate || !pTileRegionSize)
        return E_INVALIDARG;
      TiledResourceRef dst = tiledResourceRef(pDestTiledResource);
      TiledResourceRef src = tiledResourceRef(pSourceTiledResource);
      if ((!dst.buffer && !dst.image) || (!src.buffer && !src.image))
        return E_INVALIDARG;
      flushPendingBufferUpdates();
      m_backendCtx->copyTileMappings(dst, tileCoordinate(*pDestRegionStartCoordinate),
        src, tileCoordinate(*pSourceRegionStartCoordinate), tileRegionSize(*pTileRegionSize), Flags);
      return S_OK;
    }

    void STDMETHODCALLTYPE CopyTiles(
        ID3D11Resource* pTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
        const D3D11_TILE_REGION_SIZE* pTileRegionSize, ID3D11Buffer* pBuffer,
        UINT64 BufferStartOffsetInBytes, UINT Flags) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pTileRegionStartCoordinate || !pTileRegionSize || !pBuffer)
        return;
      TiledResourceRef resource = tiledResourceRef(pTiledResource);
      if (!resource.buffer && !resource.image)
        return;
      flushPendingBufferUpdates();
      m_backendCtx->copyTiles(resource, tileCoordinate(*pTileRegionStartCoordinate), tileRegionSize(*pTileRegionSize),
        getBackendBuffer(pBuffer), BufferStartOffsetInBytes, Flags);
    }

    void STDMETHODCALLTYPE UpdateTiles(
        ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestTileRegionStartCoordinate,
        const D3D11_TILE_REGION_SIZE* pDestTileRegionSize, const void* pSourceTileData, UINT Flags) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pDestTileRegionStartCoordinate || !pDestTileRegionSize || !pSourceTileData)
        return;
      TiledResourceRef resource = tiledResourceRef(pDestTiledResource);
      if (!resource.buffer && !resource.image)
        return;
      flushPendingBufferUpdates();
      m_backendCtx->updateTiles(resource, tileCoordinate(*pDestTileRegionStartCoordinate),
        tileRegionSize(*pDestTileRegionSize), pSourceTileData, Flags);
    }

    HRESULT STDMETHODCALLTYPE ResizeTilePool(ID3D11Buffer* pTilePool, UINT64 NewSizeInBytes) override {
      DXUP_CONTEXT_MT_GUARD;
      IBackendBuffer* pool = getBackendBuffer(pTilePool);
      if (!pool || !(pool->getDesc().miscFlags & kResourceMiscTilePool))
        return E_INVALIDARG;
      return m_backendCtx->resizeTilePool(pool, NewSizeInBytes) ? S_OK : E_OUTOFMEMORY;
    }

    void STDMETHODCALLTYPE TiledResourceBarrier(
        ID3D11DeviceChild* pTiledResourceOrViewAccessBeforeBarrier,
        ID3D11DeviceChild* pTiledResourceOrViewAccessAfterBarrier) override {
      DXUP_CONTEXT_MT_GUARD;
      (void)pTiledResourceOrViewAccessBeforeBarrier;
      (void)pTiledResourceOrViewAccessAfterBarrier;
      m_backendCtx->tiledResourceBarrier();
    }

    BOOL STDMETHODCALLTYPE IsAnnotationEnabled() override {
      DXUP_CONTEXT_MT_GUARD; return FALSE; }
    void STDMETHODCALLTYPE SetMarkerInt(LPCWSTR pLabel, INT Data) override {
      DXUP_CONTEXT_MT_GUARD; (void)pLabel; (void)Data; }
    void STDMETHODCALLTYPE BeginEventInt(LPCWSTR pLabel, INT Data) override {
      DXUP_CONTEXT_MT_GUARD; (void)pLabel; (void)Data; }
    void STDMETHODCALLTYPE EndEvent() override {
      DXUP_CONTEXT_MT_GUARD; }

    // ============================================================
    // ID3D11DeviceContext3
    // ============================================================

    // There is one queue, so every context type is flushed.
    void STDMETHODCALLTYPE Flush1(D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent) override {
      DXUP_CONTEXT_MT_GUARD;
      (void)ContextType;
      if (m_isDeferred)
        return;
      flushPendingBufferUpdates();
      if (hEvent)
        m_backendCtx->notifyOnCompletion(hEvent);
      else
        m_backendCtx->flush();
    }

    void STDMETHODCALLTYPE SetHardwareProtectionState(BOOL HwProtectionEnable) override {
      DXUP_CONTEXT_MT_GUARD; (void)HwProtectionEnable; }

    void STDMETHODCALLTYPE GetHardwareProtectionState(BOOL* pHwProtectionEnable) override {
      DXUP_CONTEXT_MT_GUARD;
      if (pHwProtectionEnable)
        *pHwProtectionEnable = FALSE;
    }

    // ============================================================
    // ID3D11DeviceContext4: fences, in order with the other commands
    // ============================================================

    HRESULT STDMETHODCALLTYPE Signal(ID3D11Fence* pFence, UINT64 Value) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pFence) return E_INVALIDARG;
      if (m_isDeferred) return DXGI_ERROR_INVALID_CALL;
      flushPendingBufferUpdates();
      m_backendCtx->signalFence(static_cast<D3D11Fence*>(pFence)->GetNativeFence(), Value);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Wait(ID3D11Fence* pFence, UINT64 Value) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pFence) return E_INVALIDARG;
      if (m_isDeferred) return DXGI_ERROR_INVALID_CALL;
      flushPendingBufferUpdates();
      m_backendCtx->waitFence(static_cast<D3D11Fence*>(pFence)->GetNativeFence(), Value);
      return S_OK;
    }

    void STDMETHODCALLTYPE ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect, UINT NumRects) override {
      DXUP_CONTEXT_MT_GUARD;
      if (!pView || !Color)
        return;
      if (NumRects && !pRect)
        return;

      std::vector<Rect> rects(NumRects);
      for (UINT i = 0; i < NumRects; i++)
        rects[i] = { pRect[i].left, pRect[i].top, pRect[i].right, pRect[i].bottom };

      // ClearView works on render target and unordered access views; depth
      // stencil views are not allowed.
      ComPtr<ID3D11RenderTargetView> rtv;
      if (SUCCEEDED(pView->QueryInterface(__uuidof(ID3D11RenderTargetView), reinterpret_cast<void**>(rtv.GetAddressOf())))) {
        auto* view = static_cast<D3D11RenderTargetView*>(rtv.Get());
        if (IBackendImageView* shadow = clearedForcedMsaaShadow(view->GetResourceRaw(), view->GetShadowView(), NumRects == 0))
          m_backendCtx->clearView(shadow, true, Color, NumRects, rects.data());
        if (IBackendImageView* backendRTV = getBackendRTV(rtv.Get()))
          m_backendCtx->clearView(backendRTV, true, Color, NumRects, rects.data());
        return;
      }

      ComPtr<ID3D11UnorderedAccessView> uav;
      if (SUCCEEDED(pView->QueryInterface(__uuidof(ID3D11UnorderedAccessView), reinterpret_cast<void**>(uav.GetAddressOf())))) {
        flushPendingBufferUpdates();
        writeForcedMsaaImage(static_cast<D3D11UnorderedAccessView*>(uav.Get())->GetResourceRaw(), NumRects == 0);
        if (IBackendImageView* imageView = getBackendImageUAV(uav.Get()))
          m_backendCtx->clearView(imageView, false, Color, NumRects, rects.data());
        else if (IBackendBufferView* bufferView = getBackendBufferUAV(uav.Get()))
          m_backendCtx->clearUnorderedAccessViewFloat(bufferView, Color);
      }
    }

  private:

    // ============================================================
    // Internal state tracking structures
    // ============================================================

    struct ShaderStageState {
      ID3D11DeviceChild*          shader = nullptr;
      ID3D11Buffer*               constantBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX] = {};
      UINT                        constantBufferFirst[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX] = {};
      UINT                        constantBufferCount[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX] = {};
      ID3D11ShaderResourceView*   srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX] = {};
      // Slots that views of textures with forced multisampled copies were
      // bound to (some may hold other views since).
      uint64_t                    forcedMsaaSrvs[2] = {};
      // Upper bounds of the bound SRV and UAV slots, so hazard scans skip
      // the empty tails. Only binding raises them; scans shrink them.
      UINT                        srvSlotEnd = 0;
      UINT                        uavSlotEnd = 0;
      // Upper bound of the bound constant buffer slots, raised by binding.
      UINT                        cbSlotEnd = 0;
      ID3D11SamplerState*         samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT_MAX] = {};
      ID3D11UnorderedAccessView*  uavs[D3D11_1_UAV_SLOT_COUNT_MAX] = {};
    };

    struct InputAssemblerState {
      ID3D11InputLayout*          inputLayout = nullptr;
      ID3D11Buffer*               vertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX] = {};
      UINT                        vertexStrides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX] = {};
      UINT                        vertexOffsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX] = {};
      ID3D11Buffer*               indexBuffer  = nullptr;
      DXGI_FORMAT                 indexFormat   = DXGI_FORMAT_UNKNOWN;
      UINT                        indexOffset   = 0;
      D3D11_PRIMITIVE_TOPOLOGY    topology      = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    };

    struct OutputMergerState {
      ID3D11RenderTargetView*     renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX] = {};
      ID3D11DepthStencilView*     depthStencilView  = nullptr;
      UINT                        numRenderTargets   = 0;
      ID3D11BlendState*           blendState         = nullptr;
      FLOAT                       blendFactor[4]     = { 1.0f, 1.0f, 1.0f, 1.0f };
      UINT                        sampleMask         = 0xFFFFFFFF;
      ID3D11DepthStencilState*    depthStencilState  = nullptr;
      UINT                        stencilRef         = 0;
      ID3D11UnorderedAccessView*  uavs[D3D11_PS_CS_UAV_REGISTER_COUNT_MAX] = {};
    };

    struct RasterizerStageState {
      ID3D11RasterizerState*      state = nullptr;
      D3D11_VIEWPORT              viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX] = {};
      UINT                        numViewports = 0;
      D3D11_RECT                  scissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX] = {};
      UINT                        numScissorRects = 0;
    };

    struct StreamOutputState {
      ID3D11Buffer*               targets[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};
      UINT                        offsets[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};
    };

    struct ContextState {
      InputAssemblerState   ia;
      ShaderStageState      vs;
      ShaderStageState      hs;
      ShaderStageState      ds;
      ShaderStageState      gs;
      ShaderStageState      ps;
      ShaderStageState      cs;
      OutputMergerState     om;
      RasterizerStageState  rs;
      StreamOutputState     so;
      ID3D11Predicate*      predicate = nullptr;
      BOOL                  predicateValue = FALSE;
    };

    // Every bound object is held through a private reference: the binding
    // keeps it alive after the application releases it, as native D3D11
    // does, but does not keep the device alive.
    template<typename Impl, typename T>
    static bool replaceBinding(T*& slot, T* value) {
      if (slot == value)
        return false;

      if (value)
        static_cast<Impl*>(value)->AddRefPrivate();

      T* previous = slot;
      slot = value;

      if (previous)
        static_cast<Impl*>(previous)->ReleasePrivate();

      return true;
    }

    // Objects about to be bound in place of others: their reference count
    // lines load in parallel instead of one miss after the other.
    template<typename Impl, typename T>
    static void prefetchForBind(T* const* objects, T* const* slots, UINT start, UINT count, UINT slotCount) {
      if (!objects || count < 2)
        return;
      for (UINT i = 0; i < count && start + i < slotCount; i++) {
        T* object = objects[i];
        if (object && object != slots[start + i])
          static_cast<const Impl*>(object)->prefetchForBind();
      }
    }

    template<typename T>
    static bool replaceContextReference(T*& slot, T* value) {
      return replaceBinding<typename D3D11ImplOf<T>::Type>(slot, value);
    }

    template<typename T>
    static void clearContextReference(T*& slot) {
      replaceContextReference(slot, static_cast<T*>(nullptr));
    }

    template<typename Impl>
    static void releaseShaderStageBindings(ShaderStageState& stage) {
      replaceBinding<Impl>(stage.shader, static_cast<ID3D11DeviceChild*>(nullptr));
      for (auto*& buffer : stage.constantBuffers)
        clearContextReference(buffer);
      for (auto*& srv : stage.srvs)
        clearContextReference(srv);
      for (auto*& sampler : stage.samplers)
        clearContextReference(sampler);
      for (auto*& uav : stage.uavs)
        clearContextReference(uav);
    }

    void releaseAllBindings() {
      releaseStateBindings(m_state);
    }

  public:
    // Context state held by an inactive ID3DDeviceContextState.
    using SavedState = ContextState;

    static void releaseStateBindings(ContextState& state) {
      clearContextReference(state.ia.inputLayout);
      for (auto*& buffer : state.ia.vertexBuffers)
        clearContextReference(buffer);
      clearContextReference(state.ia.indexBuffer);

      releaseShaderStageBindings<D3D11VertexShader>(state.vs);
      releaseShaderStageBindings<D3D11HullShader>(state.hs);
      releaseShaderStageBindings<D3D11DomainShader>(state.ds);
      releaseShaderStageBindings<D3D11GeometryShader>(state.gs);
      releaseShaderStageBindings<D3D11PixelShader>(state.ps);
      releaseShaderStageBindings<D3D11ComputeShader>(state.cs);

      for (auto*& rtv : state.om.renderTargets)
        clearContextReference(rtv);
      clearContextReference(state.om.depthStencilView);
      clearContextReference(state.om.blendState);
      clearContextReference(state.om.depthStencilState);
      for (auto*& uav : state.om.uavs)
        clearContextReference(uav);

      clearContextReference(state.rs.state);

      for (auto*& target : state.so.targets)
        clearContextReference(target);

      if (state.predicate) {
        reinterpret_cast<D3D11Query*>(state.predicate)->ReleasePrivate();
        state.predicate = nullptr;
      }
    }

  private:

    // ============================================================
    // Member variables
    // ============================================================

    std::atomic<ULONG>  m_refCount;
    D3D11Device*        m_device;
    Rc<IBackendContext>  m_backendCtx;
    ComPrivateData      m_privateData;
    ContextState        m_state;
    // The device keeps its pipelines; the context only points at them.
    IBackendPipeline*    m_currentPipeline = nullptr;
    // Graphics pipelines this context bound, found again by key.
    GraphicsPipelineLookup m_pipelineLookup;
    // The bound render target and depth views draw into forced multisampled
    // copies (see wantsForcedMsaa), and whether that is to be decided again.
    bool                 m_omForcedMsaa = false;
    bool                 m_msaaScopeDirty = false;
    bool                 m_forcedMsaaPassRan = false;
    // Deferred contexts: textures with forced multisampled copies the
    // recorded commands use.
    std::vector<RecordedMsaaUse> m_recordedMsaaUses;
    bool                 m_currentPipelineIsCompute = false;
    bool                 m_pipelineDirty = true;
    bool                 m_isDeferred = false;
    // Whether the backend can ask for the output merger state again (see
    // IBackendContext::outputMergerStateNeedsRebind).
    bool                 m_outputMergerRebinds = true;
    // d3d11InfoLoggingEnabled, read once for the draw paths.
    bool                 m_infoLogging = false;
    const UserOverrides& m_overrides = UserOverrides::get();
    // d3d11.upscalerAuto (immediate context only).
    std::unique_ptr<AutoTaa> m_autoTaa;
    // d3d11.upscalerInject (immediate context only); whether the backend has
    // the viewports shifted by the jitter, and of which frame.
    std::unique_ptr<InjectTaa> m_injectTaa;
    bool     m_injectViewportJittered = false;
    uint64_t m_injectJitterFrame = 0;
    // The worker ran the upscaler last frame: only then are scene draws
    // jittered. The scene color the upscaler was set up for.
    std::shared_ptr<std::atomic<bool>> m_injectReady = std::make_shared<std::atomic<bool>>(false);
    ID3D11Resource* m_injectPreparedFor = nullptr;
    // Buffer contents a deferred context replaced (WRITE_DISCARD, whole
    // UpdateSubresource), for its command list.
    std::vector<RecordedDiscard> m_recordedDiscards;
    // Deferred contexts: buffers discarded while recording, kept alive by
    // m_recordedDiscards.
    std::unordered_map<D3D11Buffer*, Rc<IBackendBuffer>> m_bufferOverrides;
    // State object whose state is active; created on the first swap.
    D3D11DeviceChild<ID3DDeviceContextState>* m_activeContextState = nullptr;

  public:
    IBackendContext* getBackendCtx() const { return m_backendCtx.get(); }
  private:

    // Staging buffers for texture Map/Unmap
    struct TextureStagingInfo {
      Rc<IBackendBuffer>  stagingBuffer;
      IBackendImage*      image;
      uint32_t            subresource;
      MapMode             mapMode;
      uint32_t            rowPitch;
      uint32_t            depthPitch;
    };
    std::unordered_map<ID3D11Resource*, TextureStagingInfo> m_textureStagingBuffers;

    struct BufferMapInfo {
      D3D11Buffer* buffer = nullptr;
      MapMode      mapMode = MapMode::Read;
      void*        data = nullptr;
    };
    std::unordered_map<ID3D11Resource*, BufferMapInfo> m_bufferMappings;

    struct PendingBufferUpdateRange {
      uint64_t offset = 0;
      std::vector<uint8_t> data;
    };

    struct PendingBufferUpdate {
      D3D11Buffer* buffer = nullptr;
      std::vector<PendingBufferUpdateRange> ranges;
    };
    std::vector<PendingBufferUpdate> m_pendingBufferUpdates;
    uint64_t m_pendingBufferUpdateBytes = 0;

    // ============================================================
    // Pipeline compilation - builds D3D12 PSO from D3D11 state
    // ============================================================

    // UpdateSubresource takes default-usage resources that are neither
    // depth-stencil nor multisampled; D3D11 drops the others.
    static bool isUpdatableResource(ID3D11Resource* resource, D3D11_RESOURCE_DIMENSION dim) {
      D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
      UINT bindFlags = 0;
      UINT samples = 1;
      switch (dim) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
          const auto& desc = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(resource))->GetDescRef();
          usage = desc.Usage;
          bindFlags = desc.BindFlags;
          break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
          D3D11_TEXTURE1D_DESC desc = {};
          static_cast<ID3D11Texture1D*>(resource)->GetDesc(&desc);
          usage = desc.Usage;
          bindFlags = desc.BindFlags;
          break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
          D3D11_TEXTURE2D_DESC desc = {};
          static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
          usage = desc.Usage;
          bindFlags = desc.BindFlags;
          samples = desc.SampleDesc.Count;
          break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
          D3D11_TEXTURE3D_DESC desc = {};
          static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
          usage = desc.Usage;
          bindFlags = desc.BindFlags;
          break;
        }
        default:
          return false;
      }
      return usage == D3D11_USAGE_DEFAULT && !(bindFlags & D3D11_BIND_DEPTH_STENCIL) && samples <= 1;
    }

    // Diagnostics: the D3D11 state a draw runs with.
    // Diagnostics (dxup_frame_dump.txt): views with the part of the
    // resource they cover, and calls besides draws.
    static std::string traceView(ID3D11View* view, bool uav) {
      if (!view)
        return "null";
      ComPtr<ID3D11Resource> resource;
      view->GetResource(resource.GetAddressOf());
      char out[200];
      if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d = {};
        static_cast<ID3D11UnorderedAccessView*>(view)->GetDesc(&d);
        sprintf_s(out, "uav %p of %p fmt=%u dim=%u args=%u,%u,%u flags=0x%x", static_cast<void*>(view),
          static_cast<void*>(resource.Get()), unsigned(d.Format), unsigned(d.ViewDimension),
          d.Buffer.FirstElement, d.Buffer.NumElements, d.Texture2DArray.ArraySize, d.Buffer.Flags);
      } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
        static_cast<ID3D11ShaderResourceView*>(view)->GetDesc(&d);
        sprintf_s(out, "srv %p of %p fmt=%u dim=%u args=%u,%u,%u,%u", static_cast<void*>(view),
          static_cast<void*>(resource.Get()), unsigned(d.Format), unsigned(d.ViewDimension),
          d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels, d.Texture2DArray.FirstArraySlice,
          d.Texture2DArray.ArraySize);
      }
      return out;
    }

    void traceStage(std::string& line, const char* name, const ShaderStageState& state, bool uavs) {
      for (UINT i = 0; i < 16; i++) {
        if (state.srvs[i])
          line += formatTrace(" %s.t%u(%s)", name, i, traceView(state.srvs[i], false).c_str());
      }
      if (uavs) {
        for (UINT i = 0; i < 8; i++) {
          if (state.uavs[i])
            line += formatTrace(" %s.u%u(%s)", name, i, traceView(state.uavs[i], true).c_str());
        }
      }
    }

    template<typename... Args>
    static std::string formatTrace(const char* format, Args... args) {
      char text[512];
      sprintf_s(text, format, args...);
      return text;
    }

    void traceCall(const std::string& line) {
      if (m_probing)
        return;
      const uint32_t index = ++m_device->diagTraceIndex;
      logMessage(LogLevel::Warn, "draw trace%s: #%u %s", m_isDeferred ? " (deferred)" : "", index, line.c_str());
    }

    // With dxup_dump_calls.txt (trace indices), the first render target and
    // the textures the pixel shader reads are written as BMPs before each
    // listed call.
    void dumpCallTargets(uint32_t index) {
      static const std::vector<uint32_t> calls = [] {
        std::vector<uint32_t> list;
        char path[MAX_PATH] = {};
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&d3d11InfoLoggingEnabled), &module);
        if (module && GetModuleFileNameA(module, path, MAX_PATH) && strrchr(path, '\\'))
          strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), "dxup_dump_calls.txt");
        if (path[0]) {
          if (FILE* file = fopen(path, "r")) {
            unsigned value = 0;
            while (fscanf_s(file, "%u", &value) == 1)
              list.push_back(value);
            fclose(file);
          }
        }
        return list;
      }();
      if (m_isDeferred || m_probing || std::find(calls.begin(), calls.end(), index) == calls.end())
        return;
      if (m_state.om.renderTargets[0]) {
        ComPtr<ID3D11Resource> resource;
        m_state.om.renderTargets[0]->GetResource(resource.GetAddressOf());
        dumpTexture(resource.Get(), formatTrace("dxup_call_%u_rt0.bmp", index).c_str());
      }
      for (UINT slot = 0; slot < 16; slot++) {
        if (!m_state.ps.srvs[slot])
          continue;
        ComPtr<ID3D11Resource> resource;
        m_state.ps.srvs[slot]->GetResource(resource.GetAddressOf());
        dumpTexture(resource.Get(), formatTrace("dxup_call_%u_ps%u.bmp", index, slot).c_str());
      }
    }

    static float halfToFloat(uint16_t half) {
      const uint32_t sign = uint32_t(half & 0x8000) << 16;
      const uint32_t exponent = (half >> 10) & 0x1f;
      const uint32_t mantissa = half & 0x3ff;
      if (!exponent)
        return (half & 0x8000 ? -1.0f : 1.0f) * float(mantissa) * (1.0f / 16777216.0f);
      const uint32_t bits = exponent == 31
        ? sign | 0x7f800000 | (mantissa << 13)
        : sign | ((exponent + 112) << 23) | (mantissa << 13);
      float value;
      std::memcpy(&value, &bits, 4);
      return value;
    }

    void dumpTexture(ID3D11Resource* resource, const char* name) {
      D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dim);
      if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return;
      D3D11_TEXTURE2D_DESC desc = {};
      static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
      if (desc.SampleDesc.Count != 1 || desc.Width < 256)
        return;
      const uint32_t texelBytes = getFormatBpp(desc.Format);
      if (texelBytes != 4 && texelBytes != 8)
        return;
      D3D11_TEXTURE2D_DESC stagingDesc = { desc.Width, desc.Height, 1, 1, desc.Format, { 1, 0 },
        D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
      ComPtr<ID3D11Texture2D> staging;
      if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf())))
        return;
      m_probing = true;
      CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resource, 0, nullptr);
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (SUCCEEDED(Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        char path[MAX_PATH] = {};
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&d3d11InfoLoggingEnabled), &module);
        if (module && GetModuleFileNameA(module, path, MAX_PATH) && strrchr(path, '\\'))
          strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), name);
        if (FILE* file = path[0] ? fopen(path, "wb") : nullptr) {
          const uint32_t imageSize = desc.Width * desc.Height * 4;
          BITMAPFILEHEADER fileHeader = {};
          BITMAPINFOHEADER infoHeader = {};
          fileHeader.bfType = 0x4D42;
          fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
          fileHeader.bfSize = fileHeader.bfOffBits + imageSize;
          infoHeader.biSize = sizeof(infoHeader);
          infoHeader.biWidth = LONG(desc.Width);
          infoHeader.biHeight = -LONG(desc.Height);
          infoHeader.biPlanes = 1;
          infoHeader.biBitCount = 32;
          infoHeader.biSizeImage = imageSize;
          fwrite(&fileHeader, sizeof(fileHeader), 1, file);
          fwrite(&infoHeader, sizeof(infoHeader), 1, file);
          auto toByte = [](float value) {
            value = value != value ? 0.0f : (std::min)((std::max)(value, 0.0f), 1.0f);
            return uint8_t(std::pow(value, 1.0f / 2.2f) * 255.0f + 0.5f);
          };
          std::vector<uint8_t> row(desc.Width * 4);
          for (UINT y = 0; y < desc.Height; y++) {
            const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; x++) {
              float rgb[3] = {};
              if (texelBytes == 8) {
                const uint16_t* halves = reinterpret_cast<const uint16_t*>(src + x * 8);
                const bool unorm = desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM;
                for (int c = 0; c < 3; c++)
                  rgb[c] = unorm ? halves[c] / 65535.0f : halfToFloat(halves[c]);
              } else {
                uint32_t word = 0;
                std::memcpy(&word, src + x * 4, 4);
                switch (desc.Format) {
                  case DXGI_FORMAT_R11G11B10_FLOAT:
                    // Small floats moved into the half layout.
                    rgb[0] = halfToFloat(uint16_t((word & 0x7ff) << 4));
                    rgb[1] = halfToFloat(uint16_t(((word >> 11) & 0x7ff) << 4));
                    rgb[2] = halfToFloat(uint16_t(((word >> 22) & 0x3ff) << 5));
                    break;
                  case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
                    rgb[0] = halfToFloat(uint16_t(word));
                    rgb[1] = halfToFloat(uint16_t(word >> 16));
                    break;
                  case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: {
                    float value;
                    std::memcpy(&value, &word, 4);
                    rgb[0] = rgb[1] = rgb[2] = value;
                    break;
                  }
                  case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
                    rgb[0] = (word & 0x3ff) / 1023.0f;
                    rgb[1] = ((word >> 10) & 0x3ff) / 1023.0f;
                    rgb[2] = ((word >> 20) & 0x3ff) / 1023.0f;
                    break;
                  default:
                    // 8-bit channels: the stored values as they are.
                    for (int c = 0; c < 3; c++)
                      rgb[c] = std::pow(((word >> (c * 8)) & 0xff) / 255.0f, 2.2f);
                    break;
                }
              }
              row[x * 4 + 0] = toByte(rgb[2]);
              row[x * 4 + 1] = toByte(rgb[1]);
              row[x * 4 + 2] = toByte(rgb[0]);
              row[x * 4 + 3] = 255;
            }
            fwrite(row.data(), row.size(), 1, file);
          }
          fclose(file);
        }
        Unmap(staging.Get(), 0);
      }
      m_probing = false;
    }

    // With dxup_probe_pixel.txt ("x y" in 3840x2160 coordinates), every
    // traced call also logs that pixel of the first render target: the
    // effect of the calls before it.
    void probeRenderTarget(std::string& line) {
      static const POINT probe = [] {
        POINT point = { -1, -1 };
        char path[MAX_PATH] = {};
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&d3d11InfoLoggingEnabled), &module);
        if (module && GetModuleFileNameA(module, path, MAX_PATH) && strrchr(path, '\\')) {
          strcpy_s(strrchr(path, '\\') + 1, MAX_PATH - (strrchr(path, '\\') + 1 - path), "dxup_probe_pixel.txt");
          if (FILE* file = fopen(path, "r")) {
            if (fscanf_s(file, "%ld %ld", &point.x, &point.y) != 2)
              point = { -1, -1 };
            fclose(file);
          }
        }
        return point;
      }();
      if (probe.x < 0 || m_isDeferred || m_probing)
        return;
      // Full-resolution textures the pixel shader reads, at the same pixel.
      for (UINT slot = 0; slot < 16; slot++) {
        if (!m_state.ps.srvs[slot])
          continue;
        ComPtr<ID3D11Resource> srvResource;
        m_state.ps.srvs[slot]->GetResource(srvResource.GetAddressOf());
        probeTexture(line, formatTrace(" ps.t%u", slot).c_str(), srvResource.Get(), probe, true);
      }
      // The first texture's mips: how much data each holds.
      if (m_state.ps.srvs[0]) {
        ComPtr<ID3D11Resource> srvResource;
        m_state.ps.srvs[0]->GetResource(srvResource.GetAddressOf());
        probeMips(line, srvResource.Get());
      }
      if (!m_state.om.renderTargets[0])
        return;
      ComPtr<ID3D11Resource> resource;
      m_state.om.renderTargets[0]->GetResource(resource.GetAddressOf());
      probeTexture(line, " probe", resource.Get(), probe, false);
    }

    void probeMips(std::string& line, ID3D11Resource* resource) {
      D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dim);
      if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return;
      D3D11_TEXTURE2D_DESC desc = {};
      static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
      if (desc.Width > 512 || desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
        return;
      D3D11_TEXTURE2D_DESC stagingDesc = desc;
      stagingDesc.Usage = D3D11_USAGE_STAGING;
      stagingDesc.BindFlags = 0;
      stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      stagingDesc.MiscFlags = 0;
      ComPtr<ID3D11Texture2D> staging;
      if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf())))
        return;
      m_probing = true;
      CopyResource(staging.Get(), resource);
      line += " mips(";
      for (UINT mip = 0; mip < desc.MipLevels; mip++) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(Map(staging.Get(), mip, D3D11_MAP_READ, 0, &mapped)))
          continue;
        const UINT rows = (std::max)(1u, (desc.Height >> mip) / 4u);
        uint64_t sum = 0, nonzero = 0;
        for (UINT row = 0; row < rows; row++) {
          const uint8_t* bytes = static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
          const UINT rowBytes = (std::min)(mapped.RowPitch, UINT(((std::max)(1u, (desc.Width >> mip) / 4u)) * 16u));
          for (UINT i = 0; i < rowBytes; i++) {
            sum += bytes[i];
            nonzero += bytes[i] != 0;
          }
        }
        line += formatTrace("%s%u:%llu/%llu", mip ? " " : "", mip, static_cast<unsigned long long>(nonzero),
          static_cast<unsigned long long>(sum));
        Unmap(staging.Get(), mip);
      }
      line += ")";
      m_probing = false;
    }

    void probeTexture(std::string& line, const char* name, ID3D11Resource* resource, POINT probe, bool fullResolutionOnly) {
      D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dim);
      if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return;
      D3D11_TEXTURE2D_DESC desc = {};
      static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
      if (desc.SampleDesc.Count != 1 || (fullResolutionOnly && desc.Width != 3840))
        return;
      const UINT x = UINT(probe.x) * desc.Width / 3840, y = UINT(probe.y) * desc.Height / 2160;
      D3D11_TEXTURE2D_DESC stagingDesc = { 1, 1, 1, 1, desc.Format, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
      ComPtr<ID3D11Texture2D> staging;
      if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf())))
        return;
      m_probing = true;
      const D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
      CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resource, 0, &box);
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (SUCCEEDED(Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        const auto* words = static_cast<const uint32_t*>(mapped.pData);
        line += formatTrace("%s(%p@%u,%u=%08x %08x)", name, static_cast<void*>(resource), x, y, words[0], words[1]);
        Unmap(staging.Get(), 0);
      }
      m_probing = false;
    }
    bool m_probing = false;

    // The arguments an indirect call finds in its buffer when it is
    // recorded (the GPU work before it finished).
    std::string traceIndirectArgs(ID3D11Buffer* buffer, UINT offset, UINT count) {
      if (!buffer || m_isDeferred || m_probing)
        return " args(?)";
      D3D11_BUFFER_DESC stagingDesc = { count * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
      ComPtr<ID3D11Buffer> staging;
      if (FAILED(m_device->CreateBuffer(&stagingDesc, nullptr, staging.GetAddressOf())))
        return " args(?)";
      m_probing = true;
      const D3D11_BOX box = { offset, 0, 0, offset + count * 4, 1, 1 };
      CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buffer, 0, &box);
      std::string text = formatTrace(" args(%p+%u:", static_cast<void*>(buffer), offset);
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (SUCCEEDED(Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        for (UINT i = 0; i < count; i++)
          text += formatTrace(" %d", static_cast<const int*>(mapped.pData)[i]);
        Unmap(staging.Get(), 0);
      }
      m_probing = false;
      return text + ")";
    }

    void traceDrawState(const char* kind) {
      if (m_probing)
        return;
      dumpCallTargets(m_device->diagTraceIndex.load(std::memory_order_relaxed) + 1);
      std::string line = kind;
      probeRenderTarget(line);
      char text[512];
      auto describeResource = [&](ID3D11Resource* resource) {
        if (!resource)
          return std::string("null");
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        resource->GetType(&dim);
        char out[160];
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
          D3D11_TEXTURE2D_DESC d = {};
          static_cast<ID3D11Texture2D*>(resource)->GetDesc(&d);
          sprintf_s(out, "tex2d %p %ux%u fmt=%u mips=%u arr=%u usage=%u bind=0x%x",
            static_cast<void*>(resource), d.Width, d.Height, static_cast<unsigned>(d.Format), d.MipLevels, d.ArraySize,
            static_cast<unsigned>(d.Usage), d.BindFlags);
        } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
          D3D11_BUFFER_DESC d = {};
          static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
          sprintf_s(out, "buf %p size=%u usage=%u bind=0x%x", static_cast<void*>(resource), d.ByteWidth,
            static_cast<unsigned>(d.Usage), d.BindFlags);
        } else {
          sprintf_s(out, "res %p dim=%u", static_cast<void*>(resource), static_cast<unsigned>(dim));
        }
        return std::string(out);
      };

      sprintf_s(text, " topo=%u il=%p vb0=%p stride=%u off=%u ib=%p ibfmt=%u",
        static_cast<unsigned>(m_state.ia.topology), static_cast<void*>(m_state.ia.inputLayout),
        static_cast<void*>(m_state.ia.vertexBuffers[0]), m_state.ia.vertexStrides[0], m_state.ia.vertexOffsets[0],
        static_cast<void*>(m_state.ia.indexBuffer), static_cast<unsigned>(m_state.ia.indexFormat));
      line += text;
      const D3D11_VIEWPORT& vp = m_state.rs.viewports[0];
      sprintf_s(text, " vp(%u)=%.0f,%.0f %.0fx%.0f z=%.2f..%.2f sc(%u)=%ld,%ld,%ld,%ld",
        m_state.rs.numViewports, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth,
        m_state.rs.numScissorRects, m_state.rs.scissorRects[0].left, m_state.rs.scissorRects[0].top,
        m_state.rs.scissorRects[0].right, m_state.rs.scissorRects[0].bottom);
      line += text;
      if (m_state.rs.state) {
        D3D11_RASTERIZER_DESC r = {};
        m_state.rs.state->GetDesc(&r);
        sprintf_s(text, " rs(cull=%u fill=%u ccw=%u scissor=%u clip=%u)", static_cast<unsigned>(r.CullMode),
          static_cast<unsigned>(r.FillMode), r.FrontCounterClockwise, r.ScissorEnable, r.DepthClipEnable);
        line += text;
      } else {
        line += " rs(default)";
      }
      if (m_state.om.blendState) {
        D3D11_BLEND_DESC b = {};
        m_state.om.blendState->GetDesc(&b);
        const auto& rt = b.RenderTarget[0];
        sprintf_s(text, " blend(en=%u src=%u dst=%u op=%u srcA=%u dstA=%u opA=%u mask=0x%x a2c=%u) factor=%.2f,%.2f,%.2f,%.2f smask=0x%x",
          rt.BlendEnable, static_cast<unsigned>(rt.SrcBlend), static_cast<unsigned>(rt.DestBlend),
          static_cast<unsigned>(rt.BlendOp), static_cast<unsigned>(rt.SrcBlendAlpha),
          static_cast<unsigned>(rt.DestBlendAlpha), static_cast<unsigned>(rt.BlendOpAlpha),
          rt.RenderTargetWriteMask, b.AlphaToCoverageEnable,
          m_state.om.blendFactor[0], m_state.om.blendFactor[1], m_state.om.blendFactor[2], m_state.om.blendFactor[3],
          m_state.om.sampleMask);
        line += text;
      } else {
        line += " blend(default)";
      }
      if (m_state.om.depthStencilState) {
        D3D11_DEPTH_STENCIL_DESC d = {};
        m_state.om.depthStencilState->GetDesc(&d);
        sprintf_s(text, " ds(depth=%u write=%u func=%u stencil=%u func=%u pass=%u ref=%u)", d.DepthEnable,
          static_cast<unsigned>(d.DepthWriteMask), static_cast<unsigned>(d.DepthFunc), d.StencilEnable,
          static_cast<unsigned>(d.FrontFace.StencilFunc), static_cast<unsigned>(d.FrontFace.StencilPassOp),
          m_state.om.stencilRef);
        line += text;
      } else {
        line += " ds(default)";
      }
      for (UINT i = 0; i < m_state.om.numRenderTargets && i < 2; i++) {
        ComPtr<ID3D11Resource> resource;
        if (m_state.om.renderTargets[i]) {
          m_state.om.renderTargets[i]->GetResource(resource.GetAddressOf());
          D3D11_RENDER_TARGET_VIEW_DESC v = {};
          m_state.om.renderTargets[i]->GetDesc(&v);
          sprintf_s(text, " rt%u(viewfmt=%u %s)", i, static_cast<unsigned>(v.Format), describeResource(resource.Get()).c_str());
        } else {
          sprintf_s(text, " rt%u(null)", i);
        }
        line += text;
      }
      if (m_state.om.depthStencilView) {
        ComPtr<ID3D11Resource> resource;
        m_state.om.depthStencilView->GetResource(resource.GetAddressOf());
        D3D11_DEPTH_STENCIL_VIEW_DESC v = {};
        m_state.om.depthStencilView->GetDesc(&v);
        sprintf_s(text, " dsv(viewfmt=%u flags=%u %s)", static_cast<unsigned>(v.Format), v.Flags,
          describeResource(resource.Get()).c_str());
        line += text;
      }
      for (UINT i = 0; i < 4; i++) {
        if (!m_state.ps.srvs[i])
          continue;
        ComPtr<ID3D11Resource> resource;
        m_state.ps.srvs[i]->GetResource(resource.GetAddressOf());
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        m_state.ps.srvs[i]->GetDesc(&v);
        sprintf_s(text, " ps.t%u(viewfmt=%u %s)", i, static_cast<unsigned>(v.Format), describeResource(resource.Get()).c_str());
        line += text;
      }
      for (UINT i = 0; i < 4; i++) {
        if (m_state.ps.constantBuffers[i]) {
          sprintf_s(text, " ps.cb%u(%s first=%u count=%u)", i, describeResource(m_state.ps.constantBuffers[i]).c_str(),
            m_state.ps.constantBufferFirst[i], m_state.ps.constantBufferCount[i]);
          line += text;
        }
        if (m_state.vs.constantBuffers[i]) {
          sprintf_s(text, " vs.cb%u(%s first=%u count=%u)", i, describeResource(m_state.vs.constantBuffers[i]).c_str(),
            m_state.vs.constantBufferFirst[i], m_state.vs.constantBufferCount[i]);
          line += text;
        }
      }
      traceStage(line, "vs", m_state.vs, false);
      traceStage(line, "ps", m_state.ps, false);
      for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT_MAX; i++) {
        if (m_state.om.uavs[i])
          line += formatTrace(" om.u%u(%s)", i, traceView(m_state.om.uavs[i], true).c_str());
      }
      sprintf_s(text, " vs=%p ps=%p gs=%p pred=%p", static_cast<void*>(m_state.vs.shader),
        static_cast<void*>(m_state.ps.shader), static_cast<void*>(m_state.gs.shader),
        static_cast<void*>(m_state.predicate));
      line += text;
      traceCall(line);
    }

    // D3D11's blend, rasterizer and depth-stencil state where no state
    // object is bound.
    static void initDefaultPipelineStates(GraphicsPipelineDesc& desc) {
      desc.blendState.alphaToCoverageEnable = false;
      desc.blendState.independentBlendEnable = false;
      for (auto& renderTarget : desc.blendState.renderTargets) {
        renderTarget.blendEnable = false;
        renderTarget.srcBlend = BlendFactor::One;
        renderTarget.dstBlend = BlendFactor::Zero;
        renderTarget.blendOp = BlendOp::Add;
        renderTarget.srcBlendAlpha = BlendFactor::One;
        renderTarget.dstBlendAlpha = BlendFactor::Zero;
        renderTarget.blendOpAlpha = BlendOp::Add;
        renderTarget.writeMask = 0x0F;
      }
      desc.rasterizerState.fillMode = FillMode::Solid;
      desc.rasterizerState.cullMode = CullMode::Back;
      desc.rasterizerState.frontCounterClockwise = false;
      desc.rasterizerState.depthBias = 0;
      desc.rasterizerState.depthBiasClamp = 0.0f;
      desc.rasterizerState.slopeScaledDepthBias = 0.0f;
      desc.rasterizerState.depthClipEnable = true;
      desc.rasterizerState.scissorEnable = false;
      desc.rasterizerState.multisampleEnable = false;
      desc.rasterizerState.antialiasedLineEnable = false;
      desc.depthStencilState.depthEnable = true;
      desc.depthStencilState.depthWriteEnable = true;
      desc.depthStencilState.depthFunc = ComparisonFunc::Less;
      desc.depthStencilState.stencilEnable = false;
      desc.depthStencilState.stencilReadMask = 0xFF;
      desc.depthStencilState.stencilWriteMask = 0xFF;
      desc.depthStencilState.frontFace = StencilOpDesc{};
      desc.depthStencilState.backFace = StencilOpDesc{};
    }

    // The hashes of the default states, which never change.
    struct DefaultPipelineStateHashes {
      uint64_t blend;
      uint64_t rasterizer;
      uint64_t depthStencil;
    };

    static const DefaultPipelineStateHashes& defaultPipelineStateHashes() {
      static const DefaultPipelineStateHashes hashes = [] {
        GraphicsPipelineDesc desc = {};
        initDefaultPipelineStates(desc);
        return DefaultPipelineStateHashes{
          state_hash::blend(desc.blendState),
          state_hash::rasterizer(desc.rasterizerState),
          state_hash::depthStencil(desc.depthStencilState) };
      }();
      return hashes;
    }

    // The key of the graphics pipeline the bound state needs, made of the
    // same values preparePipeline builds the pipeline from. False without a
    // vertex shader, or with stream output, whose declaration the key does
    // not hold.
    bool buildGraphicsPipelineKey(GraphicsPipelineKey& key) const {
      key.clear();
      auto* vsObject = static_cast<D3D11VertexShader*>(m_state.vs.shader);
      if (!vsObject || !vsObject->GetBackendShader())
        return false;
      auto setShader = [&key](uint32_t index, const ShaderIdentity& identity) {
        key.shaderHashes[index] = identity.hash;
        key.shaderSizes[index] = identity.size;
      };
      setShader(0, vsObject->GetIdentity());
      if (m_state.ps.shader)
        setShader(1, static_cast<D3D11PixelShader*>(m_state.ps.shader)->GetIdentity());
      if (m_state.gs.shader) {
        auto* gsObject = static_cast<D3D11GeometryShader*>(m_state.gs.shader);
        if (gsObject->HasStreamOutput())
          return false;
        setShader(2, gsObject->GetIdentity());
      }
      if (m_state.hs.shader)
        setShader(3, static_cast<D3D11HullShader*>(m_state.hs.shader)->GetIdentity());
      if (m_state.ds.shader)
        setShader(4, static_cast<D3D11DomainShader*>(m_state.ds.shader)->GetIdentity());

      if (m_state.ia.inputLayout) {
        auto* layout = static_cast<D3D11InputLayout*>(m_state.ia.inputLayout);
        key.inputLayoutHash = layout->GetContentHash();
        key.inputElementCount = static_cast<uint32_t>(layout->GetBackendElements().size());
      }

      const DefaultPipelineStateHashes& defaults = defaultPipelineStateHashes();
      key.blendHash = m_state.om.blendState
        ? static_cast<D3D11BlendState*>(m_state.om.blendState)->GetBackendHash()
        : defaults.blend;
      key.rasterizerHash = m_state.rs.state
        ? static_cast<D3D11RasterizerState*>(m_state.rs.state)->GetBackendHash()
        : defaults.rasterizer;
      key.depthStencilHash = m_state.om.depthStencilState
        ? static_cast<D3D11DepthStencilState*>(m_state.om.depthStencilState)->GetBackendHash()
        : defaults.depthStencil;
      key.sampleMask = m_state.om.sampleMask;

      const PrimitiveTopology topology = convertTopology(m_state.ia.topology);
      key.topology = static_cast<uint8_t>(topology);
      key.stripCutIndexType = static_cast<uint8_t>(stripCutIndexType(topology));

      UINT activeRenderTargetCount = m_state.om.numRenderTargets;
      while (activeRenderTargetCount && !m_state.om.renderTargets[activeRenderTargetCount - 1])
        activeRenderTargetCount--;
      key.renderTargetCount = static_cast<uint8_t>(activeRenderTargetCount);
      uint32_t sampleCount = 0;
      for (UINT i = 0; i < activeRenderTargetCount; i++) {
        if (auto* view = static_cast<D3D11RenderTargetView*>(m_state.om.renderTargets[i])) {
          key.rtvFormats[i] = view->GetPipelineFormat();
          if (!sampleCount)
            sampleCount = m_omForcedMsaa ? view->GetShadowSampleCount() : view->GetSampleCount();
        }
      }
      if (auto* view = static_cast<D3D11DepthStencilView*>(m_state.om.depthStencilView)) {
        key.hasDepthStencilView = 1;
        key.dsvFormat = view->GetPipelineFormat();
        const uint32_t viewSamples = m_omForcedMsaa ? view->GetShadowSampleCount() : view->GetSampleCount();
        if (sampleCount <= 1 && viewSamples)
          sampleCount = viewSamples;
      }
      key.sampleCount = sampleCount ? sampleCount : 1u;
      key.sampleShading = pipelineSampleShading(key.sampleCount) ? 1u : 0u;
      return true;
    }

    // Whether dxup.conf supersamples draws of the current state: pixel
    // shaders run per sample on multisampled targets, where that changes
    // the result (inputs besides the position are interpolated) and nothing
    // else changes meaning (unordered access writes, the coverage mask). The
    // alpha mode only supersamples shaders that discard pixels; under the
    // geometry scope forced copies only supersample depth-tested geometry.
    // With a depth buffer bound, passes over the screen stay per pixel (see
    // supersamplesWithDepth).
    bool pipelineSampleShading(uint32_t sampleCount) const {
      const UserOverrides& overrides = m_overrides;
      if (overrides.sgssaa == SgssaaMode::Off || sampleCount <= 1 || !m_state.ps.shader)
        return false;
      if (m_omForcedMsaa && overrides.msaaScope == MsaaScope::Geometry && !m_state.om.depthStencilView)
        return false;
      const IBackendShader* ps = static_cast<D3D11PixelShader*>(m_state.ps.shader)->GetBackendShader();
      const uint32_t traits = ps ? ps->getPixelShaderTraits() : 0u;
      if (!(traits & PixelShaderInterpolates) || (traits & (PixelShaderWritesUavs | PixelShaderReadsCoverage)))
        return false;
      if (!(traits & PixelShaderDiscards) && m_state.om.depthStencilView && !supersamplesWithDepth())
        return false;
      return overrides.sgssaa == SgssaaMode::Full || (traits & PixelShaderDiscards);
    }

    // With a depth buffer bound, draws are supersampled when they shade
    // surfaces: they write depth, or test it without adding to the target
    // (translucency, color passes after a depth pre-pass) or add with an
    // equal test (lights drawn per surface). Draws without the depth test
    // (full-screen passes) and additive ones that do not lie on surfaces
    // (deferred light volumes, glow) read what is already resolved: per
    // sample they would compute the same color up to four times (Kena 4K,
    // 4x: 64.8 -> 72 fps, edges and textures unchanged).
    bool supersamplesWithDepth() const {
      D3D11_DEPTH_STENCIL_DESC depth = {};
      depth.DepthEnable = TRUE;
      depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
      depth.DepthFunc = D3D11_COMPARISON_LESS;
      if (m_state.om.depthStencilState)
        m_state.om.depthStencilState->GetDesc(&depth);
      if (!depth.DepthEnable)
        return false;
      if (depth.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO || depth.DepthFunc == D3D11_COMPARISON_EQUAL)
        return true;
      if (!m_state.om.blendState)
        return true;
      D3D11_BLEND_DESC blend = {};
      m_state.om.blendState->GetDesc(&blend);
      return !blend.RenderTarget[0].BlendEnable || blend.RenderTarget[0].DestBlend != D3D11_BLEND_ONE;
    }

    // Strip topologies restart at the all-ones index of the bound format.
    IndexType stripCutIndexType(PrimitiveTopology topology) const {
      switch (topology) {
        case PrimitiveTopology::LineStrip:
        case PrimitiveTopology::TriangleStrip:
        case PrimitiveTopology::LineStripAdj:
        case PrimitiveTopology::TriangleStripAdj:
          return m_state.ia.indexFormat == DXGI_FORMAT_R32_UINT
            ? IndexType::UInt32
            : IndexType::UInt16;
        default:
          return IndexType::UInt16;
      }
    }

    void bindGraphicsPipelineState(IBackendPipeline* pipeline) {
      m_currentPipeline = pipeline;
      m_currentPipelineIsCompute = false;
      m_backendCtx->bindGraphicsPipeline(pipeline);
      m_pipelineDirty = false;
    }

    bool preparePipeline() {
      // Whether the draw renders into forced copies decides its sample count.
      updateForcedMsaaScope();
      if (!m_pipelineDirty && m_currentPipeline && !m_currentPipelineIsCompute)
        return true;

      // State changes mostly return to pipelines bound before: find those
      // by key instead of describing the whole pipeline again.
      GraphicsPipelineKey key;
      const bool keyed = buildGraphicsPipelineKey(key);
      const uint64_t keyHash = keyed ? key.hash() : 0;
      if (keyed) {
        if (IBackendPipeline* pipeline = m_pipelineLookup.find(key, keyHash)) {
          // State changes that keep the pipeline (render targets of the
          // same formats) need no bind; passes that bind pipelines of
          // their own clear m_currentPipeline or set m_forcedMsaaPassRan.
          if (pipeline == m_currentPipeline && !m_currentPipelineIsCompute && !m_forcedMsaaPassRan)
            m_pipelineDirty = false;
          else
            bindGraphicsPipelineState(pipeline);
          return true;
        }
      }

      auto* vs = m_state.vs.shader
        ? static_cast<D3D11VertexShader*>(m_state.vs.shader)->GetBackendShader()
        : nullptr;

      // At minimum we need a vertex shader
      if (!vs) return false;

      auto* ps = m_state.ps.shader
        ? static_cast<D3D11PixelShader*>(m_state.ps.shader)->GetBackendShader()
        : nullptr;
      auto* gs = m_state.gs.shader
        ? static_cast<D3D11GeometryShader*>(m_state.gs.shader)->GetBackendShader()
        : nullptr;
      auto* hs = m_state.hs.shader
        ? static_cast<D3D11HullShader*>(m_state.hs.shader)->GetBackendShader()
        : nullptr;
      auto* ds = m_state.ds.shader
        ? static_cast<D3D11DomainShader*>(m_state.ds.shader)->GetBackendShader()
        : nullptr;

      GraphicsPipelineDesc desc = {};
      initDefaultPipelineStates(desc);
      const DefaultPipelineStateHashes& defaults = defaultPipelineStateHashes();
      desc.blendStateHash = defaults.blend;
      desc.rasterizerStateHash = defaults.rasterizer;
      desc.depthStencilStateHash = defaults.depthStencil;

      // Shader bytecode
      desc.vsBytecode     = vs->getBytecode();
      desc.vsBytecodeSize = vs->getBytecodeSize();
      desc.vsBytecodeHash = vs->getBytecodeHash();
      if (ps) {
        desc.psBytecode     = ps->getBytecode();
        desc.psBytecodeSize = ps->getBytecodeSize();
        desc.psBytecodeHash = ps->getBytecodeHash();
      }
      // Stream output entries (must outlive pipeline creation)
      std::vector<GraphicsPipelineDesc::SOEntry> soEntriesStorage;
      std::vector<uint32_t> soStridesStorage;

      if (gs) {
        desc.gsBytecode     = gs->getBytecode();
        desc.gsBytecodeSize = gs->getBytecodeSize();
        desc.gsBytecodeHash = gs->getBytecodeHash();

        auto* gsObj = static_cast<D3D11GeometryShader*>(m_state.gs.shader);
        if (gsObj->HasStreamOutput()) {
          auto& d3d11Entries = gsObj->GetSOEntries();
          soEntriesStorage.resize(d3d11Entries.size());
          for (size_t i = 0; i < d3d11Entries.size(); i++) {
            soEntriesStorage[i].stream         = d3d11Entries[i].Stream;
            soEntriesStorage[i].semanticName   = d3d11Entries[i].SemanticName;
            soEntriesStorage[i].semanticIndex  = d3d11Entries[i].SemanticIndex;
            soEntriesStorage[i].startComponent = static_cast<uint8_t>(d3d11Entries[i].StartComponent);
            soEntriesStorage[i].componentCount = static_cast<uint8_t>(d3d11Entries[i].ComponentCount);
            soEntriesStorage[i].outputSlot     = static_cast<uint8_t>(d3d11Entries[i].OutputSlot);
          }
          desc.soEntries    = soEntriesStorage.data();
          desc.numSOEntries = static_cast<uint32_t>(soEntriesStorage.size());

          auto& strides = gsObj->GetSOStrides();
          soStridesStorage.assign(strides.begin(), strides.end());
          desc.soBufferStrides = soStridesStorage.data();
          desc.numSOStrides    = static_cast<uint32_t>(soStridesStorage.size());
          desc.rasterizedStream = gsObj->GetRasterizedStream();
        }
      }
      if (hs) {
        desc.hsBytecode     = hs->getBytecode();
        desc.hsBytecodeSize = hs->getBytecodeSize();
        desc.hsBytecodeHash = hs->getBytecodeHash();
      }
      if (ds) {
        desc.dsBytecode     = ds->getBytecode();
        desc.dsBytecodeSize = ds->getBytecodeSize();
        desc.dsBytecodeHash = ds->getBytecodeHash();
      }

      // Input layout
      if (m_state.ia.inputLayout) {
        auto* layout = static_cast<D3D11InputLayout*>(m_state.ia.inputLayout);
        auto& elements = layout->GetBackendElements();
        desc.inputLayout.elements    = elements.data();
        desc.inputLayout.numElements = static_cast<uint32_t>(elements.size());
        desc.inputLayoutHash         = layout->GetContentHash();
      }

      // Blend state
      if (m_state.om.blendState) {
        auto* blend = static_cast<D3D11BlendState*>(m_state.om.blendState);
        desc.blendState = blend->GetBackendState();
        desc.blendStateHash = blend->GetBackendHash();
      }

      // Rasterizer state
      if (m_state.rs.state) {
        auto* rasterizer = static_cast<D3D11RasterizerState*>(m_state.rs.state);
        desc.rasterizerState = rasterizer->GetBackendState();
        desc.rasterizerStateHash = rasterizer->GetBackendHash();
      }

      // Depth stencil state
      if (m_state.om.depthStencilState) {
        auto* depthStencil = static_cast<D3D11DepthStencilState*>(m_state.om.depthStencilState);
        desc.depthStencilState = depthStencil->GetBackendState();
        desc.depthStencilStateHash = depthStencil->GetBackendHash();
      }

      desc.sampleMask = m_state.om.sampleMask;

      // Topology
      desc.topology = convertTopology(m_state.ia.topology);
      desc.stripCutIndexType = stripCutIndexType(desc.topology);

      UINT activeRenderTargetCount = m_state.om.numRenderTargets;
      while (activeRenderTargetCount && !m_state.om.renderTargets[activeRenderTargetCount - 1])
        activeRenderTargetCount--;

      // Render target formats
      desc.numRenderTargets = activeRenderTargetCount;
      for (UINT i = 0; i < activeRenderTargetCount; i++) {
        if (m_state.om.renderTargets[i]) {
          D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
          m_state.om.renderTargets[i]->GetDesc(&rtvDesc);
          DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
          if (auto* backendRtv = boundRtv(m_state.om.renderTargets[i])) {
            if (auto* backendImage = backendRtv->getImage())
              resourceFormat = backendImage->getDesc().format;
          }
          desc.rtvFormats[i] = validateImageViewFormat(rtvDesc.Format, resourceFormat);
        }
      }

      // DSV format
      DXGI_FORMAT dsvViewFormat = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT dsvResourceFormat = DXGI_FORMAT_UNKNOWN;
      if (m_state.om.depthStencilView) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        m_state.om.depthStencilView->GetDesc(&dsvDesc);
        dsvViewFormat = dsvDesc.Format;
        if (auto* backendDsv = boundDsv(m_state.om.depthStencilView)) {
          if (auto* backendImage = backendDsv->getImage())
            dsvResourceFormat = backendImage->getDesc().format;
        }
        desc.dsvFormat = resolveDepthStencilFormat(validateImageViewFormat(dsvViewFormat, dsvResourceFormat));
      } else {
        desc.depthStencilState.depthEnable = false;
        desc.depthStencilState.depthWriteEnable = false;
        desc.depthStencilState.stencilEnable = false;
      }

      // Sample count (from render target texture)
      desc.sampleCount = 1;
      for (UINT i = 0; i < activeRenderTargetCount; i++) {
        if (auto* backendRtv = boundRtv(m_state.om.renderTargets[i])) {
          if (auto* backendImage = backendRtv->getImage()) {
            uint32_t sampleCount = backendImage->getDesc().sampleCount;
            desc.sampleCount = sampleCount ? sampleCount : 1u;
            break;
          }
        }
      }

      if (desc.sampleCount == 1 && m_state.om.depthStencilView) {
        if (auto* backendDsv = boundDsv(m_state.om.depthStencilView)) {
          if (auto* backendImage = backendDsv->getImage()) {
            uint32_t sampleCount = backendImage->getDesc().sampleCount;
            desc.sampleCount = sampleCount ? sampleCount : 1u;
          }
        }
      }

      desc.sampleShading = pipelineSampleShading(desc.sampleCount);

      static int preparePipelineOmLogCount = 0;
      if (d3d11InfoLoggingEnabled()
       && (preparePipelineOmLogCount < 64
        || (m_state.om.depthStencilView && desc.dsvFormat == DXGI_FORMAT_UNKNOWN && preparePipelineOmLogCount < 512))) {
        logMessage(LogLevel::Info,
          "preparePipeline-om: tid=%lu activeRT=%u dsv=%p viewFmt=%u resFmt=%u dsvFmt=%u sample=%u dsEnable=%u dsWrite=%u dsFunc=%u stencilEnable=%u stencilReadMask=%u stencilWriteMask=%u stencilRef=%u uav0=%p uav1=%p",
          static_cast<unsigned long>(currentThreadId()),
          activeRenderTargetCount,
          m_state.om.depthStencilView,
          static_cast<uint32_t>(dsvViewFormat),
          static_cast<uint32_t>(dsvResourceFormat),
          static_cast<uint32_t>(desc.dsvFormat),
          desc.sampleCount,
          desc.depthStencilState.depthEnable ? 1u : 0u,
          desc.depthStencilState.depthWriteEnable ? 1u : 0u,
          static_cast<uint32_t>(desc.depthStencilState.depthFunc),
          desc.depthStencilState.stencilEnable ? 1u : 0u,
          static_cast<uint32_t>(desc.depthStencilState.stencilReadMask),
          static_cast<uint32_t>(desc.depthStencilState.stencilWriteMask),
          m_state.om.stencilRef,
          m_state.om.uavs[0],
          m_state.om.uavs[1]);
        preparePipelineOmLogCount++;
      }

      auto* backend = m_device->GetBackend();
      if (!backend) return false;

      if (d3d11InfoLoggingEnabled()) {
        logMessage(LogLevel::Info,
          "preparePipeline-pre-pso: vs=%p ps=%p gs=%p hs=%p ds=%p il=%p blend=%p rs=%p dss=%p topo=%u numRT=%u dsvFmt=%u sampleCount=%u",
          desc.vsBytecode, desc.psBytecode, desc.gsBytecode, desc.hsBytecode, desc.dsBytecode,
          desc.inputLayout.elements,
          m_state.om.blendState, m_state.rs.state, m_state.om.depthStencilState,
          static_cast<uint32_t>(desc.topology), desc.numRenderTargets,
          static_cast<uint32_t>(desc.dsvFormat), desc.sampleCount);
      }

      // With asyncShaders a pipeline that is not compiled yet compiles in
      // the background and its draws are skipped meanwhile. Draws with
      // lasting effects wait for it instead: command lists of deferred
      // contexts may be replayed every frame, and stream output and UAV
      // writes stay in their buffers.
      Rc<IBackendPipeline> pipeline;
      bool pending = false;
      if (backend->asyncPipelines() && !m_isDeferred && !desc.numSOEntries && !boundOmUavs())
        pipeline = backend->requestGraphicsPipeline(desc, pending);
      else
        pipeline = backend->createGraphicsPipelineDeferred(desc);
      if (!pipeline)
        return false;

      if (keyed)
        m_pipelineLookup.insert(key, keyHash, pipeline.get());
      bindGraphicsPipelineState(pipeline.get());
      return true;
    }

    // Temporary upload buffers are recycled by exact size: rounding sizes
    // above the suballocated range to four classes per power of two lets
    // updates of varying size (boxed atlas updates) reuse them instead of
    // creating a resource each time, for at most a quarter more memory.
    static uint64_t uploadSizeClass(uint64_t size) {
      if (size <= 65536)
        return size;
      uint64_t power = 1;
      while (power < size)
        power <<= 1;
      const uint64_t step = power / 8;
      return (size + step - 1) / step * step;
    }

    bool boundOmUavs() const {
      for (auto* uav : m_state.om.uavs) {
        if (uav)
          return true;
      }
      return false;
    }

    bool prepareComputePipeline() {
      if (!m_pipelineDirty && m_currentPipeline && m_currentPipelineIsCompute)
        return true;

      auto* csObject = static_cast<D3D11ComputeShader*>(m_state.cs.shader);
      auto* cs = csObject ? csObject->GetBackendShader() : nullptr;

      if (!cs) return false;

      // The device keeps every pipeline.
      IBackendPipeline* pipeline = csObject->GetBackendPipeline().get();
      if (!pipeline)
        pipeline = csObject->GetCompiledPipeline();
      if (!pipeline) {
        ComputePipelineDesc desc = {};
        desc.csBytecode = cs->getBytecode();
        desc.csBytecodeSize = cs->getBytecodeSize();
        desc.csBytecodeHash = cs->getBytecodeHash();

        auto* backend = m_device->GetBackend();
        if (!backend) return false;

        pipeline = backend->createComputePipeline(desc).get();
        if (!pipeline) return false;
        csObject->SetCompiledPipeline(pipeline);
      }

      m_currentPipeline = pipeline;
      m_currentPipelineIsCompute = true;
      m_backendCtx->bindComputePipeline(pipeline);
      m_pipelineDirty = false;
      return true;
    }

    void markPipelineDirty() {
      m_pipelineDirty = true;
    }

    static DWORD currentThreadId() {
      return GetCurrentThreadId();
    }

    UINT getActiveRenderTargetCount() const {
      UINT count = m_state.om.numRenderTargets;
      while (count && !m_state.om.renderTargets[count - 1])
        count--;
      return count;
    }

    static bool isTrackedFinalPassImageDesc(const ImageDesc& desc) {
      uint32_t mipLevels = desc.mipLevels ? desc.mipLevels : 1u;
      uint32_t arraySize = desc.arraySize ? desc.arraySize : 1u;
      if (mipLevels != 1u || arraySize != 1u)
        return false;

      switch (desc.format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
          return desc.width == 4u && desc.height == 4u;

        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
          return desc.width == 1560u && desc.height == 908u;

        default:
          return false;
      }
    }

    static const char* getTrackedFinalPassImageLabel(const ImageDesc& desc) {
      switch (desc.format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
          return "final-bc1-4x4";

        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
          return "final-bc3-1560x908";

        default:
          return "untracked";
      }
    }

    static bool isPotentialPresentRenderTargetDesc(const ImageDesc& desc) {
      uint32_t mipLevels = desc.mipLevels ? desc.mipLevels : 1u;
      uint32_t arraySize = desc.arraySize ? desc.arraySize : 1u;
      if (mipLevels != 1u || arraySize != 1u)
        return false;
      if (desc.width < 1280u || desc.height < 720u)
        return false;

      switch (desc.format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
          return true;

        default:
          return false;
      }
    }

    static bool isWritableMapMode(MapMode mode) {
      return mode == MapMode::Write
          || mode == MapMode::ReadWrite
          || mode == MapMode::WriteDiscard
          || mode == MapMode::WriteNoOverwrite;
    }

    static uint64_t hashTrackedBytes(const uint8_t* data, uint64_t size) {
      uint64_t hash = 1469598103934665603ull;
      for (uint64_t i = 0; i < size; i++) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash = fnv64Multiply(hash);
      }
      return hash;
    }

    struct FinalPassConstantBufferSnapshot {
      bool     hasShadow = false;
      uint64_t size = 0;
      uint64_t hash = 0;
      uint64_t zeroCount = 0;
      uint64_t ffCount = 0;
      uint32_t dwords[8] = {};
      float    floats[4] = {};
    };

    static FinalPassConstantBufferSnapshot snapshotFinalPassConstantBuffer(ID3D11Buffer* buffer) {
      FinalPassConstantBufferSnapshot snapshot = {};
      auto* trackedBuffer = buffer ? static_cast<D3D11Buffer*>(buffer) : nullptr;
      if (!trackedBuffer || !trackedBuffer->HasShadowData())
        return snapshot;

      const uint8_t* data = trackedBuffer->GetShadowData();
      const uint64_t size = trackedBuffer->GetShadowSize();
      if (!data || !size)
        return snapshot;

      snapshot.hasShadow = true;
      snapshot.size = size;
      snapshot.hash = hashTrackedBytes(data, size);

      for (uint64_t i = 0; i < size; i++) {
        snapshot.zeroCount += data[i] == 0 ? 1ull : 0ull;
        snapshot.ffCount += data[i] == 0xff ? 1ull : 0ull;
      }

      const uint64_t dwordBytes = size >= 32u ? 32u : (size - (size % 4u));
      if (dwordBytes)
        std::memcpy(snapshot.dwords, data, static_cast<size_t>(dwordBytes));

      const uint64_t floatBytes = size >= 16u ? 16u : (size - (size % 4u));
      if (floatBytes)
        std::memcpy(snapshot.floats, data, static_cast<size_t>(floatBytes));

      return snapshot;
    }

    void logTrackedRenderTargetClear(
        ID3D11RenderTargetView* pRenderTargetView,
        IBackendImageView*      backendRtv,
        const FLOAT             color[4]) const {
      if (!d3d11FinalPassTraceEnabled())
        return;

      if (!backendRtv || !color)
        return;

      IBackendImage* image = backendRtv->getImage();
      if (!image)
        return;

      const auto& imageDesc = image->getDesc();
      if (!isPotentialPresentRenderTargetDesc(imageDesc))
        return;

      static uint32_t s_trackedClearLogCount = 0;
      if (s_trackedClearLogCount >= 96u)
        return;
      s_trackedClearLogCount++;

      void* frames[4] = {};
      USHORT frameCount = CaptureStackBackTrace(2, 4, frames, nullptr);
      logMessage(LogLevel::Info,
        "ClearRenderTargetView-tracked: tid=%lu rtv=%p rtImg=%p fmt=%u size=%ux%u color=%.3f,%.3f,%.3f,%.3f frames=%p %p %p %p",
        static_cast<unsigned long>(currentThreadId()),
        pRenderTargetView,
        image,
        static_cast<uint32_t>(imageDesc.format),
        imageDesc.width,
        imageDesc.height,
        color[0],
        color[1],
        color[2],
        color[3],
        frameCount > 0 ? frames[0] : nullptr,
        frameCount > 1 ? frames[1] : nullptr,
        frameCount > 2 ? frames[2] : nullptr,
        frameCount > 3 ? frames[3] : nullptr);
    }

    void logTrackedImageMutation(
        const char*      tag,
        ID3D11Resource*  resource,
        IBackendImage*   backendImage,
        UINT             subresource,
        const D3D11_BOX* box,
        UINT             srcRowPitch,
        UINT             srcDepthPitch,
        const void*      srcData) const {
      if (!d3d11FinalPassTraceEnabled())
        return;

      if (!backendImage)
        return;

      const auto& imageDesc = backendImage->getDesc();
      if (!isTrackedFinalPassImageDesc(imageDesc))
        return;

      void* frames[4] = {};
      USHORT frameCount = CaptureStackBackTrace(2, 4, frames, nullptr);
      logMessage(LogLevel::Info,
        "%s: tid=%lu resource=%p backendImg=%p kind=%s fmt=%u %ux%ux%u mip=%u array=%u sub=%u src=%p srcPitch=%u srcDepthPitch=%u box=%u,%u,%u-%u,%u,%u frames=%p %p %p %p",
        tag,
        static_cast<unsigned long>(currentThreadId()),
        resource,
        backendImage,
        getTrackedFinalPassImageLabel(imageDesc),
        static_cast<uint32_t>(imageDesc.format),
        imageDesc.width,
        imageDesc.height,
        imageDesc.depth,
        imageDesc.mipLevels,
        imageDesc.arraySize,
        subresource,
        srcData,
        srcRowPitch,
        srcDepthPitch,
        box ? box->left : 0u,
        box ? box->top : 0u,
        box ? box->front : 0u,
        box ? box->right : 0u,
        box ? box->bottom : 0u,
        box ? box->back : 0u,
        frameCount > 0 ? frames[0] : nullptr,
        frameCount > 1 ? frames[1] : nullptr,
        frameCount > 2 ? frames[2] : nullptr,
        frameCount > 3 ? frames[3] : nullptr);
    }

    void logTrackedImageCopy(
        const char*      tag,
        ID3D11Resource*  dstResource,
        IBackendImage*   dstImage,
        UINT             dstSubresource,
        UINT             dstX,
        UINT             dstY,
        UINT             dstZ,
        ID3D11Resource*  srcResource,
        IBackendImage*   srcImage,
        UINT             srcSubresource,
        const D3D11_BOX* srcBox) const {
      if (!d3d11FinalPassTraceEnabled())
        return;

      const bool trackedDst = dstImage && isTrackedFinalPassImageDesc(dstImage->getDesc());
      const bool trackedSrc = srcImage && isTrackedFinalPassImageDesc(srcImage->getDesc());
      if (!trackedDst && !trackedSrc)
        return;

      const auto& dstDesc = trackedDst ? dstImage->getDesc() : ImageDesc{};
      const auto& srcDesc = trackedSrc ? srcImage->getDesc() : ImageDesc{};
      void* frames[4] = {};
      USHORT frameCount = CaptureStackBackTrace(2, 4, frames, nullptr);
      logMessage(LogLevel::Info,
        "%s: tid=%lu dstRes=%p dstImg=%p dstKind=%s dstFmt=%u %ux%u dstSub=%u dstXYZ=%u,%u,%u srcRes=%p srcImg=%p srcKind=%s srcFmt=%u %ux%u srcSub=%u srcBox=%u,%u,%u-%u,%u,%u frames=%p %p %p %p",
        tag,
        static_cast<unsigned long>(currentThreadId()),
        dstResource,
        dstImage,
        trackedDst ? getTrackedFinalPassImageLabel(dstDesc) : "untracked",
        static_cast<uint32_t>(dstDesc.format),
        dstDesc.width,
        dstDesc.height,
        dstSubresource,
        dstX,
        dstY,
        dstZ,
        srcResource,
        srcImage,
        trackedSrc ? getTrackedFinalPassImageLabel(srcDesc) : "untracked",
        static_cast<uint32_t>(srcDesc.format),
        srcDesc.width,
        srcDesc.height,
        srcSubresource,
        srcBox ? srcBox->left : 0u,
        srcBox ? srcBox->top : 0u,
        srcBox ? srcBox->front : 0u,
        srcBox ? srcBox->right : 0u,
        srcBox ? srcBox->bottom : 0u,
        srcBox ? srcBox->back : 0u,
        frameCount > 0 ? frames[0] : nullptr,
        frameCount > 1 ? frames[1] : nullptr,
        frameCount > 2 ? frames[2] : nullptr,
        frameCount > 3 ? frames[3] : nullptr);
    }

    void logTrackedFinalPassConstantBuffer(
        const char*      op,
        UINT             indexCount,
        UINT             instanceCount,
        UINT             slot,
        ID3D11Buffer*    buffer,
        IBackendBuffer*  backendBuffer) const {
      if (!buffer)
        return;

      const auto snapshot = snapshotFinalPassConstantBuffer(buffer);
      logMessage(LogLevel::Info,
        "finalPass-d3d11-cb: op=%s idx=%u inst=%u slot=%u buf=%p backend=%p size=%llu shadow=%u hash=0x%016llX zero=%llu ff=%llu dwords=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X floats=%.3f,%.3f,%.3f,%.3f",
        op,
        indexCount,
        instanceCount,
        slot,
        buffer,
        backendBuffer,
        static_cast<unsigned long long>(snapshot.hasShadow ? snapshot.size : 0ull),
        snapshot.hasShadow ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.hasShadow ? snapshot.hash : 0ull),
        static_cast<unsigned long long>(snapshot.hasShadow ? snapshot.zeroCount : 0ull),
        static_cast<unsigned long long>(snapshot.hasShadow ? snapshot.ffCount : 0ull),
        snapshot.dwords[0],
        snapshot.dwords[1],
        snapshot.dwords[2],
        snapshot.dwords[3],
        snapshot.dwords[4],
        snapshot.dwords[5],
        snapshot.dwords[6],
        snapshot.dwords[7],
        snapshot.floats[0],
        snapshot.floats[1],
        snapshot.floats[2],
        snapshot.floats[3]);
    }

    void logTrackedFinalPassPixelShader(ID3D11DeviceChild* shader) const {
      if (!shader)
        return;

      auto* pixelShader = reinterpret_cast<D3D11PixelShader*>(shader);
      uint64_t hash = pixelShader->GetBytecodeHash();
      static std::unordered_map<uint64_t, bool> s_loggedHashes;
      if (hash) {
        if (!s_loggedHashes.emplace(hash, true).second)
          return;
      }

      logMessage(
        LogLevel::Info,
        "finalPass-d3d11-ps: ps=%p hash=0x%016llX bindings=%s",
        shader,
        static_cast<unsigned long long>(hash),
        pixelShader->GetBindingSummary().c_str());
    }

    void logTrackedFinalPassVertexShader(ID3D11DeviceChild* shader) const {
      if (!shader)
        return;

      auto* vertexShader = reinterpret_cast<D3D11VertexShader*>(shader);
      uint64_t hash = vertexShader->GetBytecodeHash();
      static std::unordered_map<uint64_t, bool> s_loggedHashes;
      if (hash) {
        if (!s_loggedHashes.emplace(hash, true).second)
          return;
      }

      logMessage(
        LogLevel::Info,
        "finalPass-d3d11-vs: vs=%p hash=0x%016llX bindings=%s inputs=%s",
        shader,
        static_cast<unsigned long long>(hash),
        vertexShader->GetBindingSummary().c_str(),
        vertexShader->GetInputSummary().c_str());
    }

    void logTrackedFinalPassInputAssemblerState() const {
      auto* indexBuffer = m_state.ia.indexBuffer ? static_cast<D3D11Buffer*>(m_state.ia.indexBuffer) : nullptr;
      auto* vertexBuffer0 = m_state.ia.vertexBuffers[0] ? static_cast<D3D11Buffer*>(m_state.ia.vertexBuffers[0]) : nullptr;
      const UINT indexBufferBytes = indexBuffer ? indexBuffer->GetDescRef().ByteWidth : 0u;
      const UINT vertexBuffer0Bytes = vertexBuffer0 ? vertexBuffer0->GetDescRef().ByteWidth : 0u;

      logMessage(
        LogLevel::Info,
        "finalPass-d3d11-ia: il=%p topo=%u ib=%p/%u/sh%u fmt=%u off=%u vb0=%p/%u/sh%u/%u/%u vb1=%p/%u/%u vb2=%p/%u/%u vb3=%p/%u/%u",
        m_state.ia.inputLayout,
        static_cast<uint32_t>(m_state.ia.topology),
        m_state.ia.indexBuffer,
        indexBufferBytes,
        indexBuffer && indexBuffer->HasShadowData() ? 1u : 0u,
        static_cast<uint32_t>(m_state.ia.indexFormat),
        m_state.ia.indexOffset,
        m_state.ia.vertexBuffers[0],
        vertexBuffer0Bytes,
        vertexBuffer0 && vertexBuffer0->HasShadowData() ? 1u : 0u,
        m_state.ia.vertexStrides[0],
        m_state.ia.vertexOffsets[0],
        m_state.ia.vertexBuffers[1],
        m_state.ia.vertexStrides[1],
        m_state.ia.vertexOffsets[1],
        m_state.ia.vertexBuffers[2],
        m_state.ia.vertexStrides[2],
        m_state.ia.vertexOffsets[2],
        m_state.ia.vertexBuffers[3],
        m_state.ia.vertexStrides[3],
        m_state.ia.vertexOffsets[3]);

      if (indexBuffer) {
        const auto& desc = indexBuffer->GetDescRef();
        logMessage(
          LogLevel::Info,
          "finalPass-d3d11-ib-desc: buf=%p size=%u tracked=%llu bind=0x%x usage=%u cpu=0x%x misc=0x%x stride=%u",
          indexBuffer,
          desc.ByteWidth,
          static_cast<unsigned long long>(indexBuffer->GetShadowSize()),
          desc.BindFlags,
          static_cast<uint32_t>(desc.Usage),
          desc.CPUAccessFlags,
          desc.MiscFlags,
          desc.StructureByteStride);
      }

      if (vertexBuffer0) {
        const auto& desc = vertexBuffer0->GetDescRef();
        logMessage(
          LogLevel::Info,
          "finalPass-d3d11-vb0-desc: buf=%p size=%u tracked=%llu bind=0x%x usage=%u cpu=0x%x misc=0x%x stride=%u sameAsIb=%u",
          vertexBuffer0,
          desc.ByteWidth,
          static_cast<unsigned long long>(vertexBuffer0->GetShadowSize()),
          desc.BindFlags,
          static_cast<uint32_t>(desc.Usage),
          desc.CPUAccessFlags,
          desc.MiscFlags,
          desc.StructureByteStride,
          vertexBuffer0 == indexBuffer ? 1u : 0u);
      }

      auto* inputLayout = m_state.ia.inputLayout ? static_cast<D3D11InputLayout*>(m_state.ia.inputLayout) : nullptr;
      if (!inputLayout)
        return;

      static std::unordered_map<const void*, bool> s_loggedLayouts;
      if (!s_loggedLayouts.emplace(inputLayout, true).second)
        return;

      const auto& elements = inputLayout->GetElements();
      for (size_t i = 0; i < elements.size() && i < 8u; i++) {
        const auto& element = elements[i];
        logMessage(
          LogLevel::Info,
          "finalPass-d3d11-ia-elem[%u]: semantic=%s%u slot=%u fmt=%u offset=%u class=%u step=%u",
          static_cast<unsigned>(i),
          element.SemanticName ? element.SemanticName : "?",
          element.SemanticIndex,
          element.InputSlot,
          static_cast<uint32_t>(element.Format),
          element.AlignedByteOffset,
          static_cast<uint32_t>(element.InputSlotClass),
          element.InstanceDataStepRate);
      }
    }

    void logTrackedFinalPassIndexBuffer(UINT indexCount) const {
      auto* indexBuffer = m_state.ia.indexBuffer ? static_cast<D3D11Buffer*>(m_state.ia.indexBuffer) : nullptr;
      if (!indexBuffer || !indexBuffer->HasShadowData())
        return;

      const auto& desc = indexBuffer->GetDescRef();
      if (!(desc.BindFlags & D3D11_BIND_INDEX_BUFFER))
        return;

      const uint8_t* data = indexBuffer->GetShadowData();
      uint64_t size = indexBuffer->GetShadowSize();
      uint64_t offset = m_state.ia.indexOffset;
      if (!data || offset >= size)
        return;

      if (m_state.ia.indexFormat == DXGI_FORMAT_R16_UINT) {
        uint16_t values[12] = {};
        uint32_t count = (std::min)(indexCount, 12u);
        uint64_t available = (size - offset) / sizeof(uint16_t);
        if (count > available)
          count = static_cast<uint32_t>(available);
        if (!count)
          return;

        std::memcpy(values, data + offset, static_cast<size_t>(count) * sizeof(uint16_t));
        logMessage(
          LogLevel::Info,
          "finalPass-d3d11-ib: buf=%p size=%u fmt=%u off=%u count=%u values=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
          indexBuffer,
          desc.ByteWidth,
          static_cast<uint32_t>(m_state.ia.indexFormat),
          m_state.ia.indexOffset,
          count,
          values[0],
          values[1],
          values[2],
          values[3],
          values[4],
          values[5],
          values[6],
          values[7],
          values[8],
          values[9],
          values[10],
          values[11]);
      } else if (m_state.ia.indexFormat == DXGI_FORMAT_R32_UINT) {
        uint32_t values[12] = {};
        uint32_t count = (std::min)(indexCount, 12u);
        uint64_t available = (size - offset) / sizeof(uint32_t);
        if (count > available)
          count = static_cast<uint32_t>(available);
        if (!count)
          return;

        std::memcpy(values, data + offset, static_cast<size_t>(count) * sizeof(uint32_t));
        logMessage(
          LogLevel::Info,
          "finalPass-d3d11-ib: buf=%p size=%u fmt=%u off=%u count=%u values=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
          indexBuffer,
          desc.ByteWidth,
          static_cast<uint32_t>(m_state.ia.indexFormat),
          m_state.ia.indexOffset,
          count,
          values[0],
          values[1],
          values[2],
          values[3],
          values[4],
          values[5],
          values[6],
          values[7],
          values[8],
          values[9],
          values[10],
          values[11]);
      }
    }

    void logTrackedFinalPassVertexBuffer(UINT slot, UINT instanceCount) const {
      if (slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX)
        return;

      auto* vertexBuffer = m_state.ia.vertexBuffers[slot] ? static_cast<D3D11Buffer*>(m_state.ia.vertexBuffers[slot]) : nullptr;
      if (!vertexBuffer || !vertexBuffer->HasShadowData())
        return;

      const auto& desc = vertexBuffer->GetDescRef();
      if (!(desc.BindFlags & D3D11_BIND_VERTEX_BUFFER))
        return;

      const uint8_t* data = vertexBuffer->GetShadowData();
      uint64_t size = vertexBuffer->GetShadowSize();
      uint32_t stride = m_state.ia.vertexStrides[slot];
      uint32_t baseOffset = m_state.ia.vertexOffsets[slot];
      if (!data || !stride || baseOffset >= size)
        return;

      uint32_t recordCount = instanceCount > 1u ? (std::min)(instanceCount, 3u) : 1u;
      for (UINT record = 0; record < recordCount; record++) {
        uint64_t recordOffset = static_cast<uint64_t>(baseOffset) + static_cast<uint64_t>(record) * stride;
        if (recordOffset >= size)
          break;

        uint32_t dwords[12] = {};
        float floats[8] = {};
        uint64_t remaining = size - recordOffset;
        uint32_t dwordBytes = static_cast<uint32_t>((std::min)(remaining, uint64_t(sizeof(dwords))) & ~uint64_t(3));
        if (!dwordBytes)
          continue;

        std::memcpy(dwords, data + recordOffset, dwordBytes);
        uint32_t floatBytes = (std::min)(dwordBytes, static_cast<uint32_t>(sizeof(floats)));
        std::memcpy(floats, data + recordOffset, floatBytes);

        logMessage(
          LogLevel::Info,
          "finalPass-d3d11-vb: slot=%u buf=%p size=%u stride=%u baseOff=%u rec=%u recOff=%llu dwords=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X floats=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
          slot,
          vertexBuffer,
          desc.ByteWidth,
          stride,
          baseOffset,
          record,
          static_cast<unsigned long long>(recordOffset),
          dwords[0],
          dwords[1],
          dwords[2],
          dwords[3],
          dwords[4],
          dwords[5],
          dwords[6],
          dwords[7],
          dwords[8],
          dwords[9],
          dwords[10],
          dwords[11],
          floats[0],
          floats[1],
          floats[2],
          floats[3],
          floats[4],
          floats[5],
          floats[6],
          floats[7]);
      }
    }

    void logTrackedFinalPassState(
        const char* op,
        UINT        indexCount,
        UINT        instanceCount,
        UINT        startIndexLocation,
        INT         baseVertexLocation,
        UINT        startInstanceLocation) const {
      if (!d3d11FinalPassTraceEnabled())
        return;

      if (indexCount != 6u || (instanceCount != 1u && instanceCount != 3u))
        return;

      UINT activeRenderTargetCount = getActiveRenderTargetCount();
      if (activeRenderTargetCount != 1u || m_state.om.depthStencilView)
        return;

      const BlendState* blendState = nullptr;
      if (m_state.om.blendState)
        blendState = &static_cast<D3D11BlendState*>(m_state.om.blendState)->GetBackendState();

      auto* rtv = m_state.om.renderTargets[0];
      IBackendImageView* backendRtv = rtv ? getBackendRTV(rtv) : nullptr;
      IBackendImage* rtImage = backendRtv ? backendRtv->getImage() : nullptr;
      const auto& rtDesc = rtImage ? rtImage->getDesc() : ImageDesc{};
      const D3D11_VIEWPORT viewport = m_state.rs.numViewports ? m_state.rs.viewports[0] : D3D11_VIEWPORT{};
      const D3D11_RECT scissorRect = m_state.rs.numScissorRects ? m_state.rs.scissorRects[0] : D3D11_RECT{};

      ID3D11Buffer* psCb[4] = {};
      IBackendBuffer* psBackendCb[4] = {};
      uint64_t psCbSize[4] = {};
      uint32_t psCbMask = 0u;
      for (UINT slot = 0; slot < 4u; slot++) {
        psCb[slot] = m_state.ps.constantBuffers[slot];
        psBackendCb[slot] = psCb[slot] ? getBackendBuffer(psCb[slot]) : nullptr;
        psCbSize[slot] = psBackendCb[slot] ? psBackendCb[slot]->getDesc().size : 0ull;
        if (psCb[slot])
          psCbMask |= (1u << slot);
      }

      void* frames[4] = {};
      USHORT frameCount = CaptureStackBackTrace(2, 4, frames, nullptr);
      logMessage(LogLevel::Info,
        "finalPass-d3d11: op=%s tid=%lu idx=%u inst=%u start=%u base=%d firstInst=%u ps=%p rtv=%p rtImg=%p rtFmt=%u rtSize=%ux%u blendObj=%p alphaCov=%u indep=%u blend0=%u src0=%u dst0=%u op0=%u srcA0=%u dstA0=%u opA0=%u writeMask0=0x%x sampleMask=0x%x blendFactor=%.3f,%.3f,%.3f,%.3f vpCount=%u vp0=%.1f,%.1f %.1fx%.1f z=%.3f..%.3f scCount=%u sc0=%ld,%ld-%ld,%ld psS0=%p psS1=%p psCBMask=0x%x psCB0=%p/%p/%llu psCB1=%p/%p/%llu psCB2=%p/%p/%llu psCB3=%p/%p/%llu frames=%p %p %p %p",
        op,
        static_cast<unsigned long>(currentThreadId()),
        indexCount,
        instanceCount,
        startIndexLocation,
        baseVertexLocation,
        startInstanceLocation,
        m_state.ps.shader,
        rtv,
        rtImage,
        static_cast<uint32_t>(rtDesc.format),
        rtDesc.width,
        rtDesc.height,
        m_state.om.blendState,
        blendState ? (blendState->alphaToCoverageEnable ? 1u : 0u) : 0u,
        blendState ? (blendState->independentBlendEnable ? 1u : 0u) : 0u,
        blendState ? (blendState->renderTargets[0].blendEnable ? 1u : 0u) : 0u,
        blendState ? static_cast<uint32_t>(blendState->renderTargets[0].srcBlend) : 0u,
        blendState ? static_cast<uint32_t>(blendState->renderTargets[0].dstBlend) : 0u,
        blendState ? static_cast<uint32_t>(blendState->renderTargets[0].blendOp) : 0u,
        blendState ? static_cast<uint32_t>(blendState->renderTargets[0].srcBlendAlpha) : 0u,
        blendState ? static_cast<uint32_t>(blendState->renderTargets[0].dstBlendAlpha) : 0u,
        blendState ? static_cast<uint32_t>(blendState->renderTargets[0].blendOpAlpha) : 0u,
        blendState ? static_cast<uint32_t>(blendState->renderTargets[0].writeMask) : 0u,
        m_state.om.sampleMask,
        m_state.om.blendFactor[0],
        m_state.om.blendFactor[1],
        m_state.om.blendFactor[2],
        m_state.om.blendFactor[3],
        m_state.rs.numViewports,
        viewport.TopLeftX,
        viewport.TopLeftY,
        viewport.Width,
        viewport.Height,
        viewport.MinDepth,
        viewport.MaxDepth,
        m_state.rs.numScissorRects,
        static_cast<long>(scissorRect.left),
        static_cast<long>(scissorRect.top),
        static_cast<long>(scissorRect.right),
        static_cast<long>(scissorRect.bottom),
        m_state.ps.samplers[0],
        m_state.ps.samplers[1],
        psCbMask,
        psCb[0],
        psBackendCb[0],
        static_cast<unsigned long long>(psCbSize[0]),
        psCb[1],
        psBackendCb[1],
        static_cast<unsigned long long>(psCbSize[1]),
        psCb[2],
        psBackendCb[2],
        static_cast<unsigned long long>(psCbSize[2]),
        psCb[3],
        psBackendCb[3],
        static_cast<unsigned long long>(psCbSize[3]),
        frameCount > 0 ? frames[0] : nullptr,
        frameCount > 1 ? frames[1] : nullptr,
        frameCount > 2 ? frames[2] : nullptr,
        frameCount > 3 ? frames[3] : nullptr);

      logTrackedFinalPassPixelShader(m_state.ps.shader);
      logTrackedFinalPassVertexShader(m_state.vs.shader);
      logTrackedFinalPassInputAssemblerState();
      logTrackedFinalPassVertexBuffer(0u, instanceCount);
      logTrackedFinalPassIndexBuffer(indexCount);

      for (UINT slot = 0; slot < 4u; slot++)
        logTrackedFinalPassConstantBuffer(op, indexCount, instanceCount, slot, psCb[slot], psBackendCb[slot]);
    }

    void setOutputMergerRenderTargets(
        UINT                          numViews,
        ID3D11RenderTargetView* const* renderTargetViews,
        ID3D11DepthStencilView*       depthStencilView,
        UINT                          maxRenderTargetCount) {
      const UINT clampedNumViews = (std::min)(numViews, UINT(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX));
      const UINT clampedMaxRenderTargetCount = (std::min)(maxRenderTargetCount, UINT(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX));
      const UINT storedRenderTargetCount = (std::min)(clampedNumViews, clampedMaxRenderTargetCount);

      ID3D11RenderTargetView* desiredRenderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX] = {};
      for (UINT slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX; slot++) {
        if (slot < storedRenderTargetCount && renderTargetViews)
          desiredRenderTargets[slot] = renderTargetViews[slot];
      }

      UINT desiredActiveRenderTargetCount = storedRenderTargetCount;
      while (desiredActiveRenderTargetCount && !desiredRenderTargets[desiredActiveRenderTargetCount - 1])
        desiredActiveRenderTargetCount--;

      bool hasOverlappingPixelUav = false;
      const UINT overlappingUavCount = (std::min)(desiredActiveRenderTargetCount, UINT(D3D11_PS_CS_UAV_REGISTER_COUNT_MAX));
      for (UINT slot = 0; slot < overlappingUavCount; slot++) {
        if (m_state.om.uavs[slot]) {
          hasOverlappingPixelUav = true;
          break;
        }
      }

      bool stateUnchanged = !hasOverlappingPixelUav
        && m_state.om.depthStencilView == depthStencilView
        && m_state.om.numRenderTargets == desiredActiveRenderTargetCount;
      if (stateUnchanged) {
        for (UINT slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX; slot++) {
          if (m_state.om.renderTargets[slot] != desiredRenderTargets[slot]) {
            stateUnchanged = false;
            break;
          }
        }
      }

      if (stateUnchanged)
        return;

      static int setOmRtLogCount = 0;
      if (setOmRtLogCount < 48) {
        logMessage(LogLevel::Info,
          "setOMRT: tid=%lu numViews=%u stored=%u max=%u dsv=%p rtv0=%p",
          static_cast<unsigned long>(currentThreadId()),
          numViews,
          storedRenderTargetCount,
          clampedMaxRenderTargetCount,
          depthStencilView,
          renderTargetViews ? renderTargetViews[0] : nullptr);
        setOmRtLogCount++;
      }

      for (UINT slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX; slot++)
        replaceContextReference(m_state.om.renderTargets[slot], desiredRenderTargets[slot]);

      replaceContextReference(m_state.om.depthStencilView, depthStencilView);
      m_state.om.numRenderTargets = desiredActiveRenderTargetCount;

      clearPixelUnorderedAccessViewsBefore(getActiveRenderTargetCount());
      unbindOutputMergerHazards();
      rebindOutputMergerState();
    }

    void clearPixelUnorderedAccessView(UINT slot) {
      if (slot >= D3D11_PS_CS_UAV_REGISTER_COUNT_MAX)
        return;

      if (!m_state.om.uavs[slot])
        return;

      clearContextReference(m_state.om.uavs[slot]);
      bindUnorderedAccessView(ShaderStage::Pixel, slot, nullptr);
    }

    void clearPixelUnorderedAccessViewsBefore(UINT slotCount) {
      const UINT clearCount = (std::min)(slotCount, UINT(D3D11_PS_CS_UAV_REGISTER_COUNT_MAX));
      for (UINT slot = 0; slot < clearCount; slot++)
        clearPixelUnorderedAccessView(slot);
    }

    void truncateOutputMergerRenderTargets(UINT maxRenderTargetCount) {
      const UINT clampedCount = (std::min)(maxRenderTargetCount, UINT(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX));
      bool stateChanged = m_state.om.numRenderTargets > clampedCount;

      for (UINT slot = clampedCount; slot < m_state.om.numRenderTargets; slot++) {
        if (m_state.om.renderTargets[slot]) {
          clearContextReference(m_state.om.renderTargets[slot]);
          stateChanged = true;
        }
      }

      m_state.om.numRenderTargets = clampedCount;
      while (m_state.om.numRenderTargets && !m_state.om.renderTargets[m_state.om.numRenderTargets - 1])
        m_state.om.numRenderTargets--;

      if (stateChanged)
        rebindOutputMergerState();
    }

    // Hazard tracking runs on every bind, so it reads a view's resource
    // without the AddRef/Release pair of the public GetResource.
    template<typename View>
    static ID3D11Resource* viewResource(View* view) {
      return view
        ? static_cast<typename D3D11ImplOf<View>::Type*>(view)->GetResourceRaw()
        : nullptr;
    }

    static UINT boundUavSlotEnd(ShaderStageState& state) {
      while (state.uavSlotEnd && !state.uavs[state.uavSlotEnd - 1])
        state.uavSlotEnd--;
      return state.uavSlotEnd;
    }

    static UINT boundSrvSlotEnd(ShaderStageState& state) {
      while (state.srvSlotEnd && !state.srvs[state.srvSlotEnd - 1])
        state.srvSlotEnd--;
      return state.srvSlotEnd;
    }

    template<typename View>
    static bool viewReferencesResource(
        View*           candidateView,
        ID3D11Resource* resource) {
      return resource && viewResource(candidateView) == resource;
    }

    static bool imageViewRangesOverlap(
        const ImageViewDesc& a,
        const ImageViewDesc& b) {
      const uint64_t aMipEnd = uint64_t(a.baseMipLevel) + a.mipLevelCount;
      const uint64_t bMipEnd = uint64_t(b.baseMipLevel) + b.mipLevelCount;
      if (a.baseMipLevel >= bMipEnd || b.baseMipLevel >= aMipEnd)
        return false;

      const uint64_t aLayerEnd = uint64_t(a.baseArrayLayer) + a.arrayLayerCount;
      const uint64_t bLayerEnd = uint64_t(b.baseArrayLayer) + b.arrayLayerCount;
      return a.baseArrayLayer < bLayerEnd && b.baseArrayLayer < aLayerEnd;
    }

    static bool shaderResourceViewOverlapsUav(
        ID3D11ShaderResourceView* srv,
        ID3D11UnorderedAccessView* uav) {
      if (!srv || !uav)
        return false;

      ID3D11Resource* srvResource = viewResource(srv);
      if (!srvResource || srvResource != viewResource(uav))
        return false;

      auto* srvImage = getBackendImageSRV(srv);
      auto* uavImage = getBackendImageUAV(uav);
      if (srvImage && uavImage)
        return imageViewRangesOverlap(srvImage->getDesc(), uavImage->getDesc());

      // Buffer views are conservatively treated as overlapping until their
      // element ranges can be normalized to bytes here.
      return true;
    }

    void rebindOutputMergerState(bool markDirty = true) {
      const UINT activeRenderTargetCount = getActiveRenderTargetCount();
      const bool forcedMsaa = !m_isDeferred && m_device->HasForcedMsaa() && wantsForcedMsaa();
      m_msaaScopeDirty = false;
      if (forcedMsaa != m_omForcedMsaa) {
        m_omForcedMsaa = forcedMsaa;
        markDirty = true;
      }
      IBackendImageView* backendRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT_MAX] = {};
      for (UINT rtvSlot = 0; rtvSlot < activeRenderTargetCount; rtvSlot++)
        backendRTVs[rtvSlot] = boundRtv(m_state.om.renderTargets[rtvSlot]);

      IBackendImageView* backendDSV = boundDsv(m_state.om.depthStencilView);

      static int rebindOmLogCount = 0;
      if (d3d11InfoLoggingEnabled()
       && (rebindOmLogCount < 64 || (activeRenderTargetCount == 0 && m_state.om.depthStencilView && rebindOmLogCount < 512))) {
        logMessage(LogLevel::Info,
          "rebindOM: tid=%lu activeRT=%u dsv=%p backendDsv=%p rtv0=%p uav0=%p uav1=%p",
          static_cast<unsigned long>(currentThreadId()),
          activeRenderTargetCount,
          m_state.om.depthStencilView,
          backendDSV,
          activeRenderTargetCount ? m_state.om.renderTargets[0] : nullptr,
          m_state.om.uavs[0],
          m_state.om.uavs[1]);
        rebindOmLogCount++;
      }

      m_backendCtx->bindRenderTargets(activeRenderTargetCount, backendRTVs, backendDSV);
      if (markDirty)
        this->markPipelineDirty();
    }

    bool rasterizerScissorEnabled() const {
      return m_state.rs.state
        && static_cast<D3D11RasterizerState*>(m_state.rs.state)
          ->GetBackendState().scissorEnable;
    }

    void applyRasterizerScissorState() {
      if (rasterizerScissorEnabled() && m_state.rs.numScissorRects) {
        Rect backendRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX] = {};
        for (UINT i = 0; i < m_state.rs.numScissorRects; i++) {
          backendRects[i].left = m_state.rs.scissorRects[i].left;
          backendRects[i].top = m_state.rs.scissorRects[i].top;
          backendRects[i].right = m_state.rs.scissorRects[i].right;
          backendRects[i].bottom = m_state.rs.scissorRects[i].bottom;
        }
        m_backendCtx->setScissorRects(m_state.rs.numScissorRects, backendRects);
      } else if (m_state.rs.numViewports) {
        Rect backendRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX] = {};
        for (UINT i = 0; i < m_state.rs.numViewports; i++) {
          const auto& viewport = m_state.rs.viewports[i];
          backendRects[i].left = static_cast<int32_t>(viewport.TopLeftX);
          backendRects[i].top = static_cast<int32_t>(viewport.TopLeftY);
          backendRects[i].right = static_cast<int32_t>(viewport.TopLeftX + viewport.Width);
          backendRects[i].bottom = static_cast<int32_t>(viewport.TopLeftY + viewport.Height);
        }
        m_backendCtx->setScissorRects(m_state.rs.numViewports, backendRects);
      }
    }

    // Records the whole context state into the backend again, after a
    // command list replaced it.
    void reapplyBackendState() {
      m_backendCtx->clearState();

      for (UINT slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX; slot++) {
        if (m_state.ia.vertexBuffers[slot]) {
          m_backendCtx->bindVertexBuffer(slot, getBackendBuffer(m_state.ia.vertexBuffers[slot]),
            m_state.ia.vertexOffsets[slot], m_state.ia.vertexStrides[slot]);
        }
      }
      if (m_state.ia.indexBuffer) {
        m_backendCtx->bindIndexBuffer(getBackendBuffer(m_state.ia.indexBuffer), m_state.ia.indexOffset,
          m_state.ia.indexFormat == DXGI_FORMAT_R32_UINT ? IndexType::UInt32 : IndexType::UInt16);
      }
      m_backendCtx->setPrimitiveTopology(convertTopology(m_state.ia.topology));

      auto reapplyStage = [&](ShaderStage stage, ShaderStageState& state) {
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX; slot++) {
          if (!state.constantBuffers[slot])
            continue;
          IBackendBuffer* buffer = getBackendBuffer(state.constantBuffers[slot]);
          if (state.constantBufferFirst[slot] || state.constantBufferCount[slot])
            m_backendCtx->bindConstantBufferRange(stage, slot, buffer,
              state.constantBufferFirst[slot], state.constantBufferCount[slot]);
          else
            m_backendCtx->bindConstantBuffer(stage, slot, buffer);
        }
        for (UINT slot = 0; slot < state.srvSlotEnd && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX; slot++) {
          ID3D11ShaderResourceView* view = state.srvs[slot];
          if (!view)
            continue;
          // A command list recorded next reads the view again.
          if (m_isDeferred && m_device->HasForcedMsaa())
            readForcedMsaa(static_cast<D3D11ShaderResourceView*>(view)->GetResourceRaw());
          if (IBackendImageView* imageView = getBackendImageSRV(view))
            m_backendCtx->bindShaderResource(stage, slot, imageView);
          else
            m_backendCtx->bindShaderResource(stage, slot, getBackendBufferSRV(view));
        }
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT_MAX; slot++) {
          if (state.samplers[slot])
            m_backendCtx->bindSampler(stage, slot, getBackendSampler(state.samplers[slot]));
        }
      };
      reapplyStage(ShaderStage::Vertex, m_state.vs);
      reapplyStage(ShaderStage::Hull, m_state.hs);
      reapplyStage(ShaderStage::Domain, m_state.ds);
      reapplyStage(ShaderStage::Geometry, m_state.gs);
      reapplyStage(ShaderStage::Pixel, m_state.ps);
      reapplyStage(ShaderStage::Compute, m_state.cs);

      auto reapplyUav = [&](ShaderStage stage, UINT slot, ID3D11UnorderedAccessView* view) {
        if (!view)
          return;
        if (IBackendImageView* imageView = getBackendImageUAV(view))
          m_backendCtx->bindUnorderedAccess(stage, slot, imageView);
        else
          m_backendCtx->bindUnorderedAccess(stage, slot, getBackendBufferUAV(view));
      };
      for (UINT slot = 0; slot < m_state.cs.uavSlotEnd && slot < D3D11_1_UAV_SLOT_COUNT_MAX; slot++)
        reapplyUav(ShaderStage::Compute, slot, m_state.cs.uavs[slot]);
      for (UINT slot = 0; slot < D3D11_PS_CS_UAV_REGISTER_COUNT_MAX; slot++)
        reapplyUav(ShaderStage::Pixel, slot, m_state.om.uavs[slot]);

      rebindOutputMergerState(false);
      m_backendCtx->setBlendFactor(m_state.om.blendFactor);
      m_backendCtx->setStencilRef(m_state.om.stencilRef);

      if (m_state.rs.numViewports) {
        Viewport viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE_MAX];
        for (UINT i = 0; i < m_state.rs.numViewports; i++) {
          const auto& viewport = m_state.rs.viewports[i];
          viewports[i].x        = viewport.TopLeftX;
          viewports[i].y        = viewport.TopLeftY;
          viewports[i].width    = viewport.Width;
          viewports[i].height   = viewport.Height;
          viewports[i].minDepth = viewport.MinDepth;
          viewports[i].maxDepth = viewport.MaxDepth;
        }
        m_backendCtx->setViewports(m_state.rs.numViewports, viewports);
      } else {
        m_backendCtx->setViewports(0, nullptr);
      }
      m_injectViewportJittered = false;
      applyRasterizerScissorState();

      IBackendBuffer* soBuffers[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};
      uint32_t soOffsets[D3D11_SO_BUFFER_SLOT_COUNT_MAX] = {};
      UINT soCount = 0;
      for (UINT i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT_MAX; i++) {
        if (!m_state.so.targets[i])
          continue;
        soBuffers[i] = getBackendBuffer(m_state.so.targets[i]);
        // Continue appending where the targets stopped.
        soOffsets[i] = UINT(-1);
        soCount = i + 1;
      }
      if (soCount)
        m_backendCtx->bindStreamOutputTargets(soCount, soBuffers, soOffsets);

      if (m_state.predicate) {
        m_backendCtx->setPredication(
          reinterpret_cast<D3D11Query*>(m_state.predicate)->GetBackendQuery(), m_state.predicateValue != FALSE);
      }

      // The backend no longer has our pipeline bound.
      m_currentPipeline = nullptr;
      m_currentPipelineIsCompute = false;
      m_pipelineDirty = true;
    }

    void rebindGraphicsDrawState() {
      if (m_overrides.upscaler && !m_isDeferred) {
        if (m_injectTaa && !m_overrides.upscalerInject && m_autoTaa && m_autoTaa->state() != AutoTaa::State::Searching) {
          // The game has a TAA pass after all (d3d11.upscalerAuto found it).
          if (m_injectViewportJittered)
            setInjectJitter(false);
          m_injectTaa.reset();
          logMessage(LogLevel::Warn, "upscalerInject: the game's TAA pass takes over");
        } else if (m_injectTaa || m_overrides.upscalerInject || (m_autoTaa && m_autoTaa->noTaaFor(1800))) {
          injectObserveDraw();
        }
      }
      if (m_device->HasForcedMsaa()) {
        readForcedMsaaShaderResources(ShaderStage::Vertex, m_state.vs);
        readForcedMsaaShaderResources(ShaderStage::Hull, m_state.hs);
        readForcedMsaaShaderResources(ShaderStage::Domain, m_state.ds);
        readForcedMsaaShaderResources(ShaderStage::Geometry, m_state.gs);
        readForcedMsaaShaderResources(ShaderStage::Pixel, m_state.ps);
        prepareForcedMsaaDraw();
        // A pass that refilled a copy bound its own pipeline.
        if (m_forcedMsaaPassRan && m_currentPipeline && !m_currentPipelineIsCompute)
          m_backendCtx->bindGraphicsPipeline(m_currentPipeline);
        m_forcedMsaaPassRan = false;
      }
      // Set* calls bind state eagerly, and the backend replays tracked dynamic
      // state after command-list resets. The device keeps every pipeline, so
      // draws hold no pipeline reference.
      // OM resources are still reasserted so Present/copy
      // transitions are returned to render-target state.

      // Perf: check atomic rebind flag first so steady-state draws skip the RT scan.
      if (m_outputMergerRebinds && m_backendCtx->outputMergerStateNeedsRebind()
       && (getActiveRenderTargetCount() || m_state.om.depthStencilView))
        rebindOutputMergerState(false);
    }

    void rebindRenamedConstantBuffer(ID3D11Buffer* buffer) {
      IBackendBuffer* backendBuffer = getBackendBuffer(buffer);
      if (!backendBuffer)
        return;

      auto rebindStage = [&](ShaderStage stage, const ShaderStageState& state) {
        for (UINT slot = 0; slot < state.cbSlotEnd; slot++) {
          if (state.constantBuffers[slot] != buffer)
            continue;

          const UINT first = state.constantBufferFirst[slot];
          const UINT count = state.constantBufferCount[slot];
          if (first != 0 || count != 0)
            m_backendCtx->bindConstantBufferRange(stage, slot, backendBuffer, first, count);
          else
            m_backendCtx->bindConstantBuffer(stage, slot, backendBuffer);
        }
      };

      rebindStage(ShaderStage::Vertex, m_state.vs);
      rebindStage(ShaderStage::Hull, m_state.hs);
      rebindStage(ShaderStage::Domain, m_state.ds);
      rebindStage(ShaderStage::Geometry, m_state.gs);
      rebindStage(ShaderStage::Pixel, m_state.ps);
      rebindStage(ShaderStage::Compute, m_state.cs);
    }

    void rebindRenamedInputAssemblerBuffer(ID3D11Buffer* buffer) {
      IBackendBuffer* backendBuffer = getBackendBuffer(buffer);
      if (!backendBuffer)
        return;

      for (UINT slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX; slot++) {
        if (m_state.ia.vertexBuffers[slot] == buffer) {
          m_backendCtx->bindVertexBuffer(
            slot,
            backendBuffer,
            m_state.ia.vertexOffsets[slot],
            m_state.ia.vertexStrides[slot]);
        }
      }

      if (m_state.ia.indexBuffer == buffer) {
        IndexType indexType = m_state.ia.indexFormat == DXGI_FORMAT_R32_UINT
          ? IndexType::UInt32
          : IndexType::UInt16;
        m_backendCtx->bindIndexBuffer(backendBuffer, m_state.ia.indexOffset, indexType);
      }
    }

    void rebindComputeDispatchState() {
      // prepareComputePipeline() already bound the PSO when it changed, and
      // the device keeps every pipeline alive.
      if (m_device->HasForcedMsaa()) {
        readForcedMsaaShaderResources(ShaderStage::Compute, m_state.cs);
        for (auto* uav : m_state.cs.uavs) {
          if (uav)
            writeForcedMsaaImage(static_cast<D3D11UnorderedAccessView*>(uav)->GetResourceRaw());
        }
        if (m_forcedMsaaPassRan && m_currentPipeline && m_currentPipelineIsCompute)
          m_backendCtx->bindComputePipeline(m_currentPipeline);
        m_forcedMsaaPassRan = false;
      }
    }

    // ============================================================
    // Forced multisampling (dxup.conf d3d11.msaaForce)
    // ============================================================

  public:
    // The texture is about to be read outside the context's own commands
    // (Present): its image gets what was drawn into the multisampled copy.
    void ReadForcedMsaa(ID3D11Resource* resource) {
      DXUP_CONTEXT_MT_GUARD;
      readForcedMsaa(resource);
    }

  private:
    // The passes that refill and resolve copies bind pipelines of their own:
    // the context binds its own again before it draws or dispatches.
    void forcedMsaaPassRan() {
      m_pipelineDirty = true;
      m_forcedMsaaPassRan = true;
    }

    static D3D11Texture2D* forcedMsaaTexture(ID3D11Resource* resource) {
      if (!resource)
        return nullptr;
      D3D11_RESOURCE_DIMENSION dim;
      resource->GetType(&dim);
      if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return nullptr;
      auto* texture = static_cast<D3D11Texture2D*>(resource);
      return texture->GetMsaaShadow() ? texture : nullptr;
    }

    // Deferred contexts draw into the textures themselves and leave the
    // order of copies and images to the command list's execution.
    void recordForcedMsaaUse(D3D11Texture2D* texture, MsaaUse use) {
      for (auto& recorded : m_recordedMsaaUses) {
        if (recorded.texture() == texture) {
          if (use != MsaaUse::Read)
            recorded.markWrite();
          return;
        }
      }
      m_recordedMsaaUses.emplace_back(texture, use);
    }

    static DXGI_FORMAT stencilReadFormat(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        default:                               return DXGI_FORMAT_UNKNOWN;
      }
    }

    // The image gets what was drawn into the copy; with stencil false a
    // depth-stencil image may keep an older stencil (readers of depth only).
    void resolveForcedMsaa(D3D11Texture2D* texture, bool stencil = true) {
      const ForcedMsaaFormats& formats = texture->GetMsaaFormats();
      if (texture->GetMsaaState() == D3D11Texture2D::MsaaState::ShadowNewer) {
        resolveForcedMsaaShadow(texture, formats);
        // The depth resolve leaves the image's stencil as it was: older,
        // unless the copy's stencil was not brought up to date either.
        if (formats.depth && stencilReadFormat(formats.typed) != DXGI_FORMAT_UNKNOWN
         && !texture->IsMsaaShadowStencilOld())
          texture->SetMsaaImageStencilOld(true);
      }
      if (stencil && texture->IsMsaaImageStencilOld()) {
        if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
          traceCall(formatTrace("forced MSAA resolve stencil %p", static_cast<void*>(static_cast<ID3D11Resource*>(texture))));
        m_backendCtx->copyStencil(texture->GetBackendImage(), texture->GetMsaaShadow(), formats.typed,
          stencilReadFormat(formats.typed));
        forcedMsaaPassRan();
        texture->SetMsaaImageStencilOld(false);
      }
    }

    // Draws with stencil into a depth-stencil copy refilled from the image:
    // the copy gets the image's stencil first.
    void refillForcedMsaaStencil(D3D11Texture2D* texture) {
      if (!texture->IsMsaaShadowStencilOld())
        return;
      const ForcedMsaaFormats& formats = texture->GetMsaaFormats();
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("forced MSAA upsample stencil %p", static_cast<void*>(static_cast<ID3D11Resource*>(texture))));
      m_backendCtx->copyStencil(texture->GetMsaaShadow(), texture->GetBackendImage(), formats.typed,
        stencilReadFormat(formats.typed));
      forcedMsaaPassRan();
      texture->SetMsaaShadowStencilOld(false);
    }

    void resolveForcedMsaaShadow(D3D11Texture2D* texture, const ForcedMsaaFormats& formats) {
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("forced MSAA resolve %p", static_cast<void*>(static_cast<ID3D11Resource*>(texture))));
      if (formats.depth) {
        m_backendCtx->resolveSampleZero(texture->GetBackendImage(), texture->GetMsaaShadow(), formats.typed, formats.depthRead);
        forcedMsaaPassRan();
      } else if (isMotionVectorFormat(formats.typed)) {
        // Velocity buffers: the average of a moving sample and a cleared one
        // is a vector nothing moved by (Kena's temporal anti-aliasing smeared
        // black over the edges of moving surfaces); one sample keeps a real one.
        m_backendCtx->resolveSampleZero(texture->GetBackendImage(), texture->GetMsaaShadow(), formats.typed, formats.typed);
        forcedMsaaPassRan();
      } else if (isFloatColorFormat(formats.typed)) {
        // A shader that is fine at the pixel center can yield NaN at
        // another sample's position (per-sample shading, centroid): one NaN
        // sample would make the averaged pixel NaN, which spreads through
        // temporal effects as black spots (Kena's water with SGSSAA).
        m_backendCtx->resolveFinite(texture->GetBackendImage(), texture->GetMsaaShadow(), formats.typed);
        forcedMsaaPassRan();
      } else
        m_backendCtx->resolveSubresource(texture->GetBackendImage(), 0, texture->GetMsaaShadow(), 0, formats.typed);
      texture->SetMsaaState(D3D11Texture2D::MsaaState::Synced);
    }

    void readForcedMsaa(ID3D11Resource* resource, bool stencil = true) {
      if (!m_device->HasForcedMsaa())
        return;
      D3D11Texture2D* texture = forcedMsaaTexture(resource);
      if (!texture)
        return;
      if (m_isDeferred)
        recordForcedMsaaUse(texture, MsaaUse::Read);
      else
        resolveForcedMsaa(texture, stencil);
    }

    // The images of textures with forced copies that the stage's shader
    // reads get what was drawn into the copies before it runs; views bound
    // to slots the shader does not declare cost nothing.
    void readForcedMsaaShaderResources(ShaderStage stage, ShaderStageState& state) {
      if (m_isDeferred || !(state.forcedMsaaSrvs[0] | state.forcedMsaaSrvs[1]) || !state.shader)
        return;
      const IBackendShader* shader = backendShaderOf(stage, state.shader);
      const uint64_t* declared = shader ? shader->getShaderResourceSlots() : nullptr;
      for (uint32_t word = 0; word < 2; word++) {
        uint64_t slots = state.forcedMsaaSrvs[word] & (declared ? declared[word] : ~0ull);
        for (uint32_t bit = 0; slots; bit++, slots >>= 1) {
          if (!(slots & 1))
            continue;
          auto* view = static_cast<D3D11ShaderResourceView*>(state.srvs[word * 64 + bit]);
          D3D11Texture2D* texture = view ? view->GetForcedMsaaTexture() : nullptr;
          if (texture)
            resolveForcedMsaa(texture, view->ReadsStencil());
          else
            state.forcedMsaaSrvs[word] &= ~(1ull << bit);
        }
      }
    }

    static const IBackendShader* backendShaderOf(ShaderStage stage, ID3D11DeviceChild* shader) {
      switch (stage) {
        case ShaderStage::Vertex:   return static_cast<D3D11VertexShader*>(shader)->GetBackendShader();
        case ShaderStage::Hull:     return static_cast<D3D11HullShader*>(shader)->GetBackendShader();
        case ShaderStage::Domain:   return static_cast<D3D11DomainShader*>(shader)->GetBackendShader();
        case ShaderStage::Geometry: return static_cast<D3D11GeometryShader*>(shader)->GetBackendShader();
        case ShaderStage::Pixel:    return static_cast<D3D11PixelShader*>(shader)->GetBackendShader();
        case ShaderStage::Compute:  return static_cast<D3D11ComputeShader*>(shader)->GetBackendShader();
        default:                    return nullptr;
      }
    }

    // The image is written by other means than the context's render target
    // views: it keeps what was drawn into the copy where it is not written,
    // unless all of it is replaced.
    void writeForcedMsaaImage(ID3D11Resource* resource, bool whole = false) {
      if (!m_device->HasForcedMsaa())
        return;
      D3D11Texture2D* texture = forcedMsaaTexture(resource);
      if (!texture)
        return;
      if (m_isDeferred) {
        recordForcedMsaaUse(texture, whole ? MsaaUse::Overwrite : MsaaUse::Write);
        return;
      }
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("forced MSAA image written %p whole=%d", static_cast<void*>(resource), whole ? 1 : 0));
      if (whole) {
        texture->ResetMsaaState(D3D11Texture2D::MsaaState::ImageNewer);
      } else {
        resolveForcedMsaa(texture);
        texture->SetMsaaState(D3D11Texture2D::MsaaState::ImageNewer);
      }
      m_msaaScopeDirty = true;
    }

    // Whether a whole-texture copy takes the source's copy along: the
    // destination was refilled after earlier copies and the two copies
    // match (decided before the source's image is brought up to date, which
    // then needs no stencil).
    bool forcedMsaaShadowCopyable(ID3D11Resource* dstResource, ID3D11Resource* srcResource) const {
      if (m_isDeferred || !m_device->HasForcedMsaa())
        return false;
      D3D11Texture2D* dst = forcedMsaaTexture(dstResource);
      D3D11Texture2D* src = forcedMsaaTexture(srcResource);
      if (!dst || !src || !dst->CopiesMsaaShadow() || src->GetMsaaState() == D3D11Texture2D::MsaaState::ImageNewer)
        return false;
      const ImageDesc& dstDesc = dst->GetMsaaShadow()->getDesc();
      const ImageDesc& srcDesc = src->GetMsaaShadow()->getDesc();
      return dstDesc.format == srcDesc.format && dstDesc.sampleCount == srcDesc.sampleCount
          && dstDesc.width == srcDesc.width && dstDesc.height == srcDesc.height;
    }

    // A whole texture was copied: the destination takes the source's copy
    // along, or notes that its image came from a copy.
    void copyForcedMsaaShadow(ID3D11Resource* dstResource, ID3D11Resource* srcResource, bool along) {
      if (m_isDeferred || !m_device->HasForcedMsaa())
        return;
      D3D11Texture2D* dst = forcedMsaaTexture(dstResource);
      if (!dst)
        return;
      if (!along) {
        dst->MarkMsaaImageCopied();
        return;
      }
      D3D11Texture2D* src = forcedMsaaTexture(srcResource);
      if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
        traceCall(formatTrace("forced MSAA copy %p -> %p", static_cast<void*>(srcResource), static_cast<void*>(dstResource)));
      m_backendCtx->copyImage(dst->GetMsaaShadow(), ImageSubresource{}, src->GetMsaaShadow(), ImageSubresource{});
      dst->CopyMsaaStateFrom(*src);
    }

    // A view of the copy is drawn into: unless all of it is written, it
    // first gets what was written into the image.
    void writeForcedMsaaShadow(D3D11Texture2D* texture, bool whole) {
      if (texture->GetMsaaState() == D3D11Texture2D::MsaaState::ImageNewer && !whole) {
        texture->MarkMsaaRefill();
        const ForcedMsaaFormats& formats = texture->GetMsaaFormats();
        if (m_device->diagTraceDraws.load(std::memory_order_relaxed))
          traceCall(formatTrace("forced MSAA upsample %p", static_cast<void*>(static_cast<ID3D11Resource*>(texture))));
        m_backendCtx->upsampleImage(texture->GetMsaaShadow(), texture->GetBackendImage(), formats.typed,
          formats.depth ? formats.depthRead : formats.typed);
        forcedMsaaPassRan();
        // The refill carries depth only.
        if (formats.depth && stencilReadFormat(formats.typed) != DXGI_FORMAT_UNKNOWN)
          texture->SetMsaaShadowStencilOld(true);
      }
      texture->SetMsaaState(D3D11Texture2D::MsaaState::ShadowNewer);
    }

    // Whether the next draw renders into the multisampled copies: every
    // bound render target and depth view has one (one sample count per
    // draw), and under the geometry scope (dxup.conf d3d11.msaaScope) a
    // depth view is bound, a target holds multisampled contents that the
    // draw adds to, or depth-tested geometry is drawn into a target at
    // other times. Targets that only post-processing and interface passes
    // draw into stay single-sampled, without copies going back and forth.
    bool depthStencilUsed() const {
      auto* state = static_cast<D3D11DepthStencilState*>(m_state.om.depthStencilState);
      return !state || state->UsesDepthStencil();
    }

    bool wantsForcedMsaa() const {
      const UINT count = getActiveRenderTargetCount();
      bool any = false;
      bool content = false;
      for (UINT i = 0; i < count; i++) {
        auto* view = static_cast<D3D11RenderTargetView*>(m_state.om.renderTargets[i]);
        if (!view)
          continue;
        if (!view->GetShadowView())
          return false;
        any = true;
        if (!content) {
          const D3D11Texture2D* texture = forcedMsaaTexture(view->GetResourceRaw());
          content = texture && (texture->IsMsaaGeometry() || (texture->HasMsaaContent()
                 && texture->GetMsaaState() != D3D11Texture2D::MsaaState::ImageNewer));
        }
      }
      auto* depthView = static_cast<D3D11DepthStencilView*>(m_state.om.depthStencilView);
      if (depthView) {
        if (!depthView->GetShadowView())
          return false;
        any = true;
      }
      // Post-processing passes that leave the depth view bound but test
      // neither depth nor stencil (Heroes of the Storm: ~15 chains a frame)
      // gain nothing from samples; each 4K pass would resolve again.
      return any && ((depthView && depthStencilUsed()) || content || m_overrides.msaaScope == MsaaScope::All);
    }

    // Decides again before a draw whether it renders into the copies, after
    // clears, copies and command lists changed what the targets hold.
    void updateForcedMsaaScope() {
      if (!m_msaaScopeDirty)
        return;
      m_msaaScopeDirty = false;
      if (m_isDeferred || !m_device->HasForcedMsaa())
        return;
      if (wantsForcedMsaa() != m_omForcedMsaa)
        rebindOutputMergerState();
    }

    IBackendImageView* boundRtv(ID3D11RenderTargetView* rtv) const {
      if (!rtv)
        return nullptr;
      auto* view = static_cast<D3D11RenderTargetView*>(rtv);
      return m_omForcedMsaa && view->GetShadowView() ? view->GetShadowView() : view->GetBackendView();
    }

    IBackendImageView* boundDsv(ID3D11DepthStencilView* dsv) const {
      if (!dsv)
        return nullptr;
      auto* view = static_cast<D3D11DepthStencilView*>(dsv);
      return m_omForcedMsaa && view->GetShadowView() ? view->GetShadowView() : view->GetBackendView();
    }

    // A draw writes its targets and unordered access views.
    void prepareForcedMsaaDraw() {
      const UINT count = getActiveRenderTargetCount();
      for (UINT i = 0; i < count; i++) {
        auto* view = static_cast<D3D11RenderTargetView*>(m_state.om.renderTargets[i]);
        D3D11Texture2D* texture = view ? forcedMsaaTexture(view->GetResourceRaw()) : nullptr;
        if (!texture)
          continue;
        if (m_omForcedMsaa) {
          writeForcedMsaaShadow(texture, false);
          // The copy resolves in the format it was last drawn in.
          texture->SetMsaaColorFormat(view->GetPipelineFormat());
          if (m_state.om.depthStencilView && depthStencilUsed())
            texture->MarkMsaaGeometry();
        } else {
          writeForcedMsaaImage(texture);
        }
      }
      if (auto* view = static_cast<D3D11DepthStencilView*>(m_state.om.depthStencilView)) {
        if (D3D11Texture2D* texture = forcedMsaaTexture(view->GetResourceRaw())) {
          // Draws that cannot change depth leave the image as current as it
          // was (the depth resolve carries depth only, not stencil): Unreal
          // Engine 4's light draws test against a writable view, mark
          // stencil and read the depth as a texture, which resolved the 4K
          // depth before each light (Kena: ~100 resolves a frame).
          auto* state = static_cast<D3D11DepthStencilState*>(m_state.om.depthStencilState);
          const bool writesDepth = !view->IsDepthReadOnly() && (!state || state->WritesDepth());
          if (!m_omForcedMsaa) {
            // Single-sampled draws use the image: written, read, or not
            // touched with depth and stencil off.
            const bool writesStencil = view->HasStencil() && !view->IsStencilReadOnly() && state && state->WritesStencil();
            if (writesDepth || writesStencil)
              writeForcedMsaaImage(texture);
            else if (depthStencilUsed())
              readForcedMsaa(texture, state && state->UsesStencil());
          } else {
            if (writesDepth)
              writeForcedMsaaShadow(texture, false);
            else if (texture->GetMsaaState() == D3D11Texture2D::MsaaState::ImageNewer) {
              // Read only: the copy only needs what the image holds.
              writeForcedMsaaShadow(texture, false);
              texture->SetMsaaState(D3D11Texture2D::MsaaState::Synced);
            }
            if (state && state->UsesStencil())
              refillForcedMsaaStencil(texture);
            // Stencil written without depth leaves the image's depth as
            // current as it was, but not its stencil.
            if (view->HasStencil() && !view->IsStencilReadOnly() && state && state->WritesStencil())
              texture->SetMsaaImageStencilOld(true);
          }
        }
      }
      for (auto* uav : m_state.om.uavs) {
        if (uav)
          writeForcedMsaaImage(static_cast<D3D11UnorderedAccessView*>(uav)->GetResourceRaw());
      }
    }

    // Clears write the texture and, from the immediate context, its copy
    // alike: a clear of all of it leaves both the same, a partial one keeps
    // what was newer. The copy's view to clear as well, if any.
    IBackendImageView* clearedForcedMsaaShadow(ID3D11Resource* resource, IBackendImageView* shadowView, bool whole) {
      if (!m_device->HasForcedMsaa())
        return nullptr;
      D3D11Texture2D* texture = forcedMsaaTexture(resource);
      if (!texture)
        return nullptr;
      if (m_isDeferred || !shadowView) {
        writeForcedMsaaImage(resource, whole);
        return nullptr;
      }
      if (whole) {
        texture->ResetMsaaState(D3D11Texture2D::MsaaState::Synced);
        m_msaaScopeDirty = true;
      }
      return shadowView;
    }

    // A discarded texture's contents are undefined in both.
    void discardForcedMsaa(ID3D11Resource* resource) {
      if (m_isDeferred || !m_device->HasForcedMsaa())
        return;
      if (D3D11Texture2D* texture = forcedMsaaTexture(resource)) {
        texture->ResetMsaaState(D3D11Texture2D::MsaaState::Synced);
        m_msaaScopeDirty = true;
      }
    }

    // A command list read and wrote textures in its own order: their images
    // are brought up to date before, unless the list first replaces them,
    // and written ones are newer after.
    void beginForcedMsaaCommandList(const std::vector<RecordedMsaaUse>& uses) {
      for (const auto& use : uses) {
        if (m_isDeferred) {
          recordForcedMsaaUse(use.texture(), use.first());
          if (use.write())
            recordForcedMsaaUse(use.texture(), MsaaUse::Write);
        } else if (use.first() != MsaaUse::Overwrite) {
          resolveForcedMsaa(use.texture());
        }
      }
    }

    void endForcedMsaaCommandList(const std::vector<RecordedMsaaUse>& uses) {
      if (m_isDeferred)
        return;
      for (const auto& use : uses) {
        if (!use.write())
          continue;
        if (use.first() == MsaaUse::Overwrite)
          use.texture()->ResetMsaaState(D3D11Texture2D::MsaaState::ImageNewer);
        else
          use.texture()->SetMsaaState(D3D11Texture2D::MsaaState::ImageNewer);
        m_msaaScopeDirty = true;
      }
    }

    bool outputMergerReferencesResource(ID3D11Resource* resource) const {
      if (!resource)
        return false;

      const UINT activeRenderTargetCount = getActiveRenderTargetCount();
      for (UINT rtvSlot = 0; rtvSlot < activeRenderTargetCount; rtvSlot++) {
        if (viewReferencesResource(m_state.om.renderTargets[rtvSlot], resource))
          return true;
      }

      return viewReferencesResource(m_state.om.depthStencilView, resource);
    }

    // The render targets, the written planes of the depth-stencil view and
    // the pixel UAVs unbind what reads or writes the same parts elsewhere.
    void unbindOutputMergerHazards() {
      for (UINT slot = 0; slot < m_state.om.numRenderTargets; slot++) {
        if (auto* rtv = m_state.om.renderTargets[slot])
          unbindForWrite(renderTargetRange(rtv), WriteSource::OutputMerger);
      }
      if (auto* dsv = m_state.om.depthStencilView) {
        const D3D11ViewRange range = depthStencilWriteRange(dsv);
        if (range.planes)
          unbindForWrite(range, WriteSource::OutputMerger);
      }
      for (UINT slot = 0; slot < D3D11_PS_CS_UAV_REGISTER_COUNT_MAX; slot++) {
        if (auto* uav = m_state.om.uavs[slot])
          unbindForWrite(unorderedAccessRange(uav), WriteSource::OutputMerger);
      }
    }

    // ---- Read/write hazards (d3d11_binding_hazards.h) ----

    enum class WriteSource { OutputMerger, Compute, StreamOutput };

    // Whether any part of resource is bound for writing.
    bool resourceBoundForWriting(ID3D11Resource* resource) {
      if (!resource)
        return false;
      for (UINT slot = 0; slot < m_state.om.numRenderTargets; slot++) {
        if (viewResource(m_state.om.renderTargets[slot]) == resource)
          return true;
      }
      if (viewResource(m_state.om.depthStencilView) == resource)
        return true;
      for (auto* uav : m_state.om.uavs) {
        if (viewResource(uav) == resource)
          return true;
      }
      const UINT csUavEnd = boundUavSlotEnd(m_state.cs);
      for (UINT slot = 0; slot < csUavEnd; slot++) {
        if (viewResource(m_state.cs.uavs[slot]) == resource)
          return true;
      }
      for (auto* target : m_state.so.targets) {
        if (target && static_cast<ID3D11Resource*>(target) == resource)
          return true;
      }
      return false;
    }

    // Whether reading range collides with a part of its resource bound for
    // writing.
    bool readHitsWrite(const D3D11ViewRange& read) {
      ID3D11Resource* resource = read.resource;
      for (UINT slot = 0; slot < m_state.om.numRenderTargets; slot++) {
        auto* rtv = m_state.om.renderTargets[slot];
        if (viewResource(rtv) == resource && viewRangesOverlap(read, renderTargetRange(rtv)))
          return true;
      }
      auto* dsv = m_state.om.depthStencilView;
      if (viewResource(dsv) == resource && viewRangesOverlap(read, depthStencilWriteRange(dsv)))
        return true;
      for (auto* uav : m_state.om.uavs) {
        if (viewResource(uav) == resource && viewRangesOverlap(read, unorderedAccessRange(uav)))
          return true;
      }
      const UINT csUavEnd = boundUavSlotEnd(m_state.cs);
      for (UINT slot = 0; slot < csUavEnd; slot++) {
        auto* uav = m_state.cs.uavs[slot];
        if (viewResource(uav) == resource && viewRangesOverlap(read, unorderedAccessRange(uav)))
          return true;
      }
      for (auto* target : m_state.so.targets) {
        if (target && static_cast<ID3D11Resource*>(target) == resource)
          return true;
      }
      return false;
    }

    ID3D11ShaderResourceView* readableView(ID3D11ShaderResourceView* view) {
      if (view && resourceBoundForWriting(viewResource(view)) && readHitsWrite(shaderResourceRange(view)))
        return nullptr;
      return view;
    }

    ID3D11Buffer* readableBuffer(ID3D11Buffer* buffer) {
      return buffer && resourceBoundForWriting(buffer) ? nullptr : buffer;
    }

    // Unbinds the reads of the parts write covers, in every stage, and the
    // writes of the other kinds; the writes of the binding call stay.
    void unbindForWrite(const D3D11ViewRange& write, WriteSource source) {
      if (!write.resource)
        return;
      auto unbindStageSrvs = [&](ShaderStage stage, ShaderStageState& state) {
        const UINT srvSlotEnd = boundSrvSlotEnd(state);
        for (UINT slot = 0; slot < srvSlotEnd; slot++) {
          auto* srv = state.srvs[slot];
          if (srv && viewResource(srv) == write.resource && viewRangesOverlap(shaderResourceRange(srv), write)) {
            clearContextReference(state.srvs[slot]);
            bindShaderResourceView(stage, slot, nullptr);
          }
        }
      };
      unbindStageSrvs(ShaderStage::Vertex, m_state.vs);
      unbindStageSrvs(ShaderStage::Hull, m_state.hs);
      unbindStageSrvs(ShaderStage::Domain, m_state.ds);
      unbindStageSrvs(ShaderStage::Geometry, m_state.gs);
      unbindStageSrvs(ShaderStage::Pixel, m_state.ps);
      unbindStageSrvs(ShaderStage::Compute, m_state.cs);

      // Buffers are read as vertex and index buffers too.
      if (write.whole) {
        for (UINT slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT_MAX; slot++) {
          ID3D11Buffer* buffer = m_state.ia.vertexBuffers[slot];
          if (buffer && static_cast<ID3D11Resource*>(buffer) == write.resource) {
            clearContextReference(m_state.ia.vertexBuffers[slot]);
            m_backendCtx->bindVertexBuffer(slot, nullptr, m_state.ia.vertexOffsets[slot], m_state.ia.vertexStrides[slot]);
          }
        }
        ID3D11Buffer* indexBuffer = m_state.ia.indexBuffer;
        if (indexBuffer && static_cast<ID3D11Resource*>(indexBuffer) == write.resource) {
          clearContextReference(m_state.ia.indexBuffer);
          m_backendCtx->bindIndexBuffer(nullptr, m_state.ia.indexOffset,
            m_state.ia.indexFormat == DXGI_FORMAT_R32_UINT ? IndexType::UInt32 : IndexType::UInt16);
        }
      }

      if (source != WriteSource::OutputMerger) {
        bool outputMergerDirty = false;
        for (UINT slot = 0; slot < m_state.om.numRenderTargets; slot++) {
          auto* rtv = m_state.om.renderTargets[slot];
          if (rtv && viewResource(rtv) == write.resource && viewRangesOverlap(renderTargetRange(rtv), write)) {
            clearContextReference(m_state.om.renderTargets[slot]);
            outputMergerDirty = true;
          }
        }
        auto* dsv = m_state.om.depthStencilView;
        if (dsv && viewResource(dsv) == write.resource && viewRangesOverlap(depthStencilWriteRange(dsv), write)) {
          clearContextReference(m_state.om.depthStencilView);
          outputMergerDirty = true;
        }
        for (UINT slot = 0; slot < D3D11_PS_CS_UAV_REGISTER_COUNT_MAX; slot++) {
          auto* uav = m_state.om.uavs[slot];
          if (uav && viewResource(uav) == write.resource && viewRangesOverlap(unorderedAccessRange(uav), write))
            clearPixelUnorderedAccessView(slot);
        }
        if (outputMergerDirty) {
          UINT active = m_state.om.numRenderTargets;
          while (active && !m_state.om.renderTargets[active - 1])
            active--;
          m_state.om.numRenderTargets = active;
          rebindOutputMergerState();
        }
      }

      if (source != WriteSource::Compute) {
        const UINT csUavEnd = boundUavSlotEnd(m_state.cs);
        for (UINT slot = 0; slot < csUavEnd; slot++) {
          auto* uav = m_state.cs.uavs[slot];
          if (uav && viewResource(uav) == write.resource && viewRangesOverlap(unorderedAccessRange(uav), write)) {
            clearContextReference(m_state.cs.uavs[slot]);
            bindUnorderedAccessView(ShaderStage::Compute, slot, nullptr);
          }
        }
      }

      if (source != WriteSource::StreamOutput) {
        bool streamOutputDirty = false;
        for (auto*& target : m_state.so.targets) {
          if (target && static_cast<ID3D11Resource*>(target) == write.resource) {
            clearContextReference(target);
            streamOutputDirty = true;
          }
        }
        if (streamOutputDirty)
          rebindStreamOutputTargets();
      }
    }

    void bindShaderResourceView(
        ShaderStage               stage,
        UINT                      slot,
        ID3D11ShaderResourceView* view) {
      IBackendImageView* backendImageView = getBackendImageSRV(view);
      if (backendImageView) {
        m_backendCtx->bindShaderResource(stage, slot, backendImageView);
      } else {
        IBackendBufferView* backendBufferView = getBackendBufferSRV(view);
        m_backendCtx->bindShaderResource(stage, slot, backendBufferView);
      }
    }

    void bindUnorderedAccessView(
        ShaderStage                stage,
        UINT                       slot,
        ID3D11UnorderedAccessView* view) {
      IBackendImageView* backendImageView = getBackendImageUAV(view);
      if (backendImageView) {
        m_backendCtx->bindUnorderedAccess(stage, slot, backendImageView);
      } else {
        IBackendBufferView* backendBufferView = getBackendBufferUAV(view);
        m_backendCtx->bindUnorderedAccess(stage, slot, backendBufferView);
      }
    }

    void rebindBufferViewsForResource(ID3D11Resource* resource) {
      if (!resource)
        return;

      auto rebindStageSrvs = [&](ShaderStage stage, ShaderStageState& state) {
        const UINT srvSlotEnd = boundSrvSlotEnd(state);
        for (UINT srvSlot = 0; srvSlot < srvSlotEnd; srvSlot++) {
          auto* srv = state.srvs[srvSlot];
          if (viewReferencesResource(srv, resource))
            bindShaderResourceView(stage, srvSlot, srv);
        }
      };

      auto rebindStageUavs = [&](ShaderStage stage, ID3D11UnorderedAccessView** stateSlots, UINT slotCount) {
        for (UINT uavSlot = 0; uavSlot < slotCount; uavSlot++) {
          auto* uav = stateSlots[uavSlot];
          if (viewReferencesResource(uav, resource))
            bindUnorderedAccessView(stage, uavSlot, uav);
        }
      };

      rebindStageSrvs(ShaderStage::Vertex, m_state.vs);
      rebindStageSrvs(ShaderStage::Hull, m_state.hs);
      rebindStageSrvs(ShaderStage::Domain, m_state.ds);
      rebindStageSrvs(ShaderStage::Geometry, m_state.gs);
      rebindStageSrvs(ShaderStage::Pixel, m_state.ps);
      rebindStageSrvs(ShaderStage::Compute, m_state.cs);

      rebindStageUavs(ShaderStage::Pixel, m_state.om.uavs, D3D11_PS_CS_UAV_REGISTER_COUNT_MAX);
      rebindStageUavs(ShaderStage::Compute, m_state.cs.uavs, boundUavSlotEnd(m_state.cs));
    }

    bool queuePendingBufferUpdate(D3D11Buffer* buffer, uint64_t offset, uint64_t size, const void* data) {
      if (!buffer || !data || !size)
        return false;

      constexpr uint64_t kMaxQueuedUpdateSize = 256ull * 1024ull;
      constexpr uint64_t kMaxQueuedBytes = 8ull * 1024ull * 1024ull;
      if (size > kMaxQueuedUpdateSize)
        return false;

      if (m_pendingBufferUpdateBytes + size > kMaxQueuedBytes)
        flushPendingBufferUpdates();

      auto it = std::find_if(
        m_pendingBufferUpdates.begin(),
        m_pendingBufferUpdates.end(),
        [&](const PendingBufferUpdate& pending) { return pending.buffer == buffer; });

      if (it == m_pendingBufferUpdates.end()) {
        buffer->AddRefPrivate();
        PendingBufferUpdate pending = {};
        pending.buffer = buffer;
        m_pendingBufferUpdates.push_back(std::move(pending));
        it = m_pendingBufferUpdates.end() - 1;
      }

      PendingBufferUpdateRange range = {};
      range.offset = offset;
      range.data.resize(static_cast<size_t>(size));
      std::memcpy(range.data.data(), data, static_cast<size_t>(size));
      it->ranges.push_back(std::move(range));
      m_pendingBufferUpdateBytes += size;
      return true;
    }

    void flushPendingBufferUpdates() {
      if (m_pendingBufferUpdates.empty())
        return;

      flushPendingBufferUpdatesSlow();
    }

    void flushPendingBufferUpdatesSlow() {
      for (auto& pending : m_pendingBufferUpdates) {
        if (!pending.buffer || pending.ranges.empty())
          continue;

        IBackendBuffer* backendBuffer = getBackendBuffer(pending.buffer);
        if (backendBuffer) {
          std::vector<BufferUpdateRegion> regions;
          regions.reserve(pending.ranges.size());
          for (auto& range : pending.ranges) {
            if (range.data.empty())
              continue;

            BufferUpdateRegion region = {};
            region.offset = range.offset;
            region.size = range.data.size();
            region.data = range.data.data();
            regions.push_back(region);
          }

          if (!regions.empty())
            m_backendCtx->updateBufferRegions(backendBuffer, regions.data(), static_cast<uint32_t>(regions.size()));
        }

        pending.buffer->ReleasePrivate();
        pending.buffer = nullptr;
      }

      m_pendingBufferUpdates.clear();
      m_pendingBufferUpdateBytes = 0;
    }

    // ============================================================
    // Internal helpers - constant buffer binding
    // ============================================================

    void setConstantBuffers1(
        ShaderStage         stage,
        ShaderStageState&   state,
        UINT                StartSlot,
        UINT                NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers,
        const UINT*         pFirstConstant,
        const UINT*         pNumConstants) {
      for (UINT i = 0; i < NumBuffers; i++) {
        UINT slot = StartSlot + i;
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX)
          break;

        ID3D11Buffer* newBuf = ppConstantBuffers ? ppConstantBuffers[i] : nullptr;
        UINT first = (pFirstConstant ? pFirstConstant[i] : 0);
        UINT count = (pNumConstants ? pNumConstants[i] : 0);
        if (state.constantBuffers[slot] == newBuf
         && state.constantBufferFirst[slot] == first
         && state.constantBufferCount[slot] == count)
          continue;

        replaceContextReference(state.constantBuffers[slot], newBuf);
        state.constantBufferFirst[slot] = first;
        state.constantBufferCount[slot] = count;
        if (newBuf && slot >= state.cbSlotEnd)
          state.cbSlotEnd = slot + 1;

        IBackendBuffer* backendBuf = getBackendBuffer(newBuf);

        if (first != 0 || count != 0) {
          m_backendCtx->bindConstantBufferRange(stage, slot, backendBuf, first, count);
        } else {
          m_backendCtx->bindConstantBuffer(stage, slot, backendBuf);
        }
      }
    }

    void getConstantBuffers1(
        const ShaderStageState& state,
        UINT                    StartSlot,
        UINT                    NumBuffers,
        ID3D11Buffer**          ppConstantBuffers,
        UINT*                   pFirstConstant,
        UINT*                   pNumConstants) {
      getConstantBuffers(
        state.constantBuffers,
        StartSlot,
        NumBuffers,
        ppConstantBuffers);

      for (UINT i = 0; i < NumBuffers; i++) {
        const UINT slot = StartSlot + i;
        if (pFirstConstant)
          pFirstConstant[i] = slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX
            ? state.constantBufferFirst[slot]
            : 0;
        if (pNumConstants)
          pNumConstants[i] = slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX
            ? state.constantBufferCount[slot]
            : 0;
      }
    }

    void setConstantBuffers(
        ShaderStage       stage,
        ShaderStageState& state,
        UINT              startSlot,
        UINT              numBuffers,
        ID3D11Buffer* const* ppBuffers) {
      prefetchForBind<D3D11Buffer>(ppBuffers, state.constantBuffers, startSlot, numBuffers,
        D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX);
      for (UINT i = 0; i < numBuffers; i++) {
        UINT slot = startSlot + i;
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX)
          break;

        ID3D11Buffer* buffer = ppBuffers ? ppBuffers[i] : nullptr;
        if (state.constantBuffers[slot] == buffer
         && state.constantBufferFirst[slot] == 0
         && state.constantBufferCount[slot] == 0)
          continue;

        replaceContextReference(state.constantBuffers[slot], buffer);
        state.constantBufferFirst[slot] = 0;
        state.constantBufferCount[slot] = 0;
        if (buffer && slot >= state.cbSlotEnd)
          state.cbSlotEnd = slot + 1;
        IBackendBuffer* backendBuf = getBackendBuffer(buffer);
        m_backendCtx->bindConstantBuffer(stage, slot, backendBuf);
      }
    }

    void getConstantBuffers(
        ID3D11Buffer* const* stateSlots,
        UINT                 startSlot,
        UINT                 numBuffers,
        ID3D11Buffer**       ppBuffers) {
      if (!ppBuffers) return;
      for (UINT i = 0; i < numBuffers; i++) {
        UINT slot = startSlot + i;
        ppBuffers[i] = (slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT_MAX)
          ? stateSlots[slot] : nullptr;
        if (ppBuffers[i])
          ppBuffers[i]->AddRef();
      }
    }

    // ============================================================
    // Internal helpers - SRV binding
    // ============================================================

    void setShaderResources(
        ShaderStage                      stage,
        ShaderStageState&                state,
        UINT                             startSlot,
        UINT                             numViews,
        ID3D11ShaderResourceView* const* ppViews) {
      prefetchForBind<D3D11ShaderResourceView>(ppViews, state.srvs, startSlot, numViews,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX);
      for (UINT i = 0; i < numViews; i++) {
        UINT slot = startSlot + i;
        if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX)
          break;

        // A view of something bound for writing is bound as NULL. A view
        // still bound passed that test when it was bound: writes bound
        // since then unbound it otherwise.
        ID3D11ShaderResourceView* requested = ppViews ? ppViews[i] : nullptr;
        if (requested == state.srvs[slot])
          continue;
        ID3D11ShaderResourceView* view = readableView(requested);
        if (view && m_device->HasForcedMsaa()) {
          // Deferred contexts record the read; the immediate context brings
          // the image up to date when a shader that reads the slot runs.
          if (m_isDeferred)
            readForcedMsaa(static_cast<D3D11ShaderResourceView*>(view)->GetResourceRaw());
          else if (static_cast<D3D11ShaderResourceView*>(view)->GetForcedMsaaTexture())
            state.forcedMsaaSrvs[slot >> 6] |= 1ull << (slot & 63);
        }
        if (replaceContextReference(state.srvs[slot], view)) {
          if (view && slot >= state.srvSlotEnd)
            state.srvSlotEnd = slot + 1;
          bindShaderResourceView(stage, slot, state.srvs[slot]);
        }
      }
    }

    void getShaderResources(
        ID3D11ShaderResourceView** stateSlots,
        UINT                       startSlot,
        UINT                       numViews,
        ID3D11ShaderResourceView** ppViews) {
      if (!ppViews) return;
      for (UINT i = 0; i < numViews; i++) {
        UINT slot = startSlot + i;
        ppViews[i] = (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT_MAX)
          ? stateSlots[slot] : nullptr;
        if (ppViews[i])
          ppViews[i]->AddRef();
      }
    }

    // ============================================================
    // Internal helpers - Sampler binding
    // ============================================================

    void setSamplers(
        ShaderStage                  stage,
        ID3D11SamplerState**         stateSlots,
        UINT                         startSlot,
        UINT                         numSamplers,
        ID3D11SamplerState* const*   ppSamplers) {
      for (UINT i = 0; i < numSamplers; i++) {
        UINT slot = startSlot + i;
        if (slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT_MAX)
          break;

        ID3D11SamplerState* sampler = ppSamplers ? ppSamplers[i] : nullptr;
        if (replaceContextReference(stateSlots[slot], sampler)) {
          IBackendSampler* backendSampler = getBackendSampler(stateSlots[slot]);
          m_backendCtx->bindSampler(stage, slot, backendSampler);
        }
      }
    }

    void getSamplers(
        ID3D11SamplerState** stateSlots,
        UINT                 startSlot,
        UINT                 numSamplers,
        ID3D11SamplerState** ppSamplers) {
      if (!ppSamplers) return;
      for (UINT i = 0; i < numSamplers; i++) {
        UINT slot = startSlot + i;
        ppSamplers[i] = (slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT_MAX)
          ? stateSlots[slot] : nullptr;
        if (ppSamplers[i])
          ppSamplers[i]->AddRef();
      }
    }

    // ============================================================
    // Internal helpers - Backend resource extraction
    //
    // These cast the D3D11 COM wrappers to our internal types
    // to retrieve the underlying backend resource pointers.
    // The COM wrapper classes (D3D11Buffer, etc.) are expected
    // to provide a getBackend*() accessor.
    // ============================================================

    // These cast D3D11 COM wrappers to our internal types.
    // Requires d3d11_device.h to be included before this header
    // (which d3d11_main.cpp already does).
    // The memory a buffer has for this context: a deferred context sees
    // what it discarded the buffer to, other contexts only once its command
    // list runs.
    IBackendBuffer* getBackendBuffer(ID3D11Buffer* buf) const {
      if (!buf) return nullptr;
      auto* buffer = static_cast<D3D11Buffer*>(buf);
      if (!m_bufferOverrides.empty()) {
        auto it = m_bufferOverrides.find(buffer);
        if (it != m_bufferOverrides.end())
          return it->second.get();
      }
      return buffer->GetBackendBuffer();
    }
    static IBackendImageView* getBackendRTV(ID3D11RenderTargetView* rtv) {
      if (!rtv) return nullptr;
      return static_cast<D3D11RenderTargetView*>(rtv)->GetBackendView();
    }
    static IBackendImageView* getBackendDSV(ID3D11DepthStencilView* dsv) {
      if (!dsv) return nullptr;
      return static_cast<D3D11DepthStencilView*>(dsv)->GetBackendView();
    }
    static IBackendImageView* getBackendImageSRV(ID3D11ShaderResourceView* srv) {
      if (!srv) return nullptr;
      return static_cast<D3D11ShaderResourceView*>(srv)->GetBackendView();
    }
    static IBackendBufferView* getBackendBufferSRV(ID3D11ShaderResourceView* srv) {
      if (!srv) return nullptr;
      return static_cast<D3D11ShaderResourceView*>(srv)->GetBackendBufferView();
    }
    static IBackendImageView* getBackendImageUAV(ID3D11UnorderedAccessView* uav) {
      if (!uav) return nullptr;
      return static_cast<D3D11UnorderedAccessView*>(uav)->GetBackendView();
    }
    static IBackendBufferView* getBackendBufferUAV(ID3D11UnorderedAccessView* uav) {
      if (!uav) return nullptr;
      return static_cast<D3D11UnorderedAccessView*>(uav)->GetBackendBufferView();
    }
    static IBackendSampler* getBackendSampler(ID3D11SamplerState* sampler) {
      if (!sampler) return nullptr;
      return static_cast<D3D11SamplerState*>(sampler)->GetBackendSampler();
    }
    static IBackendImage* getBackendImage(ID3D11Resource* res) {
      if (!res) return nullptr;
      D3D11_RESOURCE_DIMENSION dim;
      res->GetType(&dim);
      switch (dim) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
          return static_cast<D3D11Texture1D*>(res)->GetBackendImage();
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
          return static_cast<D3D11Texture2D*>(res)->GetBackendImage();
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
          return static_cast<D3D11Texture3D*>(res)->GetBackendImage();
        default:
          return nullptr;
      }
    }
    static IBackendQuery* getBackendQuery(ID3D11Asynchronous* async) {
      if (!async) return nullptr;
      return static_cast<D3D11Query*>(async)->GetBackendQuery();
    }

    // ============================================================
    // Topology conversion
    // ============================================================

    static PrimitiveTopology convertTopology(D3D11_PRIMITIVE_TOPOLOGY topology) {
      switch (topology) {
        case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:        return PrimitiveTopology::PointList;
        case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:         return PrimitiveTopology::LineList;
        case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:        return PrimitiveTopology::LineStrip;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:     return PrimitiveTopology::TriangleList;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:    return PrimitiveTopology::TriangleStrip;
        case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:     return PrimitiveTopology::LineListAdj;
        case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:    return PrimitiveTopology::LineStripAdj;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return PrimitiveTopology::TriangleListAdj;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:return PrimitiveTopology::TriangleStripAdj;
        default:
          // Patch topologies: D3D11 uses 33-64 for 1-32 control points
          if (topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
           && topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
            uint32_t controlPointOffset = static_cast<uint32_t>(topology)
              - static_cast<uint32_t>(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
            return static_cast<PrimitiveTopology>(
              static_cast<uint32_t>(PrimitiveTopology::PatchList1) + controlPointOffset);
          }
          return PrimitiveTopology::Undefined;
      }
    }
  };

  // ============================================================
  // ID3DDeviceContextState: a whole context state, swapped in and out
  // with SwapDeviceContextState. Holds its bindings like a context does.
  // ============================================================
  class D3D11DeviceContextState : public D3D11DeviceChild<ID3DDeviceContextState> {
  public:
    explicit D3D11DeviceContextState(D3D11Device* pDevice)
    : D3D11DeviceChild<ID3DDeviceContextState>(pDevice) { }

    ~D3D11DeviceContextState() override {
      D3D11ImmediateContext::releaseStateBindings(m_state);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3DDeviceContextState)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    D3D11ImmediateContext::SavedState m_state;
  };

  inline void STDMETHODCALLTYPE D3D11ImmediateContext::SwapDeviceContextState(
      ID3DDeviceContextState*  pState,
      ID3DDeviceContextState** ppPreviousState) {
    DXUP_CONTEXT_MT_GUARD;
    if (ppPreviousState)
      *ppPreviousState = nullptr;
    if (m_isDeferred)
      return;

    // The state the context started with belongs to an implicit object.
    if (!m_activeContextState) {
      auto* initial = new D3D11DeviceContextState(m_device);
      initial->AddRefPrivate();
      initial->Release();
      m_activeContextState = initial;
    }

    auto* previous = static_cast<D3D11DeviceContextState*>(m_activeContextState);
    if (ppPreviousState) {
      previous->AddRef();
      *ppPreviousState = previous;
    }

    auto* next = static_cast<D3D11DeviceContextState*>(pState);
    if (!next || next == previous)
      return;

    flushPendingBufferUpdates();

    // Park the current state in the previous object and take the next one;
    // the bindings keep their references while they move.
    std::swap(previous->m_state, m_state);
    std::swap(m_state, next->m_state);
    next->AddRefPrivate();
    m_activeContextState = next;
    previous->ReleasePrivate();

    reapplyBackendState();
  }

  inline void D3D11Device::submitThen(std::function<void()> fn) {
    m_immediateContext->submitThen(std::move(fn));
  }

  inline void STDMETHODCALLTYPE D3D11Device::GetImmediateContext2(ID3D11DeviceContext2** ppImmediateContext) {
    GetImmediateContext(reinterpret_cast<ID3D11DeviceContext**>(ppImmediateContext));
  }

  inline void STDMETHODCALLTYPE D3D11Device::GetImmediateContext3(ID3D11DeviceContext3** ppImmediateContext) {
    GetImmediateContext(reinterpret_cast<ID3D11DeviceContext**>(ppImmediateContext));
  }

  inline HRESULT STDMETHODCALLTYPE D3D11Device::CreateDeferredContext2(UINT ContextFlags, ID3D11DeviceContext2** ppDeferredContext) {
    return CreateDeferredContext(ContextFlags, reinterpret_cast<ID3D11DeviceContext**>(ppDeferredContext));
  }

  inline HRESULT STDMETHODCALLTYPE D3D11Device::CreateDeferredContext3(UINT ContextFlags, ID3D11DeviceContext3** ppDeferredContext) {
    return CreateDeferredContext(ContextFlags, reinterpret_cast<ID3D11DeviceContext**>(ppDeferredContext));
  }

  inline HRESULT STDMETHODCALLTYPE D3D11Fence::CreateSharedHandle(
      const SECURITY_ATTRIBUTES* pAttributes,
      DWORD                      dwAccess,
      LPCWSTR                    lpName,
      HANDLE*                    pHandle) {
    ID3D12Device* device = m_device ? m_device->nativeDevice() : nullptr;
    if (!device || !pHandle)
      return E_INVALIDARG;
    return device->CreateSharedHandle(m_fence.Get(), pAttributes, dwAccess, lpName, pHandle);
  }

  inline HRESULT STDMETHODCALLTYPE D3D11Device::CreateDeviceContextState(
      UINT                     Flags,
      const D3D_FEATURE_LEVEL* pFeatureLevels,
      UINT                     FeatureLevels,
      UINT                     SDKVersion,
      REFIID                   EmulatedInterface,
      D3D_FEATURE_LEVEL*       pChosenFeatureLevel,
      ID3DDeviceContextState** ppContextState) {
    (void)Flags;
    (void)SDKVersion;
    (void)EmulatedInterface;
    if (!pFeatureLevels || !FeatureLevels)
      return E_INVALIDARG;

    // The first requested level the device supports.
    D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL(0);
    for (UINT i = 0; i < FeatureLevels; i++) {
      if (pFeatureLevels[i] <= m_featureLevel) {
        chosen = pFeatureLevels[i];
        break;
      }
    }
    if (!chosen)
      return E_INVALIDARG;
    if (pChosenFeatureLevel)
      *pChosenFeatureLevel = chosen;
    if (!ppContextState)
      return S_FALSE;

    *ppContextState = new D3D11DeviceContextState(this);
    return S_OK;
  }

} // namespace dxup

#endif // _WIN32
