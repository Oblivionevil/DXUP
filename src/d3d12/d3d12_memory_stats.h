#pragma once

// GPU memory DXUP holds, by what it holds it for: a small COM object rides
// on every tracked D3D12 resource or heap as private data and takes its
// bytes off again when the resource goes. With dxup_memory_stats.txt next
// to the DLL the totals and the slices of the buffer blocks are logged
// every 600 presents, at present 3000 also the resources of 8 MB and more.

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "../util/util_types.h"

namespace dxup::d3d12::memstats {

  enum Category : int {
    TargetTexture,      // render targets and depth buffers (committed)
    Texture,            // other textures (committed)
    Buffer,             // buffers (committed)
    PlacedHeap,         // heaps of D3D12PlacedAllocator
    PlacedResource,     // what is placed in them (part of PlacedHeap)
    BlockHeap,          // blocks the buffer suballocators cut slices from
    Internal,           // DXUP's own resources
    TilePool,           // heaps of tiled resources' tile pools
    SwapChain,          // the swap chain's buffers (DXGI makes them)
    kCategories
  };

  inline const char* name(int category) {
    static const char* const kNames[kCategories] = {
      "targets", "textures", "buffers", "placed heaps", "placed in them", "buffer blocks", "internal",
      "tile pools", "swap chain" };
    return kNames[category];
  }

  struct Totals {
    std::atomic<int64_t> bytes[kCategories][2] = {};     // [category][system memory]
    std::atomic<int64_t> objects[kCategories][2] = {};
    // Textures in video memory nothing has written yet (no transition to a
    // writing state): what committing them on first use would save.
    std::atomic<int64_t> unwrittenImageBytes{ 0 };
    std::atomic<int64_t> unwrittenImages{ 0 };
  };
  inline Totals& totals() {
    static Totals t;
    return t;
  }

  // Set once by the device (dxup_memory_stats.txt).
  inline std::atomic<bool>& enabledFlag() {
    static std::atomic<bool> enabled{ false };
    return enabled;
  }
  inline bool enabled() { return enabledFlag().load(std::memory_order_relaxed); }

  // {6f8c0e1d-2b9a-4c57-9d1e-5a7f3c2b8e41}
  inline constexpr GUID kTrackerGuid = { 0x6f8c0e1d, 0x2b9a, 0x4c57, { 0x9d, 0x1e, 0x5a, 0x7f, 0x3c, 0x2b, 0x8e, 0x41 } };

  class Tracker;
  // The large resources alive, with what they are.
  struct LiveList {
    std::mutex mutex;
    std::map<const Tracker*, std::string> entries;
  };
  inline LiveList& live() {
    static LiveList list;
    return list;
  }

  class Tracker final : public IUnknown {
  public:
    void label(std::string text) {
      std::lock_guard<std::mutex> lock(live().mutex);
      live().entries[this] = std::move(text);
    }

    Tracker(int category, bool system, int64_t bytes, bool image)
    : m_category(category), m_system(system), m_bytes(bytes), m_unwritten(image && !system && category != PlacedHeap) {
      totals().bytes[m_category][m_system] += m_bytes;
      totals().objects[m_category][m_system]++;
      if (m_unwritten) {
        totals().unwrittenImageBytes += m_bytes;
        totals().unwrittenImages++;
      }
    }
    bool unwritten() const { return m_unwritten.load(); }
    void written() {
      if (m_unwritten.exchange(false)) {
        totals().unwrittenImageBytes -= m_bytes;
        totals().unwrittenImages--;
      }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
      if (!object)
        return E_POINTER;
      if (riid == __uuidof(IUnknown)) {
        *object = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
      }
      *object = nullptr;
      return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
      const ULONG refs = --m_refs;
      if (!refs)
        delete this;
      return refs;
    }

  private:
    ~Tracker() {
      {
        std::lock_guard<std::mutex> lock(live().mutex);
        live().entries.erase(this);
      }
      totals().bytes[m_category][m_system] -= m_bytes;
      totals().objects[m_category][m_system]--;
      written();
    }
    std::atomic<ULONG> m_refs{ 1 };
    int m_category;
    bool m_system;
    int64_t m_bytes;
    std::atomic<bool> m_unwritten;
  };

  inline Tracker* attach(ID3D12Object* object, int category, bool system, uint64_t bytes) {
    if (!enabled() || !object || !bytes)
      return nullptr;
    bool image = false;
    ID3D12Resource* resource = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
      image = resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
      resource->Release();
    }
    auto* tracker = new Tracker(category, system, int64_t(bytes), image);
    object->SetPrivateDataInterface(kTrackerGuid, tracker);
    tracker->Release();
    return tracker;
  }

  // A transition into a writing state.
  inline void noteWritten(ID3D12Resource* resource) {
    if (!enabled() || !resource)
      return;
    IUnknown* unknown = nullptr;
    UINT size = sizeof(unknown);
    if (FAILED(resource->GetPrivateData(kTrackerGuid, &size, &unknown)) || !unknown)
      return;
    static_cast<Tracker*>(unknown)->written();
    unknown->Release();
  }

  // A heap of its own size.
  inline void track(ID3D12Heap* heap, int category) {
    if (!enabled() || !heap)
      return;
    const D3D12_HEAP_DESC desc = heap->GetDesc();
    const bool system = desc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD || desc.Properties.Type == D3D12_HEAP_TYPE_READBACK
      || (desc.Properties.Type == D3D12_HEAP_TYPE_CUSTOM && desc.Properties.MemoryPoolPreference == D3D12_MEMORY_POOL_L0);
    attach(heap, category, system, desc.SizeInBytes);
  }

  // A resource of the size the device gives it; placed resources count
  // only what they take of their heap.
  inline void track(ID3D12Device* device, ID3D12Resource* resource, int category) {
    if (!enabled() || !device || !resource)
      return;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
    if (info.SizeInBytes == UINT64_MAX)
      return;
    D3D12_HEAP_PROPERTIES heap = {};
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    bool system = false;
    if (SUCCEEDED(resource->GetHeapProperties(&heap, &flags)))
      system = heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_READBACK
        || (heap.Type == D3D12_HEAP_TYPE_CUSTOM && heap.MemoryPoolPreference == D3D12_MEMORY_POOL_L0);
    Tracker* tracker = attach(resource, category, system, info.SizeInBytes);
    // The large ones one by one.
    if (tracker && info.SizeInBytes >= (8ull << 20)) {
      char text[160];
      std::snprintf(text, sizeof(text), "%s %llu MB: %llux%u x%u mips %u format %d samples %u flags 0x%x",
        name(category), static_cast<unsigned long long>(info.SizeInBytes >> 20), static_cast<unsigned long long>(desc.Width),
        desc.Height, desc.DepthOrArraySize, desc.MipLevels, static_cast<int>(desc.Format), desc.SampleDesc.Count,
        static_cast<unsigned>(desc.Flags));
      tracker->label(text);
    }
  }

  // The large resources alive, alike ones counted together.
  inline std::string listLive() {
    std::map<std::string, int> counts;
    {
      std::lock_guard<std::mutex> lock(live().mutex);
      for (const auto& entry : live().entries)
        counts[entry.second + (entry.first->unwritten() ? " NEVER WRITTEN" : "")]++;
    }
    std::string text;
    for (const auto& entry : counts)
      text += "\n    " + std::to_string(entry.second) + "x " + entry.first;
    return text;
  }

  inline std::string report() {
    std::string line;
    char part[128];
    int64_t video = 0;
    for (int c = 0; c < kCategories; c++) {
      const int64_t bytes = totals().bytes[c][0].load(), system = totals().bytes[c][1].load();
      if (c != PlacedResource)
        video += bytes;
      std::snprintf(part, sizeof(part), "%s%s %lld MB (%lld)", line.empty() ? "" : ", ", name(c),
        static_cast<long long>(bytes >> 20), static_cast<long long>(totals().objects[c][0].load()));
      line += part;
      if (system) {
        std::snprintf(part, sizeof(part), " + system %lld MB", static_cast<long long>(system >> 20));
        line += part;
      }
    }
    std::snprintf(part, sizeof(part), "; video memory %lld MB; textures never written %lld MB (%lld)",
      static_cast<long long>(video >> 20), static_cast<long long>(totals().unwrittenImageBytes.load() >> 20),
      static_cast<long long>(totals().unwrittenImages.load()));
    return line + part;
  }

}
