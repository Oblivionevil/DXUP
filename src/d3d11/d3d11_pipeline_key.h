#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "../directx/directx_backend.h"

namespace dxup {

  // Everything a graphics pipeline is built from, as plain values: the
  // shaders' bytecode identity, the content hashes of the input layout and
  // state objects, and what the bound views and the input assembler add.
  // Equal keys describe the same pipeline, so a context finds a pipeline it
  // bound before without building the full description again.
  struct GraphicsPipelineKey {
    uint64_t shaderHashes[5];  // VS, PS, GS, HS, DS
    uint64_t shaderSizes[5];
    uint64_t inputLayoutHash;
    uint64_t blendHash;
    uint64_t rasterizerHash;
    uint64_t depthStencilHash;
    uint32_t inputElementCount;
    uint32_t sampleMask;
    uint32_t sampleCount;
    uint32_t rtvFormats[8];
    uint32_t dsvFormat;
    uint32_t sampleQuality;
    uint8_t  topology;
    uint8_t  stripCutIndexType;
    uint8_t  renderTargetCount;
    uint8_t  hasDepthStencilView;
    uint8_t  sampleShading;
    uint8_t  reserved[7];

    // Keys are compared bytewise, so no padding may hold stray bytes.
    void clear() { std::memset(this, 0, sizeof(*this)); }

    uint64_t hash() const {
#if defined(_M_IX86)
      // 32-bit lanes: 64-bit multiplies are calls on x86. Four words a step
      // with the lanes in registers, read from the key in place (a lane
      // array indexed per word went through memory, a copy of the key
      // first waited for the stores that built it).
      constexpr size_t kWords32 = sizeof(GraphicsPipelineKey) / sizeof(uint32_t);
      static_assert(kWords32 % 4 == 0, "four lanes");
      const auto* bytes = reinterpret_cast<const uint8_t*>(this);
      uint32_t lane0 = 0x9e3779b9u, lane1 = 0x85ebca6bu, lane2 = 0xc2b2ae35u, lane3 = 0x27d4eb2fu;
      for (size_t i = 0; i < kWords32; i += 4) {
        uint32_t w[4];
        std::memcpy(&w[0], bytes + i * 4, 4);
        std::memcpy(&w[1], bytes + i * 4 + 4, 4);
        std::memcpy(&w[2], bytes + i * 4 + 8, 4);
        std::memcpy(&w[3], bytes + i * 4 + 12, 4);
        lane0 = (lane0 ^ w[0]) * 0xcc9e2d51u;
        lane1 = (lane1 ^ w[1]) * 0xcc9e2d51u;
        lane2 = (lane2 ^ w[2]) * 0xcc9e2d51u;
        lane3 = (lane3 ^ w[3]) * 0xcc9e2d51u;
      }
      uint32_t low = lane0 ^ ((lane1 >> 11) | (lane1 << 21));
      uint32_t high = lane2 ^ ((lane3 >> 17) | (lane3 << 15));
      low ^= low >> 16; low *= 0x85ebca6bu; low ^= low >> 13;
      high ^= high >> 16; high *= 0xc2b2ae35u; high ^= high >> 13;
      return ((uint64_t(high ^ low) << 32) | low) | 1u;
#else
      constexpr size_t kWords = sizeof(GraphicsPipelineKey) / sizeof(uint64_t);
      // Four independent lanes, so the multiplies overlap; in registers,
      // the words read from the key in place.
      const auto* bytes = reinterpret_cast<const uint8_t*>(this);
      auto word = [bytes](size_t i) { uint64_t w; std::memcpy(&w, bytes + i * 8, 8); return w; };
      constexpr uint64_t kMul = 0xff51afd7ed558ccdull;
      uint64_t lanes[4] = { 0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full,
                            0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull };
      size_t i = 0;
      for (; i + 4 <= kWords; i += 4) {
        lanes[0] = (lanes[0] ^ word(i)) * kMul;
        lanes[1] = (lanes[1] ^ word(i + 1)) * kMul;
        lanes[2] = (lanes[2] ^ word(i + 2)) * kMul;
        lanes[3] = (lanes[3] ^ word(i + 3)) * kMul;
      }
      for (; i < kWords; i++)
        lanes[i & 3] = (lanes[i & 3] ^ word(i)) * kMul;
      uint64_t h = lanes[0] ^ (lanes[1] >> 16 | lanes[1] << 48)
                 ^ (lanes[2] >> 32 | lanes[2] << 32) ^ (lanes[3] >> 48 | lanes[3] << 16);
      h ^= h >> 33;
      h *= 0xc4ceb9fe1a85ec53ull;
      h ^= h >> 29;
      // Zero marks a free table entry.
      return h | 1u;
#endif
    }

    bool operator==(const GraphicsPipelineKey& other) const {
      return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
  };

  static_assert(sizeof(GraphicsPipelineKey) % sizeof(uint64_t) == 0,
    "GraphicsPipelineKey is hashed as 64-bit words");
  static_assert(sizeof(GraphicsPipelineKey) == 5 * 16 + 4 * 8 + 3 * 4 + 8 * 4 + 4 + 4 + 12,
    "GraphicsPipelineKey must not have padding");

  // The pipelines one context bound, by key. The device keeps every
  // pipeline it built, so the lookup only points at them.
  class GraphicsPipelineLookup {
  public:
    // Probes the hashes, apart from the keys (a draw looks a pipeline up
    // each time its state changed: slots probed past cost no key's lines).
    IBackendPipeline* find(const GraphicsPipelineKey& key, uint64_t hash) const {
      if (m_hashes.empty())
        return nullptr;
      const size_t mask = m_hashes.size() - 1;
      for (size_t i = hash & mask; ; i = (i + 1) & mask) {
        const uint64_t slot = m_hashes[i];
        if (!slot)
          return nullptr;
        if (slot == hash && m_entries[i].key == key)
          return m_entries[i].pipeline;
      }
    }

    void insert(const GraphicsPipelineKey& key, uint64_t hash, IBackendPipeline* pipeline) {
      // A context that has seen this many pipelines starts over rather than
      // growing without bound; the device still has them all.
      if (m_count >= kMaxEntries)
        clear();
      if ((m_count + 1) * 2 > m_hashes.size())
        grow();
      place(key, hash, pipeline);
    }

    void clear() {
      m_hashes.clear();
      m_entries.clear();
      m_count = 0;
    }

  private:
    static constexpr size_t kInitialCapacity = 512;
    static constexpr size_t kMaxEntries = 16384;

    struct Entry {
      GraphicsPipelineKey key;
      IBackendPipeline*   pipeline = nullptr;
    };

    std::vector<uint64_t> m_hashes;    // 0: free slot
    std::vector<Entry>    m_entries;   // the same slots
    size_t                m_count = 0;

    void place(const GraphicsPipelineKey& key, uint64_t hash, IBackendPipeline* pipeline) {
      const size_t mask = m_hashes.size() - 1;
      for (size_t i = hash & mask; ; i = (i + 1) & mask) {
        if (!m_hashes[i]) {
          m_hashes[i] = hash;
          m_entries[i].key = key;
          m_entries[i].pipeline = pipeline;
          m_count++;
          return;
        }
        if (m_hashes[i] == hash && m_entries[i].key == key) {
          m_entries[i].pipeline = pipeline;
          return;
        }
      }
    }

    void grow() {
      std::vector<uint64_t> oldHashes = std::move(m_hashes);
      std::vector<Entry> oldEntries = std::move(m_entries);
      const size_t capacity = oldHashes.empty() ? kInitialCapacity : oldHashes.size() * 2;
      m_hashes.assign(capacity, 0);
      m_entries.clear();
      m_entries.resize(capacity);
      m_count = 0;
      for (size_t i = 0; i < oldHashes.size(); i++) {
        if (oldHashes[i])
          place(oldEntries[i].key, oldHashes[i], oldEntries[i].pipeline);
      }
    }
  };

}
