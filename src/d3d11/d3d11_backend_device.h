#pragma once

#include "../directx/directx_backend.h"
#include "d3d11_d3d12_bridge.h"

#include <mutex>

#ifdef _WIN32
#include <d3d11.h>
#endif

#ifdef DXUP_D3D12_BACKEND
#include "../d3d12/d3d12_backend_device.h"
#include "../d3d12/d3d12_cs_thread.h"
#endif

namespace dxup {

  // ============================================================
  // D3D11 Backend Device Wrapper
  //
  // Provides a D3D11-compatible device interface that delegates
  // to the D3D12 backend through IBackendDevice.
  // ============================================================
  class D3D11BackendDevice {
  public:
    D3D11BackendDevice(BackendType type, const LUID* adapterLuid = nullptr);
    ~D3D11BackendDevice();

    IBackendDevice* getBackend() const { return m_backend.get(); }
    BackendType getBackendType() const { return m_backendType; }

    // Resource creation with D3D11-style descriptors
#ifdef _WIN32
    Rc<IBackendBuffer> createBuffer(const D3D11_BUFFER_DESC* desc,
                                     const D3D11_SUBRESOURCE_DATA* initialData);
    Rc<IBackendImage> createTexture1D(const D3D11_TEXTURE1D_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* initialData);
    Rc<IBackendImage> createTexture2D(const D3D11_TEXTURE2D_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* initialData);
    Rc<IBackendImage> createTexture3D(const D3D11_TEXTURE3D_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* initialData);

    Rc<IBackendSampler> createSampler(const D3D11_SAMPLER_DESC* desc);

    Rc<IBackendShader> createVertexShader(const void* bytecode, size_t size);
    Rc<IBackendShader> createPixelShader(const void* bytecode, size_t size);
    Rc<IBackendShader> createGeometryShader(const void* bytecode, size_t size);
    Rc<IBackendShader> createGeometryShaderWithStreamOutput(const void* bytecode, size_t size);
    Rc<IBackendShader> createHullShader(const void* bytecode, size_t size);
    Rc<IBackendShader> createDomainShader(const void* bytecode, size_t size);
    Rc<IBackendShader> createComputeShader(const void* bytecode, size_t size);
#endif

    Rc<IBackendContext> createImmediateContext();
    // Records on a worker thread when the backend supports it.
    Rc<IBackendContext> createThreadedImmediateContext();
    // Records command lists without a D3D12 command list of its own.
    Rc<IBackendContext> createDeferredContext();

    bool checkFeatureSupport(BackendFeature feature) {
      return m_backend ? m_backend->checkFeatureSupport(feature) : false;
    }

    BackendLimits getLimits() {
      return m_backend ? m_backend->getLimits() : BackendLimits{};
    }

  private:
    Rc<IBackendDevice>  m_backend;
    BackendType         m_backendType;
  };

}
