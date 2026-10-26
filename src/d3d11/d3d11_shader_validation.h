#pragma once

// Shader bytecode and input layouts as D3D11 checks them: bytecode that is
// no DXBC container or a shader of another stage, and input layouts that
// do not feed the shader's inputs fail with E_INVALIDARG before they reach
// D3D12, whose pipeline creation would fail at draw time instead.

#include "d3d11_d3d12_bridge.h"

#include <d3d11_4.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace dxup {

  // Program types of the version token.
  enum class DxbcProgramType : uint32_t {
    Pixel = 0, Vertex = 1, Geometry = 2, Hull = 3, Domain = 4, Compute = 5, Invalid = ~0u,
  };

  struct DxbcChunk {
    uint32_t       fourCC = 0;
    const uint8_t* data   = nullptr;
    uint32_t       size   = 0;
  };

  constexpr uint32_t makeFourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
  }

  // The chunks of a DXBC container; false for anything else.
  inline bool parseDxbc(const void* bytecode, SIZE_T length, std::vector<DxbcChunk>* chunks) {
    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    if (!bytes || length < 32)
      return false;
    auto read = [&](SIZE_T offset) { uint32_t value; std::memcpy(&value, bytes + offset, 4); return value; };
    const uint32_t totalSize = read(24);
    const uint32_t chunkCount = read(28);
    if (read(0) != makeFourCC('D', 'X', 'B', 'C') || totalSize > length || totalSize < 32
     || chunkCount > (totalSize - 32) / 4)
      return false;
    for (uint32_t i = 0; i < chunkCount; i++) {
      const uint32_t offset = read(32 + i * 4);
      if (uint64_t(offset) + 8 > totalSize)
        return false;
      const uint32_t size = read(offset + 4);
      if (uint64_t(offset) + 8 + size > totalSize)
        return false;
      if (chunks)
        chunks->push_back({ read(offset), bytes + offset + 8, size });
    }
    return true;
  }

  inline DxbcProgramType getDxbcProgramType(const void* bytecode, SIZE_T length) {
    std::vector<DxbcChunk> chunks;
    if (!parseDxbc(bytecode, length, &chunks))
      return DxbcProgramType::Invalid;
    for (const auto& chunk : chunks) {
      if ((chunk.fourCC == makeFourCC('S', 'H', 'D', 'R') || chunk.fourCC == makeFourCC('S', 'H', 'E', 'X'))
       && chunk.size >= 8) {
        uint32_t version;
        std::memcpy(&version, chunk.data, 4);
        const uint32_t type = version >> 16;
        // Direct3D 11 runs shader models up to 5.0; 5.1 is Direct3D 12's.
        const uint32_t major = (version >> 4) & 0xfu, minor = version & 0xfu;
        if (major > 5 || (major == 5 && minor > 0))
          return DxbcProgramType::Invalid;
        return type <= uint32_t(DxbcProgramType::Compute) ? DxbcProgramType(type) : DxbcProgramType::Invalid;
      }
    }
    return DxbcProgramType::Invalid;
  }

  inline HRESULT validateShaderBytecode(const void* bytecode, SIZE_T length,
                                        std::initializer_list<DxbcProgramType> accepted) {
    const DxbcProgramType type = getDxbcProgramType(bytecode, length);
    for (DxbcProgramType candidate : accepted) {
      if (type == candidate)
        return S_OK;
    }
    return E_INVALIDARG;
  }

  struct SignatureElement {
    std::string name;
    uint32_t    semanticIndex = 0;
    uint32_t    systemValue   = 0;
    uint8_t     readMask      = 0;
  };

  // The input signature of a shader; false without a readable one.
  inline bool getInputSignature(const void* bytecode, SIZE_T length, std::vector<SignatureElement>& elements) {
    std::vector<DxbcChunk> chunks;
    if (!parseDxbc(bytecode, length, &chunks))
      return false;
    for (const auto& chunk : chunks) {
      const bool isg1 = chunk.fourCC == makeFourCC('I', 'S', 'G', '1');
      if (chunk.fourCC != makeFourCC('I', 'S', 'G', 'N') && !isg1)
        continue;
      if (chunk.size < 8)
        return false;
      auto read = [&](uint32_t offset) { uint32_t value; std::memcpy(&value, chunk.data + offset, 4); return value; };
      const uint32_t count = read(0);
      const uint32_t first = read(4);
      const uint32_t stride = isg1 ? 32u : 24u;
      if (uint64_t(first) + uint64_t(count) * stride > chunk.size)
        return false;
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t base = first + i * stride + (isg1 ? 4u : 0u);
        const uint32_t nameOffset = read(base);
        if (nameOffset >= chunk.size)
          return false;
        SignatureElement element;
        const char* name = reinterpret_cast<const char*>(chunk.data + nameOffset);
        element.name.assign(name, strnlen(name, chunk.size - nameOffset));
        element.semanticIndex = read(base + 4);
        element.systemValue = read(base + 8);
        element.readMask = chunk.data[base + 21];
        elements.push_back(std::move(element));
      }
      return true;
    }
    // Shaders without inputs have no signature to feed.
    elements.clear();
    return true;
  }

  inline bool semanticNamesEqual(const char* a, const std::string& b) {
    return _stricmp(a, b.c_str()) == 0;
  }

  inline HRESULT validateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count,
                                     const void* bytecode, SIZE_T length,
                                     const std::function<UINT(DXGI_FORMAT)>& formatSupport) {
    if (count > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)
      return E_INVALIDARG;
    std::vector<SignatureElement> signature;
    if (!getInputSignature(bytecode, length, signature))
      return E_INVALIDARG;

    for (UINT i = 0; i < count; i++) {
      const auto& e = elements[i];
      if (!e.SemanticName || e.InputSlot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
       || !(formatSupport(e.Format) & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER))
        return E_INVALIDARG;
      if (e.InputSlotClass == D3D11_INPUT_PER_VERTEX_DATA) {
        if (e.InstanceDataStepRate)
          return E_INVALIDARG;
      } else if (e.InputSlotClass != D3D11_INPUT_PER_INSTANCE_DATA) {
        return E_INVALIDARG;
      }
      if (e.AlignedByteOffset != D3D11_APPEND_ALIGNED_ELEMENT) {
        const UINT alignment = (std::min)(computeRowPitch(1, e.Format), 4u);
        if (alignment && e.AlignedByteOffset % alignment)
          return E_INVALIDARG;
      }
      for (UINT j = 0; j < i; j++) {
        if (_stricmp(elements[j].SemanticName, e.SemanticName) == 0 && elements[j].SemanticIndex == e.SemanticIndex)
          return E_INVALIDARG;
      }
    }

    // Every input the shader reads, other than system values, comes from
    // the layout.
    for (const auto& input : signature) {
      if (input.systemValue || !input.readMask)
        continue;
      bool fed = false;
      for (UINT i = 0; i < count && !fed; i++)
        fed = semanticNamesEqual(elements[i].SemanticName, input.name) && elements[i].SemanticIndex == input.semanticIndex;
      if (!fed)
        return E_INVALIDARG;
    }
    return S_OK;
  }

}
