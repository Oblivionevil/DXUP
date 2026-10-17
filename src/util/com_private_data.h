#pragma once

#ifdef _WIN32

#include <unknwn.h>
#include <winerror.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace dxup {

  // ============================================================
  // ComPrivateData
  //
  // Backing store for the GetPrivateData / SetPrivateData /
  // SetPrivateDataInterface methods of ID3D11DeviceChild,
  // ID3D11Device and IDXGIObject. Each GUID maps either to a
  // copy of caller-provided bytes or to an AddRef'd interface.
  //
  // Semantics follow native D3D11 / DXGI:
  //  - Setting data or an interface replaces any previous entry
  //    for the GUID (releasing a previously stored interface).
  //  - SetPrivateData(guid, size, nullptr) removes the entry and
  //    returns S_FALSE if there was nothing to remove.
  //  - GetPrivateData on an interface entry AddRefs the returned
  //    pointer; the caller owns that reference.
  //  - GetPrivateData with a too-small buffer returns
  //    DXGI_ERROR_MORE_DATA, reports the required size and
  //    leaves the output buffer untouched.
  //
  // Replaced interfaces are released after the lock is dropped,
  // since the final Release may destroy the object owning this
  // store (e.g. an object that stored a reference to itself).
  // ============================================================

  class ComPrivateData {
  public:
    ComPrivateData() = default;

    ComPrivateData(const ComPrivateData&) = delete;
    ComPrivateData& operator=(const ComPrivateData&) = delete;

    ~ComPrivateData() {
      for (auto& entry : m_entries) {
        if (entry.iface)
          entry.iface->Release();
      }
    }

    HRESULT setData(REFGUID guid, UINT size, const void* data) {
      IUnknown* released = nullptr;
      HRESULT hr = S_OK;

      {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!data) {
          hr = eraseEntry(guid, &released) ? S_OK : S_FALSE;
        } else {
          Entry& entry = acquireEntry(guid, &released);
          const auto* bytes = static_cast<const uint8_t*>(data);
          entry.data.assign(bytes, bytes + size);
        }
      }

      if (released)
        released->Release();
      return hr;
    }

    HRESULT setInterface(REFGUID guid, const IUnknown* iface) {
      IUnknown* released = nullptr;

      {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!iface) {
          eraseEntry(guid, &released);
        } else {
          IUnknown* ref = const_cast<IUnknown*>(iface);
          ref->AddRef();

          Entry& entry = acquireEntry(guid, &released);
          entry.iface = ref;
        }
      }

      if (released)
        released->Release();
      return S_OK;
    }

    HRESULT getData(REFGUID guid, UINT* pSize, void* pData) {
      if (!pSize)
        return E_INVALIDARG;

      std::lock_guard<std::mutex> lock(m_mutex);

      Entry* entry = findEntry(guid);
      if (!entry) {
        *pSize = 0;
        return DXGI_ERROR_NOT_FOUND;
      }

      UINT requiredSize = entry->iface
        ? UINT(sizeof(IUnknown*))
        : UINT(entry->data.size());

      if (!pData) {
        *pSize = requiredSize;
        return S_OK;
      }

      if (*pSize < requiredSize) {
        *pSize = requiredSize;
        return DXGI_ERROR_MORE_DATA;
      }

      if (entry->iface) {
        entry->iface->AddRef();
        std::memcpy(pData, &entry->iface, sizeof(IUnknown*));
      } else if (requiredSize) {
        std::memcpy(pData, entry->data.data(), requiredSize);
      }

      *pSize = requiredSize;
      return S_OK;
    }

  private:
    struct Entry {
      GUID                 guid = { };
      std::vector<uint8_t> data;
      IUnknown*            iface = nullptr;
    };

    std::mutex         m_mutex;
    std::vector<Entry> m_entries;

    Entry* findEntry(REFGUID guid) {
      for (auto& entry : m_entries) {
        if (IsEqualGUID(entry.guid, guid))
          return &entry;
      }
      return nullptr;
    }

    // Returns an empty entry for the GUID. A previously stored
    // interface is handed back through pReleased for the caller
    // to release once m_mutex is no longer held.
    Entry& acquireEntry(REFGUID guid, IUnknown** pReleased) {
      Entry* entry = findEntry(guid);
      if (entry) {
        *pReleased = entry->iface;
        entry->iface = nullptr;
        entry->data.clear();
        return *entry;
      }

      m_entries.emplace_back();
      m_entries.back().guid = guid;
      return m_entries.back();
    }

    bool eraseEntry(REFGUID guid, IUnknown** pReleased) {
      for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (IsEqualGUID(it->guid, guid)) {
          *pReleased = it->iface;
          m_entries.erase(it);
          return true;
        }
      }
      return false;
    }
  };

}

#endif
