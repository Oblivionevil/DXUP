#pragma once

#ifdef _WIN32

#include "../util/util_thread.h"
#include "../util/com_ptr.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <type_traits>
#include <vector>

#include <d3d12.h>

namespace dxup::d3d12 {

  // The command list a context records into. Each call normally goes
  // straight to D3D12. With d3d12.recordThread the calls are encoded
  // instead, and a thread of their own ("dxup-record") makes them on the
  // list in the same order, while the CS worker goes on tracking state.
  // The list is closed, executed and reset only after drain(): queue order,
  // fences and Present stay as they are.
  //
  // What a deferred call reads when it is made: its arguments are copied,
  // but the contents of CPU descriptors (render target, depth stencil and
  // clear handles) are read only when the record thread makes the call.
  // Code that rewrites such a descriptor drains first.
  class CommandListRecorder {
  public:
    CommandListRecorder() = default;
    ~CommandListRecorder();

    CommandListRecorder(const CommandListRecorder&) = delete;
    CommandListRecorder& operator=(const CommandListRecorder&) = delete;

    // Replaces the list; drained.
    void setList(ComPtr<ID3D12GraphicsCommandList> list);

    // Starts and stops the record thread; without it, calls are direct.
    void startThread();
    void stopThread();
    bool deferred() const { return m_deferred; }

    // m_cmdList->Call(...) as on the D3D12 list.
    CommandListRecorder* operator->() { return this; }
    explicit operator bool() const { return m_list.Get() != nullptr; }

    // Waits until the record thread made every call encoded so far.
    void drain() {
      if (m_deferred && m_consumed.load(std::memory_order_acquire) != m_produced)
        waitForRecordThread();
    }

    // The D3D12 list with every call so far made on it, for code that
    // records into it itself (upscalers, the GPU profiler).
    ID3D12GraphicsCommandList* raw() {
      drain();
      return m_list.Get();
    }

    bool hasList2() const { return m_list2.Get() != nullptr; }

    // Closed and reset by the thread that records the list: the driver's
    // data of the list is in that core's cache.
    HRESULT Close() {
      if (!m_deferred)
        return m_list->Close();
      HRESULT* result = &m_closeResult;
      m_closeResult = E_FAIL;
      put([result](ID3D12GraphicsCommandList* l) { *result = l->Close(); });
      drain();
      return m_closeResult;
    }

    // Deferred: a failure is logged by the record thread.
    HRESULT Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline) {
      if (!m_deferred)
        return m_list->Reset(allocator, pipeline);
      put([allocator, pipeline](ID3D12GraphicsCommandList* l) { logResetFailure(l->Reset(allocator, pipeline)); });
      return S_OK;
    }

    // The allocator of a list the GPU finished, reset before the list.
    void ResetAllocator(ID3D12CommandAllocator* allocator) {
      if (!m_deferred) {
        allocator->Reset();
        return;
      }
      put([allocator](ID3D12GraphicsCommandList*) { logResetFailure(allocator->Reset()); });
    }

    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
      if (!m_deferred)
        return m_list->ResourceBarrier(count, barriers);
      encode(bytes(barriers, count), [&](unsigned char* extra) {
        const D3D12_RESOURCE_BARRIER* b = copy(extra, barriers, count);
        return [count, b](ID3D12GraphicsCommandList* l) { l->ResourceBarrier(count, b); };
      });
    }

    void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) {
      if (!m_deferred)
        return m_list->SetDescriptorHeaps(count, heaps);
      encode(bytes(heaps, count), [&](unsigned char* extra) {
        ID3D12DescriptorHeap* const* h = copy(extra, heaps, count);
        return [count, h](ID3D12GraphicsCommandList* l) { l->SetDescriptorHeaps(count, h); };
      });
    }

    void SetPipelineState(ID3D12PipelineState* pipeline) {
      if (!m_deferred)
        return m_list->SetPipelineState(pipeline);
      put([pipeline](ID3D12GraphicsCommandList* l) { l->SetPipelineState(pipeline); });
    }

    void SetGraphicsRootSignature(ID3D12RootSignature* signature) {
      if (!m_deferred)
        return m_list->SetGraphicsRootSignature(signature);
      put([signature](ID3D12GraphicsCommandList* l) { l->SetGraphicsRootSignature(signature); });
    }

    void SetComputeRootSignature(ID3D12RootSignature* signature) {
      if (!m_deferred)
        return m_list->SetComputeRootSignature(signature);
      put([signature](ID3D12GraphicsCommandList* l) { l->SetComputeRootSignature(signature); });
    }

    void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
      if (!m_deferred)
        return m_list->SetGraphicsRootDescriptorTable(index, handle);
      put([index, handle](ID3D12GraphicsCommandList* l) { l->SetGraphicsRootDescriptorTable(index, handle); });
    }

    void SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
      if (!m_deferred)
        return m_list->SetComputeRootDescriptorTable(index, handle);
      put([index, handle](ID3D12GraphicsCommandList* l) { l->SetComputeRootDescriptorTable(index, handle); });
    }

    void SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT offset) {
      if (!m_deferred)
        return m_list->SetGraphicsRoot32BitConstant(index, value, offset);
      put([index, value, offset](ID3D12GraphicsCommandList* l) { l->SetGraphicsRoot32BitConstant(index, value, offset); });
    }

    void SetComputeRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) {
      if (!m_deferred)
        return m_list->SetComputeRoot32BitConstants(index, count, data, offset);
      encode(bytes(static_cast<const uint32_t*>(data), count), [&](unsigned char* extra) {
        const uint32_t* d = copy(extra, static_cast<const uint32_t*>(data), count);
        return [index, count, d, offset](ID3D12GraphicsCommandList* l) {
          l->SetComputeRoot32BitConstants(index, count, d, offset);
        };
      });
    }

    void SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
      if (!m_deferred)
        return m_list->SetGraphicsRootConstantBufferView(index, address);
      put([index, address](ID3D12GraphicsCommandList* l) { l->SetGraphicsRootConstantBufferView(index, address); });
    }

    void SetComputeRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
      if (!m_deferred)
        return m_list->SetComputeRootConstantBufferView(index, address);
      put([index, address](ID3D12GraphicsCommandList* l) { l->SetComputeRootConstantBufferView(index, address); });
    }

    void SetComputeRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
      if (!m_deferred)
        return m_list->SetComputeRootShaderResourceView(index, address);
      put([index, address](ID3D12GraphicsCommandList* l) { l->SetComputeRootShaderResourceView(index, address); });
    }

    void SetComputeRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
      if (!m_deferred)
        return m_list->SetComputeRootUnorderedAccessView(index, address);
      put([index, address](ID3D12GraphicsCommandList* l) { l->SetComputeRootUnorderedAccessView(index, address); });
    }

    void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
      if (!m_deferred)
        return m_list->IASetPrimitiveTopology(topology);
      put([topology](ID3D12GraphicsCommandList* l) { l->IASetPrimitiveTopology(topology); });
    }

    void IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
      if (!m_deferred)
        return m_list->IASetVertexBuffers(start, count, views);
      encode(bytes(views, count), [&](unsigned char* extra) {
        const D3D12_VERTEX_BUFFER_VIEW* v = copy(extra, views, count);
        return [start, count, v](ID3D12GraphicsCommandList* l) { l->IASetVertexBuffers(start, count, v); };
      });
    }

    void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
      if (!m_deferred)
        return m_list->IASetIndexBuffer(view);
      encode(bytes(view, 1), [&](unsigned char* extra) {
        const D3D12_INDEX_BUFFER_VIEW* v = copy(extra, view, 1);
        return [v](ID3D12GraphicsCommandList* l) { l->IASetIndexBuffer(v); };
      });
    }

    void SOSetTargets(UINT start, UINT count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views) {
      if (!m_deferred)
        return m_list->SOSetTargets(start, count, views);
      encode(bytes(views, count), [&](unsigned char* extra) {
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v = copy(extra, views, count);
        return [start, count, v](ID3D12GraphicsCommandList* l) { l->SOSetTargets(start, count, v); };
      });
    }

    void RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
      if (!m_deferred)
        return m_list->RSSetViewports(count, viewports);
      encode(bytes(viewports, count), [&](unsigned char* extra) {
        const D3D12_VIEWPORT* v = copy(extra, viewports, count);
        return [count, v](ID3D12GraphicsCommandList* l) { l->RSSetViewports(count, v); };
      });
    }

    void RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
      if (!m_deferred)
        return m_list->RSSetScissorRects(count, rects);
      encode(bytes(rects, count), [&](unsigned char* extra) {
        const D3D12_RECT* r = copy(extra, rects, count);
        return [count, r](ID3D12GraphicsCommandList* l) { l->RSSetScissorRects(count, r); };
      });
    }

    void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleRange,
                            const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
      if (!m_deferred)
        return m_list->OMSetRenderTargets(count, rtvs, singleRange, dsv);
      const UINT handles = singleRange ? (count ? 1u : 0u) : count;
      encode(bytes(rtvs, handles) + bytes(dsv, 1), [&](unsigned char* extra) {
        const D3D12_CPU_DESCRIPTOR_HANDLE* r = copy(extra, rtvs, handles);
        const D3D12_CPU_DESCRIPTOR_HANDLE* d = copy(extra, dsv, 1);
        return [count, r, singleRange, d](ID3D12GraphicsCommandList* l) { l->OMSetRenderTargets(count, r, singleRange, d); };
      });
    }

    void OMSetStencilRef(UINT ref) {
      if (!m_deferred)
        return m_list->OMSetStencilRef(ref);
      put([ref](ID3D12GraphicsCommandList* l) { l->OMSetStencilRef(ref); });
    }

    void OMSetBlendFactor(const FLOAT factor[4]) {
      if (!m_deferred)
        return m_list->OMSetBlendFactor(factor);
      encode(bytes(factor, 4), [&](unsigned char* extra) {
        const FLOAT* f = copy(extra, factor, 4);
        return [f](ID3D12GraphicsCommandList* l) { l->OMSetBlendFactor(f); };
      });
    }

    void SetPredication(ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP op) {
      if (!m_deferred)
        return m_list->SetPredication(buffer, offset, op);
      put([buffer, offset, op](ID3D12GraphicsCommandList* l) { l->SetPredication(buffer, offset, op); });
    }

    void DrawInstanced(UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance) {
      if (!m_deferred)
        return m_list->DrawInstanced(vertices, instances, firstVertex, firstInstance);
      put([vertices, instances, firstVertex, firstInstance](ID3D12GraphicsCommandList* l) {
        l->DrawInstanced(vertices, instances, firstVertex, firstInstance);
      });
    }

    void DrawIndexedInstanced(UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance) {
      if (!m_deferred)
        return m_list->DrawIndexedInstanced(indices, instances, firstIndex, baseVertex, firstInstance);
      put([indices, instances, firstIndex, baseVertex, firstInstance](ID3D12GraphicsCommandList* l) {
        l->DrawIndexedInstanced(indices, instances, firstIndex, baseVertex, firstInstance);
      });
    }

    void Dispatch(UINT x, UINT y, UINT z) {
      if (!m_deferred)
        return m_list->Dispatch(x, y, z);
      put([x, y, z](ID3D12GraphicsCommandList* l) { l->Dispatch(x, y, z); });
    }

    void ExecuteIndirect(ID3D12CommandSignature* signature, UINT maxCount, ID3D12Resource* arguments,
                         UINT64 argumentOffset, ID3D12Resource* countBuffer, UINT64 countOffset) {
      if (!m_deferred)
        return m_list->ExecuteIndirect(signature, maxCount, arguments, argumentOffset, countBuffer, countOffset);
      put([signature, maxCount, arguments, argumentOffset, countBuffer, countOffset](ID3D12GraphicsCommandList* l) {
        l->ExecuteIndirect(signature, maxCount, arguments, argumentOffset, countBuffer, countOffset);
      });
    }

    void CopyBufferRegion(ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 size) {
      if (!m_deferred)
        return m_list->CopyBufferRegion(dst, dstOffset, src, srcOffset, size);
      put([dst, dstOffset, src, srcOffset, size](ID3D12GraphicsCommandList* l) {
        l->CopyBufferRegion(dst, dstOffset, src, srcOffset, size);
      });
    }

    void CopyResource(ID3D12Resource* dst, ID3D12Resource* src) {
      if (!m_deferred)
        return m_list->CopyResource(dst, src);
      put([dst, src](ID3D12GraphicsCommandList* l) { l->CopyResource(dst, src); });
    }

    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                           const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
      if (!m_deferred)
        return m_list->CopyTextureRegion(dst, x, y, z, src, box);
      encode(bytes(dst, 1) + bytes(src, 1) + bytes(box, 1), [&](unsigned char* extra) {
        const D3D12_TEXTURE_COPY_LOCATION* d = copy(extra, dst, 1);
        const D3D12_TEXTURE_COPY_LOCATION* s = copy(extra, src, 1);
        const D3D12_BOX* b = copy(extra, box, 1);
        return [d, x, y, z, s, b](ID3D12GraphicsCommandList* l) { l->CopyTextureRegion(d, x, y, z, s, b); };
      });
    }

    void CopyTiles(ID3D12Resource* resource, const D3D12_TILED_RESOURCE_COORDINATE* start,
                   const D3D12_TILE_REGION_SIZE* size, ID3D12Resource* buffer, UINT64 bufferOffset,
                   D3D12_TILE_COPY_FLAGS flags) {
      if (!m_deferred)
        return m_list->CopyTiles(resource, start, size, buffer, bufferOffset, flags);
      encode(bytes(start, 1) + bytes(size, 1), [&](unsigned char* extra) {
        const D3D12_TILED_RESOURCE_COORDINATE* c = copy(extra, start, 1);
        const D3D12_TILE_REGION_SIZE* s = copy(extra, size, 1);
        return [resource, c, s, buffer, bufferOffset, flags](ID3D12GraphicsCommandList* l) {
          l->CopyTiles(resource, c, s, buffer, bufferOffset, flags);
        };
      });
    }

    void ResolveSubresource(ID3D12Resource* dst, UINT dstSubresource, ID3D12Resource* src, UINT srcSubresource,
                            DXGI_FORMAT format) {
      if (!m_deferred)
        return m_list->ResolveSubresource(dst, dstSubresource, src, srcSubresource, format);
      put([dst, dstSubresource, src, srcSubresource, format](ID3D12GraphicsCommandList* l) {
        l->ResolveSubresource(dst, dstSubresource, src, srcSubresource, format);
      });
    }

    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rectCount,
                               const D3D12_RECT* rects) {
      if (!m_deferred)
        return m_list->ClearRenderTargetView(rtv, color, rectCount, rects);
      encode(bytes(color, 4) + bytes(rects, rectCount), [&](unsigned char* extra) {
        const FLOAT* c = copy(extra, color, 4);
        const D3D12_RECT* r = copy(extra, rects, rectCount);
        return [rtv, c, rectCount, r](ID3D12GraphicsCommandList* l) { l->ClearRenderTargetView(rtv, c, rectCount, r); };
      });
    }

    void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                               UINT rectCount, const D3D12_RECT* rects) {
      if (!m_deferred)
        return m_list->ClearDepthStencilView(dsv, flags, depth, stencil, rectCount, rects);
      encode(bytes(rects, rectCount), [&](unsigned char* extra) {
        const D3D12_RECT* r = copy(extra, rects, rectCount);
        return [dsv, flags, depth, stencil, rectCount, r](ID3D12GraphicsCommandList* l) {
          l->ClearDepthStencilView(dsv, flags, depth, stencil, rectCount, r);
        };
      });
    }

    void ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                      ID3D12Resource* resource, const UINT values[4], UINT rectCount,
                                      const D3D12_RECT* rects) {
      if (!m_deferred)
        return m_list->ClearUnorderedAccessViewUint(gpu, cpu, resource, values, rectCount, rects);
      encode(bytes(values, 4) + bytes(rects, rectCount), [&](unsigned char* extra) {
        const UINT* v = copy(extra, values, 4);
        const D3D12_RECT* r = copy(extra, rects, rectCount);
        return [gpu, cpu, resource, v, rectCount, r](ID3D12GraphicsCommandList* l) {
          l->ClearUnorderedAccessViewUint(gpu, cpu, resource, v, rectCount, r);
        };
      });
    }

    void ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                       ID3D12Resource* resource, const FLOAT values[4], UINT rectCount,
                                       const D3D12_RECT* rects) {
      if (!m_deferred)
        return m_list->ClearUnorderedAccessViewFloat(gpu, cpu, resource, values, rectCount, rects);
      encode(bytes(values, 4) + bytes(rects, rectCount), [&](unsigned char* extra) {
        const FLOAT* v = copy(extra, values, 4);
        const D3D12_RECT* r = copy(extra, rects, rectCount);
        return [gpu, cpu, resource, v, rectCount, r](ID3D12GraphicsCommandList* l) {
          l->ClearUnorderedAccessViewFloat(gpu, cpu, resource, v, rectCount, r);
        };
      });
    }

    void BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
      if (!m_deferred)
        return m_list->BeginQuery(heap, type, index);
      put([heap, type, index](ID3D12GraphicsCommandList* l) { l->BeginQuery(heap, type, index); });
    }

    void EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
      if (!m_deferred)
        return m_list->EndQuery(heap, type, index);
      put([heap, type, index](ID3D12GraphicsCommandList* l) { l->EndQuery(heap, type, index); });
    }

    void ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count,
                          ID3D12Resource* destination, UINT64 offset) {
      if (!m_deferred)
        return m_list->ResolveQueryData(heap, type, start, count, destination, offset);
      put([heap, type, start, count, destination, offset](ID3D12GraphicsCommandList* l) {
        l->ResolveQueryData(heap, type, start, count, destination, offset);
      });
    }

    // ID3D12GraphicsCommandList2; only with hasList2().
    void WriteBufferImmediate(UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* params,
                              const D3D12_WRITEBUFFERIMMEDIATE_MODE* modes) {
      if (!m_deferred)
        return m_list2->WriteBufferImmediate(count, params, modes);
      ID3D12GraphicsCommandList2* list2 = m_list2.Get();
      encode(bytes(params, count) + bytes(modes, count), [&](unsigned char* extra) {
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p = copy(extra, params, count);
        const D3D12_WRITEBUFFERIMMEDIATE_MODE* m = copy(extra, modes, count);
        return [list2, count, p, m](ID3D12GraphicsCommandList*) { list2->WriteBufferImmediate(count, p, m); };
      });
    }

  private:
    // An encoded call; its arguments follow. replay is null for the end of
    // a block: the next call is at the start of the next block.
    struct Command {
      void (*replay)(ID3D12GraphicsCommandList* list, const Command* self);
      uint32_t size;
    };

    template <typename Fn>
    struct TypedCommand final : Command {
      explicit TypedCommand(Fn&& f) : fn(std::move(f)) {
        replay = [](ID3D12GraphicsCommandList* list, const Command* self) {
          static_cast<const TypedCommand*>(self)->fn(list);
        };
      }
      Fn fn;
    };

    struct Block {
      Block*  next;
      size_t  capacity;
      size_t  pad[2];
      unsigned char* data() { return reinterpret_cast<unsigned char*>(this + 1); }
    };

    static constexpr size_t kAlign = 8;
    static constexpr size_t kBlockSize = 64 * 1024;
    // Blocks encoded and not yet made before the worker waits.
    static constexpr uint32_t kMaxBlocksAhead = 64;
    static constexpr size_t kPublishBytes = 1024;

    static constexpr size_t alignUp(size_t size) { return (size + kAlign - 1) & ~(kAlign - 1); }

    template <typename T>
    static size_t bytes(const T* data, size_t count) {
      return data && count ? alignUp(sizeof(T) * count) : 0;
    }

    template <typename T>
    static const T* copy(unsigned char*& extra, const T* data, size_t count) {
      if (!data || !count)
        return nullptr;
      T* out = reinterpret_cast<T*>(extra);
      std::memcpy(static_cast<void*>(out), data, sizeof(T) * count);
      extra += alignUp(sizeof(T) * count);
      return out;
    }

    // make(extra) copies the arguments to extra and returns the call. Out
    // of line: the calls made directly stay small.
    template <typename Make>
    __declspec(noinline) void encode(size_t extraBytes, Make&& make) {
      using Fn = decltype(make(static_cast<unsigned char*>(nullptr)));
      using Cmd = TypedCommand<Fn>;
      static_assert(std::is_trivially_destructible_v<Fn>, "encoded calls are never destroyed");
      static_assert(alignof(Cmd) <= kAlign, "blocks align calls to 8 bytes");
      const size_t head = alignUp(sizeof(Cmd));
      const size_t size = head + extraBytes;
      unsigned char* mem = reserve(size);
      Cmd* cmd = new (mem) Cmd(make(mem + head));
      cmd->size = static_cast<uint32_t>(size);
      m_produced++;
      // Published in batches: the record thread then reads lines the worker
      // is done with instead of taking each line back and forth.
      m_unpublishedBytes += size;
      if (m_unpublishedBytes >= kPublishBytes)
        publish();
    }

    template <typename Fn>
    void put(Fn&& fn) {
      encode(0, [&](unsigned char*) { return std::move(fn); });
    }

    unsigned char* reserve(size_t size) {
      // Room is kept for the end of the block.
      if (m_writePos + size + sizeof(Command) > m_writeBlock->capacity)
        nextBlock(size);
      unsigned char* mem = m_writeBlock->data() + m_writePos;
      m_writePos += size;
      return mem;
    }

    void publish() {
      m_unpublishedBytes = 0;
      m_published.store(m_produced, std::memory_order_release);
      if (m_sleeping.load(std::memory_order_relaxed))
        wake();
    }

    static void logResetFailure(HRESULT hr);
    void nextBlock(size_t size);
    void wake();
    void waitForRecordThread();
    void threadMain();
    bool replayUntil(uint32_t target);
    Block* allocateBlock(size_t size);
    void releaseBlock(Block* block);

    ComPtr<ID3D12GraphicsCommandList>  m_list;
    ComPtr<ID3D12GraphicsCommandList2> m_list2;
    bool                               m_deferred = false;
    HRESULT                            m_closeResult = S_OK;

    // Written by the worker.
    Block*    m_writeBlock = nullptr;
    size_t    m_writePos = 0;
    uint32_t  m_produced = 0;
    uint32_t  m_blocksAhead = 0;
    size_t    m_unpublishedBytes = 0;

    // Written by the record thread.
    Block*    m_readBlock = nullptr;
    size_t    m_readPos = 0;
    uint32_t  m_consumedLocal = 0;

    alignas(64) std::atomic<uint32_t> m_published{0};
    std::atomic<bool>                 m_sleeping{false};
    alignas(64) std::atomic<uint32_t> m_consumed{0};
    std::atomic<uint32_t>             m_blocksReleased{0};
    std::atomic<bool>                 m_drainWaiting{false};
    std::atomic<bool>                 m_stopping{false};

    std::mutex           m_freeMutex;
    std::vector<Block*>  m_freeBlocks;

    HANDLE       m_wakeEvent = nullptr;
    HANDLE       m_drainedEvent = nullptr;
    dxup::Thread m_thread;
  };

}

#endif
