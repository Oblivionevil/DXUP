#pragma once

// Textures placed in large heaps: a committed resource each costs a kernel
// allocation and residency work, about ten times what the texture's own
// creation takes. A range returns to its heap when the resource placed on it
// is destroyed, whoever held the last reference. The next heap is made
// ahead on a thread of its own once the free space runs low: creating one
// takes a millisecond in a quiet process and up to 15 in a loading game,
// on whichever thread wanted the resource.

#ifdef _WIN32

#include <d3d12.h>
#include "d3d12_memory_stats.h"
#include "../util/com_ptr.h"
#include "../util/util_thread.h"
#include "../util/util_types.h"

#include <algorithm>
#include <condition_variable>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace dxup::d3d12 {

  // How often idle memory goes back: 10 seconds (DXUP_TRIM_INTERVAL_MS for
  // tests).
  inline uint64_t trimIntervalMs() {
    static const uint64_t interval = [] {
      const char* text = std::getenv("DXUP_TRIM_INTERVAL_MS");
      return text && *text ? std::strtoull(text, nullptr, 10) : 10000ull;
    }();
    return interval;
  }

  class D3D12PlacedAllocator : public std::enable_shared_from_this<D3D12PlacedAllocator> {
  public:
    static constexpr uint64_t kHeapSize = 64ull << 20;

    // Larger resources than maxPlacedSize take a committed resource of
    // their own.
    D3D12PlacedAllocator(D3D12_HEAP_FLAGS heapFlags, uint64_t maxPlacedSize,
                         D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT)
    : m_heapFlags(heapFlags), m_maxPlacedSize(maxPlacedSize), m_heapType(heapType),
      m_lowWater((std::min)(kHeapSize / 2, (std::max)(maxPlacedSize, kHeapSize / 4))) { }

    ~D3D12PlacedAllocator() { shutdown(); }

    // Ends the thread that makes heaps ahead and lets the device go; the
    // device calls it when destroyed, the resources may outlive it.
    void shutdown() {
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopped = true;
        m_wake.notify_one();
      }
      if (m_grower.joinable())
        m_grower.join();
      std::lock_guard<std::mutex> lock(m_mutex);
      m_device = nullptr;
    }

    // Gives back heaps nothing is placed in while more than the low water
    // mark and a heap stay free: the textures of a level left would keep
    // their heaps for good. Every 10 seconds at most, at present.
    void trim() {
      // One heap given up goes a present: releasing one costs a few hundred
      // microseconds.
      if (m_releasingCount.load(std::memory_order_relaxed)) {
        ComPtr<ID3D12Heap> going;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_releasing.empty()) {
          going = std::move(m_releasing.back());
          m_releasing.pop_back();
        }
        m_releasingCount.store(m_releasing.size(), std::memory_order_relaxed);
      }
      const uint64_t now = GetTickCount64();
      uint64_t last = m_lastTrim.load(std::memory_order_relaxed);
      if (now - last < trimIntervalMs() || !m_lastTrim.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;
      std::lock_guard<std::mutex> lock(m_mutex);
      size_t released = 0;
      for (auto& entry : m_heaps) {
        if (m_freeBytes < m_lowWater + 2 * kHeapSize)
          break;
        if (entry.heap && entry.free.size() == 1 && entry.free.begin()->first == 0
         && entry.free.begin()->second == kHeapSize) {
          m_releasing.push_back(std::move(entry.heap));
          entry.free.clear();
          m_freeBytes -= kHeapSize;
          released++;
        }
      }
      m_releasingCount.store(m_releasing.size(), std::memory_order_relaxed);
      if (released)
        logMessage(LogLevel::Info, "D3D12PlacedAllocator: %zu empty heaps go back", released);
    }

    // Creates resource placed in one of the heaps; false when it does not
    // fit, and the caller creates a committed resource instead.
    bool createPlaced(ID3D12Device* device, D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES state,
                      const D3D12_CLEAR_VALUE* clearValue, ComPtr<ID3D12Resource>& resource) {
      desc.Alignment = 0;
      D3D12_RESOURCE_ALLOCATION_INFO info = allocationInfo(device, desc);
      if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes || info.SizeInBytes > m_maxPlacedSize)
        return false;

      Range range;
      if (!allocate(device, info.SizeInBytes, info.Alignment, range))
        return false;
      if (FAILED(device->CreatePlacedResource(range.heap, range.offset, &desc, state, clearValue,
          IID_PPV_ARGS(&resource)))) {
        release(range);
        return false;
      }
      // The range goes back when the resource is destroyed.
      auto* releaser = new RangeReleaser(shared_from_this(), range);
      resource->SetPrivateDataInterface(kReleaserGuid, releaser);
      releaser->Release();
      memstats::attach(resource.Get(), memstats::PlacedResource, m_heapType == D3D12_HEAP_TYPE_UPLOAD, range.size);
      return true;
    }

  private:
    // Small textures may use the small placement alignment; buffers always
    // take 64 KB. Streamed resources repeat their descriptions: the answer
    // is kept per description.
    D3D12_RESOURCE_ALLOCATION_INFO allocationInfo(ID3D12Device* device, D3D12_RESOURCE_DESC& desc) {
      DescKey key;
      std::memcpy(&key.desc, &desc, sizeof(desc));
      {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        auto it = m_infoCache.find(key);
        if (it != m_infoCache.end()) {
          desc.Alignment = it->second.placementAlignment;
          return it->second.info;
        }
      }
      D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
      // Planar formats (video, depth/stencil) never take the small alignment.
      D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = { desc.Format, 0 };
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
       && info.SizeInBytes <= D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
       && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo)))
       && formatInfo.PlaneCount <= 1) {
        D3D12_RESOURCE_DESC smallDesc = desc;
        smallDesc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
        const D3D12_RESOURCE_ALLOCATION_INFO smallInfo = device->GetResourceAllocationInfo(0, 1, &smallDesc);
        if (smallInfo.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT && smallInfo.SizeInBytes != UINT64_MAX) {
          desc = smallDesc;
          info = smallInfo;
        }
      }
      std::lock_guard<std::mutex> lock(m_infoMutex);
      if (m_infoCache.size() < 4096)
        m_infoCache[key] = CachedInfo{ info, desc.Alignment };
      return info;
    }

    struct DescKey {
      D3D12_RESOURCE_DESC desc;
      bool operator==(const DescKey& other) const { return std::memcmp(&desc, &other.desc, sizeof(desc)) == 0; }
    };
    struct DescKeyHash {
      size_t operator()(const DescKey& key) const {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&key.desc);
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(key.desc); i++)
          hash = fnv64Multiply(hash ^ bytes[i]);
        return size_t(hash);
      }
    };
    struct CachedInfo {
      D3D12_RESOURCE_ALLOCATION_INFO info;
      uint64_t                       placementAlignment;
    };
    std::mutex                                             m_infoMutex;
    std::unordered_map<DescKey, CachedInfo, DescKeyHash>   m_infoCache;

    struct Range {
      ID3D12Heap* heap = nullptr;
      uint32_t    heapIndex = 0;
      uint64_t    offset = 0;
      uint64_t    size = 0;
    };

    struct Heap {
      ComPtr<ID3D12Heap> heap;
      // Free ranges: offset -> size.
      std::map<uint64_t, uint64_t>       free;
    };

    class RangeReleaser final : public IUnknown {
    public:
      RangeReleaser(std::shared_ptr<D3D12PlacedAllocator> allocator, const Range& range)
      : m_allocator(std::move(allocator)), m_range(range) { }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object)
          return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
          *object = this;
          AddRef();
          return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
      }
      ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --m_refs;
        if (!refs) {
          m_allocator->release(m_range);
          delete this;
        }
        return refs;
      }

    private:
      std::atomic<ULONG>                    m_refs{ 1 };
      std::shared_ptr<D3D12PlacedAllocator> m_allocator;
      Range                                 m_range;
    };

    // {7D2D1C5B-3E44-4C8A-9B6E-5A1F0D2E8C41}
    static constexpr GUID kReleaserGuid =
      { 0x7d2d1c5b, 0x3e44, 0x4c8a, { 0x9b, 0x6e, 0x5a, 0x1f, 0x0d, 0x2e, 0x8c, 0x41 } };

    bool allocate(ID3D12Device* device, uint64_t size, uint64_t alignment, Range& range) {
      std::unique_lock<std::mutex> lock(m_mutex);
      while (true) {
        if (takeAny(size, alignment, range)) {
          growAhead(device);
          return true;
        }
        if (!m_growing)
          break;
        // A heap is on its way: waiting for it costs less than making
        // another one.
        m_grown.wait(lock, [this] { return !m_growing; });
      }
      // Without the lock, so that resources released meanwhile (on the CS
      // worker) do not wait for it.
      m_growing = true;
      lock.unlock();
      ComPtr<ID3D12Heap> heap = createHeap(device);
      lock.lock();
      m_growing = false;
      m_grown.notify_all();
      if (!heap)
        return false;
      // Ranges freed meanwhile are taken first; the new heap stays for later.
      addHeap(std::move(heap));
      const bool taken = takeAny(size, alignment, range);
      growAhead(device);
      return taken;
    }

    ComPtr<ID3D12Heap> createHeap(ID3D12Device* device) const {
      D3D12_HEAP_DESC heapDesc = {};
      heapDesc.SizeInBytes = kHeapSize;
      heapDesc.Properties.Type = m_heapType;
      heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      heapDesc.Flags = m_heapFlags;
      ComPtr<ID3D12Heap> heap;
      if (FAILED(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap))))
        return nullptr;
      memstats::track(heap.Get(), memstats::PlacedHeap);
      if (m_heapType == D3D12_HEAP_TYPE_UPLOAD) {
        // The first Map of a resource in a CPU-visible heap maps the whole
        // heap, 4-5 ms for 64 MB on NVIDIA; the mapping outlives the
        // resource. Here, not on the thread that creates the next buffer.
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        void* mapped = nullptr;
        const D3D12_RANGE noRead = { 0, 0 };
        if (SUCCEEDED(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&resource))))
          resource->Map(0, &noRead, &mapped);
      }
      return heap;
    }

    // With m_mutex held. A slot trim emptied is taken again: ranges name
    // their heap by index.
    void addHeap(ComPtr<ID3D12Heap> heap) {
      Heap* slot = nullptr;
      for (auto& entry : m_heaps) {
        if (!entry.heap) {
          slot = &entry;
          break;
        }
      }
      if (!slot) {
        m_heaps.emplace_back();
        slot = &m_heaps.back();
      }
      slot->heap = std::move(heap);
      slot->free.clear();
      slot->free[0] = kHeapSize;
      m_freeBytes += kHeapSize;
    }

    // With m_mutex held.
    bool takeAny(uint64_t size, uint64_t alignment, Range& range) {
      for (uint32_t i = 0; i < m_heaps.size(); i++) {
        if (takeFrom(i, size, alignment, range)) {
          m_freeBytes -= size;
          return true;
        }
      }
      return false;
    }

    // With m_mutex held: has the next heap made in the background when the
    // free space runs low.
    void growAhead(ID3D12Device* device) {
      if (m_growing || m_stopped || m_freeBytes >= m_lowWater)
        return;
      if (!m_grower.joinable()) {
        m_device = device;
        try {
          m_grower = Thread([this] { runGrower(); });
        } catch (const std::exception&) {
          m_stopped = true;
          return;
        }
      }
      m_growing = true;
      m_wake.notify_one();
    }

    void runGrower() {
      std::unique_lock<std::mutex> lock(m_mutex);
      while (true) {
        m_wake.wait(lock, [this] { return m_stopped || m_growing; });
        if (m_stopped)
          break;
        lock.unlock();
        ComPtr<ID3D12Heap> heap = createHeap(m_device.Get());
        lock.lock();
        if (heap)
          addHeap(std::move(heap));
        m_growing = false;
        m_grown.notify_all();
      }
      // A resource that waits for this heap makes its own.
      m_growing = false;
      m_grown.notify_all();
    }

    bool takeFrom(uint32_t heapIndex, uint64_t size, uint64_t alignment, Range& range) {
      auto& heap = m_heaps[heapIndex];
      for (auto it = heap.free.begin(); it != heap.free.end(); ++it) {
        const uint64_t start = it->first, length = it->second;
        const uint64_t aligned = (start + alignment - 1) & ~(alignment - 1);
        if (aligned + size > start + length)
          continue;
        heap.free.erase(it);
        if (aligned > start)
          heap.free[start] = aligned - start;
        if (aligned + size < start + length)
          heap.free[aligned + size] = start + length - aligned - size;
        range.heap = heap.heap.Get();
        range.heapIndex = heapIndex;
        range.offset = aligned;
        range.size = size;
        return true;
      }
      return false;
    }

    void release(const Range& range) {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto& free = m_heaps[range.heapIndex].free;
      uint64_t start = range.offset, length = range.size;
      // Merge with the neighbouring free ranges.
      auto next = free.lower_bound(start);
      if (next != free.end() && next->first == start + length) {
        length += next->second;
        next = free.erase(next);
      }
      if (next != free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start) {
          start = previous->first;
          length += previous->second;
          free.erase(previous);
        }
      }
      free[start] = length;
      m_freeBytes += range.size;
    }

    const D3D12_HEAP_FLAGS m_heapFlags;
    const uint64_t         m_maxPlacedSize;
    const D3D12_HEAP_TYPE  m_heapType;
    const uint64_t         m_lowWater;
    std::mutex             m_mutex;
    std::vector<Heap>      m_heaps;
    uint64_t               m_freeBytes = 0;
    // A heap is being made, in the background or by a resource.
    bool                   m_growing = false;
    std::atomic<uint64_t>  m_lastTrim{ 0 };
    // Heaps trim gave up, released one at each present.
    std::vector<ComPtr<ID3D12Heap>> m_releasing;
    std::atomic<size_t>    m_releasingCount{ 0 };
    bool                   m_stopped = false;
    std::condition_variable m_wake;
    std::condition_variable m_grown;
    ComPtr<ID3D12Device>   m_device;
    Thread                 m_grower;
  };

}

#endif
