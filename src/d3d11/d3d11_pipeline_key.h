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
    uint8_t  topology;
    uint8_t  stripCutIndexType;
    uint8_t  renderTargetCount;
    uint8_t  hasDepthStencilView;
    uint8_t  sampleShading;
    uint8_t  reserved[3];

    // Keys are compared bytewise, so no padding may hold stray bytes.
    void clear() { std::memset(this, 0, sizeof(*this)); }

    uint64_t hash() const {
#if defined(_M_IX86)
      // 32-bit lanes: 64-bit multiplies are calls on x86.
      constexpr size_t kWords32 = sizeof(GraphicsPipelineKey) / sizeof(uint32_t);
      uint32_t words32[kWords32];
      std::memcpy(words32, this, sizeof(words32));
      uint32_t lanes32[4] = { 0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu };
      for (size_t i = 0; i < kWords32; i++)
        lanes32[i & 3] = (lanes32[i & 3] ^ words32[i]) * 0xcc9e2d51u;
      uint32_t low = lanes32[0] ^ ((lanes32[1] >> 11) | (lanes32[1] << 21));
      uint32_t high = lanes32[2] ^ ((lanes32[3] >> 17) | (lanes32[3] << 15));
      low ^= low >> 16; low *= 0x85ebca6bu; low ^= low >> 13;
      high ^= high >> 16; high *= 0xc2b2ae35u; high ^= high >> 13;
      return ((uint64_t(high ^ low) << 32) | low) | 1u;
#else
      constexpr size_t kWords = sizeof(GraphicsPipelineKey) / sizeof(uint64_t);
      uint64_t words[kWords];
      std::memcpy(words, this, sizeof(words));
      // Four independent lanes, so the multiplies overlap.
      uint64_t lanes[4] = { 0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full,
                            0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull };
      for (size_t i = 0; i < kWords; i++)
        lanes[i & 3] = (lanes[i & 3] ^ words[i]) * 0xff51afd7ed558ccdull;
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
  static_assert(sizeof(GraphicsPipelineKey) == 5 * 16 + 4 * 8 + 3 * 4 + 8 * 4 + 4 + 4 + 4,
    "GraphicsPipelineKey must not have padding");

  // The pipelines one context bound, by key. The device keeps every
  // pipeline it built, so the lookup only points at them.
  class GraphicsPipelineLookup {
  public:
    IBackendPipeline* find(const GraphicsPipelineKey& key, uint64_t hash) const {
      if (m_entries.empty())
        return nullptr;
      const size_t mask = m_entries.size() - 1;
      for (size_t i = hash & mask; ; i = (i + 1) & mask) {
        const Entry& entry = m_entries[i];
        if (!entry.hash)
          return nullptr;
        if (entry.hash == hash && entry.key == key)
          return entry.pipeline;
      }
    }

    void insert(const GraphicsPipelineKey& key, uint64_t hash, IBackendPipeline* pipeline) {
      // A context that has seen this many pipelines starts over rather than
      // growing without bound; the device still has them all.
      if (m_count >= kMaxEntries)
        clear();
      if ((m_count + 1) * 2 > m_entries.size())
        grow();
      place(key, hash, pipeline);
    }

    void clear() {
      m_entries.clear();
      m_count = 0;
    }

  private:
    static constexpr size_t kInitialCapacity = 512;
    static constexpr size_t kMaxEntries = 16384;

    struct Entry {
      uint64_t            hash = 0;
      GraphicsPipelineKey key;
      IBackendPipeline*   pipeline = nullptr;
    };

    std::vector<Entry> m_entries;
    size_t             m_count = 0;

    void place(const GraphicsPipelineKey& key, uint64_t hash, IBackendPipeline* pipeline) {
      const size_t mask = m_entries.size() - 1;
      for (size_t i = hash & mask; ; i = (i + 1) & mask) {
        Entry& entry = m_entries[i];
        if (!entry.hash) {
          entry.hash = hash;
          entry.key = key;
          entry.pipeline = pipeline;
          m_count++;
          return;
        }
        if (entry.hash == hash && entry.key == key) {
          entry.pipeline = pipeline;
          return;
        }
      }
    }

    void grow() {
      std::vector<Entry> old = std::move(m_entries);
      m_entries.clear();
      m_entries.resize(old.empty() ? kInitialCapacity : old.size() * 2);
      m_count = 0;
      for (const Entry& entry : old) {
        if (entry.hash)
          place(entry.key, entry.hash, entry.pipeline);
      }
    }
  };

}
