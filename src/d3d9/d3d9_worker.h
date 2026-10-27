#pragma once

// The device's calls on the legacy core made by a thread of their own
// (dxup.conf d3d9.workerThread): the application's thread records them into
// chunks of commands, the worker ("dxup-d3d9") makes them in order. Draws
// are most of what the core costs a game's render thread; with the worker
// that thread only copies their arguments.
//
// Calls that return what the core holds or waits for (getters, locks that
// read, Reset) wait until the worker made everything recorded before
// (Device::Direct), then call the core themselves.

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "../util/util_thread.h"

namespace dxup::legacy { class Device; }

namespace dxup::d3d9 {

  class Worker {
  public:
    explicit Worker(legacy::Device* core);
    // Makes what is recorded, then ends the thread.
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // fn(core) on the worker, after the calls recorded before.
    template<typename Fn>
    __forceinline void emit(Fn&& fn) {
      using T = Typed<std::decay_t<Fn>>;
      static_assert(alignof(T) <= kAlign, "commands are aligned to 8 bytes");
      constexpr uint32_t size = uint32_t((sizeof(T) + kAlign - 1) & ~(kAlign - 1));
      uint8_t* at = m_pos;
      if (uint32_t(m_end - at) < size)
        at = reserve(size);
      new (at) T(std::forward<Fn>(fn), size);
      m_pos = at + size;
    }

    // fn(core, data) with a copy of `bytes` bytes that fill(data) writes:
    // what the application points at may change once the call returns.
    template<typename Fill, typename Fn>
    void emitFilled(size_t bytes, Fill&& fill, Fn&& fn) {
      using T = DataTyped<std::decay_t<Fn>>;
      static_assert(alignof(T) <= kAlign, "commands are aligned to 8 bytes");
      constexpr uint32_t header = uint32_t((sizeof(T) + 15) & ~size_t(15));
      if (bytes <= kMaxInlineData) {
        const uint32_t size = uint32_t((header + bytes + kAlign - 1) & ~(kAlign - 1));
        uint8_t* at = m_pos;
        if (uint32_t(m_end - at) < size)
          at = reserve(size);
        new (at) T(std::forward<Fn>(fn), size, header);
        fill(at + header);
        m_pos = at + size;
        return;
      }
      // Larger: on the heap, with the command.
      std::unique_ptr<uint8_t[]> data(new uint8_t[bytes]);
      fill(data.get());
      emit([fn = std::forward<Fn>(fn), data = std::move(data)](legacy::Device& core) mutable {
        fn(core, static_cast<const uint8_t*>(data.get()));
      });
    }

    template<typename Fn>
    void emitData(const void* data, size_t bytes, Fn&& fn) {
      emitFilled(bytes, [&](uint8_t* to) { std::memcpy(to, data, bytes); }, std::forward<Fn>(fn));
    }

    // An object the worker deletes once it made the calls recorded so far
    // (from any thread: applications release objects anywhere). Recorded
    // with the next submit.
    void retire(void (*destroy)(void*), void* object);
    // What is recorded goes to the worker.
    void submit();
    // Waits until the worker made all recorded calls, taking the messages
    // sent to this thread meanwhile (the worker's Present sends some to the
    // window, whose thread may be this one).
    void synchronize();
    // After recording a Present: waits until the worker made the Present
    // before it (the application stays at most a frame ahead).
    void endFrame();
    // Nothing recorded since the last submit.
    bool recordedNothing() const { return m_pos == m_begin; }

  private:
    static constexpr size_t kAlign = 8;
    static constexpr size_t kChunkSize = 64u << 10;
    static constexpr size_t kMaxInlineData = 16u << 10;

    // Makes the call and destroys the command (also when the call throws).
    struct Command {
      void (*invoke)(Command* command, legacy::Device& core);
      uint32_t size;
    };

    template<typename Fn>
    struct Typed : Command {
      Fn fn;
      template<typename F>
      Typed(F&& f, uint32_t s) : fn(std::forward<F>(f)) {
        invoke = &call;
        size = s;
      }
      static void call(Command* command, legacy::Device& core) {
        auto* self = static_cast<Typed*>(command);
        if constexpr (std::is_trivially_destructible_v<Fn>) {
          self->fn(core);
        } else {
          struct Destroy { Typed* t; ~Destroy() { t->fn.~Fn(); } } destroy{ self };
          self->fn(core);
        }
      }
    };

    template<typename Fn>
    struct DataTyped : Command {
      Fn fn;
      uint32_t header;
      template<typename F>
      DataTyped(F&& f, uint32_t s, uint32_t h) : fn(std::forward<F>(f)), header(h) {
        invoke = &call;
        size = s;
      }
      static void call(Command* command, legacy::Device& core) {
        auto* self = static_cast<DataTyped*>(command);
        const uint8_t* data = reinterpret_cast<const uint8_t*>(self) + self->header;
        if constexpr (std::is_trivially_destructible_v<Fn>) {
          self->fn(core, data);
        } else {
          struct Destroy { DataTyped* t; ~Destroy() { t->fn.~Fn(); } } destroy{ self };
          self->fn(core, data);
        }
      }
    };

    struct Chunk {
      uint8_t* used = nullptr;   // the end of its commands
      alignas(64) uint8_t data[kChunkSize];
    };

    // Room for a command of size bytes: the chunk goes to the worker when
    // full.
    __declspec(noinline) uint8_t* reserve(uint32_t size);
    void submitChunk();
    void recordRetired();
    Chunk* takeChunk();
    void execute(Chunk* chunk);
    void freeChunk(Chunk* chunk);
    void workerMain();
    // Waits until done() holds (see synchronize).
    template<typename Done>
    void waitFor(Done done);

    legacy::Device* m_core;
    // The chunk being recorded (application side).
    Chunk*   m_chunk = nullptr;
    uint8_t* m_begin = nullptr;
    uint8_t* m_pos = nullptr;
    uint8_t* m_end = nullptr;
    uint64_t m_submitted = 0;
    uint64_t m_framesRecorded = 0;

    std::mutex              m_mutex;
    std::condition_variable m_wake;
    std::deque<Chunk*>      m_queue;
    std::atomic<uint32_t>   m_queued{0};
    bool                    m_stopping = false;
    std::vector<Chunk*>     m_freeChunks;   // under m_mutex

    std::atomic<uint64_t> m_executed{0};
    std::atomic<uint64_t> m_framesDone{0};
    std::atomic<bool>     m_waiting{false};
    HANDLE                m_doneEvent = nullptr;

    std::mutex m_retireMutex;
    std::vector<std::pair<void (*)(void*), void*>> m_retired;
    std::atomic<bool> m_anyRetired{false};

    dxup::Thread m_thread;
  };

}
