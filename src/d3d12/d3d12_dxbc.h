#pragma once

// Minimal DXBC container access: shader signatures, rebuilding a container
// with a replaced chunk, and the container checksum the runtime verifies.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dxup::d3d12::dxbc {

  constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
  }

  constexpr uint32_t kISGN = fourcc('I', 'S', 'G', 'N');
  constexpr uint32_t kISG1 = fourcc('I', 'S', 'G', '1');
  constexpr uint32_t kOSGN = fourcc('O', 'S', 'G', 'N');
  constexpr uint32_t kOSG1 = fourcc('O', 'S', 'G', '1');
  constexpr uint32_t kOSG5 = fourcc('O', 'S', 'G', '5');

  struct SignatureElement {
    std::string name;
    uint32_t    semanticIndex = 0;
    uint32_t    systemValue   = 0;
    uint32_t    componentType = 0;
    uint32_t    reg           = 0;
    uint8_t     mask          = 0;
    uint8_t     rwMask        = 0;
    uint32_t    stream        = 0;
    uint32_t    minPrecision  = 0;
  };

  struct Signature {
    uint32_t                      chunk = 0;
    std::vector<SignatureElement> elements;
  };

  inline uint32_t readU32(const uint8_t* p) {
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
  }

  inline void writeU32(uint8_t* p, uint32_t value) {
    std::memcpy(p, &value, 4);
  }

  // Chunk data of the first chunk of one of the given types.
  inline bool findChunk(const void* code, size_t size, const uint32_t* types, size_t typeCount,
                        uint32_t& type, const uint8_t*& data, uint32_t& dataSize) {
    const auto* bytes = static_cast<const uint8_t*>(code);
    if (!bytes || size < 32 || readU32(bytes) != fourcc('D', 'X', 'B', 'C'))
      return false;
    const uint32_t count = readU32(bytes + 28);
    if (32 + uint64_t(count) * 4 > size)
      return false;
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t offset = readU32(bytes + 32 + i * 4);
      if (uint64_t(offset) + 8 > size)
        return false;
      const uint32_t chunkType = readU32(bytes + offset);
      const uint32_t chunkSize = readU32(bytes + offset + 4);
      if (uint64_t(offset) + 8 + chunkSize > size)
        return false;
      for (size_t t = 0; t < typeCount; t++) {
        if (chunkType == types[t]) {
          type = chunkType;
          data = bytes + offset + 8;
          dataSize = chunkSize;
          return true;
        }
      }
    }
    return false;
  }

  // The program type of DXBC code (0 pixel, 1 vertex, 2 geometry, 3 hull,
  // 4 domain, 5 compute); -1 without readable code.
  inline int programType(const void* code, size_t size) {
    const uint32_t types[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t type = 0, dataSize = 0;
    const uint8_t* data = nullptr;
    if (!findChunk(code, size, types, 2, type, data, dataSize) || dataSize < 4)
      return -1;
    return int(readU32(data) >> 16);
  }

  inline uint32_t elementSize(uint32_t chunk) {
    return chunk == kISG1 || chunk == kOSG1 ? 32u : chunk == kOSG5 ? 28u : 24u;
  }

  // The input (vertex attributes, interpolants) or output signature.
  inline bool readSignature(const void* code, size_t size, bool output, Signature& signature) {
    const uint32_t inputs[] = { kISG1, kISGN };
    const uint32_t outputs[] = { kOSG1, kOSG5, kOSGN };
    uint32_t type = 0, dataSize = 0;
    const uint8_t* data = nullptr;
    if (!findChunk(code, size, output ? outputs : inputs, output ? 3 : 2, type, data, dataSize) || dataSize < 8)
      return false;
    const uint32_t count = readU32(data);
    const uint32_t first = readU32(data + 4);
    const uint32_t stride = elementSize(type);
    if (uint64_t(first) + uint64_t(count) * stride > dataSize)
      return false;
    signature.chunk = type;
    signature.elements.clear();
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t* e = data + first + i * stride;
      const bool streamed = type != kISGN && type != kOSGN;
      SignatureElement element;
      const uint8_t* fields = streamed ? e + 4 : e;
      element.stream = streamed ? readU32(e) : 0;
      const uint32_t nameOffset = readU32(fields);
      element.semanticIndex = readU32(fields + 4);
      element.systemValue = readU32(fields + 8);
      element.componentType = readU32(fields + 12);
      element.reg = readU32(fields + 16);
      element.mask = fields[20];
      element.rwMask = fields[21];
      element.minPrecision = stride == 32 ? readU32(fields + 24) : 0;
      if (nameOffset >= dataSize)
        return false;
      const char* name = reinterpret_cast<const char*>(data + nameOffset);
      element.name.assign(name, strnlen(name, dataSize - nameOffset));
      signature.elements.push_back(std::move(element));
    }
    return true;
  }

  inline std::vector<uint8_t> writeSignature(const Signature& signature) {
    const uint32_t stride = elementSize(signature.chunk);
    const bool streamed = signature.chunk != kISGN && signature.chunk != kOSGN;
    const uint32_t count = uint32_t(signature.elements.size());
    std::vector<uint8_t> data(8 + size_t(count) * stride, 0);
    writeU32(data.data(), count);
    writeU32(data.data() + 4, 8);
    std::vector<std::pair<std::string, uint32_t>> names;
    for (uint32_t i = 0; i < count; i++) {
      const auto& element = signature.elements[i];
      uint32_t nameOffset = 0;
      for (const auto& entry : names) {
        if (entry.first == element.name)
          nameOffset = entry.second;
      }
      if (!nameOffset) {
        nameOffset = uint32_t(data.size());
        names.emplace_back(element.name, nameOffset);
        data.insert(data.end(), element.name.begin(), element.name.end());
        data.push_back(0);
      }
      uint8_t* e = data.data() + 8 + i * stride;
      uint8_t* fields = streamed ? e + 4 : e;
      if (streamed)
        writeU32(e, element.stream);
      writeU32(fields, nameOffset);
      writeU32(fields + 4, element.semanticIndex);
      writeU32(fields + 8, element.systemValue);
      writeU32(fields + 12, element.componentType);
      writeU32(fields + 16, element.reg);
      fields[20] = element.mask;
      fields[21] = element.rwMask;
      if (stride == 32)
        writeU32(fields + 24, element.minPrecision);
    }
    while (data.size() & 3)
      data.push_back(0xab);
    return data;
  }

  // The container checksum: MD5 over everything after the digest, with the
  // DXBC way of padding (bit count first in the last block).
  inline void computeChecksum(const uint8_t* data, uint32_t size, uint32_t digest[4]) {
    static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint32_t R[64] = {
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
      5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    auto transform = [&](const uint32_t* x) {
      uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
      for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        const uint32_t sum = a + f + K[i] + x[g];
        a = d; d = c; c = b;
        b = b + ((sum << R[i]) | (sum >> (32 - R[i])));
      }
      state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    };
    uint32_t block[16];
    uint32_t offset = 0;
    for (; offset + 64 <= size; offset += 64) {
      std::memcpy(block, data + offset, 64);
      transform(block);
    }
    const uint32_t left = size - offset;
    uint8_t tail[128] = {};
    if (left < 56) {
      // Bit count, the rest of the data, 0x80, zeros, and (size << 1) | 1.
      writeU32(tail, size << 3);
      std::memcpy(tail + 4, data + offset, left);
      tail[4 + left] = 0x80;
      writeU32(tail + 60, (size << 1) | 1);
      std::memcpy(block, tail, 64);
      transform(block);
    } else {
      std::memcpy(tail, data + offset, left);
      tail[left] = 0x80;
      std::memcpy(block, tail, 64);
      transform(block);
      std::memset(tail, 0, sizeof(tail));
      writeU32(tail, size << 3);
      writeU32(tail + 60, (size << 1) | 1);
      std::memcpy(block, tail, 64);
      transform(block);
    }
    std::memcpy(digest, state, 16);
  }

  inline bool checksumMatches(const void* code, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(code);
    if (size < 32)
      return false;
    uint32_t digest[4];
    computeChecksum(bytes + 20, uint32_t(size - 20), digest);
    return std::memcmp(digest, bytes + 4, 16) == 0;
  }

  inline bool isDeclaration(uint32_t opcode) {
    // dcl_resource .. dcl_global_flags, the D3D11 declarations, and
    // dcl_gs_instance_count; custom data holds immediate constant buffers.
    return (opcode >= 0x58 && opcode <= 0x6a) || (opcode >= 0x8f && opcode <= 0xa2) || opcode == 0xce || opcode == 0x35;
  }

  // Code chunk tokens with outputs added that the shader sets to (0, 0, 0, 1)
  // before anything else: declarations after the existing ones, moves first.
  inline bool addConstantOutputCode(const uint8_t* data, uint32_t size, const std::vector<SignatureElement>& outputs,
                                    std::vector<uint8_t>& out) {
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    if (tokens[1] != tokens.size())
      return false;
    size_t insert = 2;
    while (insert < tokens.size()) {
      const uint32_t opcode = tokens[insert] & 0x7ff;
      uint32_t length = (tokens[insert] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = insert + 1 < tokens.size() ? tokens[insert + 1] : 0;
      if (!length || insert + length > tokens.size())
        return false;
      if (!isDeclaration(opcode))
        break;
      insert += length;
    }
    std::vector<uint32_t> added;
    for (const auto& output : outputs) {
      const uint32_t operand = 2u | (uint32_t(output.mask & 0xf) << 4) | (2u << 12) | (1u << 20);
      added.insert(added.end(), { 0x65u | (3u << 24), operand, output.reg });
    }
    for (const auto& output : outputs) {
      const uint32_t operand = 2u | (uint32_t(output.mask & 0xf) << 4) | (2u << 12) | (1u << 20);
      const uint32_t one = output.componentType == 3 ? 0x3f800000u : 1u;
      added.insert(added.end(), { 0x36u | (8u << 24), operand, output.reg, 2u | (4u << 12), 0u, 0u, 0u, one });
    }
    tokens.insert(tokens.begin() + insert, added.begin(), added.end());
    tokens[1] = uint32_t(tokens.size());
    out.resize(tokens.size() * 4);
    std::memcpy(out.data(), tokens.data(), out.size());
    return true;
  }

  // Code chunk tokens of a pixel shader that interpolates its inputs at each
  // sample instead of the pixel center or centroid, which makes it run once
  // per sample. False when nothing is interpolated: such a shader computes
  // the same for every sample of a pixel.
  inline bool sampleFrequencyCode(const uint8_t* data, uint32_t size, std::vector<uint8_t>& out) {
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    const uint32_t version = tokens[0];
    const uint32_t programType = version >> 16;
    const uint32_t major = (version >> 4) & 0xf;
    const uint32_t minor = version & 0xf;
    if (tokens[1] != tokens.size() || programType != 0 || major < 4)
      return false;
    bool changed = false;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      if (!isDeclaration(opcode))
        break;
      // dcl_input_ps and dcl_input_ps_siv hold the interpolation mode in
      // bits 11-14: linear and centroid become sample, with or without
      // perspective.
      if (opcode == 0x62 || opcode == 0x64) {
        const uint32_t mode = (tokens[at] >> 11) & 0xf;
        uint32_t sampleMode = mode;
        if (mode == 2 || mode == 3)
          sampleMode = 6;
        else if (mode == 4 || mode == 5)
          sampleMode = 7;
        if (sampleMode != mode) {
          tokens[at] = (tokens[at] & ~(0xfu << 11)) | (sampleMode << 11);
          changed = true;
        }
      }
      at += length;
    }
    if (!changed)
      return false;
    // Sample interpolation came with shader model 4.1.
    if (major == 4 && minor == 0)
      tokens[0] = (version & ~0xfu) | 1u;
    out.resize(tokens.size() * 4);
    std::memcpy(out.data(), tokens.data(), out.size());
    return true;
  }

  // The container with the first chunk of the given type replaced.
  inline bool replaceChunk(const void* code, size_t size, uint32_t type, const std::vector<uint8_t>& chunkData,
                           std::vector<uint8_t>& out) {
    const auto* bytes = static_cast<const uint8_t*>(code);
    if (!bytes || size < 32 || readU32(bytes) != fourcc('D', 'X', 'B', 'C'))
      return false;
    const uint32_t count = readU32(bytes + 28);
    if (32 + uint64_t(count) * 4 > size)
      return false;
    out.assign(bytes, bytes + 32 + size_t(count) * 4);
    bool replaced = false;
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t offset = readU32(bytes + 32 + i * 4);
      if (uint64_t(offset) + 8 > size)
        return false;
      const uint32_t chunkType = readU32(bytes + offset);
      const uint32_t chunkSize = readU32(bytes + offset + 4);
      if (uint64_t(offset) + 8 + chunkSize > size)
        return false;
      writeU32(out.data() + 32 + i * 4, uint32_t(out.size()));
      if (chunkType == type && !replaced) {
        uint8_t header[8];
        writeU32(header, chunkType);
        writeU32(header + 4, uint32_t(chunkData.size()));
        out.insert(out.end(), header, header + 8);
        out.insert(out.end(), chunkData.begin(), chunkData.end());
        replaced = true;
      } else {
        out.insert(out.end(), bytes + offset, bytes + offset + 8 + chunkSize);
      }
    }
    if (!replaced)
      return false;
    writeU32(out.data() + 24, uint32_t(out.size()));
    uint32_t digest[4];
    computeChecksum(out.data() + 20, uint32_t(out.size() - 20), digest);
    std::memcpy(out.data() + 4, digest, 16);
    return true;
  }

  // What a pixel shader's code does (PixelShaderTraits bits): whether it
  // interpolates inputs other than the pixel position, discards pixels,
  // writes unordered access views or reads the coverage mask.
  inline uint32_t pixelShaderTraitsCode(const uint8_t* data, uint32_t size) {
    if (size < 8 || (size & 3))
      return 0;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    if (tokens[1] != tokens.size() || (tokens[0] >> 16) != 0)
      return 0;
    uint32_t traits = 0;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return traits;
      const uint32_t mode = (tokens[at] >> 11) & 0xf;
      const bool interpolated = mode >= 2 && mode <= 5;
      // dcl_input_ps; dcl_input_ps_siv names its system value last, 1 is
      // the position.
      if ((opcode == 0x62 && interpolated) || (opcode == 0x64 && interpolated && tokens[at + length - 1] != 1))
        traits |= 1u;
      else if (opcode == 0x0d)
        traits |= 2u;
      else if (opcode >= 0x9c && opcode <= 0x9e)
        traits |= 4u;
      // dcl_input of the input coverage mask (operand type 0x23).
      else if (opcode == 0x5f && length > 1 && ((tokens[at + 1] >> 12) & 0xff) == 0x23)
        traits |= 8u;
      at += length;
    }
    return traits;
  }

  // The shader resource registers (t#) a shader declares, as a bit mask of
  // 128 slots. False when the code cannot tell (shader model 5.1 ranges).
  inline bool shaderResourceSlotsCode(const uint8_t* data, uint32_t size, uint64_t mask[2]) {
    mask[0] = mask[1] = 0;
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    if (tokens[1] != tokens.size())
      return false;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      if (!isDeclaration(opcode))
        break;
      // dcl_resource, dcl_resource_raw, dcl_resource_structured: a resource
      // operand with a one-dimensional immediate index.
      if (opcode == 0x58 || opcode == 0xa1 || opcode == 0xa2) {
        if (length < 3)
          return false;
        const uint32_t operand = tokens[at + 1];
        const uint32_t indexDimension = (operand >> 20) & 3;
        const uint32_t indexRepresentation = (operand >> 22) & 7;
        if (indexDimension != 1 || indexRepresentation != 0)
          return false;
        const uint32_t slot = tokens[at + 2];
        if (slot >= 128)
          return false;
        mask[slot >> 6] |= 1ull << (slot & 63);
      }
      at += length;
    }
    return true;
  }

  // The resource dimension (D3D10_SB_RESOURCE_DIMENSION: 1 buffer .. 10 cube
  // array, 11 raw, 12 structured) each shader resource (t#) and unordered
  // access view (u#) register is declared with; 0 where none is declared.
  inline bool resourceDimensionsCode(const uint8_t* data, uint32_t size, uint8_t srv[128], uint8_t uav[64]) {
    std::memset(srv, 0, 128);
    std::memset(uav, 0, 64);
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    if (tokens[1] != tokens.size())
      return false;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      if (!isDeclaration(opcode))
        break;
      // dcl_resource(_raw, _structured), dcl_uav_typed(_raw, _structured):
      // a register operand with a one-dimensional immediate index.
      const bool isSrv = opcode == 0x58 || opcode == 0xa1 || opcode == 0xa2;
      const bool isUav = opcode >= 0x9c && opcode <= 0x9e;
      if ((isSrv || isUav) && length >= 3) {
        const uint32_t operand = tokens[at + 1];
        if (((operand >> 20) & 3) == 1 && ((operand >> 22) & 7) == 0) {
          const uint32_t slot = tokens[at + 2];
          const uint8_t dimension = opcode == 0xa1 || opcode == 0x9d ? 11
                                  : opcode == 0xa2 || opcode == 0x9e ? 12
                                  : uint8_t((tokens[at] >> 11) & 0x1f);
          if (isSrv && slot < 128)
            srv[slot] = dimension;
          else if (isUav && slot < 64)
            uav[slot] = dimension;
        }
      }
      at += length;
    }
    return true;
  }

  // How a shader accesses each unordered access view register (u#), as
  // UavAccess bits: loads, stores, and atomic operations without a return
  // value by kind. Draws or dispatches whose accesses to a view all use the
  // same one of those atomic operations give the same result in any order.
  enum UavAccess : uint16_t {
    kUavRead = 1u << 0,
    kUavWrite = 1u << 1,
    kUavAtomicAnd = 1u << 2,
    kUavAtomicOr = 1u << 3,
    kUavAtomicXor = 1u << 4,
    kUavAtomicAdd = 1u << 5,
    kUavAtomicIMax = 1u << 6,
    kUavAtomicIMin = 1u << 7,
    kUavAtomicUMax = 1u << 8,
    kUavAtomicUMin = 1u << 9,
    kUavReadWrite = kUavRead | kUavWrite,
  };

  // The tokens one operand takes, or 0 when they run past the end.
  inline uint32_t operandLength(const uint32_t* tokens, uint32_t available) {
    if (!available)
      return 0;
    const uint32_t operand = tokens[0];
    uint32_t at = 1;
    // Extended operand tokens (modifiers, minimum precision) chain.
    bool extended = (operand >> 31) != 0;
    while (extended) {
      if (at >= available)
        return 0;
      extended = (tokens[at++] >> 31) != 0;
    }
    const uint32_t type = (operand >> 12) & 0xff;
    const uint32_t components = operand & 3;
    if (type == 4 || type == 5) {
      // Immediate values: one, or four 32-bit (two 64-bit) components.
      uint32_t count = components == 1 ? (type == 5 ? 2u : 1u) : components == 2 ? 4u : 0u;
      at += count;
      return at <= available ? at : 0;
    }
    const uint32_t indexDimension = (operand >> 20) & 3;
    for (uint32_t index = 0; index < indexDimension; index++) {
      const uint32_t representation = (operand >> (22 + 3 * index)) & 7;
      if (representation == 0 || representation == 3)
        at += 1;
      else if (representation == 1 || representation == 4)
        at += 2;
      else if (representation != 2)
        return 0;
      if (representation >= 2 && representation <= 4) {
        if (at >= available)
          return 0;
        const uint32_t relative = operandLength(tokens + at, available - at);
        if (!relative)
          return 0;
        at += relative;
      }
      if (at > available)
        return 0;
    }
    return at;
  }

  inline uint16_t uavAccessOfOpcode(uint32_t opcode) {
    switch (opcode) {
      case 0x3d: // resinfo
      case 0x79: // bufinfo
        return 0;
      case 0xa3: // ld_uav_typed
      case 0xa5: // ld_raw
      case 0xa7: // ld_structured
        return kUavRead;
      case 0xa4: // store_uav_typed
      case 0xa6: // store_raw
      case 0xa8: // store_structured
        return kUavWrite;
      case 0xa9: return kUavAtomicAnd;
      case 0xaa: return kUavAtomicOr;
      case 0xab: return kUavAtomicXor;
      case 0xad: return kUavAtomicAdd;
      case 0xae: return kUavAtomicIMax;
      case 0xaf: return kUavAtomicIMin;
      case 0xb0: return kUavAtomicUMax;
      case 0xb1: return kUavAtomicUMin;
      default:
        // atomic_cmp_store, the atomics returning values, counters and
        // anything else depend on the order.
        return kUavReadWrite;
    }
  }

  // False when the code cannot tell (shader model 5.1 ranges, unknown
  // encodings): callers then assume every declared view is read and written.
  inline bool uavAccessCode(const uint8_t* data, uint32_t size, uint16_t access[64]) {
    std::memset(access, 0, 64 * sizeof(uint16_t));
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    const uint32_t major = (tokens[0] >> 4) & 0xf, minor = tokens[0] & 0xf;
    if (tokens[1] != tokens.size() || major > 5 || (major == 5 && minor > 0))
      return false;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      // fcall carries a function table index before its operand.
      if (opcode == 0x78)
        return false;
      if (!isDeclaration(opcode)) {
        // Extended opcode tokens chain; the operands follow.
        uint32_t operandAt = uint32_t(at) + 1;
        bool extended = (tokens[at] >> 31) != 0;
        while (extended && operandAt < at + length)
          extended = (tokens[operandAt++] >> 31) != 0;
        const uint32_t end = uint32_t(at + length);
        while (operandAt < end) {
          const uint32_t operandSize = operandLength(&tokens[operandAt], end - operandAt);
          if (!operandSize)
            return false;
          const uint32_t operand = tokens[operandAt];
          if (((operand >> 12) & 0xff) == 0x1e) {
            // u# with a one-dimensional immediate index.
            uint32_t tokenAt = operandAt + 1;
            bool extendedOperand = (operand >> 31) != 0;
            while (extendedOperand && tokenAt < end)
              extendedOperand = (tokens[tokenAt++] >> 31) != 0;
            if (((operand >> 20) & 3) != 1 || ((operand >> 22) & 7) != 0 || tokenAt >= end || tokens[tokenAt] >= 64)
              return false;
            access[tokens[tokenAt]] |= uavAccessOfOpcode(opcode);
          }
          operandAt += operandSize;
        }
      }
      at += length;
    }
    return true;
  }

  inline bool uavAccess(const void* code, size_t size, uint16_t access[64]) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && uavAccessCode(codeData, codeSize, access);
  }

  // One past the highest constant buffer (cb#), shader resource (t#),
  // sampler (s#) and unordered access view (u#) register the code declares,
  // for code whose reflection data was stripped (Unreal Engine 4 ships its
  // shaders so). False for shader model 5.1 ranges and unknown encodings.
  inline bool declaredSlotCountsCode(const uint8_t* data, uint32_t size, uint32_t& cbv, uint32_t& srv,
                                     uint32_t& sampler, uint32_t& uav) {
    cbv = srv = sampler = uav = 0;
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    const uint32_t major = (tokens[0] >> 4) & 0xf, minor = tokens[0] & 0xf;
    if (tokens[1] != tokens.size() || major > 5 || (major == 5 && minor > 0))
      return false;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      // dcl_resource(_raw, _structured), dcl_constantbuffer, dcl_sampler,
      // dcl_uav_typed(_raw, _structured): the register's slot is the first
      // immediate index of the operand (constant buffers add their size).
      uint32_t* count = opcode == 0x58 || opcode == 0xa1 || opcode == 0xa2 ? &srv
                      : opcode == 0x59 ? &cbv
                      : opcode == 0x5a ? &sampler
                      : opcode >= 0x9c && opcode <= 0x9e ? &uav : nullptr;
      if (count) {
        if (length < 3)
          return false;
        const uint32_t operand = tokens[at + 1];
        const uint32_t dimension = (operand >> 20) & 3;
        if ((operand >> 31) || dimension != (opcode == 0x59 ? 2u : 1u) || ((operand >> 22) & 7) != 0)
          return false;
        const uint32_t slot = tokens[at + 2];
        if (slot >= 128)
          return false;
        *count = (std::max)(*count, slot + 1);
      }
      at += length;
    }
    return true;
  }

  inline bool declaredSlotCounts(const void* code, size_t size, uint32_t& cbv, uint32_t& srv,
                                 uint32_t& sampler, uint32_t& uav) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && declaredSlotCountsCode(codeData, codeSize, cbv, srv, sampler, uav);
  }

  inline bool resourceDimensions(const void* code, size_t size, uint8_t srv[128], uint8_t uav[64]) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && resourceDimensionsCode(codeData, codeSize, srv, uav);
  }

  inline bool shaderResourceSlots(const void* code, size_t size, uint64_t mask[2]) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && shaderResourceSlotsCode(codeData, codeSize, mask);
  }

  inline uint32_t pixelShaderTraits(const void* code, size_t size) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
      ? pixelShaderTraitsCode(codeData, codeSize) : 0u;
  }

  // A pixel shader that runs once per sample (see sampleFrequencyCode).
  inline bool makeSampleFrequency(const void* code, size_t size, std::vector<uint8_t>& out) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    std::vector<uint8_t> newCode;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && sampleFrequencyCode(codeData, codeSize, newCode)
        && replaceChunk(code, size, codeType, newCode, out);
  }

  // ---- Shader outputs nothing takes ----

  // Code chunk tokens of a pixel or vertex shader whose writes to o#
  // keep only the components in masks[register] (registers from maskCount
  // on stay whole); an instruction left writing nothing becomes nops.
  // Direct3D 11 drivers compile a shader for what the pipeline uses and drop
  // the work behind the rest: a float4 into an R8 target (the texture reads
  // behind green, blue and alpha), a color without a target, vertex outputs
  // the pixel shader does not read. NVIDIA's Direct3D 12 compiler keeps it.
  // False when nothing changes or the code is not understood.
  inline bool maskOutputWritesCode(const uint8_t* data, uint32_t size, const uint8_t* masks, uint32_t maskCount,
                                   std::vector<uint8_t>& out) {
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    const uint32_t programType = tokens[0] >> 16, major = (tokens[0] >> 4) & 0xf, minor = tokens[0] & 0xf;
    if (tokens[1] != tokens.size() || programType > 1 || major < 4 || major > 5 || (major == 5 && minor > 0))
      return false;
    bool changed = false;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      // fcall carries a function table index before its operand.
      if (opcode == 0x78)
        return false;
      if (!isDeclaration(opcode)) {
        uint32_t operandAt = uint32_t(at) + 1;
        bool extended = (tokens[at] >> 31) != 0;
        while (extended && operandAt < at + length)
          extended = (tokens[operandAt++] >> 31) != 0;
        const uint32_t end = uint32_t(at + length);
        // imul, udiv, sincos, umul, uaddc, usubb and swapc have two
        // destinations; the immediate atomics write a UAV besides theirs.
        const bool keepInstruction = opcode == 0x26 || opcode == 0x4d || opcode == 0x4e || opcode == 0x51
          || opcode == 0x84 || opcode == 0x85 || opcode == 0x8e || (opcode >= 0xb2 && opcode <= 0xbd);
        bool writesNothing = false;
        while (operandAt < end) {
          const uint32_t operandSize = operandLength(&tokens[operandAt], end - operandAt);
          if (!operandSize)
            return false;
          uint32_t& operand = tokens[operandAt];
          // o# (pixel and vertex shaders read no output: every one is a
          // destination).
          if (((operand >> 12) & 0xff) == 2) {
            uint32_t indexAt = operandAt + 1;
            bool extendedOperand = (operand >> 31) != 0;
            while (extendedOperand && indexAt < end)
              extendedOperand = (tokens[indexAt++] >> 31) != 0;
            // One immediate index, four components selected by a mask.
            if (((operand >> 20) & 3) != 1 || ((operand >> 22) & 7) != 0 || indexAt >= end
             || (operand & 3) != 2 || ((operand >> 2) & 3) != 0)
              return false;
            const uint32_t reg = tokens[indexAt];
            const uint32_t mask = (operand >> 4) & 0xf;
            const uint32_t kept = reg < maskCount ? mask & masks[reg] : mask;
            if (kept != mask) {
              if (kept) {
                operand = (operand & ~(0xfu << 4)) | (kept << 4);
                changed = true;
              } else if (!keepInstruction) {
                writesNothing = true;
              }
            }
          }
          operandAt += operandSize;
        }
        if (writesNothing) {
          for (size_t i = at; i < at + length; i++)
            tokens[i] = 0x3au | (1u << 24);
          changed = true;
        }
      }
      at += length;
    }
    if (!changed)
      return false;
    out.resize(tokens.size() * 4);
    std::memcpy(out.data(), tokens.data(), out.size());
    return true;
  }

  inline bool maskOutputWrites(const void* code, size_t size, const uint8_t* masks, uint32_t maskCount,
                               std::vector<uint8_t>& out) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    std::vector<uint8_t> newCode;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && maskOutputWritesCode(codeData, codeSize, masks, maskCount, newCode)
        && replaceChunk(code, size, codeType, newCode, out);
  }

  // The components of a vertex shader's output registers (32) the next
  // stage takes: system values (position, clip and cull distances, render
  // target and viewport index) and the elements the pixel shader reads by
  // semantic (none without one). Registers outside the signature stay whole.
  // False when every output is taken or a signature cannot be read.
  inline bool vertexOutputMasks(const void* vs, size_t vsSize, const void* ps, size_t psSize, uint8_t masks[32]) {
    Signature outputs, inputs;
    if (!readSignature(vs, vsSize, true, outputs) || (ps && !readSignature(ps, psSize, false, inputs)))
      return false;
    for (uint32_t i = 0; i < 32; i++)
      masks[i] = 0xf;
    for (const auto& element : outputs.elements) {
      if (element.reg < 32)
        masks[element.reg] = 0;
    }
    bool dropped = false;
    for (const auto& element : outputs.elements) {
      if (element.reg >= 32 || element.stream)
        continue;
      bool taken = element.systemValue != 0;
      for (size_t i = 0; i < inputs.elements.size() && !taken; i++) {
        const auto& input = inputs.elements[i];
        taken = input.rwMask && input.semanticIndex == element.semanticIndex
          && _stricmp(input.name.c_str(), element.name.c_str()) == 0;
      }
      if (taken)
        masks[element.reg] |= element.mask;
      else
        dropped = true;
    }
    return dropped;
  }

  // A vertex shader that also writes (0, 0, 0, 1) to the given outputs, as
  // D3D11 gives a pixel shader for registers nothing writes.
  inline bool addConstantOutputs(const void* code, size_t size, const std::vector<SignatureElement>& outputs,
                                 std::vector<uint8_t>& out) {
    Signature signature;
    if (outputs.empty() || !readSignature(code, size, true, signature))
      return false;
    for (const auto& output : outputs) {
      size_t at = 0;
      while (at < signature.elements.size() && signature.elements[at].reg <= output.reg)
        at++;
      signature.elements.insert(signature.elements.begin() + at, output);
    }
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    std::vector<uint8_t> newCode, withSignature;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && addConstantOutputCode(codeData, codeSize, outputs, newCode)
        && replaceChunk(code, size, signature.chunk, writeSignature(signature), withSignature)
        && replaceChunk(withSignature.data(), withSignature.size(), codeType, newCode, out);
  }

  // ---- Indexable temporaries indexed only by constants ----

  struct TempArray {
    uint32_t size = 0;
    uint32_t base = 0;
    bool     declared = false;
    bool     dynamic = false;
    bool     promote = false;
  };

  // Marks the arrays (x#) an operand indexes by registers; the operand's
  // token count, 0 when it cannot tell.
  inline uint32_t scanTempArrays(const uint32_t* t, uint32_t available, std::vector<TempArray>& arrays) {
    const uint32_t length = operandLength(t, available);
    if (!length)
      return 0;
    const uint32_t operand = t[0];
    const uint32_t type = (operand >> 12) & 0xff;
    if (type == 4 || type == 5)
      return length;
    uint32_t at = 1;
    bool extended = (operand >> 31) != 0;
    while (extended)
      extended = (t[at++] >> 31) != 0;
    const uint32_t dimensions = (operand >> 20) & 3;
    uint32_t reg = 0;
    for (uint32_t i = 0; i < dimensions; i++) {
      const uint32_t representation = (operand >> (22 + 3 * i)) & 7;
      if (type == 3 && i == 0) {
        // The array register is always given by a constant.
        if (representation != 0)
          return 0;
        reg = t[at];
      }
      if (type == 3 && i == 1 && representation != 0) {
        if (reg >= 4096)
          return 0;
        if (reg >= arrays.size())
          arrays.resize(reg + 1);
        arrays[reg].dynamic = true;
      }
      if (representation == 0 || representation == 3)
        at += 1;
      else if (representation == 1 || representation == 4)
        at += 2;
      if (representation >= 2 && representation <= 4) {
        const uint32_t relative = scanTempArrays(t + at, length - at, arrays);
        if (!relative)
          return 0;
        at += relative;
      }
    }
    return length;
  }

  // Copies an operand, the promoted arrays' elements as ordinary temporaries.
  inline uint32_t rewriteTempArrays(const uint32_t* t, uint32_t available, const std::vector<TempArray>& arrays,
                                    std::vector<uint32_t>& out) {
    const uint32_t length = operandLength(t, available);
    if (!length)
      return 0;
    const uint32_t operand = t[0];
    const uint32_t type = (operand >> 12) & 0xff;
    uint32_t at = 1;
    bool extended = (operand >> 31) != 0;
    while (extended)
      extended = (t[at++] >> 31) != 0;
    if (type == 3 && ((operand >> 20) & 3) == 2 && ((operand >> 22) & 0x3f) == 0) {
      const uint32_t reg = t[at], element = t[at + 1];
      if (reg < arrays.size() && arrays[reg].promote) {
        // r# with a one-dimensional constant index.
        out.push_back((operand & ~((0xffu << 12) | (3u << 20) | (0x1ffu << 22))) | (1u << 20));
        out.insert(out.end(), t + 1, t + at);
        out.push_back(arrays[reg].base + element);
        return length;
      }
    }
    if (type == 4 || type == 5) {
      out.insert(out.end(), t, t + length);
      return length;
    }
    out.insert(out.end(), t, t + at);
    const uint32_t dimensions = (operand >> 20) & 3;
    for (uint32_t i = 0; i < dimensions; i++) {
      const uint32_t representation = (operand >> (22 + 3 * i)) & 7;
      if (representation == 0 || representation == 3) {
        out.push_back(t[at++]);
      } else if (representation == 1 || representation == 4) {
        out.insert(out.end(), t + at, t + at + 2);
        at += 2;
      }
      if (representation >= 2 && representation <= 4) {
        const uint32_t relative = rewriteTempArrays(t + at, length - at, arrays, out);
        if (!relative)
          return 0;
        at += relative;
      }
    }
    return length;
  }

  // Indexable temporaries (x#[n]) that only constants index become ordinary
  // temporaries. fxc keeps arrays it indexes by constants in them (unrolled
  // loops over arrays); NVIDIA's Direct3D 11 compiler gives them registers,
  // through Direct3D 12 (DXIL) they stay in local memory: a full-screen pass
  // of such a shader took 1.6x as long.
  inline bool promoteIndexableTempsCode(const uint8_t* data, uint32_t size, std::vector<uint8_t>& out) {
    constexpr uint32_t kMaxPromotedElements = 64;
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), data, size);
    const uint32_t programType = tokens[0] >> 16, major = (tokens[0] >> 4) & 0xf, minor = tokens[0] & 0xf;
    // Hull shaders declare temporaries per phase; shader model 5.1 encodes
    // registers otherwise.
    if (tokens[1] != tokens.size() || major < 4 || major > 5 || (major == 5 && minor > 0) || programType == 3)
      return false;
    std::vector<TempArray> arrays;
    uint32_t tempCount = 0, tempDeclarations = 0;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      if (opcode == 0x78)   // fcall: interfaces
        return false;
      if (opcode == 0x68 && length >= 2) {
        tempCount = tokens[at + 1];
        tempDeclarations++;
      } else if (opcode == 0x69 && length >= 4) {
        const uint32_t reg = tokens[at + 1];
        if (reg >= 4096)
          return false;
        if (reg >= arrays.size())
          arrays.resize(reg + 1);
        arrays[reg].size = tokens[at + 2];
        arrays[reg].declared = true;
      } else if (!isDeclaration(opcode)) {
        uint32_t operandAt = uint32_t(at) + 1;
        bool extended = (tokens[at] >> 31) != 0;
        while (extended && operandAt < at + length)
          extended = (tokens[operandAt++] >> 31) != 0;
        const uint32_t end = uint32_t(at + length);
        while (operandAt < end) {
          const uint32_t operandSize = scanTempArrays(&tokens[operandAt], end - operandAt, arrays);
          if (!operandSize)
            return false;
          operandAt += operandSize;
        }
      }
      at += length;
    }
    if (tempDeclarations > 1)
      return false;
    uint32_t newCount = tempCount;
    bool any = false;
    for (auto& array : arrays) {
      if (!array.declared || array.dynamic || !array.size || array.size > kMaxPromotedElements)
        continue;
      array.promote = true;
      array.base = newCount;
      newCount += array.size;
      any = true;
    }
    if (!any || newCount > 4096)
      return false;

    std::vector<uint32_t> rewritten;
    rewritten.reserve(tokens.size() + 4);
    rewritten.push_back(tokens[0]);
    rewritten.push_back(0);
    bool tempsDeclared = false;
    at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = tokens[at + 1];
      if (opcode == 0x68) {
        rewritten.push_back(tokens[at]);
        rewritten.push_back(newCount);
        tempsDeclared = true;
      } else if (opcode == 0x69 && tokens[at + 1] < arrays.size() && arrays[tokens[at + 1]].promote) {
        // Its elements are temporaries now.
      } else if (isDeclaration(opcode)) {
        rewritten.insert(rewritten.end(), tokens.begin() + at, tokens.begin() + at + length);
      } else {
        if (!tempsDeclared) {
          rewritten.push_back(0x68u | (2u << 24));
          rewritten.push_back(newCount);
          tempsDeclared = true;
        }
        const size_t start = rewritten.size();
        uint32_t operandAt = uint32_t(at) + 1;
        bool extended = (tokens[at] >> 31) != 0;
        while (extended && operandAt < at + length)
          extended = (tokens[operandAt++] >> 31) != 0;
        rewritten.insert(rewritten.end(), tokens.begin() + at, tokens.begin() + operandAt);
        const uint32_t end = uint32_t(at + length);
        while (operandAt < end) {
          const uint32_t operandSize = rewriteTempArrays(&tokens[operandAt], end - operandAt, arrays, rewritten);
          if (!operandSize)
            return false;
          operandAt += operandSize;
        }
        const uint32_t newLength = uint32_t(rewritten.size() - start);
        if (newLength > 0x7f)
          return false;
        rewritten[start] = (rewritten[start] & ~(0x7fu << 24)) | (newLength << 24);
      }
      at += length;
    }
    rewritten[1] = uint32_t(rewritten.size());
    out.resize(rewritten.size() * 4);
    std::memcpy(out.data(), rewritten.data(), out.size());
    return true;
  }

  inline bool promoteIndexableTemps(const void* code, size_t size, std::vector<uint8_t>& out) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    std::vector<uint8_t> newCode;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && promoteIndexableTempsCode(codeData, codeSize, newCode)
        && replaceChunk(code, size, codeType, newCode, out);
  }

  // ---- Immediate constant buffers as constant buffers ----

  // Copies an operand with immediate constant buffer reads (icb[...]) as
  // reads of constant buffer slot `slot`.
  inline uint32_t rewriteIcbOperand(const uint32_t* t, uint32_t available, uint32_t slot, std::vector<uint32_t>& out) {
    const uint32_t length = operandLength(t, available);
    if (!length)
      return 0;
    const uint32_t operand = t[0];
    const uint32_t type = (operand >> 12) & 0xff;
    if (type == 4 || type == 5) {
      out.insert(out.end(), t, t + length);
      return length;
    }
    uint32_t at = 1;
    bool extended = (operand >> 31) != 0;
    while (extended)
      extended = (t[at++] >> 31) != 0;
    const uint32_t dimensions = (operand >> 20) & 3;
    if (type == 9) {
      if (dimensions != 1)
        return 0;
      // cb<slot>[index]: two dimensions, the slot a constant, the index as
      // it was.
      const uint32_t representation = (operand >> 22) & 7;
      out.push_back((operand & ~((0xffu << 12) | (3u << 20) | (0x1ffu << 22))) | (8u << 12) | (2u << 20)
                    | (representation << 25));
      out.insert(out.end(), t + 1, t + at);
      out.push_back(slot);
    } else {
      out.insert(out.end(), t, t + at);
    }
    for (uint32_t i = 0; i < dimensions; i++) {
      const uint32_t representation = (operand >> (22 + 3 * i)) & 7;
      if (representation == 0 || representation == 3) {
        out.push_back(t[at++]);
      } else if (representation == 1 || representation == 4) {
        out.insert(out.end(), t + at, t + at + 2);
        at += 2;
      }
      if (representation >= 2 && representation <= 4) {
        const uint32_t relative = rewriteIcbOperand(t + at, length - at, slot, out);
        if (!relative)
          return 0;
        at += relative;
      }
    }
    return length;
  }

  // The immediate constant buffer of a shader becomes constant buffer slot
  // `slot` (the first after the declared ones): `data` gets its contents,
  // which the caller binds there. Read through DXIL, an immediate constant
  // buffer the shader indexes by registers took 1.5x as long as the same
  // values in a constant buffer.
  inline bool icbToConstantBufferCode(const uint8_t* code, uint32_t size, std::vector<uint8_t>& out, uint32_t& slot,
                                      std::vector<uint32_t>& data) {
    constexpr uint32_t kConstantBufferSlots = 14;
    if (size < 8 || (size & 3))
      return false;
    std::vector<uint32_t> tokens(size / 4);
    std::memcpy(tokens.data(), code, size);
    const uint32_t major = (tokens[0] >> 4) & 0xf, minor = tokens[0] & 0xf;
    if (tokens[1] != tokens.size() || major < 4 || major > 5 || (major == 5 && minor > 0))
      return false;
    size_t icbAt = 0;
    uint32_t icbLength = 0, usedSlots = 0;
    size_t at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < tokens.size() ? tokens[at + 1] : 0;
      if (!length || at + length > tokens.size())
        return false;
      if (opcode == 0x78)   // fcall: interfaces
        return false;
      if (opcode == 0x35 && (tokens[at] >> 11) == 3) {
        if (icbAt)
          return false;
        icbAt = at;
        icbLength = length;
      } else if (opcode == 0x59 && length >= 3) {
        // dcl_constantbuffer cb<slot>[size]
        const uint32_t declared = tokens[at + 2];
        if (declared < 32)
          usedSlots |= 1u << declared;
      }
      at += length;
    }
    if (!icbAt || icbLength < 2 || ((icbLength - 2) & 3))
      return false;
    slot = 0;
    while (slot < kConstantBufferSlots && (usedSlots >> slot))
      slot++;
    if (slot >= kConstantBufferSlots)
      return false;
    data.assign(tokens.begin() + icbAt + 2, tokens.begin() + icbAt + icbLength);
    const uint32_t vectors = uint32_t(data.size() / 4);

    std::vector<uint32_t> rewritten;
    rewritten.reserve(tokens.size());
    rewritten.push_back(tokens[0]);
    rewritten.push_back(0);
    at = 2;
    while (at < tokens.size()) {
      const uint32_t opcode = tokens[at] & 0x7ff;
      uint32_t length = (tokens[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = tokens[at + 1];
      if (at == icbAt) {
        // dcl_constantbuffer cb<slot>[vectors], dynamicIndexed
        rewritten.insert(rewritten.end(), { 0x59u | (1u << 11) | (4u << 24), 0x208e46u, slot, vectors });
      } else if (isDeclaration(opcode)) {
        rewritten.insert(rewritten.end(), tokens.begin() + at, tokens.begin() + at + length);
      } else {
        const size_t start = rewritten.size();
        uint32_t operandAt = uint32_t(at) + 1;
        bool extended = (tokens[at] >> 31) != 0;
        while (extended && operandAt < at + length)
          extended = (tokens[operandAt++] >> 31) != 0;
        rewritten.insert(rewritten.end(), tokens.begin() + at, tokens.begin() + operandAt);
        const uint32_t end = uint32_t(at + length);
        while (operandAt < end) {
          const uint32_t operandSize = rewriteIcbOperand(&tokens[operandAt], end - operandAt, slot, rewritten);
          if (!operandSize)
            return false;
          operandAt += operandSize;
        }
        const uint32_t newLength = uint32_t(rewritten.size() - start);
        if (newLength > 0x7f)
          return false;
        rewritten[start] = (rewritten[start] & ~(0x7fu << 24)) | (newLength << 24);
      }
      at += length;
    }
    rewritten[1] = uint32_t(rewritten.size());
    out.resize(rewritten.size() * 4);
    std::memcpy(out.data(), rewritten.data(), out.size());
    return true;
  }

  inline bool icbToConstantBuffer(const void* code, size_t size, std::vector<uint8_t>& out, uint32_t& slot,
                                  std::vector<uint32_t>& data) {
    const uint32_t codeTypes[] = { fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R') };
    uint32_t codeType = 0, codeSize = 0;
    const uint8_t* codeData = nullptr;
    std::vector<uint8_t> newCode;
    return findChunk(code, size, codeTypes, 2, codeType, codeData, codeSize)
        && icbToConstantBufferCode(codeData, codeSize, newCode, slot, data)
        && replaceChunk(code, size, codeType, newCode, out);
  }

}
