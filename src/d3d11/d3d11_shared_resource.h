#pragma once

// Resources shared between devices through handles. D3D12 makes NT handles
// only: a legacy (KMT) handle that DXUP hands out is an NT handle the
// resource owns, which the devices of this process open like the handle
// itself. Handles that other runtimes created, legacy ones included, open
// through D3D12.
//
// Limits: sharing works within the process. A legacy handle of DXUP means
// nothing in another process (NT handles are per process), the keyed mutex
// is a lock of this process, and resources opened from handles of other
// processes or runtimes have no keyed mutex (D3D12 does not tell whether
// they were created with one). Each limit is logged once when it is hit.

#include <d3d11_4.h>

#include "../d3d12/d3d12_shared_sync.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dxup {

  class D3D11SharedResource {
  public:
    ~D3D11SharedResource() {
      for (HANDLE handle : m_handles)
        CloseHandle(handle);
      if (m_kmtHandle && m_ownsKmtHandle)
        CloseHandle(m_kmtHandle);
    }

    // What the devices that open the resource report, as created.
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    union {
      D3D11_BUFFER_DESC    buffer;
      D3D11_TEXTURE1D_DESC texture1D;
      D3D11_TEXTURE2D_DESC texture2D;
      D3D11_TEXTURE3D_DESC texture3D;
    } desc = {};
    std::shared_ptr<d3d12::D3D12SharedSync> sync;
    // Opened from a handle of another process or runtime: the description
    // is derived from the D3D12 resource.
    bool foreign = false;

    UINT miscFlags() const {
      switch (dimension) {
        case D3D11_RESOURCE_DIMENSION_BUFFER:    return desc.buffer.MiscFlags;
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return desc.texture1D.MiscFlags;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return desc.texture2D.MiscFlags;
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return desc.texture3D.MiscFlags;
        default:                                 return 0;
      }
    }
    bool hasKeyedMutex() const { return (miscFlags() & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0; }

    // Handles that name the resource, for the devices that open it.
    // Duplicates owned here: the application may close its own.
    void addHandle(HANDLE handle) {
      HANDLE duplicate = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
      std::lock_guard<std::mutex> lock(m_handleMutex);
      m_handles.push_back(duplicate);
    }
    bool names(HANDLE handle) {
      std::lock_guard<std::mutex> lock(m_handleMutex);
      if (m_kmtHandle == handle)
        return true;
      for (HANDLE own : m_handles) {
        if (own == handle || sameObject(own, handle))
          return true;
      }
      return m_kmtHandle && m_ownsKmtHandle && sameObject(m_kmtHandle, handle);
    }

    // The legacy handle GetSharedHandle returns, the same every time.
    template<typename Create>
    HANDLE kmtHandle(Create&& create) {
      std::lock_guard<std::mutex> lock(m_handleMutex);
      if (!m_kmtHandle) {
        m_kmtHandle = create();
        m_ownsKmtHandle = true;
      }
      return m_kmtHandle;
    }
    // Whether handle is the legacy handle of the resource, which for
    // resources DXUP created is an NT handle.
    bool isKmtHandle(HANDLE handle) {
      std::lock_guard<std::mutex> lock(m_handleMutex);
      return handle && handle == m_kmtHandle;
    }

    // A legacy handle of another runtime the resource was opened with.
    void setForeignKmtHandle(HANDLE handle) {
      std::lock_guard<std::mutex> lock(m_handleMutex);
      m_kmtHandle = handle;
      m_ownsKmtHandle = false;
    }

    // Keyed mutex: released with key 0 when created. Ownership passes on
    // when the releasing device's work is submitted; the shared sync then
    // orders the next owner's work after it. Devices of this process only:
    // another process's runtime never sees this lock.
    std::mutex              keyMutex;
    std::condition_variable keyChanged;
    bool                    keyHeld = false;
    bool                    keyReleasePending = false;
    bool                    keyAbandoned = false;
    uint64_t                key = 0;
    const void*             keyOwner = nullptr;

    static bool isNtHandle(HANDLE handle) {
      DWORD flags = 0;
      return handle && GetHandleInformation(handle, &flags);
    }

  private:
    static bool sameObject(HANDLE a, HANDLE b) {
      using CompareFn = BOOL (WINAPI*)(HANDLE, HANDLE);
      static CompareFn compare = [] {
        HMODULE module = GetModuleHandleW(L"kernelbase.dll");
        return module ? reinterpret_cast<CompareFn>(GetProcAddress(module, "CompareObjectHandles")) : nullptr;
      }();
      return compare && isNtHandle(b) && compare(a, b);
    }

    std::mutex          m_handleMutex;
    std::vector<HANDLE> m_handles;
    HANDLE              m_kmtHandle = nullptr;
    bool                m_ownsKmtHandle = false;
  };

  // The shared resources of this process that some device still holds.
  class D3D11SharedResourceRegistry {
  public:
    static void add(const std::shared_ptr<D3D11SharedResource>& resource) {
      auto& registry = instance();
      std::lock_guard<std::mutex> lock(registry.m_mutex);
      registry.m_resources.erase(std::remove_if(registry.m_resources.begin(), registry.m_resources.end(),
        [](const std::weak_ptr<D3D11SharedResource>& entry) { return entry.expired(); }), registry.m_resources.end());
      registry.m_resources.push_back(resource);
    }

    static std::shared_ptr<D3D11SharedResource> find(HANDLE handle) {
      auto& registry = instance();
      std::lock_guard<std::mutex> lock(registry.m_mutex);
      for (const auto& entry : registry.m_resources) {
        if (auto resource = entry.lock(); resource && resource->names(handle))
          return resource;
      }
      return nullptr;
    }

  private:
    static D3D11SharedResourceRegistry& instance() {
      static D3D11SharedResourceRegistry registry;
      return registry;
    }

    std::mutex                                     m_mutex;
    std::vector<std::weak_ptr<D3D11SharedResource>> m_resources;
  };

}
